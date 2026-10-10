/*
 *  Copyright (c) 2026 Jeremy HU <jeremy-at-dust3d dot org>. All rights reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */
#include <AutoRemesher/AutoRemesher>
#include <AutoRemesher/IsotropicRemesher>
#include <AutoRemesher/MeshSeparator>
#include <AutoRemesher/Parameterizer>
#include <AutoRemesher/QuadExtractor>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <queue>
#include <set>
// Qt defines `emit` as a macro, which collides with TBB profiling.h's `void emit()`.
// macOS `<mach/mach.h>` also defines `emit`. Undefine before including TBB headers.
#if defined(__APPLE__) || defined(emit)
#undef emit
#endif

// oneAPI TBB (2021+) moved headers under <oneapi/tbb/>. Use __has_include where
// available (clang + GCC) to pick the right path, falling back to the legacy path.
#if defined(__has_include)
#if __has_include(<oneapi/tbb/blocked_range.h>)
#include <oneapi/tbb/blocked_range.h>
#else
#include <tbb/blocked_range.h>
#endif
#if __has_include(<oneapi/tbb/mutex.h>)
#include <oneapi/tbb/mutex.h>
#else
#include <tbb/mutex.h>
#endif
#if __has_include(<oneapi/tbb/parallel_for.h>)
#include <oneapi/tbb/parallel_for.h>
#else
#include <tbb/parallel_for.h>
#endif
#else
#include <tbb/blocked_range.h>
#include <tbb/mutex.h>
#include <tbb/parallel_for.h>
#endif
#include <unordered_map>
#include <unordered_set>

namespace AutoRemesher {

namespace {
    const float parallelPhaseBegin = 0.03f;
    const float parallelPhaseEnd = 0.95f;
}

const double AutoRemesher::m_defaultSharpEdgeDegrees = 90;

double AutoRemesher::calculateAverageEdgeLength(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces)
{
    double sumOfLength = 0.0;
    size_t edgeCount = 0;
    for (const auto& face : faces) {
        for (size_t i = 0; i < face.size(); ++i) {
            size_t j = (i + 1) % face.size();
            sumOfLength += (vertices[face[i]] - vertices[face[j]]).length();
            ++edgeCount;
        }
    }
    if (0 == edgeCount)
        return 0.0;
    return sumOfLength / edgeCount;
}

void AutoRemesher::initializeVoxelSize()
{
    double area = calculateMeshArea(m_vertices, m_triangles);
    double triangleArea = area / m_targetTriangleCount;
    m_voxelSize = std::sqrt(triangleArea / (0.86602540378 * 0.5));
#if AUTO_REMESHER_DEBUG
    std::cerr << "Area: " << area << " voxelSize: " << m_voxelSize << std::endl;
#endif
}

double AutoRemesher::calculateMeshArea(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& triangles)
{
    double area = 0.0;
    for (const auto& it : triangles) {
        area += Vector3::area(vertices[it[0]], vertices[it[1]], vertices[it[2]]);
    }
    return area;
}

void AutoRemesher::resample(std::vector<Vector3>& vertices,
    std::vector<std::vector<size_t>>& triangles,
    double voxelSize,
    double adaptivity,
    double sharpEdgeDegrees,
    double smoothNormalDegrees,
    size_t islandIndex)
{
    std::vector<double> vertexTargetLengths;
    if (adaptivity > 0.0 && !vertices.empty()) {
        // A target-length field redistributes the uniform triangle budget.  The
        // field is deliberately computed on the input mesh: IsotropicRemesher
        // propagates it to vertices created by edge splits.
        const double minRatio = 0.35;
        const double maxRatio = 3.0;
        const double epsilon = 1e-12;

        // Do not add face normals directly from parallel workers: adjacent
        // faces write to the same vertex.  Compute faces in parallel, then do
        // the small accumulation pass serially.
        std::vector<Vector3> faceNormals(triangles.size());
        std::vector<double> faceAreas(triangles.size(), 0.0);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, triangles.size()),
            [&](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i != range.end(); ++i) {
                    const auto& tri = triangles[i];
                    faceAreas[i] = Vector3::area(
                        vertices[tri[0]], vertices[tri[1]], vertices[tri[2]]);
                    if (faceAreas[i] > epsilon)
                        faceNormals[i] = Vector3::normal(
                            vertices[tri[0]], vertices[tri[1]], vertices[tri[2]]);
                }
            });

        std::vector<Vector3> normals(vertices.size());
        std::vector<std::vector<size_t>> neighbors(vertices.size());
        for (size_t i = 0; i < triangles.size(); ++i) {
            const auto& tri = triangles[i];
            if (faceAreas[i] <= epsilon)
                continue;
            const Vector3 weightedNormal = faceNormals[i] * faceAreas[i];
            for (size_t j = 0; j < 3; ++j) {
                normals[tri[j]] += weightedNormal;
                neighbors[tri[j]].push_back(tri[(j + 1) % 3]);
                neighbors[tri[j]].push_back(tri[(j + 2) % 3]);
            }
        }
        tbb::parallel_for(tbb::blocked_range<size_t>(0, normals.size()),
            [&](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i != range.end(); ++i) {
                    normals[i].normalize();
                    auto& ring = neighbors[i];
                    std::sort(ring.begin(), ring.end());
                    ring.erase(std::unique(ring.begin(), ring.end()), ring.end());
                }
            });

        // Mean normal variation per unit length is less sensitive to a single
        // bad triangle than the previous maximum-one-ring estimate.
        std::vector<double> vertexCurvature(vertices.size(), 0.0);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, vertices.size()),
            [&](const tbb::blocked_range<size_t>& range) {
                for (size_t v = range.begin(); v != range.end(); ++v) {
                    const auto& ring = neighbors[v];
                    if (ring.empty() || normals[v].lengthSquared() <= epsilon)
                        continue;
                    double weightedCurvature = 0.0;
                    double totalWeight = 0.0;
                    for (const auto& u : ring) {
                        const double length = (vertices[u] - vertices[v]).length();
                        if (length <= epsilon || normals[u].lengthSquared() <= epsilon)
                            continue;
                        double cosine = Vector3::dotProduct(normals[v], normals[u]);
                        cosine = std::max(-1.0, std::min(1.0, cosine));
                        weightedCurvature += std::acos(cosine);
                        totalWeight += length;
                    }
                    if (totalWeight > epsilon)
                        vertexCurvature[v] = weightedCurvature / totalWeight;
                }
            });

        // A percentile reference prevents a few very sharp/noisy vertices from
        // making the rest of the surface appear flat.
        std::vector<double> nonZeroCurvatures;
        nonZeroCurvatures.reserve(vertexCurvature.size());
        for (double curvature : vertexCurvature) {
            if (curvature > epsilon)
                nonZeroCurvatures.push_back(curvature);
        }
        if (!nonZeroCurvatures.empty()) {
            const size_t referenceIndex = (nonZeroCurvatures.size() - 1) * 3 / 4;
            std::nth_element(nonZeroCurvatures.begin(),
                nonZeroCurvatures.begin() + referenceIndex, nonZeroCurvatures.end());
            const double curvatureReference = nonZeroCurvatures[referenceIndex];
            vertexTargetLengths.resize(vertices.size());
            std::vector<double> importance(vertices.size(), 1.0);
            const double strength = std::min(adaptivity, 2.0) * 7.0;
            tbb::parallel_for(tbb::blocked_range<size_t>(0, vertices.size()),
                [&](const tbb::blocked_range<size_t>& range) {
                    for (size_t v = range.begin(); v != range.end(); ++v) {
                        const double normalized = std::min(4.0,
                            vertexCurvature[v] / std::max(curvatureReference, epsilon));
                        importance[v] += strength * normalized * normalized;
                    }
                });

            // Keep integral(area / h^2) equal to the uniform field, which
            // preserves the budget implied by voxelSize while moving triangles
            // from flat regions to detailed ones.
            double totalArea = 0.0;
            double weightedImportance = 0.0;
            for (size_t i = 0; i < triangles.size(); ++i) {
                const auto& tri = triangles[i];
                totalArea += faceAreas[i];
                weightedImportance += faceAreas[i] * (importance[tri[0]] + importance[tri[1]] + importance[tri[2]]) / 3.0;
            }
            if (totalArea > epsilon) {
                const double averageImportance = weightedImportance / totalArea;
                for (size_t v = 0; v < vertices.size(); ++v) {
                    double multiplier = std::sqrt(averageImportance / importance[v]);
                    multiplier = std::max(minRatio, std::min(maxRatio, multiplier));
                    vertexTargetLengths[v] = voxelSize * multiplier;
                }
            } else {
                vertexTargetLengths.clear();
            }
        }
    }

#if AUTO_REMESHER_DEBUG
    std::cerr << "Island[" << islandIndex << "]: Uniformly remeshing on target edge length: " << voxelSize << std::endl;
#endif
    IsotropicRemesher isotropicRemesher(vertices, triangles);
    isotropicRemesher.setTargetEdgeLength(voxelSize);
    if (!vertexTargetLengths.empty())
        isotropicRemesher.setVertexTargetEdgeLengths(&vertexTargetLengths);
    isotropicRemesher.setSharpEdgeDegrees(sharpEdgeDegrees);
    isotropicRemesher.setSmoothNormalDegrees(smoothNormalDegrees);
    isotropicRemesher.remesh();
    vertices = isotropicRemesher.remeshedVertices();
    triangles = isotropicRemesher.remeshedTriangles();
#if AUTO_REMESHER_DEBUG
    std::cerr << "Island[" << islandIndex << "]: Uniformly remesh done, vertex count: " << vertices.size() << " triangle count: " << triangles.size() << std::endl;
#endif
}

void AutoRemesher::updateProgress(size_t threadIndex, float progress)
{
    if (nullptr == m_progressHandler)
        return;

    std::lock_guard<std::mutex> lock(m_progressMutex);
    if (progress > m_threadProgress[threadIndex])
        m_threadProgress[threadIndex] = progress;
    float islandWeightedAvg = 0.0;
    for (size_t i = 0; i < m_threadProgress.size(); ++i)
        islandWeightedAvg += m_threadProgress[i] * m_threadProgressWeights[i];
    m_progressHandler(m_tag,
        parallelPhaseBegin + (parallelPhaseEnd - parallelPhaseBegin) * islandWeightedAvg, "");
}

bool AutoRemesher::hasClosedOrientedEdges(const std::vector<std::vector<size_t>>& faces)
{
    struct EdgeUse {
        size_t count = 0;
        int direction = 0;
    };
    std::map<std::pair<size_t, size_t>, EdgeUse> edges;
    for (const auto& face : faces) {
        for (size_t i = 0; i < face.size(); ++i) {
            const size_t from = face[i];
            const size_t to = face[(i + 1) % face.size()];
            auto& use = edges[std::minmax(from, to)];
            ++use.count;
            use.direction += from < to ? 1 : -1;
        }
    }
    return !edges.empty() && std::all_of(edges.begin(), edges.end(), [](const auto& edge) {
        return edge.second.count == 2 && edge.second.direction == 0;
    });
}

const char* AutoRemesher::validateOutputIsland(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces, bool closedInput)
{
    if (vertices.empty() || faces.empty())
        return "no usable faces";
    for (const auto& vertex : vertices) {
        if (!std::isfinite(vertex.x()) || !std::isfinite(vertex.y()) || !std::isfinite(vertex.z()))
            return "non-finite vertex";
    }
    std::set<std::vector<size_t>> cycles;
    for (const auto& face : faces) {
        if (face.size() < 3)
            return "face has fewer than three corners";
        std::unordered_set<size_t> corners;
        for (const size_t index : face) {
            if (index >= vertices.size())
                return "face vertex index is out of range";
            if (!corners.insert(index).second)
                return "face repeats a corner";
        }

        // Unique indices give a unique minimum corner. The smaller neighbor
        // chooses the canonical direction, independent of winding and rotation.
        const size_t first = std::min_element(face.begin(), face.end()) - face.begin();
        const bool forward = face[(first + 1) % face.size()] < face[(first + face.size() - 1) % face.size()];
        std::vector<size_t> cycle;
        cycle.reserve(face.size());
        for (size_t i = 0; i < face.size(); ++i)
            cycle.push_back(face[(first + (forward ? i : face.size() - i)) % face.size()]);
        if (!cycles.insert(std::move(cycle)).second)
            return "duplicate polygon cycle";

        // Center and scale this face before its area calculation. Checking for
        // exact zero imposes no world-unit tolerance on small or translated faces.
        const Vector3 origin = vertices[face.front()];
        double scale = 0.0;
        for (const size_t index : face) {
            const Vector3 edge = vertices[index] - origin;
            if (!std::isfinite(edge.x()) || !std::isfinite(edge.y()) || !std::isfinite(edge.z()))
                return "face extent is not finite";
            for (size_t axis = 0; axis < 3; ++axis)
                scale = std::max(scale, std::fabs(edge[axis]));
        }
        if (0.0 == scale)
            return "zero-area face";
        Vector3 normal(0.0, 0.0, 0.0);
        for (size_t i = 0; i < face.size(); ++i)
            normal += Vector3::crossProduct((vertices[face[i]] - origin) / scale,
                (vertices[face[(i + 1) % face.size()]] - origin) / scale);
        if (normal.x() == 0.0 && normal.y() == 0.0 && normal.z() == 0.0)
            return "zero-area face";
    }
    // The GUI/core also accepts open input. Preserve that path's existing
    // boundary behavior; the CLI's supported closed input must remain closed.
    if (closedInput) {
        if (!hasClosedOrientedEdges(faces))
            return "edges are not paired with opposite directions";
        std::vector<std::vector<std::vector<size_t>>> islands;
        MeshSeparator::splitToIslands(faces, islands);
        if (islands.size() != 1)
            return "input island split into multiple output components";
    }
    return nullptr;
}

bool AutoRemesher::remesh()
{
    m_remeshedVertices.clear();
    m_remeshedQuads.clear();
    if (m_vertices.empty() || m_triangles.empty() || m_targetTriangleCount == 0) {
        std::cerr << "Invalid remesh input" << std::endl;
        return false;
    }
    for (const auto& triangle : m_triangles) {
        if (triangle.size() != 3
            || triangle[0] >= m_vertices.size()
            || triangle[1] >= m_vertices.size()
            || triangle[2] >= m_vertices.size()) {
            std::cerr << "Invalid remesh triangle" << std::endl;
            return false;
        }
    }

    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag, 0.0, "Preparing...");

    auto t_start = std::chrono::high_resolution_clock::now();

    auto t_voxelStart = std::chrono::high_resolution_clock::now();
    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag, 0.0f, "Analyzing target density...");
    initializeVoxelSize();
    auto t_voxelEnd = std::chrono::high_resolution_clock::now();

    std::vector<std::vector<std::vector<size_t>>> trianglesIslands;
    auto t_splitStart = std::chrono::high_resolution_clock::now();
    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag, 0.01f, "Finding connected parts...");
    MeshSeparator::splitToIslands(m_triangles, trianglesIslands);
    auto t_afterSplit = std::chrono::high_resolution_clock::now();

    if (trianglesIslands.empty()) {
        std::cerr << "Input mesh is empty" << std::endl;
        if (nullptr != m_progressHandler)
            m_progressHandler(m_tag, 1.0, "Input mesh is empty");
        return false;
    }

#if AUTO_REMESHER_DEBUG
    std::cerr << "Split to islands: " << trianglesIslands.size() << std::endl;
#endif

    struct IslandContext {
        std::vector<Vector3> vertices;
        std::vector<std::vector<size_t>> triangles;
        bool closedInput;
        double voxelSize;
        double scaling;
        double adaptivity;
        double anisotropy;
        double sharpEdgeDegrees;
        double smoothNormalDegrees;
    };

    std::vector<IslandContext> islandContexes;
    islandContexes.reserve(trianglesIslands.size());
    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag, 0.02f, "Preparing connected parts...");
    for (size_t islandIndex = 0; islandIndex < trianglesIslands.size(); ++islandIndex) {
        const auto& island = trianglesIslands[islandIndex];
        IslandContext context;
        std::unordered_set<size_t> addedIndices;
        std::unordered_map<size_t, size_t> oldToNewVertexMap;
        for (const auto& face : island) {
            std::vector<size_t> triangle;
            for (size_t i = 0; i < 3; ++i) {
                auto insertResult = addedIndices.insert(face[i]);
                if (insertResult.second) {
                    oldToNewVertexMap.insert({ face[i], context.vertices.size() });
                    context.vertices.push_back(m_vertices[face[i]]);
                }
                triangle.push_back(oldToNewVertexMap[face[i]]);
            }
            context.triangles.push_back(triangle);
        }

        // Preserve the input contract before resampling mutates the island.
        context.closedInput = hasClosedOrientedEdges(context.triangles);
        context.scaling = m_scaling;
        context.voxelSize = m_voxelSize;
        context.adaptivity = m_adaptivity;
        context.anisotropy = m_anisotropy;
        context.sharpEdgeDegrees = m_sharpEdgeDegrees;
        context.smoothNormalDegrees = m_smoothNormalDegrees;

        islandContexes.push_back(context);
    }
    auto t_buildEnd = std::chrono::high_resolution_clock::now();

    {
        if (nullptr != m_progressHandler)
            m_progressHandler(m_tag, parallelPhaseBegin, "Building target topology...");
        m_threadProgressWeights.resize(islandContexes.size(), 1.0);
        for (size_t i = 0; i < islandContexes.size(); ++i) {
            if (!m_triangles.empty())
                m_threadProgressWeights[i] = (float)(((double)islandContexes[i].triangles.size() / m_triangles.size()));
        }
        m_threadProgress.resize(islandContexes.size());

        struct IsotropicPhase {
            IsotropicPhase(std::vector<IslandContext>* contexts,
                AutoRemesher* remesher,
                std::atomic<long long>* resampleTime,
                std::vector<std::vector<Vector3>>* islandVertices,
                std::vector<std::vector<std::vector<size_t>>>* islandTriangles)
                : m_contexts(contexts)
                , m_remesher(remesher)
                , m_resampleTime(resampleTime)
                , m_islandVertices(islandVertices)
                , m_islandTriangles(islandTriangles)
            {
            }

            void operator()(const tbb::blocked_range<size_t>& range) const
            {
                for (size_t i = range.begin(); i != range.end(); ++i) {
                    auto& ctx = (*m_contexts)[i];

                    m_remesher->updateProgress(i, 0.0f);

                    auto t0 = std::chrono::high_resolution_clock::now();
                    resample(ctx.vertices, ctx.triangles, ctx.voxelSize, ctx.adaptivity, ctx.sharpEdgeDegrees, ctx.smoothNormalDegrees, i);
                    auto t1 = std::chrono::high_resolution_clock::now();
                    *m_resampleTime += std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

                    (*m_islandVertices)[i] = ctx.vertices;
                    (*m_islandTriangles)[i] = ctx.triangles;

                    m_remesher->updateProgress(i, 0.3f);
                }
            }

        private:
            std::vector<IslandContext>* m_contexts = nullptr;
            AutoRemesher* m_remesher = nullptr;
            std::atomic<long long>* m_resampleTime = nullptr;
            std::vector<std::vector<Vector3>>* m_islandVertices = nullptr;
            std::vector<std::vector<std::vector<size_t>>>* m_islandTriangles = nullptr;
        };

        std::atomic<long long> resampleTime(0);

        m_isotropicVertices.clear();
        m_isotropicTriangles.clear();
        std::vector<std::vector<Vector3>> isotropicIslandVertices(islandContexes.size());
        std::vector<std::vector<std::vector<size_t>>> isotropicIslandTriangles(islandContexes.size());
        tbb::parallel_for(tbb::blocked_range<size_t>(0, islandContexes.size()),
            IsotropicPhase(&islandContexes, this, &resampleTime,
                &isotropicIslandVertices, &isotropicIslandTriangles));
        for (size_t i = 0; i < isotropicIslandVertices.size(); ++i) {
            const size_t vertexOffset = m_isotropicVertices.size();
            m_isotropicVertices.insert(m_isotropicVertices.end(),
                isotropicIslandVertices[i].begin(), isotropicIslandVertices[i].end());
            for (const auto& triangle : isotropicIslandTriangles[i]) {
                std::vector<size_t> offsetTriangle;
                offsetTriangle.reserve(triangle.size());
                for (const size_t index : triangle)
                    offsetTriangle.push_back(index + vertexOffset);
                m_isotropicTriangles.push_back(std::move(offsetTriangle));
            }
        }
    }

    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag,
            parallelPhaseBegin + (parallelPhaseEnd - parallelPhaseBegin) * 0.3f,
            "Fitting target topology...");

    class ParameterizationThread {
    public:
        ~ParameterizationThread()
        {
            delete parameterizer;
            delete remesher;
        }

        size_t islandIndex = 0;
        IslandContext* island = nullptr;
        Parameterizer* parameterizer = nullptr;
        QuadExtractor* remesher = nullptr;
        AutoRemesher* autoRemesher = nullptr;
        std::vector<std::vector<Vector2>> capturedUvs;
        std::vector<std::vector<Vector2>> capturedOriginalUvs;
        std::vector<uint8_t> capturedExtractedConnectionMoved;
        std::vector<Vector3> capturedSingularVertices;
        std::vector<size_t> capturedSingularVertexIndices;
        std::vector<std::pair<Vector3, Vector3>> capturedExtractedConnections;
        bool succeeded = false;
    };

    std::vector<ParameterizationThread> parameterizationThreads(islandContexes.size());
    for (size_t i = 0; i < islandContexes.size(); ++i) {
        auto& thread = parameterizationThreads[i];
        auto& context = islandContexes[i];
        thread.islandIndex = i;
        thread.island = &context;
        thread.autoRemesher = this;
    }

    class SurfaceParameterizer {
    public:
        SurfaceParameterizer(std::vector<ParameterizationThread>* parameterizationThreads,
            std::atomic<long long>* parameterizeTime,
            std::atomic<long long>* extractTime)
            : m_parameterizationThreads(parameterizationThreads)
            , m_parameterizeTime(parameterizeTime)
            , m_extractTime(extractTime)
        {
        }

        void operator()(const tbb::blocked_range<size_t>& range) const
        {
            for (size_t i = range.begin(); i != range.end(); ++i) {
                auto& thread = (*m_parameterizationThreads)[i];

                auto t0 = std::chrono::high_resolution_clock::now();

                const auto& vertices = thread.island->vertices;
                const auto& triangles = thread.island->triangles;

                if (vertices.empty() || triangles.empty())
                    continue;

                thread.autoRemesher->updateProgress(thread.islandIndex, 0.3f);
                thread.parameterizer = new Parameterizer(&vertices,
                    &triangles,
                    nullptr);
                if (thread.island->scaling > 0.0)
                    thread.parameterizer->setScaling(thread.island->scaling);
                thread.parameterizer->setGradientAdaptivity(thread.island->adaptivity);
                thread.parameterizer->setAnisotropy(thread.island->anisotropy);
                thread.parameterizer->setSharpEdgeDegrees(thread.island->sharpEdgeDegrees);
                bool parameterizeSucceeded = true;
                try {
                    parameterizeSucceeded = thread.parameterizer->parameterize();
                } catch (const std::exception& e) {
                    parameterizeSucceeded = false;
                    std::cerr << "Island " << (thread.islandIndex + 1)
                              << ": parameterization failed (" << e.what()
                              << ")." << std::endl;
                } catch (...) {
                    parameterizeSucceeded = false;
                    std::cerr << "Island " << (thread.islandIndex + 1)
                              << ": parameterization failed (unknown error)." << std::endl;
                }

                auto t1 = std::chrono::high_resolution_clock::now();
                *m_parameterizeTime += std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

                if (parameterizeSucceeded) {
                    thread.autoRemesher->updateProgress(thread.islandIndex, 0.9f);
                    std::vector<std::vector<Vector2>>* uvs = thread.parameterizer->takeTriangleUvs();
                    if (uvs) {
                        // Save a copy of UVs for the [param] preview overlay
                        thread.capturedUvs = *uvs;
                        thread.capturedOriginalUvs = thread.parameterizer->originalTriangleUvs();
                    }
                    // Capture singular vertex positions for the [param] preview
                    thread.capturedSingularVertices = thread.parameterizer->singularVertexPositions();
                    thread.capturedSingularVertexIndices = thread.parameterizer->singularVertexIndices();
                    thread.remesher = new QuadExtractor(&vertices,
                        &triangles,
                        uvs);
                    thread.remesher->setOriginalTriangleUvs(&thread.capturedOriginalUvs);
                    thread.remesher->setSingularVertices(&thread.capturedSingularVertexIndices);
                    if (!thread.remesher->extract()) {
                        delete thread.remesher;
                        thread.remesher = nullptr;
                    } else {
                        const char* reason = validateOutputIsland(thread.remesher->remeshedVertices(),
                            thread.remesher->remeshedQuads(), thread.island->closedInput);
                        thread.succeeded = nullptr == reason;
                        if (reason)
                            std::cerr << "Island " << thread.islandIndex + 1
                                      << ": invalid reconstruction: " << reason << std::endl;
                        thread.capturedExtractedConnections = thread.remesher->extractedConnections();
                        thread.capturedExtractedConnectionMoved = thread.remesher->extractedConnectionMoved();
                    }
                    delete uvs;
                }
                thread.autoRemesher->updateProgress(thread.islandIndex, 1.0f);
                auto t2 = std::chrono::high_resolution_clock::now();
                *m_extractTime += std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
            }
        }

    private:
        std::vector<ParameterizationThread>* m_parameterizationThreads = nullptr;
        std::atomic<long long>* m_parameterizeTime = nullptr;
        std::atomic<long long>* m_extractTime = nullptr;
    };
    std::atomic<long long> parameterizeTimeAccumulated(0);
    std::atomic<long long> extractTimeAccumulated(0);

    tbb::parallel_for(tbb::blocked_range<size_t>(0, parameterizationThreads.size()),
        SurfaceParameterizer(&parameterizationThreads,
            &parameterizeTimeAccumulated,
            &extractTimeAccumulated));
    auto t_parallelEnd = std::chrono::high_resolution_clock::now();

    if (!std::all_of(parameterizationThreads.begin(), parameterizationThreads.end(),
            [](const ParameterizationThread& thread) { return thread.succeeded; })) {
        std::cerr << "One or more mesh islands failed" << std::endl;
        return false;
    }

    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag, parallelPhaseEnd, "Combining connected parts...");

    // Merge isotropic UVs from all islands (for [param] preview)
    m_isotropicTriangleUvs.clear();
    m_isotropicOriginalTriangleUvs.clear();
    for (size_t i = 0; i < parameterizationThreads.size(); ++i) {
        auto& thread = parameterizationThreads[i];
        if (thread.capturedUvs.empty())
            continue;
        m_isotropicTriangleUvs.insert(m_isotropicTriangleUvs.end(),
            thread.capturedUvs.begin(), thread.capturedUvs.end());
        m_isotropicOriginalTriangleUvs.insert(m_isotropicOriginalTriangleUvs.end(),
            thread.capturedOriginalUvs.begin(), thread.capturedOriginalUvs.end());
    }

    // Merge singular vertex positions from all islands (for [param] preview)
    m_isotropicSingularVertices.clear();
    for (size_t i = 0; i < parameterizationThreads.size(); ++i) {
        auto& thread = parameterizationThreads[i];
        if (thread.capturedSingularVertices.empty())
            continue;
        m_isotropicSingularVertices.insert(m_isotropicSingularVertices.end(),
            thread.capturedSingularVertices.begin(), thread.capturedSingularVertices.end());
    }

    // Merge the raw quad-extraction connections for the [param] preview.
    m_isotropicExtractedConnections.clear();
    m_isotropicExtractedConnectionMoved.clear();
    for (const auto& thread : parameterizationThreads) {
        m_isotropicExtractedConnections.insert(m_isotropicExtractedConnections.end(),
            thread.capturedExtractedConnections.begin(), thread.capturedExtractedConnections.end());
        m_isotropicExtractedConnectionMoved.resize(m_isotropicExtractedConnections.size()
                - thread.capturedExtractedConnections.size(),
            0);
        m_isotropicExtractedConnectionMoved.insert(m_isotropicExtractedConnectionMoved.end(),
            thread.capturedExtractedConnectionMoved.begin(),
            thread.capturedExtractedConnectionMoved.end());
        m_isotropicExtractedConnectionMoved.resize(m_isotropicExtractedConnections.size(), 0);
    }
    for (size_t i = 0; i < parameterizationThreads.size(); ++i) {
        auto& thread = parameterizationThreads[i];
        if (nullptr == thread.remesher)
            continue;
        const auto& quads = thread.remesher->remeshedQuads();
        if (quads.empty())
            continue;
        const auto& vertices = thread.remesher->remeshedVertices();
        size_t vertexStartIndex = m_remeshedVertices.size();
        m_remeshedVertices.reserve(m_remeshedVertices.size() + vertices.size());
        for (const auto& it : vertices) {
            m_remeshedVertices.push_back(it);
        }
        for (const auto& it : quads) {
            std::vector<size_t> quad;
            quad.reserve(it.size());
            for (const auto& v : it)
                quad.push_back(vertexStartIndex + v);
            m_remeshedQuads.push_back(quad);
        }
    }

    auto t_mergeEnd = std::chrono::high_resolution_clock::now();

    auto t_voxelMs = std::chrono::duration_cast<std::chrono::milliseconds>(t_voxelEnd - t_voxelStart).count();
    auto t_splitMs = std::chrono::duration_cast<std::chrono::milliseconds>(t_afterSplit - t_splitStart).count();
    auto t_buildMs = std::chrono::duration_cast<std::chrono::milliseconds>(t_buildEnd - t_afterSplit).count();
    auto t_parallelWallMs = std::chrono::duration_cast<std::chrono::milliseconds>(t_parallelEnd - t_buildEnd).count();
    auto t_mergeMs = std::chrono::duration_cast<std::chrono::milliseconds>(t_mergeEnd - t_parallelEnd).count();
    auto t_totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(t_mergeEnd - t_start).count();

    std::cerr << "Quad mesh breakdown: total " << t_totalMs << "ms"
              << " | voxel " << t_voxelMs << "ms"
              << " | split " << t_splitMs << "ms"
              << " | build " << t_buildMs << "ms"
              << " | parallel " << t_parallelWallMs << "ms"
              << " (param " << parameterizeTimeAccumulated.load() << "ms"
              << " | extract " << extractTimeAccumulated.load() << "ms)"
              << " | merge " << t_mergeMs << "ms" << std::endl;

#if AUTO_REMESHER_DEBUG
    std::cerr << "Remesh done" << std::endl;
#endif

    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag, 1.0, "Done");

    return !m_remeshedVertices.empty() && !m_remeshedQuads.empty();
}

}

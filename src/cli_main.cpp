/*
 * Copyright (c) 2026 AutoRemesher Project.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define TINYOBJLOADER_USE_DOUBLE
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include <AutoRemesher/AutoRemesher>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if __has_include(<oneapi/tbb/global_control.h>)
#include <oneapi/tbb/global_control.h>
#else
#include <tbb/global_control.h>
#endif

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr size_t kMaxInputVertices = 1000000;
constexpr size_t kMaxInputTriangles = 1000000;
constexpr size_t kMaxTargetQuads = 5000;

#ifndef _WIN32
const char* volatile gTemporaryOutputPath = nullptr;

void handleSigterm(int signalNumber)
{
    if (gTemporaryOutputPath)
        unlink(gTemporaryOutputPath);
    _exit(128 + signalNumber);
}
#endif

struct Options {
    std::string inputPath;
    std::string outputPath;
    size_t targetQuads = 5000;
    size_t threads = 1;
};

struct MeshData {
    std::vector<AutoRemesher::Vector3> vertices;
    std::vector<std::vector<size_t>> triangles;
};

bool parsePositive(const char* value, size_t* result)
{
    try {
        const std::string text(value);
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
            return false;
        size_t consumed = 0;
        const unsigned long long parsed = std::stoull(text, &consumed);
        if (parsed == 0 || consumed != text.size())
            return false;
        *result = static_cast<size_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parseOptions(int argc, char** argv, Options* options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string name(argv[i]);
        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << name << std::endl;
            return false;
        }
        const char* value = argv[++i];
        if (name == "--input")
            options->inputPath = value;
        else if (name == "--output")
            options->outputPath = value;
        else if (name == "--target-quads") {
            if (!parsePositive(value, &options->targetQuads)) {
                std::cerr << "Invalid --target-quads" << std::endl;
                return false;
            }
        } else if (name == "--threads") {
            if (!parsePositive(value, &options->threads)) {
                std::cerr << "Invalid --threads" << std::endl;
                return false;
            }
        } else {
            std::cerr << "Unknown option: " << name << std::endl;
            return false;
        }
    }
    if (options->inputPath.empty() || options->outputPath.empty()) {
        std::cerr << "--input and --output are required" << std::endl;
        return false;
    }
    if (options->targetQuads > kMaxTargetQuads) {
        std::cerr << "--target-quads exceeds the supported limit of " << kMaxTargetQuads << std::endl;
        return false;
    }
    return true;
}

bool loadTriangles(const std::string& path, MeshData* mesh)
{
    tinyobj::attrib_t attributes;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warning;
    std::string error;
    if (!tinyobj::LoadObj(&attributes, &shapes, &materials, &warning, &error,
            path.c_str(), nullptr, false)) {
        std::cerr << "Failed to read input: " << error << std::endl;
        return false;
    }
    if (attributes.vertices.empty()) {
        std::cerr << "Input mesh is empty" << std::endl;
        return false;
    }
    if (attributes.vertices.size() / 3 > kMaxInputVertices) {
        std::cerr << "Input exceeds the supported vertex limit of " << kMaxInputVertices << std::endl;
        return false;
    }
    mesh->vertices.reserve(attributes.vertices.size() / 3);
    for (size_t i = 0; i < attributes.vertices.size(); i += 3) {
        if (!std::isfinite(attributes.vertices[i])
            || !std::isfinite(attributes.vertices[i + 1])
            || !std::isfinite(attributes.vertices[i + 2])) {
            std::cerr << "Input contains a non-finite vertex" << std::endl;
            return false;
        }
        mesh->vertices.emplace_back(attributes.vertices[i],
            attributes.vertices[i + 1], attributes.vertices[i + 2]);
    }
    for (const auto& shape : shapes) {
        size_t offset = 0;
        for (const auto count : shape.mesh.num_face_vertices) {
            if (count != 3) {
                std::cerr << "Input contains a non-triangle face" << std::endl;
                return false;
            }
            std::vector<size_t> triangle;
            for (size_t i = 0; i < 3; ++i) {
                const int index = shape.mesh.indices[offset + i].vertex_index;
                if (index < 0 || static_cast<size_t>(index) >= mesh->vertices.size()) {
                    std::cerr << "Input contains an invalid vertex index" << std::endl;
                    return false;
                }
                triangle.push_back(static_cast<size_t>(index));
            }
            if (triangle[0] == triangle[1] || triangle[1] == triangle[2] || triangle[2] == triangle[0]) {
                std::cerr << "Input contains a degenerate triangle" << std::endl;
                return false;
            }
            const auto areaNormal = AutoRemesher::Vector3::crossProduct(
                mesh->vertices[triangle[1]] - mesh->vertices[triangle[0]],
                mesh->vertices[triangle[2]] - mesh->vertices[triangle[0]]);
            if (areaNormal.x() == 0.0 && areaNormal.y() == 0.0 && areaNormal.z() == 0.0) {
                std::cerr << "Input contains a degenerate triangle" << std::endl;
                return false;
            }
            if (mesh->triangles.size() >= kMaxInputTriangles) {
                std::cerr << "Input exceeds the supported triangle limit of " << kMaxInputTriangles << std::endl;
                return false;
            }
            mesh->triangles.push_back(std::move(triangle));
            offset += count;
        }
    }
    if (mesh->triangles.empty()) {
        std::cerr << "Input mesh is empty" << std::endl;
        return false;
    }
    return true;
}

bool isClosedOrientedManifold(const MeshData& mesh)
{
    struct EdgeUse {
        size_t count = 0;
        int orientation = 0;
    };
    std::map<std::pair<size_t, size_t>, EdgeUse> edges;
    for (const auto& triangle : mesh.triangles) {
        for (size_t i = 0; i < 3; ++i) {
            const size_t first = triangle[i];
            const size_t second = triangle[(i + 1) % 3];
            const auto edge = std::minmax(first, second);
            auto& use = edges[{ edge.first, edge.second }];
            ++use.count;
            use.orientation += first < second ? 1 : -1;
        }
    }
    return std::all_of(edges.begin(), edges.end(), [](const auto& item) {
        return item.second.count == 2 && item.second.orientation == 0;
    });
}

std::string oneLine(std::string text)
{
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    return text;
}

struct ProgressReporter {
    std::mutex mutex;
    std::string phase = "Working";
};

void reportProgress(void* tag, float progress, const char* status)
{
    auto* reporter = static_cast<ProgressReporter*>(tag);
    std::lock_guard<std::mutex> lock(reporter->mutex);
    if (status && status[0] != '\0')
        reporter->phase = oneLine(status);
    const int percent = std::max(0, std::min(100, static_cast<int>(progress * 100.0f)));
    std::cout << "PROGRESS " << percent << " " << reporter->phase << std::endl;
}

bool writeOutput(const std::string& path,
    const std::string& temporaryPath,
    const std::vector<AutoRemesher::Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces)
{
    for (const auto& vertex : vertices) {
        if (!std::isfinite(vertex.x()) || !std::isfinite(vertex.y()) || !std::isfinite(vertex.z())) {
            std::cerr << "Remeshing produced a non-finite vertex" << std::endl;
            return false;
        }
    }
    for (const auto& face : faces) {
        if (face.size() < 3) {
            std::cerr << "Remeshing produced an invalid face" << std::endl;
            return false;
        }
        for (size_t i = 0; i < face.size(); ++i) {
            if (face[i] >= vertices.size()) {
                std::cerr << "Remeshing produced an invalid vertex index" << std::endl;
                return false;
            }
            if (std::find(face.begin() + i + 1, face.end(), face[i]) != face.end()) {
                std::cerr << "Remeshing produced a face with repeated vertices" << std::endl;
                return false;
            }
        }
    }

    std::ofstream stream(temporaryPath, std::ios::out | std::ios::trunc);
    if (!stream) {
        std::cerr << "Failed to create output" << std::endl;
        return false;
    }
    stream << std::setprecision(17);
    for (const auto& vertex : vertices)
        stream << "v " << vertex.x() << " " << vertex.y() << " " << vertex.z() << "\n";
    for (const auto& face : faces) {
        stream << "f";
        for (const auto index : face)
            stream << " " << index + 1;
        stream << "\n";
    }
    stream.close();
    if (!stream || std::rename(temporaryPath.c_str(), path.c_str()) != 0) {
        std::remove(temporaryPath.c_str());
        std::cerr << "Failed to finalize output" << std::endl;
        return false;
    }
    return true;
}

}

int main(int argc, char** argv)
{
    Options options;
    if (!parseOptions(argc, argv, &options))
        return 2;
    MeshData input;
    if (!loadTriangles(options.inputPath, &input))
        return 3;
    if (!isClosedOrientedManifold(input)) {
        std::cerr << "Input must be a closed oriented manifold triangle mesh" << std::endl;
        return 3;
    }
#ifdef _WIN32
    const int processId = _getpid();
#else
    const int processId = getpid();
#endif
    const std::string temporaryOutputPath = options.outputPath + ".tmp-" + std::to_string(processId);
#ifndef _WIN32
    gTemporaryOutputPath = temporaryOutputPath.c_str();
    if (std::signal(SIGTERM, handleSigterm) == SIG_ERR) {
        std::cerr << "Failed to install cancellation handler" << std::endl;
        return 2;
    }
#endif

    tbb::global_control threadLimit(tbb::global_control::max_allowed_parallelism, options.threads);
    ProgressReporter reporter;
    AutoRemesher::AutoRemesher remesher(input.vertices, input.triangles);
    remesher.setTargetTriangleCount(options.targetQuads * 2);
    remesher.setTag(&reporter);
    remesher.setProgressHandler(reportProgress);

    const auto start = std::chrono::steady_clock::now();
    if (!remesher.remesh()) {
        std::cerr << "Remeshing failed" << std::endl;
        return 4;
    }
    const auto& vertices = remesher.remeshedVertices();
    const auto& faces = remesher.remeshedQuads();
    if (vertices.empty() || faces.empty()) {
        std::cerr << "Remeshing produced no result" << std::endl;
        return 4;
    }
    if (!writeOutput(options.outputPath, temporaryOutputPath, vertices, faces))
        return 5;
#ifndef _WIN32
    gTemporaryOutputPath = nullptr;
#endif

    size_t quadCount = 0;
    for (const auto& face : faces) {
        if (face.size() == 4)
            ++quadCount;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    std::cout << "RESULT vertices=" << vertices.size()
              << " faces=" << faces.size()
              << " quads=" << quadCount
              << " non_quads=" << faces.size() - quadCount
              << " elapsed_ms=" << elapsed.count() << std::endl;
    return 0;
}

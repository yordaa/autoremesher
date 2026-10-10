#include <AutoRemesher/AutoRemesher>
#include <AutoRemesher/PositionKey>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

using AutoRemesher::Vector3;
using Faces = std::vector<std::vector<size_t>>;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void checkPositions(const std::vector<Vector3>& positions,
    const std::vector<Vector3>& centers, double radius)
{
    for (const auto& position : positions) {
        bool placed = false;
        for (const auto& center : centers) {
            const auto local = (position - center) / radius;
            placed |= std::isfinite(local.x()) && std::isfinite(local.y()) && std::isfinite(local.z())
                && std::max({ std::fabs(local.x()), std::fabs(local.y()), std::fabs(local.z()) }) < 1.2;
        }
        require(placed, "a public position output was not restored to the input coordinates");
    }
}

struct Quality {
    double quads;
    double volume;
};

// Real remeshing integration: compare observable coordinates/topology/quality,
// rather than exact scheduling-dependent face counts or output hashes.
Quality remeshCube(double radius, const Vector3& origin, size_t parts = 1,
    size_t subdivisions = 0, size_t targetQuads = 1000)
{
    const std::vector<Vector3> cube = {
        { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
        { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 }
    };
    const Faces cubeFaces = {
        { 0, 3, 2 }, { 0, 2, 1 }, { 4, 5, 6 }, { 4, 6, 7 },
        { 0, 1, 5 }, { 0, 5, 4 }, { 1, 2, 6 }, { 1, 6, 5 },
        { 2, 3, 7 }, { 2, 7, 6 }, { 3, 0, 4 }, { 3, 4, 7 }
    };
    std::vector<Vector3> vertices, centers;
    Faces faces;
    for (size_t part = 0; part < parts; ++part) {
        centers.push_back(origin + Vector3(part * 4 * radius, 0, 0));
        const size_t offset = vertices.size();
        for (const auto& point : cube)
            vertices.push_back(point * radius + centers.back());
        for (const auto& face : cubeFaces)
            faces.push_back({ face[0] + offset, face[1] + offset, face[2] + offset });
    }
    for (size_t level = 0; level < subdivisions; ++level) {
        std::map<std::pair<size_t, size_t>, size_t> middle;
        auto midpoint = [&](size_t a, size_t b) {
            const auto edge = std::minmax(a, b);
            const auto added = middle.emplace(edge, vertices.size());
            if (added.second)
                vertices.push_back((vertices[a] + vertices[b]) * 0.5);
            return added.first->second;
        };
        Faces next;
        for (const auto& face : faces) {
            const auto ab = midpoint(face[0], face[1]);
            const auto bc = midpoint(face[1], face[2]);
            const auto ca = midpoint(face[2], face[0]);
            next.insert(next.end(), { { face[0], ab, ca }, { ab, face[1], bc }, { ca, bc, face[2] }, { ab, bc, ca } });
        }
        faces = std::move(next);
    }

    AutoRemesher::AutoRemesher remesher(vertices, faces);
    remesher.setTargetTriangleCount(targetQuads * 2);
    require(remesher.remesh(), "scale-equivalent cube failed to remesh");
    const auto& outputVertices = remesher.remeshedVertices();
    const auto& outputFaces = remesher.remeshedQuads();
    require(!outputVertices.empty() && !outputFaces.empty(), "empty final output");
    checkPositions(outputVertices, centers, radius);
    require(!remesher.isotropicVertices().empty(), "missing isotropic preview");
    checkPositions(remesher.isotropicVertices(), centers, radius);
    require(!remesher.isotropicSingularVertices().empty(), "missing singular-position coverage");
    checkPositions(remesher.isotropicSingularVertices(), centers, radius);
    require(!remesher.isotropicExtractedConnections().empty(), "missing extracted-connection coverage");
    for (const auto& edge : remesher.isotropicExtractedConnections())
        checkPositions({ edge.first, edge.second }, centers, radius);
    require(remesher.isotropicTriangleUvs().size() == remesher.isotropicTriangles().size(),
        "UVs no longer correspond to the public preview triangles");
    require(remesher.isotropicOriginalTriangleUvs().size() == remesher.isotropicTriangles().size(),
        "original UVs no longer correspond to the public preview triangles");

    std::map<std::pair<size_t, size_t>, size_t> edges;
    std::map<size_t, std::set<size_t>> adjacent;
    size_t quads = 0;
    double volume = 0;
    for (const auto& face : outputFaces) {
        require(face.size() >= 3 && std::set<size_t>(face.begin(), face.end()).size() == face.size(), "invalid final polygon");
        quads += face.size() == 4;
        for (size_t i = 0; i < face.size(); ++i) {
            const auto a = face[i], b = face[(i + 1) % face.size()];
            require(a < outputVertices.size() && b < outputVertices.size(), "invalid vertex index");
            ++edges[{ a, b }];
            adjacent[a].insert(b);
            adjacent[b].insert(a);
        }
        const auto a = (outputVertices[face[0]] - origin) / radius;
        for (size_t i = 1; i + 1 < face.size(); ++i) {
            const auto b = (outputVertices[face[i]] - origin) / radius;
            const auto c = (outputVertices[face[i + 1]] - origin) / radius;
            volume += Vector3::dotProduct(a, Vector3::crossProduct(b, c)) / 6;
        }
    }
    for (const auto& edge : edges)
        require(edge.second == 1 && edges[{ edge.first.second, edge.first.first }] == 1, "output is not closed and consistently oriented");
    std::set<size_t> unseen;
    for (const auto& vertex : adjacent)
        unseen.insert(vertex.first);
    std::set<size_t> placedParts;
    while (!unseen.empty()) {
        std::vector<size_t> pending { *unseen.begin() };
        unseen.erase(pending.back());
        Vector3 center;
        size_t count = 0;
        while (!pending.empty()) {
            const auto index = pending.back();
            pending.pop_back();
            center += outputVertices[index];
            ++count;
            for (const auto neighbor : adjacent[index]) {
                if (unseen.erase(neighbor))
                    pending.push_back(neighbor);
            }
        }
        center /= count;
        size_t nearest = 0;
        for (size_t i = 1; i < centers.size(); ++i)
            if ((center - centers[i]).length() < (center - centers[nearest]).length())
                nearest = i;
        require((center - centers[nearest]).length() / radius < 0.2, "component placement changed");
        require(placedParts.insert(nearest).second, "two components were restored onto the same input part");
    }
    require(placedParts.size() == centers.size(), "an input part was lost");
    const Quality quality { double(quads) / outputFaces.size(), std::fabs(volume) / (parts * 8) };
    require(quality.quads > 0.9 && quality.volume > 0.85 && quality.volume < 1.1, "cube reconstruction quality collapsed");
    std::cout << "METRIC faces=" << outputFaces.size() << " quads=" << quality.quads
              << " volume=" << quality.volume << " parts=" << placedParts.size() << '\n';
    return quality;
}

void comparable(const Quality& result, const Quality& control)
{
    require(std::fabs(result.quads - control.quads) < 0.03, "quad quality differs from the unit-scale control");
    require(std::fabs(result.volume - control.volume) < 0.03, "volume quality differs from the unit-scale control");
}

int main()
{
    int failures = 0;
    auto check = [&](const char* name, auto test) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            ++failures;
        }
    };
    Quality control {}, denseControl {};
    check("unit cube", [&] { control = remeshCube(1, Vector3()); });
    check("tiny cube 1e-6", [&] { comparable(remeshCube(1e-6, Vector3(7e-6, -3e-6, 2e-6)), control); });
    check("placed tiny cube 1e-9", [&] { comparable(remeshCube(1e-9, Vector3(7, -3, 2)), control); });
    check("two tiny islands", [&] { comparable(remeshCube(1e-6, Vector3(.001, -.002, .003), 2), control); });
    check("large translated cube", [&] { comparable(remeshCube(1, Vector3(1e8, -1e8, 1e8)), control); });
    // Master has no decimation stage; q200 is a retained negative control.
    // Use q1000 to exercise dense world-position captures without changing quality bounds.
    check("dense unit cube", [&] { denseControl = remeshCube(1, Vector3(), 1, 5, 1000); });
    check("dense tiny cube", [&] { comparable(remeshCube(1e-6, Vector3(4e-6, -2e-6, 1e-6), 1, 5, 1000), denseControl); });
    check("checked fixed-width position keys", [] {
        require(!(AutoRemesher::PositionKey(1e8, 0, 0) == AutoRemesher::PositionKey(1e8 + 1, 0, 0)), "large keys collided");
        const double exclusive = std::ldexp(1.0, 63) / 100000;
        AutoRemesher::PositionKey lower(-exclusive, 0, 0);
        AutoRemesher::PositionKey upper(std::nextafter(exclusive, 0.0), 0, 0);
        require(lower < upper, "valid signed key bounds were rejected");
        for (double value : { exclusive, 1e308, -1e308,
                 std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() }) {
            bool rejected = false;
            try {
                AutoRemesher::PositionKey invalid(value, 0, 0);
            } catch (const std::out_of_range&) {
                rejected = true;
            }
            require(rejected, "unsupported position key was not rejected before integer conversion");
        }
    });
    check("unsupported input range returns failure", [] {
        const Faces faces { { 0, 2, 1 }, { 0, 1, 3 }, { 0, 3, 2 }, { 1, 2, 3 } };
        for (double scale : { 0.0, 1e-200, 1e200,
                 std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() }) {
            std::vector<Vector3> vertices { { scale, scale, scale }, { -scale, -scale, scale },
                { -scale, scale, -scale }, { scale, -scale, -scale } };
            AutoRemesher::AutoRemesher remesher(vertices, faces);
            remesher.setTargetTriangleCount(2000);
            require(!remesher.remesh(), "unsupported input range returned success");
            require(remesher.remeshedVertices().empty() && remesher.remeshedQuads().empty(), "unsupported input published geometry");
        }
    });
    return failures ? 1 : 0;
}

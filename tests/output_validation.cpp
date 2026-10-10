#include <AutoRemesher/AutoRemesher>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace AutoRemesher {

struct AutoRemesherOutputValidationTest {
    static void require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    static void rejectsOppositeFaces()
    {
        // Exact failure pattern from the hole-only icosahedron target-19 output.
        const std::vector<Vector3> vertices = {
            { -1, -1, 0 }, { 1, -1, 0 }, { 1, 1, 0 }, { -1, 1, 0 }
        };
        const std::vector<std::vector<size_t>> faces = { { 0, 1, 2, 3 }, { 3, 2, 1, 0 } };
        require(AutoRemesher::hasClosedOrientedEdges(faces), "counterexample must pass edge-use checks");
        require(AutoRemesher::validateOutputIsland(vertices, faces, true) != nullptr,
            "coincident reverse polygon cycles were accepted");
    }

    static void checksOutputContract()
    {
        const std::vector<Vector3> vertices = {
            { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
            { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 }
        };
        const std::vector<std::vector<size_t>> faces = {
            { 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 },
            { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 }
        };
        for (const double scale : { 1.0, 1e-9, 1e9, 1e-150 }) {
            auto transformed = vertices;
            for (auto& vertex : transformed)
                vertex *= scale;
            require(nullptr == AutoRemesher::validateOutputIsland(transformed, faces, true),
                "uniformly scaled closed cube rejected");
        }
        auto translated = vertices;
        for (auto& vertex : translated)
            vertex += Vector3(1e8, -1e8, 1e8);
        require(nullptr == AutoRemesher::validateOutputIsland(translated, faces, true), "translated cube rejected");
        auto thin = vertices;
        for (auto& vertex : thin)
            vertex[2] *= 1e-18;
        require(nullptr == AutoRemesher::validateOutputIsland(thin, faces, true), "thin closed cube rejected");
        auto elongated = vertices;
        for (auto& vertex : elongated)
            vertex[0] *= 1e12;
        require(nullptr == AutoRemesher::validateOutputIsland(elongated, faces, true), "elongated closed cube rejected");

        // The final output contract is polygonal, not a triangle-only input check.
        const std::vector<Vector3> prism = {
            { 0, 0, -1 }, { 2, 0, -1 }, { 3, 1, -1 }, { 1, 3, -1 }, { -1, 1, -1 },
            { 0, 0, 1 }, { 2, 0, 1 }, { 3, 1, 1 }, { 1, 3, 1 }, { -1, 1, 1 }
        };
        const std::vector<std::vector<size_t>> prismFaces = {
            { 0, 4, 3, 2, 1 }, { 5, 6, 7, 8, 9 }, { 0, 1, 6, 5 },
            { 1, 2, 7, 6 }, { 2, 3, 8, 7 }, { 3, 4, 9, 8 }, { 4, 0, 5, 9 }
        };
        require(nullptr == AutoRemesher::validateOutputIsland(prism, prismFaces, true), "polygonal prism rejected");

        auto badFaces = faces;
        badFaces.push_back({ 2, 1, 0, 3 });
        require(AutoRemesher::validateOutputIsland(vertices, badFaces, true) != nullptr,
            "rotated duplicate polygon cycle accepted");
        badFaces = faces;
        badFaces[0] = { 0, 1, 2, 3 };
        require(AutoRemesher::validateOutputIsland(vertices, badFaces, true) != nullptr,
            "equal-direction edge uses accepted");
        badFaces = faces;
        badFaces[0][0] = vertices.size();
        require(AutoRemesher::validateOutputIsland(vertices, badFaces, true) != nullptr,
            "out-of-range corner accepted");
        badFaces = faces;
        badFaces[0][0] = badFaces[0][1];
        require(AutoRemesher::validateOutputIsland(vertices, badFaces, true) != nullptr,
            "repeated face corner accepted");
        auto badVertices = vertices;
        badVertices[0] = Vector3(std::numeric_limits<double>::infinity(), 0, 0);
        require(AutoRemesher::validateOutputIsland(badVertices, faces, true) != nullptr,
            "non-finite output accepted");
        require(AutoRemesher::validateOutputIsland({ { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 } },
                    { { 0, 1, 2 } }, false)
                != nullptr,
            "zero-area face accepted");
        require(AutoRemesher::validateOutputIsland(vertices, { { 0, 1 } }, false) != nullptr,
            "face with fewer than three corners accepted");
        require(AutoRemesher::validateOutputIsland(vertices, {}, true) != nullptr, "empty output accepted");

        // A closed input island cannot silently become two individually closed pieces.
        auto disconnectedVertices = vertices;
        for (const auto& vertex : vertices)
            disconnectedVertices.push_back(vertex + Vector3(4, 0, 0));
        auto disconnectedFaces = faces;
        for (auto face : faces) {
            for (auto& index : face)
                index += vertices.size();
            disconnectedFaces.push_back(std::move(face));
        }
        require(AutoRemesher::hasClosedOrientedEdges(disconnectedFaces), "two cubes must have closed edge uses");
        require(AutoRemesher::validateOutputIsland(disconnectedVertices, disconnectedFaces, true) != nullptr,
            "one input island split into multiple closed outputs");

        const std::vector<std::vector<size_t>> openFace = { faces.front() };
        require(!AutoRemesher::hasClosedOrientedEdges(openFace), "open input misclassified as closed");
        require(nullptr == AutoRemesher::validateOutputIsland(vertices, openFace, false), "open input path restricted");
        require(AutoRemesher::validateOutputIsland(vertices, openFace, true) != nullptr, "closed output allowed open edges");
    }
};

}

int main()
{
    try {
        AutoRemesher::AutoRemesherOutputValidationTest::rejectsOppositeFaces();
        AutoRemesher::AutoRemesherOutputValidationTest::checksOutputContract();
        std::cout << "PASS final island topology contract\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}

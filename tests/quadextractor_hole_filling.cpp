#include <AutoRemesher/QuadExtractor>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace AutoRemesher {

struct QuadExtractorHoleFillingTest {
    static void require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    static void closePrism(size_t sides)
    {
        QuadExtractor extractor(nullptr, nullptr, nullptr);
        for (int layer = 0; layer < 2; ++layer) {
            for (size_t i = 0; i < sides; ++i) {
                const double angle = 2 * std::acos(-1.0) * i / sides;
                extractor.m_remeshedVertices.emplace_back(std::cos(angle), std::sin(angle), layer);
            }
        }
        std::vector<size_t> bottom;
        for (size_t i = 0; i < sides; ++i) {
            bottom.push_back(sides - 1 - i);
            const size_t next = (i + 1) % sides;
            extractor.m_remeshedPolygons.push_back({ i, next, sides + next, sides + i });
        }
        extractor.m_remeshedPolygons.push_back(bottom);
        extractor.rebuildHalfEdges();

        // Exercise boundary discovery and both score passes, including 6 -> 4 edges.
        const size_t before = extractor.m_remeshedPolygons.size();
        extractor.fixHoles();
        require(extractor.m_remeshedPolygons.size() == before + (sides - 1) / 2,
            "a completed hole was filled more than once");

        std::map<std::pair<size_t, size_t>, size_t> directedEdges;
        for (const auto& face : extractor.remeshedQuads()) {
            for (size_t i = 0; i < face.size(); ++i)
                ++directedEdges[{ face[i], face[(i + 1) % face.size()] }];
        }
        for (const auto& edge : directedEdges) {
            const auto reverse = directedEdges.find({ edge.first.second, edge.first.first });
            require(edge.second == 1 && reverse != directedEdges.end() && reverse->second == 1,
                "filled prism has repeated or unpaired directed edges");
        }
    }

    static void consumeTriangle()
    {
        QuadExtractor extractor(nullptr, nullptr, nullptr);
        std::vector<size_t> hole = { 0, 1, 2 };
        extractor.fixHoleWithQuads(hole);
        require(hole.empty(), "completed triangle hole was not consumed");
        require(extractor.remeshedQuads().size() == 1, "triangle hole did not produce one face");
    }
};

}

int main()
{
    int failures = 0;
    auto check = [&](const char* name, void (*test)()) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            ++failures;
        }
    };
    check("triangle prism", [] { AutoRemesher::QuadExtractorHoleFillingTest::closePrism(3); });
    check("quad prism", [] { AutoRemesher::QuadExtractorHoleFillingTest::closePrism(4); });
    check("hexagonal prism", [] { AutoRemesher::QuadExtractorHoleFillingTest::closePrism(6); });
    check("triangle consumed", AutoRemesher::QuadExtractorHoleFillingTest::consumeTriangle);
    return failures ? 1 : 0;
}

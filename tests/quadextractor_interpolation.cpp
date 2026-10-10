#include <AutoRemesher/PositionKey>
#include <AutoRemesher/QuadExtractor>
#include <iostream>
#include <set>
#include <tbb/global_control.h>

using namespace AutoRemesher;

int main()
{
    tbb::global_control threads(tbb::global_control::max_allowed_parallelism, 1);
    // One real cube-228 triangle's UVs, with its constant plane moved onto a
    // PositionKey grid boundary. The second solve differed only by roundoff.
    const double plane = 0.3;
    const std::vector<Vector3> vertices = {
        Vector3(plane, 0.3102688141650144, -0.23782029311742625),
        Vector3(plane, 0.21953842845505786, -0.08219804634788434),
        Vector3(plane, 0.3681173731953163, -0.10495111748363055)
    };
    const std::vector<std::vector<size_t>> triangles = { { 0, 1, 2 } };
    const std::vector<std::vector<std::vector<Vector2>>> solutions = {
        { { Vector2(0.10551613140711708, 2.4386811732370384),
            Vector2(-0.3010991146992701, 1.6802062315787152),
            Vector2(0.34731993475154865, 1.726281054979718) } },
        { { Vector2(0.10551613140708051, 2.4386811732371143),
            Vector2(-0.30109911469930994, 1.6802062315787272),
            Vector2(0.34731993475152956, 1.7262810549797603) } }
    };
    std::set<std::pair<PositionKey, PositionKey>> previous;
    for (const auto& uvs : solutions) {
        QuadExtractor extractor(&vertices, &triangles, &uvs);
        // This open patch checks the public raw graph, not closed output quality.
        extractor.extract();
        if (extractor.extractedConnections().empty()) {
            std::cerr << "Expected extracted isoline connections\n";
            return 1;
        }
        std::set<std::pair<PositionKey, PositionKey>> graph;
        for (const auto& connection : extractor.extractedConnections()) {
            for (const auto& point : { connection.first, connection.second }) {
                if (!(PositionKey(point) == PositionKey(Vector3(plane, point.y(), point.z())))) {
                    std::cerr << "A constant-coordinate crossing changed its position key\n";
                    return 1;
                }
            }
            graph.emplace(PositionKey(connection.first), PositionKey(connection.second));
        }
        if (!previous.empty() && graph != previous) {
            std::cerr << "Tiny UV differences changed the extracted connection graph\n";
            return 1;
        }
        previous = graph;
    }

    // Integer UV endpoints must retain the exact source position, including
    // when the edge ordering reverses. All positions fit the island frame.
    const std::vector<Vector3> endpointVertices = {
        Vector3(-0.3470354292320371, 0, 0),
        Vector3(0.3, 0.4, 0),
        Vector3(0.3, 0, 0.4)
    };
    for (double direction : { 1.0, -1.0 }) {
        const std::vector<std::vector<Vector2>> uvs = {
            { Vector2(-direction, -direction), Vector2(0, 0), Vector2(direction, -direction) }
        };
        QuadExtractor extractor(&endpointVertices, &triangles, &uvs);
        extractor.extract();
        std::set<size_t> endpoints;
        for (const auto& connection : extractor.extractedConnections()) {
            for (const auto& point : { connection.first, connection.second }) {
                for (size_t i = 0; i < endpointVertices.size(); ++i) {
                    const auto& endpoint = endpointVertices[i];
                    if (point.y() != endpoint.y() || point.z() != endpoint.z())
                        continue;
                    if (point.x() != endpoint.x() || !(PositionKey(point) == PositionKey(endpoint))) {
                        std::cerr << "An exact UV endpoint changed its source position\n";
                        return 1;
                    }
                    endpoints.insert(i);
                }
            }
        }
        if (endpoints.size() != endpointVertices.size()) {
            std::cerr << "Expected every source endpoint in the extracted graph\n";
            return 1;
        }
    }
    std::cout << "Extraction preserves constant coordinates and exact endpoint identity\n";
    return 0;
}

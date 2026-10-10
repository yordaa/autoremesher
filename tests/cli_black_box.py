#!/usr/bin/env python3

import collections
import pathlib
import re
import subprocess
import sys
import tempfile
import time
import unittest


CUBE_OBJ = """\
v -1 -1 -1
v 1 -1 -1
v 1 1 -1
v -1 1 -1
v -1 -1 1
v 1 -1 1
v 1 1 1
v -1 1 1
f 1 4 3
f 1 3 2
f 5 6 7
f 5 7 8
f 1 2 6
f 1 6 5
f 2 3 7
f 2 7 6
f 3 4 8
f 3 8 7
f 4 1 5
f 4 5 8
"""


def mesh_metrics(path):
    vertex_count = 0
    faces = []
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith("v "):
                vertex_count += 1
            elif line.startswith("f "):
                faces.append([int(value.split("/")[0]) - 1 for value in line.split()[1:]])
    edges = collections.Counter()
    adjacency = collections.defaultdict(set)
    used_vertices = set()
    for face in faces:
        if len(face) < 3 or any(index < 0 or index >= vertex_count for index in face):
            raise AssertionError("invalid output face")
        used_vertices.update(face)
        for index, first in enumerate(face):
            second = face[(index + 1) % len(face)]
            edges[tuple(sorted((first, second)))] += 1
            adjacency[first].add(second)
            adjacency[second].add(first)
    component_count = 0
    unseen = set(used_vertices)
    while unseen:
        component_count += 1
        pending = [unseen.pop()]
        while pending:
            for neighbor in adjacency[pending.pop()]:
                if neighbor in unseen:
                    unseen.remove(neighbor)
                    pending.append(neighbor)
    return {
        "faces": len(faces),
        "quads": sum(len(face) == 4 for face in faces),
        "boundaries": sum(count == 1 for count in edges.values()),
        "non_manifold": sum(count > 2 for count in edges.values()),
        "components": component_count,
    }


class CliBlackBoxTest(unittest.TestCase):
    def run_cli(self, source, output, target_quads="100", threads="1", measured_case=None):
        command = [
            sys.argv[1],
            "--input",
            str(source),
            "--output",
            str(output),
            "--target-quads",
            target_quads,
            "--threads",
            threads,
        ]
        if measured_case and sys.platform == "darwin":
            command = ["/usr/bin/time", "-l", *command]
        started = time.monotonic()
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
        )
        if measured_case:
            peak_match = re.search(r"(\d+)\s+maximum resident set size", result.stderr)
            peak_rss = peak_match.group(1) if peak_match else "unavailable"
            print(
                f"METRIC case={measured_case} wall_ms={int((time.monotonic() - started) * 1000)} "
                f"peak_rss_bytes={peak_rss}",
                flush=True,
            )
        return result

    def assert_topology_preserved(self, source, output, result, measured_case):
        input_metrics = mesh_metrics(source)
        output_metrics = mesh_metrics(output)
        self.assertGreater(output_metrics["faces"], 0)
        self.assertEqual(output_metrics["boundaries"], input_metrics["boundaries"])
        self.assertEqual(output_metrics["components"], input_metrics["components"])
        self.assertEqual(output_metrics["non_manifold"], 0)
        self.assertIn(f"faces={output_metrics['faces']}", result.stdout)
        self.assertIn(f"quads={output_metrics['quads']}", result.stdout)
        quad_percentage = 100 * output_metrics["quads"] / output_metrics["faces"]
        print(
            f"TOPOLOGY case={measured_case} faces={output_metrics['faces']} "
            f"quad_percentage={quad_percentage:.1f} boundaries={output_metrics['boundaries']} "
            f"components={output_metrics['components']} non_manifold={output_metrics['non_manifold']}",
            flush=True,
        )

    def test_invalid_input_fails_without_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "invalid.obj"
            output = root / "output.obj"
            source.write_text("this is not an obj\n", encoding="utf-8")

            result = self.run_cli(source, output)

            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output.exists())

    def test_negative_thread_limit_is_rejected_before_loading(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "empty.obj"
            output = root / "output.obj"
            source.write_text("", encoding="utf-8")

            result = self.run_cli(source, output, threads="-1")

            self.assertEqual(result.returncode, 2)
            self.assertIn("Invalid --threads", result.stderr)
            self.assertFalse(output.exists())

    def test_excessive_target_is_rejected_before_loading(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "empty.obj"
            output = root / "output.obj"
            source.write_text("", encoding="utf-8")

            result = self.run_cli(source, output, target_quads="5001")

            self.assertEqual(result.returncode, 2)
            self.assertIn("supported limit", result.stderr)
            self.assertFalse(output.exists())

    def test_excessive_input_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "too-large.obj"
            output = root / "output.obj"
            source.write_text("v 0 0 0\n" * 1000001, encoding="utf-8")

            result = self.run_cli(source, output)

            self.assertEqual(result.returncode, 3)
            self.assertIn("vertex limit of 1000000", result.stderr)
            self.assertFalse(output.exists())

    def test_closed_organic_mesh_produces_progress_and_valid_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "icosahedron.obj"
            output = root / "output.obj"
            source.write_text(
                (pathlib.Path(__file__).parent / "fixtures" / "icosahedron.obj").read_text(
                    encoding="utf-8"
                ),
                encoding="utf-8",
            )

            result = self.run_cli(
                source, output, target_quads="100", threads="1", measured_case="organic"
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("PROGRESS ", result.stdout)
            self.assertIn("Building target topology", result.stdout)
            self.assertIn("Fitting target topology", result.stdout)
            self.assertIn("RESULT ", result.stdout)
            self.assert_topology_preserved(source, output, result, "organic")

    def test_closed_hard_surface_mesh_produces_valid_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "cube.obj"
            output = root / "output.obj"
            source.write_text(CUBE_OBJ, encoding="utf-8")

            result = self.run_cli(
                source, output, target_quads="50", threads="1", measured_case="hard-surface"
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assert_topology_preserved(source, output, result, "hard-surface")

    def test_obj_double_precision_preserves_translated_cube(self):
        reference_bounds = None
        for offset in (0.0, 1e8):
            with self.subTest(offset=offset), tempfile.TemporaryDirectory() as directory:
                source = pathlib.Path(directory) / "cube.obj"
                output = pathlib.Path(directory) / "output.obj"
                lines = []
                for line in CUBE_OBJ.splitlines():
                    values = line.split()
                    if values[0] == "v":
                        line = "v " + " ".join(f"{float(value) + offset:.17g}" for value in values[1:])
                    lines.append(line)
                source.write_text("\n".join(lines) + "\n", encoding="utf-8")

                result = self.run_cli(source, output, target_quads="1000")

                self.assertEqual(result.returncode, 0, result.stderr)
                self.assert_topology_preserved(source, output, result, f"cube-offset-{offset:g}")
                positions = []
                directed_edges = collections.Counter()
                for line in output.read_text(encoding="utf-8").splitlines():
                    values = line.split()
                    if values[0] == "v":
                        positions.append(tuple(float(value) for value in values[1:]))
                    elif values[0] == "f":
                        face = [int(value) - 1 for value in values[1:]]
                        for index, first in enumerate(face):
                            directed_edges[(first, face[(index + 1) % len(face)])] += 1
                        origin = positions[face[0]]
                        area_squared = 0.0
                        for index in range(1, len(face) - 1):
                            left = [value - base for value, base in zip(positions[face[index]], origin)]
                            right = [value - base for value, base in zip(positions[face[index + 1]], origin)]
                            area_squared += sum(
                                (left[i] * right[(i + 1) % 3] - left[(i + 1) % 3] * right[i]) ** 2
                                for i in range(3)
                            )
                        self.assertGreater(area_squared, 0.0, "output has a zero-area face")
                for (first, second), count in directed_edges.items():
                    self.assertEqual(count, 1)
                    self.assertEqual(directed_edges[(second, first)], 1)
                bounds = [
                    (min(point[axis] for point in positions) - offset,
                     max(point[axis] for point in positions) - offset)
                    for axis in range(3)
                ]
                print(f"PLACEMENT case=cube-offset-{offset:g} local_bounds={bounds}", flush=True)
                # Compare placement to the unshifted run without assuming exact remesh geometry.
                for lower, upper in bounds:
                    self.assertAlmostEqual(lower, -1.0, delta=0.1)
                    self.assertAlmostEqual(upper, 1.0, delta=0.1)
                if reference_bounds is None:
                    reference_bounds = bounds
                else:
                    for actual, reference in zip(bounds, reference_bounds):
                        for coordinate, expected in zip(actual, reference):
                            self.assertAlmostEqual(coordinate, expected, delta=0.02)

    def test_open_mesh_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "open.obj"
            output = root / "output.obj"
            source.write_text(
                """\
v 0 0 0
v 1 0 0
v 0 1 0
f 1 2 3
""",
                encoding="utf-8",
            )

            result = self.run_cli(source, output)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("closed oriented manifold", result.stderr)
            self.assertFalse(output.exists())

    def test_non_triangle_input_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "quad-cube.obj"
            output = root / "output.obj"
            source.write_text(
                """\
v -1 -1 -1
v 1 -1 -1
v 1 1 -1
v -1 1 -1
v -1 -1 1
v 1 -1 1
v 1 1 1
v -1 1 1
f 1 4 3 2
f 5 6 7 8
f 1 2 6 5
f 2 3 7 6
f 3 4 8 7
f 4 1 5 8
""",
                encoding="utf-8",
            )

            result = self.run_cli(source, output)

            self.assertEqual(result.returncode, 3)
            self.assertIn("non-triangle", result.stderr)
            self.assertFalse(output.exists())

    def test_non_manifold_input_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "non-manifold.obj"
            output = root / "output.obj"
            source.write_text(
                """\
v 1 1 1
v -1 -1 1
v -1 1 -1
v 1 -1 -1
f 1 3 2
f 1 2 4
f 1 4 3
f 2 3 4
f 1 3 2
""",
                encoding="utf-8",
            )

            result = self.run_cli(source, output)

            self.assertEqual(result.returncode, 3)
            self.assertIn("closed oriented manifold", result.stderr)
            self.assertFalse(output.exists())

    def test_zero_area_closed_mesh_fails_without_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "zero-area.obj"
            output = root / "output.obj"
            source.write_text(
                """\
v 0 0 0
v 1 0 0
v 2 0 0
v 3 0 0
f 1 3 2
f 1 2 4
f 1 4 3
f 2 3 4
""",
                encoding="utf-8",
            )

            result = self.run_cli(source, output, target_quads="24")

            self.assertEqual(result.returncode, 3)
            self.assertIn("degenerate triangle", result.stderr)
            self.assertFalse(output.exists())

    def test_already_rounded_cube_is_rejected_before_remeshing(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "rounded-cube.obj"
            output = pathlib.Path(directory) / "output.obj"
            lines = [
                "v 100000000 100000000 100000000" if line.startswith("v ") else line
                for line in CUBE_OBJ.splitlines()
            ]
            source.write_text("\n".join(lines) + "\n", encoding="utf-8")

            result = self.run_cli(source, output, target_quads="1000")

            self.assertEqual(result.returncode, 3)
            self.assertIn("degenerate triangle", result.stderr)
            self.assertNotIn("RESULT ", result.stdout)
            self.assertFalse(output.exists())

    def test_process_can_be_cancelled_without_final_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "many-islands.obj"
            output = root / "output.obj"
            lines = []
            for island in range(2000):
                base = island * 4
                offset = island * 4
                lines.extend(
                    [
                        f"v {offset + 1} 1 1",
                        f"v {offset - 1} -1 1",
                        f"v {offset - 1} 1 -1",
                        f"v {offset + 1} -1 -1",
                        f"f {base + 1} {base + 3} {base + 2}",
                        f"f {base + 1} {base + 2} {base + 4}",
                        f"f {base + 1} {base + 4} {base + 3}",
                        f"f {base + 2} {base + 3} {base + 4}",
                    ]
                )
            source.write_text("\n".join(lines) + "\n", encoding="utf-8")

            process = subprocess.Popen(
                [
                    sys.argv[1],
                    "--input",
                    str(source),
                    "--output",
                    str(output),
                    "--target-quads",
                    "5000",
                    "--threads",
                    "1",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            self.assertTrue(process.stdout.readline().startswith("PROGRESS "))
            process.terminate()
            process.communicate(timeout=5)

            self.assertNotEqual(process.returncode, 0)
            self.assertFalse(output.exists())
            self.assertEqual(list(root.glob("output.obj.tmp-*")), [])


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: cli_black_box.py /path/to/autoremesher-cli")
    unittest.main(argv=[sys.argv[0]])

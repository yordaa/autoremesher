# Qt-free CLI

The `autoremesher-cli` target runs the remeshing core without Qt. Its pinned upstream base is `5d631db45107378c6852289bd013cbbb1b9322ab`.

## Build on macOS arm64

```sh
brew install cmake ninja tbb
cmake -S . -B build-headless -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-headless
ctest --test-dir build-headless --output-on-failure
```

The build uses a CMake-provided TBB package and contains no Qt dependency. Windows standalone builds use the bundled static TBB by default; package managers can pass `-DAUTOREMESHER_USE_BUNDLED_TBB=OFF`.

## Run

```sh
build-headless/autoremesher-cli \
  --input input.obj \
  --output output.obj \
  --target-quads 5000 \
  --threads 4
```

Input must be a non-empty, closed, consistently oriented manifold triangle OBJ. The safety limits are 1,000,000 input vertices, 1,000,000 input triangles, and 5,000 requested quads. Output is polygon OBJ and may contain non-quads.

Stdout contains flushed progress and one final result line:

```text
PROGRESS 30 Fitting target topology...
RESULT vertices=100 faces=98 quads=98 non_quads=0 elapsed_ms=11
```

Diagnostics go to stderr. Exit status is non-zero for invalid arguments, input, remeshing, output validation, or writing. Output is written beside the destination and renamed only after a complete successful write. On macOS, SIGTERM removes that temporary output and stops the process without publishing a partial destination.

## macOS spike evidence

The release build was measured on macOS arm64 at the pinned base using four TBB threads:

| Input | Request | Result | Topology | Wall time | Peak RSS |
| --- | ---: | --- | --- | ---: | ---: |
| OpenSubdiv `loop_icosahedron_level4.obj` (20,480 triangles) | 5,000 quads | 3,537 faces, 99.8% quad | Closed, one component, no non-manifold edges | 0.25 s | 102.2 MiB |
| OpenSubdiv `loop_cube_level4.obj` (24,576 triangles) | 5,000 quads | 2,910 faces, 100% quad | Closed, one component, no non-manifold edges | 0.28 s | 107.9 MiB |
| OpenSubdiv `loop_saddle_edgeonly_level2.obj` | 100 quads | Rejected before remeshing | Open input | <0.01 s | Not measured |

The black-box suite also generates its own organic and hard-surface fixtures, records wall time, peak RSS, face and quad counts, and verifies boundary, component, index, and manifold invariants. Run `ctest --test-dir build-headless -V` to see the recorded metrics. `otool -L` reports only Apple Accelerate, libc++, and libSystem. Qt and dynamic TBB are absent.

## Windows x64

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release -V --output-on-failure
```

The Windows target builds the bundled TBB 2017.0 and the MSVC runtime statically. The `windows-cli` GitHub Actions job runs the same black-box contract as macOS, rejects Qt/TBB/MSVC-runtime DLL imports, records wall time and peak RSS, packages the required notices, hashes the executable and ZIP, and uploads the evidence with the standalone artifact.

The accepted platform difference is TBB 2017.0 on Windows versus oneTBB 2023.0.0 on macOS. Both are statically linked and pass the same observable CLI contract; exact output face counts may differ with platform scheduling and floating-point behavior.

Windows process-tree termination is abrupt and cannot run the CLI's in-process temporary-file cleanup. The CLI starts no child processes, and the final destination is still absent or complete because publication is the last atomic rename. If termination lands during that final write, `<output>.tmp-<pid>` can remain. This is an accepted platform difference: EzDesign must place the process files in its existing unique temporary directory and remove that directory after termination, as required by #731.

## Decision

**GO to EzDesign integration (#731), but do not ship yet.** macOS arm64 and Windows x64 now prove the external executable boundary, honest progress, process cancellation, bounded concurrency, output validation, and a commercially usable compiled dependency closure. Packaging and signing inside EzDesign remain the release gate. The conservative size limits above remain until the production benchmark corpus justifies changing them.

## Compiled dependency closure

| Component | Version/source | License evidence |
| --- | --- | --- |
| AutoRemesher core and CLI | pinned repository revision above | [`LICENSE`](LICENSE), MIT |
| isotropicremesher | bundled source at this revision | [`thirdparty/isotropicremesher/LICENSE`](thirdparty/isotropicremesher/LICENSE), MIT |
| Eigen | bundled 5.0.1, built with `EIGEN_MPL2_ONLY` | [`thirdparty/eigen/COPYING.README`](thirdparty/eigen/COPYING.README) and `COPYING.MPL2` |
| TBB | CMake package on macOS; bundled 2017.0 by default on Windows | [`thirdparty/tbb/LICENSE`](thirdparty/tbb/LICENSE), Apache-2.0 |
| tinyobjloader | bundled header used by the CLI | MIT notice at the top of [`src/tiny_obj_loader.h`](src/tiny_obj_loader.h) |
| Apple Accelerate, libc++, libSystem, Windows system DLLs | operating-system components | Not redistributed separately |
| MSVC runtime | GitHub-hosted Visual Studio 2022 toolchain, statically linked on Windows | No separate runtime files redistributed |

The Qt GUI and its dependencies are not part of this target. Geogram, Exploragram, OpenNL, CoMISo, libigl, and zlib are not compiled or linked into this target.

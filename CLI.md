# Qt-free CLI

The `autoremesher-cli` target runs the remeshing core without Qt. Its pinned upstream base is `5d631db45107378c6852289bd013cbbb1b9322ab`.

## Build on macOS arm64

```sh
brew install cmake ninja tbb
test "$(brew list --versions tbb)" = "tbb 2023.0.0"
cmake -S . -B build-headless -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-headless
ctest --test-dir build-headless --output-on-failure
```

The build requires oneTBB 2023.0.0 exactly and links its static archive. Pass a different exact installation with `-DAUTOREMESHER_TBB_ROOT=/path/to/tbb`. The resulting executable does not require a Homebrew or Qt dynamic library at runtime.

## Run

```sh
build-headless/autoremesher-cli \
  --input input.obj \
  --output output.obj \
  --target-quads 5000 \
  --threads 4
```

Input must be a non-empty, closed, consistently oriented manifold triangle OBJ. The initial measured safety limits are 25,000 input vertices, 25,000 input triangles, and 5,000 requested quads. Output is polygon OBJ and may contain non-quads.

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

## Decision

**GO to Windows parity (#730), but do not ship yet.** The macOS spike proves the external executable boundary, honest progress, cancellation, bounded concurrency, output validation, and a commercially usable compiled dependency closure. Windows must reproduce the same contract and bundle the recorded license notices before EzDesign integration begins. The conservative size limits above remain until the production benchmark corpus justifies changing them.

## Compiled dependency closure

| Component | Version/source | License evidence |
| --- | --- | --- |
| AutoRemesher core and CLI | pinned repository revision above | [`LICENSE`](LICENSE), MIT |
| isotropicremesher | bundled source at this revision | [`thirdparty/isotropicremesher/LICENSE`](thirdparty/isotropicremesher/LICENSE), MIT |
| Eigen | bundled 5.0.1, built with `EIGEN_MPL2_ONLY` | [`thirdparty/eigen/COPYING.README`](thirdparty/eigen/COPYING.README) and `COPYING.MPL2` |
| oneTBB | Homebrew 2023.0.0, statically linked | [`thirdparty/tbb/LICENSE`](thirdparty/tbb/LICENSE), Apache-2.0 |
| tinyobjloader | bundled header used by the CLI | MIT notice at the top of [`src/tiny_obj_loader.h`](src/tiny_obj_loader.h) |
| Apple Accelerate, libc++, libSystem | operating-system components | Not redistributed separately |

The Qt GUI and its dependencies are not part of this target. Geogram, Exploragram, OpenNL, CoMISo, libigl, and zlib are not compiled or linked into this target.

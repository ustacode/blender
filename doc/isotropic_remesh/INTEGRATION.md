# Isotropic Remesh — Blender Geometry Node Integration

This document describes the integration of
[huxingyi/isotropicremesher](https://github.com/huxingyi/isotropicremesher) (MIT) as a native
**Isotropic Remesh** Geometry Node in a custom Blender build.

- **Base branch:** `blender-v5.0-release` (Blender 5.0.1, the stable release chosen for the fork)
- **Feature branch:** `isotropic-remesh-node`
- **Upstream remesher commit:** `d4674ce63288d7a55a70735181049895aebc3bf7`
- **Build target produced here:** macOS arm64 (Apple Silicon). Windows instructions are in
  [§9](#9-windows-build).

---

## 1. Architecture & data flow

The integration is split into three layers so the third-party code stays isolated and the
Blender-facing logic is a clean, reusable function:

```
Geometry Node  ───────────────►  Reusable geometry function  ───────────────►  Vendored library
node_geo_isotropic_remesh.cc     source/blender/geometry/          extern/isotropicremesher/
(sockets, GN evaluation)         intern/isotropic_remesh.cc        (MIT, unmodified algorithm)
                                 (Mesh <-> triangle-soup,
                                  attribute resampling)
```

1. **Node** (`node_geo_isotropic_remesh.cc`) — declares sockets, reads inputs during Geometry
   Nodes evaluation, and calls the reusable function. It only runs when Geometry Nodes actually
   evaluates the node, and it never writes to the input mesh: it produces a brand-new `Mesh` and
   hands it to `GeometrySet::replace_mesh`.
2. **Reusable function** `blender::geometry::isotropic_remesh()` — converts a Blender `Mesh` to
   the library's triangle soup, runs the algorithm, converts the resulting half-edge mesh back to
   a new `Mesh`, and resamples attributes. This is the "port as a reusable function under
   `source/blender/geometry/`" required by the task and can be called from anywhere (nodes,
   operators, tests).
3. **Vendored library** `extern/isotropicremesher/` — the upstream MIT algorithm, kept verbatim
   except for two small, clearly tagged hooks (boundary toggle + progress/cancel callback).

---

## 2. Files added

| File | Lines | Purpose |
|------|------:|---------|
| `extern/isotropicremesher/src/*.{h,cpp}` | ~2260 | Vendored MIT remesher (10 files). |
| `extern/isotropicremesher/CMakeLists.txt` | 40 | Builds `extern_isotropicremesher`; defines `_USE_MATH_DEFINES` on Windows. |
| `extern/isotropicremesher/LICENSE` | — | Upstream MIT license (copied verbatim). |
| `extern/isotropicremesher/README.blender` | 25 | Provenance + list of local modifications. |
| `source/blender/geometry/GEO_isotropic_remesh.hh` | 51 | Public API: `IsotropicRemeshParams` + `isotropic_remesh()`. |
| `source/blender/geometry/intern/isotropic_remesh.cc` | 383 | Mesh↔library conversion + nearest-surface attribute transfer. |
| `source/blender/nodes/geometry/nodes/node_geo_isotropic_remesh.cc` | 111 | The Geometry Node. |
| `source/blender/geometry/tests/GEO_isotropic_remesh_test.cc` | 325 | GTest regression suite. |

## 3. Files modified

| File | Change |
|------|--------|
| `extern/CMakeLists.txt` | `add_subdirectory(isotropicremesher)` (placed after `remove_strict_flags()` so third-party warnings don't fail the build). |
| `source/blender/blenkernel/BKE_node_legacy_types.hh` | `#define GEO_NODE_ISOTROPIC_REMESH 2158` (next free node type id). |
| `source/blender/makesrna/intern/rna_nodetree.cc` | `define("GeometryNode", "GeometryNodeIsotropicRemesh")` — registers the node's RNA struct. **Required:** without it `node_type_base` dereferences a null `StructRNA` and Blender crashes at startup. |
| `source/blender/geometry/CMakeLists.txt` | Added the include path to the vendored headers, the adapter `.cc`/`.hh`, the `extern_isotropicremesher` link dependency, and the test source. |
| `source/blender/nodes/geometry/CMakeLists.txt` | Registered `node_geo_isotropic_remesh.cc`. |
| `scripts/startup/bl_ui/node_add_menu_geometry.py` | Added **Isotropic Remesh** to *Add ▸ Mesh ▸ Operations*. |

The vendored library files `isotropicremesher.{h,cpp}` also contain three small additions, each
tagged with the comment `BLENDER MODIFICATION` and documented in
`extern/isotropicremesher/README.blender`:
- `setPreserveBoundaries(bool)` — gate boundary-edge pinning.
- `setSharpEdgeThresholdRadians(double)` — set the sharp threshold directly in Blender's
  face-normal-angle convention.
- an optional progress/cancellation callback argument to `remesh()`.

No **DNA** changes and no custom **RNA properties** are required: every parameter is exposed as a
node **input socket** (the modern "everything is a socket" pattern used by e.g. *Subdivide Mesh*),
so there is no node storage to version. The only RNA touch is the one-line struct registration
above, which every node type needs regardless of whether it has properties.

---

## 4. Node reference — *Isotropic Remesh*

Location: **Add ▸ Mesh ▸ Operations ▸ Isotropic Remesh** (`GeometryNodeIsotropicRemesh`).

| Socket | Type | Default | Meaning |
|--------|------|---------|---------|
| **Mesh** (in) | Geometry (Mesh) | — | Mesh to remesh. Non-triangle faces are triangulated internally. |
| **Mesh** (out) | Geometry (Mesh) | — | New uniform-triangle mesh. |
| **Target Edge Length** | Float (distance) | 0.1 | Desired edge length. **0 = use the input's average edge length.** |
| **Iterations** | Int | 10 | Number of split/collapse/flip/relax/project passes. |
| **Preserve Boundary** | Bool | On | Pin open boundary edges so holes/outlines keep their shape. |
| **Preserve Sharp Edges** | Bool | Off | Pin edges whose adjacent faces bend more than *Sharp Angle*. |
| **Sharp Angle** | Float (angle) | 30° | Face-normal angle above which an edge counts as a sharp feature. |

Notes:
- Enabling **Preserve Sharp Edges** also preserves boundaries (the feature-detection pass pins
  both), which matches the upstream algorithm.
- The algorithm targets **manifold triangle-convertible** surfaces. Non-manifold input is handled
  best-effort (see [§6](#6-edge-case-behavior)).

---

## 5. Attribute preservation policy

Isotropic remeshing rebuilds topology completely (vertices, edges and faces all change), so there
is no 1:1 correspondence with the input. Attributes are therefore **resampled by nearest-surface
interpolation** (reusing Blender's `bke::mesh_surface_sample` utilities) rather than copied:

**Preserved (best-effort, when *transfer_attributes* is on — always on from the node):**
- **Point-domain** attributes (custom vertex data, vertex colors on points, sculpt-style masks,
  vertex groups stored as generic attributes, …) — barycentric interpolation from the nearest
  original triangle.
- **Face-domain** attributes, including **`material_index`** — taken from the nearest original
  face at each output face centroid. Material slots are preserved via
  `BKE_mesh_copy_parameters_for_eval`.
- **Corner-domain** attributes, including **UV maps** — sampled at each output corner. (UV seams
  are blurred slightly because sampling is per output vertex; this is inherent to retopology.)

**Always lost (documented and intentional):**
- **Edge-domain** attributes (e.g. crease, bevel weight, edge seams, edge sharp flags) — edges are
  fully rebuilt and have no meaningful mapping.
- **String** attributes — nearest-surface interpolation is undefined for strings; they are skipped.
- Exact per-vertex identity / original indices — the output is new geometry.

Only `position` and the topology built-ins (`.corner_vert`, `.corner_edge`, `.edge_verts`) are
produced directly by the rebuild; everything else follows the policy above and additionally
honors the Geometry Nodes attribute-propagation filter (`params.get_attribute_filter("Mesh")`).

---

## 6. Edge-case behavior

| Case | Behavior |
|------|----------|
| **Non-triangle faces (quads/ngons)** | Triangulated internally via `Mesh::corner_tris()`. |
| **Loose vertices** (not used by any face) | Dropped — only surface geometry is remeshed. |
| **Degenerate triangles** (repeated vertex index) | Skipped before the half-edge build so they can't create zero-length edges / crashes. |
| **Boundaries / open meshes** | Preserved when *Preserve Boundary* is on; the half-edge builder marks boundary vertices as features. |
| **Disconnected geometry** | Each island is remeshed independently in a single pass. |
| **Empty mesh / no faces** | The node leaves the geometry untouched (no-op). |
| **Non-manifold input** | Best-effort. The upstream half-edge builder keeps the first of any duplicated directed edge; results may be imperfect but should not corrupt the `Mesh`. |
| **No output produced** | The node keeps the input mesh and emits a warning. |

---

## 7. Progress & cancellation

Cancellation is plumbed end-to-end at the algorithm level: `remesh()` accepts a callback invoked
once per iteration with progress in `[0, 1]`; returning `false` stops the loop and returns the
partially-remeshed (still valid) mesh. `isotropic_remesh()` forwards a `std::function<bool(float)>`
to it. This is exercised by the `PerformanceAndCancellation` test.

The interactive Geometry Nodes evaluator in Blender 5.0 does not expose a per-node mid-execution
cancel signal to `geometry_node_execute`, so the shipped node runs to completion; the callback
hook is in place for any caller (operator, background job, future evaluator) that has a cancel
source. This is a deliberate, documented limitation rather than a missing piece.

---

## 8. License compliance (MIT ↔ GPL)

- **Blender** is distributed under **GPL-2.0-or-later**. All new Blender-side files
  (`isotropic_remesh.cc/.hh`, `node_geo_isotropic_remesh.cc`) carry
  `SPDX-License-Identifier: GPL-2.0-or-later`; tests use `Apache-2.0` like the rest of Blender's
  test suite.
- **The remesher** is **MIT**, which is **GPL-compatible**. MIT code may be combined with and
  distributed as part of a GPL work; the combined binary is offered under GPL-2.0-or-later while
  the MIT portions retain their MIT terms.
- **MIT's only obligation** — retain the copyright + permission notice in all copies — is met:
  - every vendored source file keeps its original MIT header (© 2020-2021 Jeremy HU);
  - `extern/isotropicremesher/LICENSE` contains the full MIT text;
  - `extern/isotropicremesher/README.blender` records provenance and every local modification;
  - the MIT license text already ships with Blender at `doc/license/MIT-license.txt`.

**Conclusion: redistributing this custom build is permitted.** Ship it under GPL-2.0-or-later and
keep `doc/license/` (which includes the MIT text) and the `extern/isotropicremesher/LICENSE`
alongside the binaries. This mirrors how Blender already bundles other MIT/permissive libraries
(e.g. quadriflow, xxHash). No CLA is required to *use/distribute*; contributing this upstream to
blender.org would additionally require their contributor agreement, which is out of scope here.

---

## 9. Windows build

Blender **cannot be cross-compiled** from macOS/Linux to Windows — the build must run natively on
Windows. All source changes above are platform-independent and already include the one Windows
detail that matters (`-D_USE_MATH_DEFINES` for `M_PI`, set in the extern `CMakeLists.txt`). To
produce a Windows build from this feature branch:

**Prerequisites**
- Windows 10/11 x64
- Visual Studio 2022 (Community is fine) with **Desktop development with C++** (MSVC v143, Windows
  10/11 SDK)
- Git + Git LFS, CMake ≥ 3.10, Python 3

**Steps (Developer Command Prompt for VS 2022)**
```bat
:: 1. Get this branch (or copy the modified tree over a fresh clone)
git clone https://projects.blender.org/blender/blender.git
cd blender
git checkout blender-v5.0-release
::   then apply the isotropic-remesh changes (this branch / patch)

:: 2. Fetch the precompiled Windows libraries for this branch (~several GB, uses Git LFS)
make update

:: 3. Configure + build a Release Blender
make
::   'make' wraps CMake+MSBuild and writes to ..\build_windows_x64_vc17_Release
```
Alternatively, configure manually:
```bat
cmake -S . -B ..\build_windows -G "Visual Studio 17 2022" -A x64
cmake --build ..\build_windows --config Release --target INSTALL
```
The result is `..\build_windows_x64_vc17_Release\bin\Release\blender.exe`. The new node appears at
**Add ▸ Mesh ▸ Operations ▸ Isotropic Remesh**, identical to the macOS build. (For **Windows on ARM**,
the libraries `lib/windows_arm64` are also referenced in `.gitmodules`; use the ARM64 VS toolchain.)

---

## 10. Testing

**C++ regression suite** (`GEO_isotropic_remesh_test.cc`, target `bf_geometry_tests`):
covers closed manifolds, refinement, boundary preservation, disconnected geometry, degenerate
triangles, point/face/corner attribute transfer, and a performance + cancellation smoke test.

```bash
# configure with tests
cmake -S blender -B build_darwin -G Ninja -DCMAKE_BUILD_TYPE=Release -DWITH_GTESTS=ON
ninja -C build_darwin bf_geometry_tests
./build_darwin/bin/tests/bf_geometry_tests --gtest_filter='IsotropicRemeshTest.*'
```

**Manual test in the app:** add a Cube, give it a Geometry Nodes modifier, drop in *Isotropic
Remesh*, and vary Target Edge Length / Iterations / the preserve toggles.

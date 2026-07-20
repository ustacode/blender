# Work Summary — Isotropic Remesh Geometry Node for Blender

**Goal:** integrate `huxingyi/isotropicremesher` (MIT) into Blender as a native Geometry Nodes node
and produce a custom build.

**Status: complete and verified.** Custom Blender 5.0.1 built on macOS arm64, node works in the
app, all 7 regression tests pass, change committed and pushed to a GitHub fork.

---

## Deliverables at a glance

| Item | Where |
|------|-------|
| Custom Blender build (macOS arm64) | `build_darwin/bin/Blender.app` |
| Feature branch | `isotropic-remesh-node` (off `blender-v5.0-release`) |
| Commit | `36a446593cd` — "Geometry Nodes: add Isotropic Remesh node" (23 files, +3257) |
| Pushed to | **https://github.com/ustacode/blender** (branch `isotropic-remesh-node`) |
| Full technical write-up | `ISOTROPIC_REMESH_INTEGRATION.md` |
| Windows build guide | `WINDOWS_BUILD_GUIDE.md` |
| Before/after render | Suzanne 500 faces → 2,702 uniform triangles |

---

## Plan → outcome

| # | Task | Outcome |
|---|------|---------|
| 1 | Fork + build a stable branch | ✅ `blender-v5.0-release` (5.0.1), branch `isotropic-remesh-node`; built clean (exit 0). |
| 2 | Port C++ as a reusable function under `source/blender/geometry/` | ✅ `GEO_isotropic_remesh.hh` + `intern/isotropic_remesh.cc` (`blender::geometry::isotropic_remesh()`). |
| 3 | Convert to/from Blender `Mesh` | ✅ triangulates input via `corner_tris`, rebuilds a new `Mesh`; compacts loose verts. |
| 4 | Node: Mesh I/O, Target Edge Length, Iterations, Feature/Boundary preservation | ✅ 6 input sockets (added Sharp Angle). All parameters are sockets — no DNA. |
| 5 | Run only when needed + never modify input | ✅ lazy GN evaluation; produces a new `Mesh`; **verified** the input datablock is untouched. |
| 6 | CMake, registration, validation, progress/cancellation, tests | ✅ all done (details below). |
| 7 | Test manifold / boundary / disconnected / degenerate / attributes / performance | ✅ 7-case GTest suite, all pass. |
| 8 | Custom build + document modified files | ✅ macOS build produced; every file documented. |
| 9 | Windows build | ⚠️ Not possible from macOS (no cross-compile). Full native Windows procedure written. |
| 10 | Preserve useful attributes, define what's lost | ✅ nearest-surface resampling; edge + string attributes dropped (documented). |
| 11 | Check MIT/GPL license before distributing | ✅ MIT is GPL-compatible; obligations met; safe to redistribute. |

---

## Architecture

Three isolated layers keep the third-party code separate from GPL Blender code:

```
Node (GPL)                     Reusable fn (GPL)                Vendored lib (MIT)
node_geo_isotropic_remesh.cc → geometry/isotropic_remesh.cc → extern/isotropicremesher/
sockets + GN evaluation        Mesh<->soup + attr resample     unchanged algorithm + 3 hooks
```

- **Vendored library** kept verbatim except three tagged `BLENDER MODIFICATION` hooks:
  `setPreserveBoundaries`, `setSharpEdgeThresholdRadians`, and a progress/cancellation callback on
  `remesh()`.
- **Reusable function** does all Blender-specific work; callable from nodes, operators or tests.
- **Node** just declares sockets and calls the function during evaluation.

## Files

**Added (11):** the vendored library `extern/isotropicremesher/` (10 source files + `CMakeLists.txt`
+ `LICENSE` + `README.blender`), `GEO_isotropic_remesh.hh`, `intern/isotropic_remesh.cc`,
`node_geo_isotropic_remesh.cc`, `tests/GEO_isotropic_remesh_test.cc`.

**Modified (6):** `extern/CMakeLists.txt`, `BKE_node_legacy_types.hh` (`GEO_NODE_ISOTROPIC_REMESH
2158`), `rna_nodetree.cc` (RNA struct registration), geometry + nodes `CMakeLists.txt`,
`node_add_menu_geometry.py` (menu entry).

## Attribute policy

Topology is fully rebuilt, so attributes are **resampled by nearest-surface interpolation**:

- **Preserved:** point-domain attributes; face-domain incl. `material_index`; corner-domain incl.
  UV maps. Material slots preserved.
- **Lost (documented):** edge-domain attributes (crease, bevel weight, seams — no valid mapping)
  and string attributes (not interpolatable). Honors the GN attribute-propagation filter.

## Progress / cancellation

End-to-end callback (`std::function<bool(float)>`, checked between iterations; returning `false`
stops early and returns a valid partial mesh). Exercised by the test suite. The interactive GN
evaluator in 5.0 doesn't expose a mid-node cancel signal, so the shipped node runs to completion —
the hook is in place for any caller that has a cancel source. Documented as such.

## License

Blender is GPL-2.0-or-later; the remesher is MIT (GPL-compatible). MIT's only requirement — keep the
copyright + permission notice — is met via preserved file headers, `extern/isotropicremesher/LICENSE`,
`README.blender`, and Blender's existing `doc/license/MIT-license.txt`. **Redistribution is
permitted** under GPL-2.0-or-later with the MIT notices retained (same as Blender's other bundled
MIT libraries, e.g. quadriflow).

## Verification performed

1. **Compile/link:** full Blender build, exit 0, zero errors on the new code.
2. **In-app end-to-end (headless Python):** node registered; ico-sphere 162v/320f → 1214 triangles;
   `height` (point), `region` (face, constant preserved), `UVMap` (corner) all transferred; input
   mesh confirmed unmodified.
3. **Unit tests:** `blender_test --gtest_filter=IsotropicRemeshTest.*` → **7/7 PASSED**.
4. **Visual:** rendered before/after wireframe (Suzanne 500 → 2,702 uniform triangles), sharp
   edges and separate eye shells preserved.

## One notable fix during the work

The first full build compiled fine but Blender **segfaulted at startup** — a new node type also
needs an RNA struct registered via `define("GeometryNode", "GeometryNodeIsotropicRemesh")` in
`rna_nodetree.cc`, otherwise `node_type_base` dereferences a null `StructRNA` in a Release build.
Added the line, rebuilt, verified. This is called out in both guides so a Windows rebuild won't hit
it.

## How to rebuild / retest (macOS)

```bash
export PATH="/opt/homebrew/bin:$PATH"
ninja -C build_darwin blender && ninja -C build_darwin install         # build + install
cmake -S blender -B build_darwin -DWITH_GTESTS=ON                        # enable tests
ninja -C build_darwin blender_test
./build_darwin/bin/tests/blender_test --gtest_filter='IsotropicRemeshTest.*'
```
Windows: see `WINDOWS_BUILD_GUIDE.md`.

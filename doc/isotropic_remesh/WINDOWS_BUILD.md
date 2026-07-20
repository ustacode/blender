# Building the Isotropic-Remesh Blender on Windows

A complete, standalone guide to producing a Windows build of the custom Blender that contains the
**Isotropic Remesh** geometry node. Everything in the feature branch is platform-independent, so no
source edits are needed on Windows — you only need to build it natively.

> **Why native?** Blender cannot be cross-compiled from macOS/Linux to Windows. The macOS build I
> produced cannot be turned into a `blender.exe`; the build below must run **on a Windows machine**.
> The one Windows-specific detail (`M_PI` needs `_USE_MATH_DEFINES` under MSVC) is already handled
> in `extern/isotropicremesher/CMakeLists.txt`, so a clean build "just works".

---

## 1. Prerequisites

Install these first (all free):

| Tool | Notes |
|------|-------|
| **Windows 10 or 11, 64-bit** | x64 (or ARM64, see §7). |
| **Visual Studio 2022** (Community is fine) | In the VS Installer, select the **"Desktop development with C++"** workload. This must include: **MSVC v143 build tools**, the **Windows 11 SDK** (or Windows 10 SDK), and **C++ CMake tools for Windows**. |
| **Git** | https://git-scm.com — during install, enable **Git LFS** (or `git lfs install` afterwards). |
| **CMake ≥ 3.10** | Bundled with VS, or install standalone and add to `PATH`. |
| **Python 3** | https://www.python.org — needed by Blender's `make update` script. Tick "Add Python to PATH". |
| **~40 GB free disk** | ~10 GB source + libraries, ~15-25 GB build output. |

Open a **"x64 Native Tools Command Prompt for VS 2022"** (Start menu → search it) for all commands
below — it puts MSVC on `PATH`.

---

## 2. Get the source at the right revision

You have two options. **Option A** (recommended) uses my pushed fork directly.

### Option A — clone the fork with the node already on it
```bat
cd %USERPROFILE%
git clone https://github.com/ustacode/blender.git
cd blender
git checkout isotropic-remesh-node
```

### Option B — start from the official 5.0 release and apply the change
```bat
cd %USERPROFILE%
git clone https://projects.blender.org/blender/blender.git
cd blender
git checkout blender-v5.0-release
:: then add the fork and pull just the feature branch:
git remote add fork https://github.com/ustacode/blender.git
git fetch fork isotropic-remesh-node
git checkout isotropic-remesh-node
```

Either way, confirm you are on the branch and at Blender 5.0:
```bat
git branch --show-current
type source\blender\blenkernel\BKE_blender_version.h | findstr BLENDER_VERSION
```

---

## 3. Fetch the precompiled Windows libraries

Blender does **not** build its heavy dependencies from source; it downloads precompiled ones. This
step pulls the `lib/windows_x64` set (several GB, via Git LFS) matched to the 5.0 branch:

```bat
make update
```

`make.bat` lives in the repo root; `make update` runs `build_files/utils/make_update.py`, which
selects the correct library set for your platform automatically. If it complains it can't find
`svn`/`git`, make sure Git (with LFS) is on `PATH` and re-run.

*(If you only want the libraries and not a Blender source pull, `python build_files\utils\make_update.py --no-blender` does the same thing as on macOS.)*

---

## 4. Configure and build

The simplest path uses the `make.bat` wrapper, which configures CMake and builds a Release Blender:

```bat
make
```

This writes to `..\build_windows_x64_vc17_Release\` and produces
`..\build_windows_x64_vc17_Release\bin\Release\blender.exe`.

**Manual CMake** (equivalent, if you prefer to control it or open the solution in the VS IDE):
```bat
cmake -S . -B ..\build_windows -G "Visual Studio 17 2022" -A x64
cmake --build ..\build_windows --config Release --target INSTALL
```
Open `..\build_windows\Blender.sln` in Visual Studio if you want to build/debug from the IDE
(set the **INSTALL** project as startup, configuration **Release**).

A full build takes roughly 30-90 minutes depending on the machine and core count.

---

## 5. Run and verify the node

```bat
..\build_windows_x64_vc17_Release\bin\Release\blender.exe
```
In Blender: create any mesh → add a **Geometry Nodes** modifier → **Add ▸ Mesh ▸ Operations ▸
Isotropic Remesh**. Wire it between Group Input and Group Output.

Headless smoke test (no window):
```bat
blender.exe --background --factory-startup --python-expr "import bpy; print('OK' if hasattr(bpy.types,'GeometryNodeIsotropicRemesh') else 'MISSING')"
```
It should print `OK`.

---

## 6. Build and run the regression tests (optional)

```bat
cmake -S . -B ..\build_windows -DWITH_GTESTS=ON
cmake --build ..\build_windows --config Release --target blender_test
..\build_windows\bin\tests\blender_test.exe --gtest_filter=IsotropicRemeshTest.*
```
Expected: `[  PASSED  ] 7 tests.` (manifold, refinement, boundary, disconnected, degenerate,
attribute transfer, performance/cancellation).

---

## 7. Windows on ARM (ARM64)

The `.gitmodules` in this branch already references `lib/windows_arm64`. On an ARM64 machine, use
the ARM toolchain:
```bat
cmake -S . -B ..\build_windows_arm -G "Visual Studio 17 2022" -A ARM64
cmake --build ..\build_windows_arm --config Release --target INSTALL
```
`make update` fetches the ARM64 libraries automatically when run on an ARM64 host.

---

## 8. Troubleshooting

| Symptom | Fix |
|---------|-----|
| `make` not recognized | Run it from the repo root in the **x64 Native Tools Command Prompt**; the file is `make.bat`. |
| CMake can't find a compiler | You opened a plain `cmd`. Use the **x64 Native Tools Command Prompt for VS 2022**, or pass `-G "Visual Studio 17 2022"`. |
| `make update` fails on LFS | `git lfs install` once, then re-run. Check you have disk space and network for the multi-GB pull. |
| Link errors mentioning `extern_isotropicremesher` | Ensure `make update` completed and you did a clean configure (delete the build dir and re-run). |
| `M_PI` undefined | Should not happen — it's guarded and `_USE_MATH_DEFINES` is set for Windows in `extern/isotropicremesher/CMakeLists.txt`. If you see it, you're not on the `isotropic-remesh-node` branch. |
| Blender crashes at startup after adding your own node later | A new node type also needs a `define("GeometryNode", "GeometryNode<Name>")` line in `source/blender/makesrna/intern/rna_nodetree.cc`, or `node_type_base` dereferences a null RNA struct. (Already done for Isotropic Remesh.) |
| Out of disk during build | Release build output is large; free up space or point the build dir to another drive. |

---

## 9. What the branch changes (all portable, nothing Windows-specific to edit)

- `extern/isotropicremesher/` — vendored MIT remesher + CMake (`_USE_MATH_DEFINES` on Windows).
- `source/blender/geometry/{GEO_isotropic_remesh.hh, intern/isotropic_remesh.cc}` — reusable
  `blender::geometry::isotropic_remesh()` (Mesh ↔ algorithm + attribute resampling).
- `source/blender/nodes/geometry/nodes/node_geo_isotropic_remesh.cc` — the node.
- `source/blender/geometry/tests/GEO_isotropic_remesh_test.cc` — tests.
- Registration/wiring: `extern/CMakeLists.txt`, `BKE_node_legacy_types.hh`, `rna_nodetree.cc`,
  the two geometry `CMakeLists.txt`, and `node_add_menu_geometry.py`.

None of these require changes on Windows — build straight from the branch.

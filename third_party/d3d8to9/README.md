# d3d8to9 (vendored)

Direct3D 8 → Direct3D 9 translation layer by Patrick Mours.

- Upstream: https://github.com/crosire/d3d8to9
- Commit: `255338f698c8270b537f0a91a13f795f4f988250` (2026-09-24, "Fix build with Visual Studio 2026")
- License: BSD-2-Clause, see [LICENSE.md](LICENSE.md)

`source/` is an **unmodified** copy of upstream's `source/` directory. Only the
build integration is ours ([CMakeLists.txt](CMakeLists.txt)): it compiles the
sources into a static library that RetroShim links. RetroShim's `Direct3DCreate8`
hook calls into it when the game is launched with `--d3d8to9`.

Integration notes:

- Upstream's own log file (`d3d8.log` in the game folder) is disabled with
  `D3D8TO9NOLOG`. RetroShim logs the bridge instead.
- Upstream calls `Direct3DCreate9` directly. RetroShim provides that symbol as a
  lazy forwarder to `d3d9.dll` (see `src/shim/gfx/D3D8.cpp`), so the shim never
  imports `d3d9.dll` statically, and the call goes through RetroShim's Direct3D 9
  hooks. Those handle containment, pacing and `--d3d9on12`.
- D3D8 shaders are translated with D3DX (`d3dx9_43.dll`, from the DirectX End-User
  Runtime). Without it, upstream shows a message box, and games using shaders
  won't render correctly.

To update: replace `source/` with a newer upstream `source/`, update the commit
above, then rebuild and run `ctest`.

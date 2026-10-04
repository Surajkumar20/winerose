# Winerose

An original wavetable/hybrid synth (JUCE 8, C++20) that loads Serum 2 / Serum 1 presets.
Spec: the PDF in this repo. Architecture and branch plan: [PLAN.md](PLAN.md).

## Layout

| Path | Target | JUCE? |
|---|---|---|
| `src/params`  | `winerose_params`  (ConfigManager, ParamRegistry, ParamHandle, …) | no |
| `src/engine`  | `winerose_engine`  (the synth) | no |
| `src/presets` | `winerose_presets` (preset / wavetable I/O, Serum compat) | no |
| `src/control` | `winerose_control` (IController, the API every UI uses) | no |
| `src/ui`      | `winerose_ui_juce` (JUCE editor; talks to IController only) | yes |
| `src/plugin`  | `Winerose` (VST3 / CLAP / Standalone host adapter) | yes |
| `tests/unit`  | `winerose_tests` (Catch2) | no |

The layer rules are checked at configure time (`cmake/WineroseLayering.cmake`).

## Build (Windows, Visual Studio 2026)

CMake 4.0 has no VS 2026 generator, so use the CMake bundled with Visual Studio. "Developer PowerShell for
VS" puts it on PATH, or call it directly:

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build/vs -G "Visual Studio 18 2026" -A x64
& $cmake --build build/vs --config Release --parallel
.\build\vs\tests\Release\winerose_tests.exe
```

Artefacts are written to `build/vs/src/plugin/Winerose_artefacts/Release/` (`VST3/Winerose.vst3`,
`CLAP/Winerose.clap`, `Standalone/Winerose.exe`). To use the plugin in FL Studio, copy `Winerose.vst3` to
`C:\Program Files\Common Files\VST3\`, then go to Options → Manage plugins → Find more plugins.

Core layers only, with no JUCE download:

```powershell
& $cmake -S . -B build/core -G "Visual Studio 18 2026" -A x64 -DWINEROSE_BUILD_PLUGIN=OFF
```

Options: `WINEROSE_BUILD_PLUGIN` (ON), `WINEROSE_BUILD_CLAP` (ON), `WINEROSE_BUILD_TESTS` (ON).

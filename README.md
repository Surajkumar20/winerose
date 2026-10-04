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
| `libs/`       | vendored `hiir` (oversampling) and `pffft` (FFT) | no |
| `tools/render`       | `winerose_render` (offline engine host, WAV I/O) | no |
| `tools/param_schema` | dumps every module's schema (INI + JSON) and the default state | no |
| `tools/preset_dump`  | inspects preset containers | no |
| `tools/measure_host` | hosts a VST3 (e.g. Serum 2) to list/sweep parameters and save state | yes |
| `tests/`      | unit, golden and layering-checker tests | no |

The layer rules are checked at configure time (`cmake/WineroseLayering.cmake`).

## Build (Windows)

One command configures, builds and runs every test, including pluginval at strictness 10:

```powershell
.\scripts\build.ps1                               # preset vs2026, Release
.\scripts\build.ps1 -Config Debug -SkipPluginval
.\scripts\build.ps1 -Preset vs2026-core           # JUCE-free layers only, no JUCE download
.\scripts\build.ps1 -Preset vs2022 -Clean
```

The script picks a CMake new enough for the preset. CMake 4.0 has no Visual Studio 2026 generator, so for
`vs2026` it uses the CMake bundled with Visual Studio. pluginval is downloaded once into `build\tools\`.

Presets (`CMakePresets.json`): `vs2026`, `vs2026-core`, `vs2022`, `ninja`, `ninja-core`. Each builds into
`build/<preset>/`. Without the script (using a CMake that supports the generator):

```powershell
cmake --preset vs2026
cmake --build build/vs2026 --config Release --parallel
ctest --test-dir build/vs2026 -C Release --output-on-failure
```

Artefacts are written to `build/<preset>/src/plugin/Winerose_artefacts/Release/` (`VST3/Winerose.vst3`,
`CLAP/Winerose.clap`, `Standalone/Winerose.exe`). To use the plugin in FL Studio, copy `Winerose.vst3` to
`C:\Program Files\Common Files\VST3\`, then go to Options → Manage plugins → Find more plugins.

Options: `WINEROSE_BUILD_PLUGIN`, `WINEROSE_BUILD_CLAP`, `WINEROSE_BUILD_TESTS`, `WINEROSE_BUILD_TOOLS`
(all ON), and `WINEROSE_PLUGINVAL` (a path; adds the pluginval ctest).

## Golden renders

`tests/golden/data/*.wav` hold reference engine output. A test fails if a render differs by more than
-100 dB RMS, and writes the new output to `build/<preset>/golden-actual/`. When a sound change is
intentional, accept it with:

```powershell
$env:WINEROSE_UPDATE_GOLDEN = "1"
.\build\vs2026\tests\Release\winerose_golden_tests.exe
Remove-Item Env:WINEROSE_UPDATE_GOLDEN
```

Commit the updated WAVs together with the change that caused them.

## Measuring Serum 2 (black-box)

```powershell
$mh = ".\build\vs2026\tools\measure_host\measure_host_artefacts\Release\measure_host.exe"
$serum = "C:\Program Files\Common Files\VST3\Serum2.vst3"
& $mh list  $serum --out measure\serum2_params.csv
& $mh sweep $serum --steps 1025 --out measure\serum2_sweep.csv
& $mh state $serum --set "<parameter name>=0.5" --out measure\state_a.bin   # name, host ID or index
```

`state` writes the plugin's own state chunk, unwrapped from JUCE's host XML. To map state keys to
parameters (SPEC §2.3, §5.7), diff two chunks that differ in one parameter. Use it only with your own
licensed copy.

# Winerose — Implementation Plan

Source of truth for *what* to build: `Rebuilding Serum 2_ Technical Analysis and Claude Code Spec for a
Preset-Compatible JUCE Synth.pdf` (Part 5 = SPEC; read "Tincture" as **Winerose**, PLUGIN_CODE `Wnrs`).
This file settles *how*:
- a **modular, layered architecture**: the engine runs without JUCE, and the UI is replaceable (e.g. Electron later);
- the **parameter architecture** (ParamRegistry + ConfigManager, adapted from `example-paramregistry.txt`,
  `example-configmanager.h` and `example-designdecisionsandplanning.txt`);
- how the work is split across the branches.

---

## 1. Modularity — the governing rule

> Every layer talks only to the layer below it through a plain C++ interface. JUCE appears only in the
> outermost adapters. A future Electron (or any other) UI replaces one adapter and touches nothing else.

### 1.1 Layers (each one is its own CMake static-library target)

```
                ┌──────────────────────┐   ┌────────────────────────┐   ┌──────────────────────────┐
  adapters      │ winerose_plugin      │   │ winerose_ui_juce       │   │ (future) winerose_bridge │
  (JUCE / web)  │ AudioProcessor, VST3 │   │ JUCE editor components │   │ WebSocket/IPC ⇄ Electron │
                └──────────┬───────────┘   └───────────┬────────────┘   └────────────┬─────────────┘
                           │                           │                             │
                ┌──────────▼───────────────────────────▼─────────────────────────────▼─────────────┐
  control API   │ winerose_control   — IController: schema, get/set/modify, gestures, presets,      │
  (UI-agnostic) │                      change events, meters/scope FIFOs, undo/redo.  JSON-able.   │
                └──────────┬───────────────────────────────────────────┬──────────────────────────┘
                           │                                           │
                ┌──────────▼───────────┐                   ┌───────────▼───────────┐
  domain        │ winerose_engine      │                   │ winerose_presets      │
                │ voices, osc, filter, │                   │ .SerumPreset / .fxp   │
                │ mod, seq, fx         │                   │ readers, mappers, own │
                │                      │                   │ format, AssetResolver │
                └──────────┬───────────┘                   └───────────┬───────────┘
                           │                                           │
                ┌──────────▼───────────────────────────────────────────▼───────────┐
  core          │ winerose_params — ConfigManager, ParamRegistry, ParamHandle,      │
                │                   EngineSnapshot, GlobalSettings                  │
                └───────────────────────────────────────────────────────────────────┘
```

| Target | May link | Must NOT link | Third-party |
|---|---|---|---|
| `winerose_params` | std only | JUCE, engine, presets | nlohmann_json (state serialization) |
| `winerose_engine` | params | JUCE, presets, control | xsimd, pffft, HIIR |
| `winerose_presets` | params | JUCE, engine DSP, control | zstd, zlib, nlohmann_json |
| `winerose_control` | params, engine, presets | JUCE | nlohmann_json |
| `winerose_plugin` | control (and transitively the rest) | `winerose_ui_juce` internals | JUCE, clap-juce-extensions |
| `winerose_ui_juce` | control **only** | engine, presets, params internals | JUCE |

The "must not link" column is enforced: `tests/arch/` contains a CMake check that fails the build if a
forbidden target or a `juce_*` target shows up in a lower layer's `LINK_LIBRARIES`. A CI grep also rejects
`#include <juce_` under `src/params|engine|presets|control`.

### 1.2 Consequences for SPEC items that assumed JUCE inside the engine

| SPEC assumed | Winerose uses (engine layer) |
|---|---|
| `juce::dsp::FFT` | pffft (already in SPEC's library list) |
| `juce::MPEInstrument` | own `MpeState` in `engine/` (per-channel pitch bend, pressure and timbre → voice) |
| `juce::ThreadPool` for asset/mipmap builds | `engine/WorkerPool` (std::thread + lock-free job queue) |
| `juce::ScopedNoDenormals` | `engine/Denormals.h` (FTZ/DAZ via MXCSR; ARM FPCR on macOS) |
| `juce::GZIPDecompressorInputStream` | zlib, linked directly into `winerose_presets` |
| APVTS as the parameter store | `winerose_params` owns the values; the plugin adapter exposes them to the host (§3.4) |
| JUCE `UndoManager` | `control/EditHistory` (works the same for the JUCE and Electron UIs) |

### 1.3 Engine API (what every host adapter calls)

```cpp
class Engine {                                                 // winerose_engine — no JUCE types
public:
    explicit Engine(std::shared_ptr<ConfigManager>);           // modules register their ParamRegistrys here
    void prepare(double sampleRate, int maxBlockSize);         // allocates pools; non-realtime
    void process(float* const* out, int numChannels, int numSamples,
                 const MidiEvent* events, int numEvents,        // sample-offset-stamped, POD
                 const TransportInfo& transport) noexcept;      // bpm, ppq, playing — POD
    void drainMidiOut(MidiEventSink&) noexcept;                // arp / clip MIDI out
    int  latencySamples() const noexcept;
    void reset() noexcept;
};
```

Because the API uses only POD types, the same engine can run inside the VST3/CLAP plugin, a standalone app,
a Node native addon (Electron standalone), an offline renderer (golden tests, `measure_host`), or a fuzz
harness.

### 1.4 Control API (what every UI calls)

```cpp
class IController {                                           // winerose_control — no JUCE types
public:
    virtual std::vector<ParamSchema> schema() const = 0;      // from every registry's getAll()
    virtual ParamValue get(std::string_view nsKey) const = 0; // "Filter0.cutoff"
    virtual bool set(std::string_view nsKey, const ParamValue&) = 0;
    virtual bool modify(std::string_view nsKey, std::string_view text) = 0;   // → ParamRegistry::modify
    virtual void beginGesture(std::string_view nsKey) = 0;    // host automation write grouping
    virtual void endGesture(std::string_view nsKey) = 0;
    virtual Result loadPreset(std::span<const uint8_t>, std::string_view hint) = 0;
    virtual std::vector<uint8_t> savePreset() const = 0;      // own format
    virtual bool undo() = 0;  virtual bool redo() = 0;
    virtual Subscription onChange(std::function<void(const ParamChange&)>) = 0;  // message thread
    virtual MeterReader meters() = 0;                         // lock-free SPSC: levels, scope, wavetable pos
};
```

Every type above has a `to_json`/`from_json` (nlohmann). That one decision is what lets Electron plug in
later: `winerose_bridge` is a thin JSON-RPC-over-WebSocket (or Node addon) shim that maps 1:1 onto
`IController`, and the JUCE editor calls the same interface in-process.

> **Decision: follow the Serum / Analog Lab model.** The full UI is drawn *inside the DAW's plugin window*,
> and the same UI also ships as a standalone app. There is no separate companion window and no second
> process for the plugin.
> - **Now:** the UI is native JUCE (`winerose_ui_juce`), resizable and vector-drawn, in the plugin editor.
>   The JUCE Standalone wrapper provides the standalone app.
> - **Later (modern web UI):** the editor becomes a JUCE 8 `WebBrowserComponent` (WebView2 on Windows,
>   WKWebView on macOS) hosting an HTML/JS front end *inside the plugin window*, calling `IController` through
>   JUCE's native-function bridge with the same JSON types. The same front-end bundle can also run in an
>   Electron **standalone** app via `winerose_bridge` (Node addon linking `winerose_control` directly).
>   Electron is never used inside the DAW, because a plugin window can't host it.
> - Either way, the only code that changes is `WineroseProcessor::createEditor()` plus the new UI target.
>   Nothing below `winerose_control` changes. SPEC's `JUCE_WEB_BROWSER=0` stays until the web UI starts.

---

## 2. Branch split

All branches currently sit at `d8f58e2 starting point`. Each branch rebases onto `master` after the branches
it depends on have merged.

| # | Branch | Owns (CMake targets) | SPEC refs | Depends on | Done when |
|---|--------|------|-----------|-----------|-----------|
| 1 | `VST_cmake_config` | Root `CMakeLists.txt` (FetchContent pins from §5.2) and the **layer skeleton**: every target in §1.1 exists with its own `src/<layer>/CMakeLists.txt`. **`winerose_params` in full** (§3). `winerose_control` with `IController` + a minimal `Controller`. `winerose_plugin`: `juce_add_plugin` (VST3 + Standalone, CLAP), host-param adapter (§3.4), state save/load. A stub `Engine` that outputs silence, plus one `Global.masterVolume` param proving the path end-to-end. | 5.1, 5.2, 5.3, 5.9 prompt 1 | — | Builds with MSVC 2022 x64; loads in FL Studio; pluginval strictness 10; state round-trips; layering check passes. |
| 2 | `feature/build` | `CMakePresets.json`, `scripts/build.ps1`, Catch2 targets (`tests/unit`, `tests/golden`, `tests/arch`), offline render harness (links `winerose_engine` directly, no JUCE), pluginval invocation, Windows CI, vendored `libs/` (HIIR, pffft), `tools/param_schema`, `tools/measure_host`, `tools/preset_dump`. | 5.7, 5.8 | 1 | One script runs configure, build, ctest and pluginval locally and in CI; `param_schema` emits `params/<module>/schema.ini`. |
| 3 | `feature/audio_engine` | `winerose_engine`: `engine/`, `osc/`, `filter/`, `mod/`, `seq/`, `fx/`. One sub-PR per SPEC phase (1–5, then the phase 7 oscillators and seq). Every module owns a `ParamRegistry` and a `registerParams()`. | Phases 1–5, 7; 5.5 | 1, 2 | SPEC 5.4 acceptance per phase; golden renders pass headless. |
| 4 | `feature/presets` | `winerose_presets`: `io/` (WavetableWav clm, SerumPresetReader, FxpReader, Cbor, Md5, AssetResolver, own format) and `compat/` (serum_tables.hpp, Serum2Mapper, Serum1Mapper, MigrationS1toS2). Plus `IController::loadPreset/savePreset`. | Phase 6; 5.6; Part 2 | 1, 2 — and the registry names/keys from 3 for the mapping tables | 100% of the corpus parses; ≥90% of mapped params within tolerance; Serum import → own export → import gives an identical PatchModel. |
| 5 | `feature/UI` | `winerose_ui_juce`: the original editor, the generic **ParamTableView** (double-click-to-edit + floating Save, same UX as the example's TabParamConfigView, calling `IController::modify`), knobs bound by namespaced key, preset browser, undo/redo buttons. Talks to `IController` **only**. | Phase 8 | 1 (+ 3/4 for real content) | 0 pluginval warnings; FL state recall; `winerose_ui_juce` links nothing but control + JUCE. |

Parallelism: after 1 and 2 merge, branches 3, 4 and 5 can proceed at the same time. They meet only at
registry names/keys (owned by 3) and `IController` (owned by 1). Neither is expected to change much once
set, and changes go through `master`.

`wavetable clm` I/O lives in `feature/presets` (it's file I/O), but Phase 1 of the engine needs it. Land it
in presets first as a small early PR, or let the engine's Phase 1 tests generate tables in code until it
does.

---

## 3. Parameter architecture (ParamRegistry + ConfigManager, JUCE-free)

### 3.1 Carried over unchanged from the examples

- **ConfigManager is the only class that serializes state.** ParamRegistry never does I/O and holds a
  `std::shared_ptr<ConfigManager>` (composition, not inheritance).
- **ParamRegistry is per-instance, not a singleton.** Its constructor claims a collision-free name via
  `attachParamRegistry()` before any key is registered; the destructor detaches.
- **Namespaced key = `resolvedName + "." + key`.** The key doubles as the display label.
- **Hydrate at registration** (stored value, else default; write it back; record the *original* default).
- `register{Int,Float,Double,Bool,Enum,String,Duration}` keep the example signatures (default, min, max), as
  do `get<T>/set<T>/modify/getAll/validate/clampAndApply/beginBatch/endBatch`.
- `writeParamSchema(resolvedName, defs)` uses the same INI schema format.
- Lock discipline: ParamRegistry never holds its own mutex while calling into ConfigManager.

### 3.2 Adapted for a plugin

| Example (desktop app) | Winerose | Why |
|---|---|---|
| One ConfigManager per **process** | One per **Engine instance** (the plugin adapter creates one per processor) | FL loads many instances in one process. A process-wide registry would rename instance 2's params and break its automation. |
| One ParamRegistry per **plugin** | One per **engine module**, named after the Serum 2 CBOR module keys: `Oscillator0..4`, `Filter0..1`, `Env0..3`, `LFO0..9`, `Macro0..7`, `ModSlot0..63`, `RoutingSlot*`, `FXRack0..2`, `Arp0`, `Global` | Mirrors the `.SerumPreset` layout (module → `plainParams`), so presets map module-to-registry 1:1 (§3.6). Collision resolution stays as a safety net. |
| Backing store = `app.ini` map, disk write per `set` | Backing store = ConfigManager's own **value slots**: one `std::atomic<float>` per numeric/enum/bool param (plain value) + a PatchModel tree (strings, curves, tables, clips, matrix topology). Serialized to the own format (nlohmann JSON/CBOR) when the host asks for state | JUCE-free, realtime-readable, and the host owns persistence. |
| `beginBatch/endBatch` = one disk save | = one **EngineSnapshot** rebuild + one batched change notification | Preset load touches ~1,500 params. |
| `get<T>` from anywhere | `get/set/modify` are **non-realtime only** (debug assert). The audio thread reads `ParamHandle` (§3.5) | SPEC 5.0: no locks or allocation on the audio thread. |
| `writeParamSchema` at every init | Only `tools/param_schema` calls it | No file I/O while a DAW scans the plugin. |
| — | **`GlobalSettings`**: the example's write-through INI ConfigManager, almost verbatim, holding machine-level settings (Serum content folders, default quality, UI scale) | These belong to the machine, not to one patch. |
| — | **`IParamListener`** on ConfigManager (`onParamChanged(nsKey, plain)`, `onBatchEnd()`) | How the plugin adapter, control layer and UIs hear about changes without the core knowing they exist. |

### 3.3 ParamDef extensions

```cpp
struct NumericMeta {
    enum class SubType { INT, FLOAT, DOUBLE } subtype;
    double min_val, max_val;
    enum class Curve { Linear, Exp, Power } curve = Curve::Linear;  // NEW
    double curve_param = 1.0;                                        // NEW — Power exponent
    std::string unit;                                                 // NEW — "Hz", "s", "st", "%", "dB"
};
struct ParamDef {
    std::string key, group, tooltip, default_value;
    ParamMeta   meta;
    bool automatable = true;    // NEW — exposed to the host by the plugin adapter
    int  vst3_id     = -1;      // NEW — SPEC 5.3 ID blocks
};
// ParamRegistry gains toNormalized()/fromNormalized() per key — the curve math lives in the core,
// so the JUCE adapter, a web UI and the preset mappers all agree on it.
```

Registration takes an optional trailing `ParamOpts { curve, curve_param, unit, automatable, vst3_id }`.
**Stored values are plain (real-unit)**, matching Serum 2 `plainParams`. Normalization happens only at
host/UI edges.

### 3.4 Plugin adapter (`winerose_plugin`, the only JUCE ↔ params seam)

```
WineroseProcessor()
  1. m_config = std::make_shared<ConfigManager>();
  2. m_engine = std::make_unique<Engine>(m_config);   // every module registers; all 10 LFOs / 64 slots up front
  3. m_controller = std::make_unique<Controller>(*m_engine, m_config);
  4. for each registry, for each automatable def:     // param set is fixed at construction, as hosts require
         addParameter(new RegistryParameter(*reg, def)); // juce::AudioProcessorParameterWithID wrapping one slot
  5. m_config->addListener(&m_hostSync);              // UI/preset change → setValueNotifyingHost

RegistryParameter::getValue()/setValue()  → reg.toNormalized/fromNormalized on the core atomic (realtime-safe)
prepareToPlay  → m_engine->prepare(sr, block)
processBlock   → convert juce::MidiBuffer → MidiEvent[]; m_engine->process(...)
get/setStateInformation → m_config->serialize() / restore() → validate() → clampAndApply()
createEditor   → WineroseEditor(*m_controller)        // swap this line for a WebBrowserComponent editor later
```

Host parameter IDs = namespaced keys (`Filter0.cutoff`). JUCE derives each VST3 numeric ID from a hash of
that string, so the numeric ID is stable as long as the key is. `vst3_id` is metadata only: the Serum 2 ID,
used by `compat/` and `measure_host`.
**Both are permanent once shipped.** They are engine names, never Serum's `kParam*` names; that mapping lives
in `compat/`.

### 3.5 Audio-thread access

```cpp
struct ParamHandle {                       // trivially copyable; no strings, no locks
    const std::atomic<float>* slot = nullptr;
    float load() const noexcept { return slot->load(std::memory_order_relaxed); }
};
ParamHandle ParamRegistry::handle(const std::string& key) const;   // resolved once in prepare()
```

DSP wraps handles in `Smoother` (5–20 ms). Non-automatable state goes into an immutable **EngineSnapshot**,
which ConfigManager rebuilds on change (coalesced per batch) and publishes via an atomic pointer. The audio
thread adopts it at block start, and old snapshots retire through a lock-free FIFO (SPEC 5.3).

### 3.6 Write paths

```
Host automation  → RegistryParameter::setValue → core atomic                     (audio or host thread)
JUCE knob / web  → IController::beginGesture/set/endGesture → ParamRegistry::set → ConfigManager
                     → slot/PatchModel → IParamListener → host notified, other UIs refreshed, EditHistory
ParamTableView   → IController::modify(nsKey, text) → ParamRegistry::modify → (as above)
Preset import    → cm.beginBatch();
                     for (module, plainParams) in CBOR root:
                         reg = cm.find(module)                                    // 1:1 by design
                         for (kParam, v): m = serum_tables::lookup(module, kParam)
                             m ? reg->set<T>(m->key, m->transform(v)) : patch.unknown[module][kParam] = v;
                   cm.endBatch();                                                 // one snapshot, one notify
plainParams == "default" → reg->resetToDefaults()
.fxp             → Serum1Mapper → MigrationS1toS2 → the same Serum-2-shaped path
```

### 3.7 Threading

| Lock | Guards | Taken by |
|---|---|---|
| `ConfigManager::m_mutex_` | PatchModel, key→slot index | message/UI thread |
| `ConfigManager::m_registry_mutex_` | attached-registry map | construction, tools |
| `ParamRegistry::m_mutex_` | its `m_params_` | message thread; never held across ConfigManager calls |
| *(none)* | value-slot atomics, EngineSnapshot pointer, meter FIFOs | audio thread |

---

## 4. File layout (deltas vs SPEC 5.1)

```
src/params/     ParamTypes ConfigManager ParamRegistry ParamHandle.h ParamListener.h Realtime.h
                SnapshotExchange.h GlobalSettings                             → winerose_params
                (each layer: src/<layer>/include/<layer>/*.h + src/<layer>/src/*.cpp)
src/engine/     Engine EngineTypes.h Smoother.h (+ EngineSnapshot); osc/ filter/ mod/ seq/ fx/ → winerose_engine
src/io/ compat/                                                              → winerose_presets
src/control/    IController.h Controller.{h,cpp} EditHistory.{h,cpp} Json.h  → winerose_control
src/plugin/     WineroseProcessor RegistryParameter HostSync                 → winerose_plugin (JUCE)
src/ui/         WineroseEditor ParamTableView …                              → winerose_ui_juce (JUCE)
src/bridge/     (future) JSON-RPC/WebSocket or Node addon over IController   → winerose_bridge
tests/unit tests/golden tests/arch      tools/param_schema tools/measure_host tools/preset_dump
```

---

## 5. Verification per branch

1. **VST_cmake_config:** pluginval strictness 10. Two FL instances keep independent `Global.*` values across
   save/reload. Automation on `Global.masterVolume` survives reload. The `tests/arch` layering check passes.
   `winerose_params` builds and unit-tests with JUCE absent.
2. **feature/build:** clean `scripts/build.ps1` run (configure, build, ctest, pluginval). Headless golden
   render links only `winerose_engine`. `param_schema` output is deterministic.
3. **feature/audio_engine:** SPEC 5.4 per phase. Debug assert proves no `get/set/modify` call from
   `process()`.
4. **feature/presets:** corpus parse rate; mapping tolerance; lossless round trip; fuzzed container
   parsing.
5. **feature/UI:** an edit in ParamTableView shows up in the host automation lane; undo reverts it;
   `winerose_ui_juce` links only control + JUCE.

---

## 6. As built on `VST_cmake_config`

Where the code differs from, or adds detail to, the sections above:

- **Toolchain:** Visual Studio 2026 (MSVC 14.51), using the CMake 4.3 bundled with VS (generator
  `Visual Studio 18 2026`); CMake 4.0 has no VS 2026 generator. JUCE **8.0.15** (the SPEC's major version;
  9.0.3 also exists). clap-juce-extensions is pinned to commit `55525c9` because the 0.26.0 tag doesn't
  compile against JUCE ≥ 8.0.11. Static MSVC runtime.
- **`WINEROSE_BUILD_PLUGIN=OFF`** builds params/engine/presets/control + tests without fetching JUCE. This is
  the standing proof that those layers don't depend on JUCE.
- **Layering enforcement** is `cmake/WineroseLayering.cmake`, run at configure time: link-graph rules plus an
  `#include` scan for each layer. It covers what PLAN.md §2 listed under `tests/arch`.
- **`winerose_ui_juce` is an INTERFACE library**, compiled inside the plugin target. A static library that
  links JUCE modules would compile them twice. Its "control only" rule is enforced by the link rule and the
  include scan.
- **Generic pieces live in params, engine-specific ones in engine:** `SnapshotExchange<T>` (lock-free
  publish/retire) is in params, and `EngineSnapshot` is in engine.
- **Host automation path:** `RegistryParameter::setValue` stores into the ConfigManager slot. A 30 Hz
  `HostSync` timer turns that into `ConfigManager::notifyChanged` (never echoed back to the host). Control-layer
  changes are pushed with `setValueNotifyingHost` under `ScopedOwnWrite`, so the exact plain value is never
  re-derived from a normalized float.
- **Batches nest** (a depth counter), unlike the example's single flag. `Controller::loadState` = batch {reset
  every registry to defaults → `restore` → `clampAndApply`}, so keys missing from older states don't keep the
  previous patch's values.
- **Unit tests (Catch2) start here** for the core layers (43 cases). `feature/build` adds golden renders,
  pluginval scripting and CI.
- **Verified:** core-only and full builds with 0 warnings in Winerose code; all unit tests pass;
  `pluginval 1.0.4 --strictness-level 10` passes on the Release VST3. **Not yet verified:** loading in FL Studio.

## 7. As built on `feature/build`

- **`scripts/build.ps1`** (PowerShell 5.1+) runs configure → build → ctest, with pluginval as a ctest at
  strictness 10. It finds a CMake new enough for the preset (VS-bundled for `vs2026`) and downloads pluginval
  once into `build/tools/`.
- **`CMakePresets.json`**: `vs2026`, `vs2026-core`, `vs2022`, `ninja`, `ninja-core`, each building into
  `build/<preset>/`.
- **Tests (ctest):** `unit.*` (Catch2), `golden.*`, `arch.*` (the layering checker must reject direct,
  transitive and `#include` violations, and must accept clean graphs and `$<LINK_ONLY:>` deps), `tools.*`
  (smoke tests) and `plugin.pluginval`.
- **Golden renders:** `tools/render` (`winerose_render`, JUCE-free) drives the Engine exactly as a host does.
  `tests/golden` compares 44.1/48/96 kHz renders against `tests/golden/data/*.wav` at -100 dB RMS, and checks
  that output is identical for host block sizes 1…512. **Engine branches must keep block-size invariance**:
  run modulation on a fixed internal sub-block (SPEC §1.9), never per host block. Update goldens with
  `WINEROSE_UPDATE_GOLDEN=1`.
- **Vendored libs:** `libs/hiir` (WTFPL) and `libs/pffft` (FFTPACK BSD-like) as static targets, with smoke
  tests.
- **Tools:**
  - `param_schema` writes INI schemas, JSON schema and default state; output is deterministic (tested).
  - `preset_dump` shows the container, the `.SerumPreset` header/metadata and the `.fxp`/`.fxb` header.
    `feature/presets` extends it with CBOR decoding.
  - `measure_host` provides `list` / `sweep` / `state`. `state` unwraps JUCE's `VST3PluginState` XML to the
    plugin's raw component chunk. It is tested end to end against Winerose's own VST3: a value set through
    the VST3 interface appears in the saved state. Probe renders (SPEC §5.7 step 4) come once the engine
    makes sound.
- **CI** (`.github/workflows/ci.yml`): Windows `vs2022` full build + pluginval with artefact upload, plus
  core-only Linux (gcc) and macOS (clang) jobs. **Not yet run:** the repo isn't pushed.
- **Portability check done locally:** every JUCE-free source compiles under GCC 13 with
  `-Wall -Wextra -Wpedantic` and no warnings. A full CMake+GCC build couldn't run on this machine (w64devkit
  links fail under CMake's compiler detection), so the Linux CI job is the real check.

## 8. As built on `feature/audio_engine`

### Phase 1 (SPEC §5.4): done

- **Modules** (`src/engine/.../modules`). Each owns a `ParamRegistry` named after its Serum 2 CBOR module:
  `Oscillator0` (enabled, level, pan, octave, semi, fine, wtPos, phase, random), `Filter0` (enabled,
  cutoff [8.18 Hz–22.05 kHz, exp], resonance) and `Env0` (attack/hold/decay/release 0–32 s on a cubic knob,
  sustain). `Global` adds `polyphony` (1–64, default 16). Defaults that still need measuring against Serum 2
  are tagged TODO-MEASURE in their tooltips.
- **DSP** (`dsp/`):
  - `WavetableBank`: 11 FFT-mipmapped levels (pffft), DC removed, 4-point Hermite, smooth frame morph.
  - `Svf`: TPT, Zavalishin/Simper.
  - `Envelope`: SPEC curve formula; attack and release start from the current level.
  - `RealFft`: wrapper over pffft.
  - Built-in "Basic Shapes" table: saw, square, triangle, sine (original content).
- **Level-selection deviation from SPEC §5.5:** `floor(log2(inc·2048)) + 1`, crossfading to the next level.
  The SPEC formula would put partials up to an octave above Nyquist. Measured worst alias for a C8 saw at
  48 kHz: **-147.6 dBFS** (criterion: < -90).
- **Voices:** fixed 64-voice pool. Stealing order is oldest released voice, then oldest voice; steals and
  retriggers don't click. Sustain pedal, CC120 and CC123 work. Velocity doesn't affect amplitude (Serum
  default). Start phases come from a seeded generator, so renders are deterministic.
- **Block-size independence:** `Engine::process` splits host blocks at MIDI events and at 32-sample
  boundaries of the absolute sample clock. Parameters are read only on those boundaries. The golden test
  confirms identical output for host blocks of 1–512 samples.
- **Wavetables reach the audio thread only through `EngineSnapshot`** (`Engine::setOscillatorTable`).
  Voices never cache table pointers across blocks.
- **Verified:** 84/84 ctest, including pluginval strictness 10 on the sounding plugin. **Not yet measured:**
  CPU, which is a Phase 2 criterion.

### Phase 2 (SPEC §5.4): done

- **Oscillators:** A/B/C (`Oscillator0..2`, B and C off in the init patch), noise (`Oscillator3`: built-in
  white/pink/brown loops, keytrack, pitch) and sub (`Oscillator4`: sine, rounded rect, triangle, saw,
  square, pulse; octave). Each WT oscillator has its own table in the snapshot
  (`Engine::setOscillatorTable(index, table)`).
- **Unison (per oscillator, up to 16):**
  - detune × range (0–48 st, default 2)
  - tuning modes: Linear, Super ^1.3, Exp ^2, Inv ^0.5, Random (per note)
  - blend: 0.75 = even; 0 = centre only; 1 = sides only; constant power
  - width, stack (12 1x, 12 2x, +7, +7+12, centre -12/-24)
  - span (WT-position spread), rand start (shared ↔ independent phases), warp spread
  - Span and rand-start meanings are INFERRED; the stack order is TODO-MEASURE.
- **Warps:** all Serum 1 modes from SPEC §1.2 plus FM from noise/sub, in two chained slots. FM/AM/RM take
  the paired oscillator (A←B, B←A, C←A). Remap 1/2 use an identity curve until drawable curves land
  (Phase 3). The menu order is TODO-MEASURE.
- **Oscillator options:** smooth/stepped frame interpolation, start phase with 100% = Mem, random phase.
- **Quality:** Good/High/Ultra = 1×/2×/4× oversampling of the oscillators, active only while a warp needs it,
  decimated with HIIR (12 coefficients, transition 0.04). For a sync warp, Ultra measures 14 dB less aliasing
  than Good. Hard-sync discontinuities limit what oversampling alone can do.
- **Filter is stereo now** (unison width). Routing is still "everything through Filter0 when enabled" until
  Phase 4.
- **CPU (Release, MSVC 14.51, this machine):**

  | Scenario | Load (% of one core) |
  |---|---|
  | 16 voices × 3 osc × 7 unison | **21.3%** (criterion < 25%) |
  | + filter | 21.8% |
  | + sync warp, Good | 32% |
  | + sync warp, Ultra | 116% |
  | 16 voices × 3 osc × 16 unison | 47% |

  `tests/benchmark` asserts the criterion in optimized builds (ctest label `benchmark`).
- **Performance note:** `WavetableBank::read` mixes frames and mip levels on the four input samples, then runs
  one Hermite; this is exact because Hermite is linear in its samples. Keep its scalar locals: an array-based
  version made MSVC stall on store forwarding and tripled the cost of the level crossfade.

### Phase 3 (SPEC §5.4): done

- **Modulation architecture:** every numeric module parameter is registered in `modulation::ModTargets`.
  At each control tick the Engine reads all of them into a base array. Each voice copies it, applies its
  matrix slots in normalized space (clamped), and rebuilds its settings through `EngineModules::build`.
  Modules read plain values from that array, so modulated and unmodulated values take the same code path.
- **Sources** (all normalized to 0..1; a slot's bipolar switch maps to -1..1): Env 1–4 (`Env0` is still
  amplitude), LFO 1–10, Macro 1–8, velocity, note, mod wheel (CC1), pitch bend, channel aftertouch, two
  per-note randoms, fixed.
- **Matrix** (`ModSlot0..63`):
  - Fields: source, destination (string, e.g. `"Filter0.cutoff"`), amount (-1..1, the only host-automatable
    field), bipolar, curve, aux, aux depth, aux invert, output scale, bypass.
  - Destinations are resolved to target indices in the `EngineSnapshot` whenever a string changes.
  - Macros are destinations too: slots into macros run first, so a macro feeding a macro reads the
    unmodulated value.
  - Aux deviates from the SPEC's literal `aux × auxAmt`: the factor is `1 - depth + depth·aux`, so depth 0
    means "no aux" instead of muting the slot.
- **LFOs** (`LFO0..9`):
  - Shapes: drawable Path (string curve, band-limited into a mip table), sine, triangle, saws, square, S&H,
    smooth random, Lorenz, Rössler (RK4, running min/max).
  - Modes: Free (one phase shared by all voices), Trig, Env (one-shot, then hold).
  - Rate: Hz (0.01 Hz–1 kHz) or tempo-synced, 8 bars to 1/64 with dotted/triplet.
  - Phase, delay, rise, smoothing.
  - Measured: worst alias of a 1 kHz saw LFO is -158.8 dB (criterion: alias-free).
- **Audio-rate path:** slots from a non-chaos LFO to oscillator coarse/fine/level/wavetable position or
  filter cutoff run per sample. A 1 kHz sine LFO on pitch produces clean ±1 kHz sidebands; the region where
  control-rate stepping would alias sits at -148 dB. All other slots run at the 32-sample control rate.
- **Also new:** `Oscillator*.coarse` (continuous ±48 st, the usual pitch-mod target); envelope curve
  parameters (attack/decay/release, -10..10); drawable `remapCurve` per oscillator (completes warps Remap 1/2);
  `Global.bendUp`/`bendDown` (default ±2 st).
- **Voice pool moved to the heap:** each voice now carries about 10 KB of modulation state.
- **CPU:** 16 voices × 3 osc × 7 unison = 23.8% of one core with the matrix active (Phase 2: 21.3%). Still
  under the 25% criterion, but the headroom is small; SIMD unison is the next lever.
- **Generic editor:** about 900 rows, 640 of them for the 64 matrix slots. It's usable with the scroller, but
  the real mod-matrix UI belongs to `feature/UI`.
- **Not yet:** BPM-synced envelopes, Start/End on envelopes 2–4, LFO loopback point (Env mode), poly
  aftertouch / MPE sources, and audio-rate oscillator/filter outputs as sources (Serum 2) — later phases.

### Phase 4 (SPEC §5.4): done

- **Enum ownership (applies project-wide):** every enum Winerose exposes (filter types, warps, unison stacks,
  sub shapes, LFO shapes, mod sources, …) is **our** list in **our** order; its index is what hosts store for
  automation. Serum's menus are mapped onto ours by `compat/` in `feature/presets`, so measuring Serum never
  renumbers a Winerose enum. Append new entries at the end only. The "TODO-MEASURE order" notes in earlier
  tooltips now mean "the compat mapping needs measuring", not "our order may change".
- **Filters:** `dsp::FilterUnit` (stereo, per voice) implements 59 types in 19 families (`FilterTypes.h`):
  - SVF 6/12/18/24 dB in LP/HP/BP/notch/peak/allpass, SVF morphs, dual SVF, linear ladder (6–24, HP)
  - driven ladder, acid, Sallen-Key (the nonlinear families, 2× oversampled with HIIR)
  - combs ±, damped combs, flanges, phasers (4/8/12, negative feedback), formant (male/female vowels)
  - sample & hold, ring mod, twin LP, FDN reverb, disperser, diffuser, PZ morph (X/Y)
- **Per-filter controls:** type, cutoff, resonance, drive (0..+24 dB), Clean (drive without the input
  saturator: linear types stay clean), var, X/Y, stereo (±½ octave L/R), mix, level, keytrack (100% = 1 oct/oct
  from C4), output (Main/Direct). `Filter1` has the same parameters as `Filter0`.
- **Routing:**
  - Every source (`Oscillator0..4`) has `route` (Filter/Main/Direct/None) and `filterBalance`. The defaults
    follow Serum: A → Filter, everything else → Main.
  - `Routing.filterRouting` sets serial (F1 → F2) or parallel. A disabled filter passes its signal through;
    bypassed filters are folded into the routing at control rate, so they cost nothing.
  - Voices render separate Main and Direct buses; the Phase 5 FX rack goes on Main.
- **Acceptance:**
  - Linear responses vs bilinear analytic: worst deviation **0.0013 dB** over 112 checks (criterion 0.5 dB).
  - Feedback comb vs `(1-|g|)/(1-g·z^-D)` exact.
  - All six nonlinear types self-oscillate stably at 999–1000 Hz for a 1 kHz cutoff, peak < 0.8.
  - Every type stays finite under 40 blocks of random parameters, random input up to 4.0 and audio-rate
    cutoff jumps.
- **CPU:** routing pushed the acceptance scenario to 26.5%. Two fixes brought it back to **24.3%** (criterion
  < 25%): folding bypassed filters into the routing, and skipping the noise/sub reads unless audible or needed
  as an FM source (that also fixed "FM (Sub)" with the sub off). **Headroom is now small: SIMD across unison
  voices is the next performance task before more per-voice work lands.**
- **CI** runs with `-SkipBenchmark` / `-LE benchmark`: CPU thresholds only make sense on known hardware.
- **Not yet:** filter-type mapping from Serum (feature/presets); BUS 1/2 sends (with the Phase 5 buses); Serum 2's
  "osc/filter as modulation source".

### Performance: SIMD unison reads (between Phases 4 and 5)

- `WavetableBank::read4()` reads four unison voices at once. Table loads are scalar (SSE2 has no gather)
  and go straight into `_mm_set_ps`, so values never round-trip through memory. The frame morph, level
  crossfade and Hermite run four lanes wide, in the same operation order as `read()`; a test checks they
  match bit for bit.
- The voice uses it in two specialized loops (plain and warped; warps stay per lane). Trailing groups of 2–3
  voices are padded. Spread unison (span > 0) stays scalar.
- Results (Release):

  | Scenario | Before | After |
  |---|---|---|
  | Acceptance (16 voices × 3 osc × 7 unison) | 24.3% | ≈ 22% |
  | 16 unison voices | 57% | ≈ 44% |
  | Sync warp, Good | 41% | ≈ 30% |
  | Sync warp, Ultra | 137% | ≈ 114% |

  Run-to-run noise is about ±1%.
- **Next levers if needed:** AVX2 gathers (needs a runtime CPU dispatch), and a struct-of-arrays unison
  layout.

### Phase 5 (SPEC §5.4): done

- **Racks:** three, Main / Bus 1 / Bus 2 (Serum 2's FXRack0..2), each with 8 slots (`FXRack<r>Slot<s>`).
  - Slot fields: type (patch state, not automatable), enabled, mix, and p0..p7 (generic 0..1 knobs, as in
    Serum 2's generic "FX Params"; meanings per type in `fx::paramNames()`).
  - Effects are created on the message thread when a slot's type changes (the Engine listens on its
    ConfigManager) and handed over through `ObjectExchange<Effect>` (lock-free, retired objects deleted on the
    message thread). The audio thread never allocates.
- **Effects** (our list; Serum's mapped by compat):
  - Distortion: 8 modes, 4× oversampled, pre/post filter, DC bias.
  - Flanger, Phaser (2–12 stages), Chorus (4 voices).
  - Delay: normal / ping-pong / tap, free or tempo-synced, band-pass feedback filter, time glide.
  - Compressor: soft knee, stereo-linked, limiter at the top of the ratio.
  - Multiband: OTT-style, 3 bands at 88 Hz / 2.5 kHz, upward + downward.
  - Reverb: Plate = Dattorro tank; Hall = 8-line Hadamard FDN.
  - EQ: 2 RBJ bands. Filter: any FilterType. Hyper/Dimension. Bode shifter (HIIR Hilbert pair).
  - Convolve: zero-latency, 64 direct taps plus 64/256 uniform-partitioned FFT stages; built-in decaying-noise
    IR until IR loading arrives with presets.
  - Utility: gain, pan, width, invert, swap, bass mono.
- **Splitters** (low/high, low/mid/high, mid/side) use Linkwitz-Riley 4th-order crossovers, and the low band is
  allpass-aligned in 3-band mode. Band k runs through slot `splitter + 1 + k`; bands are summed or M/S-decoded,
  with per-band levels.
- **Routing:** every source has `bus1Send` / `bus2Send` (pre-filter, post amp envelope; off when the route is
  None). `Mixer` sets `directLevel`, `bus1Level` and `bus2Level`. Output = Main rack + Bus 1 rack · level +
  Bus 2 rack · level + Direct · level.
- **Acceptance:**
  - Null tests: EQ (shelves and peaks at 0 dB), utility at neutral and the compressor below threshold give a
    residual of exactly 0.
  - Reverb RT60 (T20, broadband, size 0.5):

    | Mode | 0.6 s | 1.5 s | 4 s |
    |---|---|---|---|
    | Hall | 0.594 | 1.490 | 3.995 |
    | Plate | 0.608 | 1.615 | 4.139 |

  - Two Plate fixes were needed to get there: first-order allpass interpolation on the modulated tank taps (no
    per-trip loss), and diffusion coefficients capped so the allpasses' own ringing stays under half the target
    decay. Without the cap there was a fixed ~0.4 s floor.
  - **Known limit:** at maximum size with decays shorter than about 0.7 s, one trip around the plate's tank takes
    longer than the decay, so the decay arrives as discrete steps.
- **Also verified:** convolution equals the IR to 5e-9 with zero latency; Bode +200 Hz with the image 51 dB
  down; delay echoes sample-accurate, ping-pong alternating; splitter sums flat to 0.05 dB; FX output identical
  for host block sizes 1…512; every effect stays finite under fuzzing.
- **CPU:** acceptance 23–24% (criterion < 25%; the six-bus routing costs about 1.5%). The Main FX chain adds
  5–10%. **The margin is thin again: AVX2 gathers with runtime dispatch, or a struct-of-arrays unison layout, are
  the next levers.**
- **Not yet:**
  - FX parameter modulation (FX are global, while the matrix is per voice; needs a global matrix pass).
  - User IRs for Convolve (feature/presets).
  - Per-type parameter names in the generic editor (`fx::paramNames()` exists for feature/UI).
  - Resetting p0..p7 to the type's defaults when a user picks a new type. That is UI policy: the engine must not
    do it, or preset loads would be overwritten.

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

Host parameter IDs = namespaced keys (`Filter0.cutoff`), and the VST3 numeric IDs come from `vst3_id`.
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
src/params/     ConfigManager.{h,cpp,ipp} ParamRegistry.{h,cpp,ipp} ParamHandle.h ParamListener.h
                EngineSnapshot.h GlobalSettings.{h,cpp}                       → winerose_params
src/engine/ osc/ filter/ mod/ seq/ fx/                                       → winerose_engine
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

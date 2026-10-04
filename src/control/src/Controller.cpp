#include "control/Controller.h"

#include "engine/Engine.h"
#include "engine/fx/Effect.h"
#include "params/ConfigManager.h"
#include "params/ParamRegistry.h"
#include "presets/PresetFormat.h"
#include "presets/SerumImport.h"

#include <array>
#include <cstdlib>
#include <optional>
#include <map>
#include <mutex>

namespace winerose::control {

namespace {

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

ParamValue defaultOf(const ParamDef& def)
{
    if (std::holds_alternative<StringMeta>(def.meta)) return ParamValue(def.default_value);
    // Stringified default: a number, or an enum label ("Good").
    return ParamValue(parsePlain(def.meta, def.default_value).value_or(0.0));
}

std::string unitOf(const ParamMeta& meta)
{
    if (const auto* m = std::get_if<NumericMeta>(&meta)) return m->unit;
    return {};
}

// FX slot parameters ("FXRack<r>Slot<s>.p<i>" / ".type") are generic: their meaning depends on the slot's
// current effect type. index = -1 for the type itself.
struct FxRef {
    std::string module;
    int index = -1;
};

std::optional<FxRef> fxRef(std::string_view nsKey)
{
    const auto dot = nsKey.find('.');
    if (dot == std::string_view::npos) return std::nullopt;
    const std::string_view module = nsKey.substr(0, dot), key = nsKey.substr(dot + 1);
    if (module.rfind("FXRack", 0) != 0 || module.find("Slot") == std::string_view::npos) return std::nullopt;
    if (key == "type") return FxRef{std::string(module), -1};
    if (key.size() == 2 && key[0] == 'p' && key[1] >= '0' && key[1] < '0' + fx::kParamCount)
        return FxRef{std::string(module), key[1] - '0'};
    return std::nullopt;
}

} // namespace

struct Controller::Subscribers {
    std::mutex                                             mutex;
    std::map<int, std::function<void(const ParamChange&)>> callbacks;
    int                                                    nextId = 1;
};

Controller::Controller(Engine& engine, std::shared_ptr<ConfigManager> config)
    : m_engine(engine)
    , m_config(std::move(config))
    , m_subscribers(std::make_shared<Subscribers>())
{
    m_config->addListener(this);
}

Controller::~Controller()
{
    m_config->removeListener(this);
}

// --- Resolution -------------------------------------------------------------------------------------

ParamRegistry* Controller::resolve(std::string_view nsKey, std::string& keyOut) const
{
    // Module names never contain '.', keys may ("Robot.IP" style), so split at the FIRST dot.
    const auto dot = nsKey.find('.');
    if (dot == std::string_view::npos) return nullptr;
    keyOut = std::string(nsKey.substr(dot + 1));
    return m_config->findParamRegistry(std::string(nsKey.substr(0, dot)));
}

// --- Schema / values --------------------------------------------------------------------------------

std::vector<ParamSchema> Controller::schema() const
{
    std::vector<ParamSchema> out;
    for (const auto& [module, registry] : m_config->getAttachedParamRegistries()) {
        for (const auto& def : registry->getAll()) {
            ParamSchema s;
            s.nsKey        = registry->namespacedKey(def.key);
            s.module       = module;
            s.key          = def.key;
            s.group        = def.group;
            s.tooltip      = def.tooltip;
            s.type         = typeName(def.meta);
            s.defaultValue = defaultOf(def);
            s.automatable  = def.automatable;
            s.vst3Id       = def.vst3_id;
            std::visit(overloaded{
                [&](const NumericMeta& m) {
                    s.min = m.min_val; s.max = m.max_val;
                    s.curve = curveName(m.curve); s.curveParam = m.curve_param; s.unit = m.unit;
                },
                [&](const EnumMeta& m) {
                    for (const auto& c : m.choices) s.choices.emplace_back(c.value, c.label);
                    if (!m.choices.empty()) { s.min = m.choices.front().value; s.max = m.choices.back().value; }
                },
                [](const StringMeta&) {},
            }, def.meta);
            out.push_back(std::move(s));
        }
    }
    return out;
}

ParamValue Controller::get(std::string_view nsKey) const
{
    std::string key;
    auto* reg = resolve(nsKey, key);
    if (!reg) return {};
    const auto def = reg->find(key);
    if (!def) return {};
    if (std::holds_alternative<StringMeta>(def->meta)) return ParamValue(reg->get<std::string>(key));
    return ParamValue(reg->get<double>(key));
}

bool Controller::write(std::string_view nsKey, const ParamValue& value, bool recordHistory)
{
    std::string key;
    auto* reg = resolve(nsKey, key);
    if (!reg) return false;
    const auto def = reg->find(key);
    if (!def) return false;

    const ParamValue before = get(nsKey);
    const auto fx = fxRef(nsKey);
    const bool fxTypeEdit = recordHistory && fx && fx->index < 0;
    if (fxTypeEdit) m_history.beginGroup();   // the type and its default knobs undo as one step

    bool ok = true;
    if (std::holds_alternative<StringMeta>(def->meta)) {
        if (value.isNumber()) ok = false;
        else reg->set<std::string>(key, value.text());
    } else if (value.isNumber()) {
        reg->set<double>(key, value.number());
    } else if (!reg->modify(key, value.text())) {
        ok = false;
    }
    if (ok && recordHistory) m_history.record({std::string(nsKey), before, get(nsKey)});

    // A user picking a new effect type gets that effect's default knob positions. This is the user-edit
    // path only: state/preset loads go through loadState() and keep their stored values.
    if (ok && fxTypeEdit && !(get(nsKey) == before)) {
        const auto type = static_cast<fx::FxType>(reg->get<int>("type"));
        const auto& defaults = fx::defaultParams(type);
        for (int i = 0; i < fx::kParamCount; ++i)
            write(fx->module + ".p" + std::to_string(i), ParamValue(static_cast<double>(defaults[static_cast<std::size_t>(i)])), true);
    }
    if (fxTypeEdit) m_history.endGroup();
    return ok;
}

bool Controller::set(std::string_view nsKey, const ParamValue& value)
{
    return write(nsKey, value, /*recordHistory=*/true);
}

bool Controller::modify(std::string_view nsKey, std::string_view text)
{
    std::string key;
    auto* reg = resolve(nsKey, key);
    if (!reg) return false;
    const ParamValue before = get(nsKey);
    if (!reg->modify(key, std::string(text))) return false;
    m_history.record({std::string(nsKey), before, get(nsKey)});
    return true;
}

// --- Value math -------------------------------------------------------------------------------------

double Controller::toNormalized(std::string_view nsKey, double plain) const
{
    std::string key;
    auto* reg = resolve(nsKey, key);
    return reg ? reg->toNormalized(key, plain) : 0.0;
}

double Controller::fromNormalized(std::string_view nsKey, double normalized) const
{
    std::string key;
    auto* reg = resolve(nsKey, key);
    return reg ? reg->fromNormalized(key, normalized) : 0.0;
}

std::string Controller::format(std::string_view nsKey, double plain) const
{
    std::string key;
    auto* reg = resolve(nsKey, key);
    if (!reg) return {};
    if (const auto fx = fxRef(nsKey); fx && fx->index >= 0) {
        std::array<float, fx::kParamCount> values {};
        for (int i = 0; i < fx::kParamCount; ++i) values[static_cast<std::size_t>(i)] = reg->get<float>("p" + std::to_string(i));
        values[static_cast<std::size_t>(fx->index)] = static_cast<float>(plain);
        return fx::formatParam(static_cast<fx::FxType>(reg->get<int>("type")), fx->index, values);
    }
    const auto def = reg->find(key);
    if (!def) return {};
    std::string text = formatPlain(def->meta, plain);
    if (const auto unit = unitOf(def->meta); !unit.empty()) text += " " + unit;
    return text;
}

std::string Controller::label(std::string_view nsKey) const
{
    std::string key;
    auto* reg = resolve(nsKey, key);
    if (!reg) return {};
    if (const auto fx = fxRef(nsKey); fx && fx->index >= 0) {
        const auto type = static_cast<fx::FxType>(reg->get<int>("type"));
        const char* name = fx::paramSpec(type, fx->index).name;
        return (name != nullptr && name[0] != '\0') ? std::string(name) : key + " (unused)";
    }
    return key;
}

// --- Gestures ---------------------------------------------------------------------------------------

void Controller::beginGesture(std::string_view nsKey)
{
    m_history.beginGroup();
    if (m_gestureSink) m_gestureSink->beginGesture(std::string(nsKey));
}

void Controller::endGesture(std::string_view nsKey)
{
    if (m_gestureSink) m_gestureSink->endGesture(std::string(nsKey));
    m_history.endGroup();
}

// --- State / presets --------------------------------------------------------------------------------

std::string Controller::saveState() const
{
    return m_config->serialize();
}

Result Controller::loadState(const std::string& state)
{
    if (!ConfigManager::isValidState(state))
        return Result::failure("not a Winerose state document");

    // One batch: defaults first (so keys missing from an older state don't keep the previous patch's
    // values), then the stored values, then clamp anything out of range. Listeners hear one onBatchEnd.
    const auto registries = m_config->getAttachedParamRegistries();
    m_config->beginBatch();
    for (const auto& [name, reg] : registries) reg->resetToDefaults();
    m_config->erasePrefix("Serum2.");   // values preserved from an earlier import; the state re-adds its own
    m_config->erasePrefix("Serum1.");
    m_config->restore(state);
    for (const auto& [name, reg] : registries)
        for (const auto& def : reg->getAll()) reg->clampAndApply(def);
    m_config->endBatch();

    m_history.clear();
    return Result::success();
}

Result Controller::loadPreset(std::span<const std::uint8_t> bytes)
{
    const auto format = detectPresetFormat(bytes);
    if (format == PresetFormat::WineroseState)
        return loadState(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));

    // A bare wavetable: load it onto oscillator A (a common drag-and-drop case).
    if (format == PresetFormat::Unknown) {
        std::string error;
        if (auto wt = presets::Wavetable::read(bytes, error)) {
            m_engine.setOscillatorTable(0, dsp::WavetableBank::build(wt->samples, wt->frameSize, "Imported"));
            return Result::success("Loaded a " + std::to_string(wt->frameCount) + "-frame wavetable on oscillator A");
        }
        return Result::failure("unrecognized preset format");
    }

    // Serum presets: decode first (nothing changes if the file is unreadable), then map in one batch.
    std::string error;
    std::optional<presets::SerumPresetFile> serum2;
    std::optional<presets::FxpFile> serum1;
    if (format == PresetFormat::SerumPreset) serum2 = presets::SerumPresetFile::read(bytes, error);
    else                                     serum1 = presets::FxpFile::read(bytes, error);
    if (!serum2 && !serum1) return Result::failure(std::string(presetFormatName(format)) + ": " + error);

    presets::SerumImporter importer(m_config, m_assetRoots);
    m_config->beginBatch();
    for (const auto& [name, reg] : m_config->getAttachedParamRegistries()) reg->resetToDefaults();
    const presets::ImportReport report = serum2 ? importer.importSerum2(*serum2) : importer.importSerum1(serum1->programs.front());
    m_config->endBatch();

    for (const auto& w : report.wavetables)
        m_engine.setOscillatorTable(w.oscillator, dsp::WavetableBank::build(w.table.samples, w.table.frameSize, w.table.name));
    m_history.clear();
    m_lastImport = report.toJson();
    std::string summary = report.summary();
    if (serum1 && serum1->programs.size() > 1)
        summary += " (bank with " + std::to_string(serum1->programs.size()) + " programs: loaded the first)";
    return Result::success(summary);
}

// --- Undo -------------------------------------------------------------------------------------------

bool Controller::undo()
{
    const auto step = m_history.undo();
    if (!step) return false;
    for (auto it = step->rbegin(); it != step->rend(); ++it) {
        if (m_gestureSink) m_gestureSink->beginGesture(it->nsKey);
        write(it->nsKey, it->before, /*recordHistory=*/false);
        if (m_gestureSink) m_gestureSink->endGesture(it->nsKey);
    }
    return true;
}

bool Controller::redo()
{
    const auto step = m_history.redo();
    if (!step) return false;
    for (const auto& edit : *step) {
        if (m_gestureSink) m_gestureSink->beginGesture(edit.nsKey);
        write(edit.nsKey, edit.after, /*recordHistory=*/false);
        if (m_gestureSink) m_gestureSink->endGesture(edit.nsKey);
    }
    return true;
}

// --- Observation ------------------------------------------------------------------------------------

Subscription Controller::onChange(std::function<void(const ParamChange&)> callback)
{
    int id = 0;
    {
        std::lock_guard<std::mutex> lock(m_subscribers->mutex);
        id = m_subscribers->nextId++;
        m_subscribers->callbacks.emplace(id, std::move(callback));
    }
    std::weak_ptr<Subscribers> weak = m_subscribers;
    return Subscription([weak, id] {
        if (auto subs = weak.lock()) {
            std::lock_guard<std::mutex> lock(subs->mutex);
            subs->callbacks.erase(id);
        }
    });
}

MeterSnapshot Controller::meters() const
{
    const auto& m = m_engine.meters();
    return {m.peakLeft.load(std::memory_order_relaxed), m.peakRight.load(std::memory_order_relaxed)};
}

void Controller::dispatch(const ParamChange& change)
{
    std::vector<std::function<void(const ParamChange&)>> callbacks;
    {
        std::lock_guard<std::mutex> lock(m_subscribers->mutex);
        for (const auto& [id, cb] : m_subscribers->callbacks) callbacks.push_back(cb);
    }
    for (const auto& cb : callbacks) cb(change);
}

void Controller::onParamChanged(const std::string& nsKey)
{
    // Non-numeric params (strings today; tables/curves later) feed the EngineSnapshot, not a ParamHandle.
    if (m_config->numericSlot(nsKey) == nullptr) m_engine.publishSnapshot();
    dispatch(ParamChange{nsKey, get(nsKey), false});
}

void Controller::onBatchEnd()
{
    m_engine.publishSnapshot();
    dispatch(ParamChange{{}, {}, true});
}

} // namespace winerose::control

#include "control/Controller.h"

#include "engine/Engine.h"
#include "params/ConfigManager.h"
#include "params/ParamRegistry.h"
#include "presets/PresetFormat.h"

#include <cstdlib>
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
    if (std::holds_alternative<StringMeta>(def->meta)) {
        if (value.isNumber()) return false;
        reg->set<std::string>(key, value.text());
    } else if (value.isNumber()) {
        reg->set<double>(key, value.number());
    } else if (!reg->modify(key, value.text())) {
        return false;
    }
    if (recordHistory) m_history.record({std::string(nsKey), before, get(nsKey)});
    return true;
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
    const auto def = reg->find(key);
    if (!def) return {};
    std::string text = formatPlain(def->meta, plain);
    if (const auto unit = unitOf(def->meta); !unit.empty()) text += " " + unit;
    return text;
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
    switch (format) {
        case PresetFormat::WineroseState:
            return loadState(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        case PresetFormat::SerumPreset:
        case PresetFormat::SerumFxp:
        case PresetFormat::SerumFxb:
            return Result::failure(std::string(presetFormatName(format)) + " import is not implemented yet (feature/presets)");
        case PresetFormat::Unknown:
            break;
    }
    return Result::failure("unrecognized preset format");
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

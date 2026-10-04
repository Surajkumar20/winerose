#include "params/ParamRegistry.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace winerose {

namespace {
// Values live in float slots, so a double bound like 0.1 reads back as 0.100000001… — that is not
// "out of range". Treat differences below float resolution as equal.
bool nearlyEqual(double a, double b) noexcept
{
    return std::abs(a - b) <= 1e-6 * std::max(1.0, std::max(std::abs(a), std::abs(b)));
}
} // namespace

ParamRegistry::ParamRegistry(std::shared_ptr<ConfigManager> configManager, const std::string& requestedName)
    : m_config_(std::move(configManager))
{
    assert(m_config_ && "ParamRegistry needs a ConfigManager");
    m_resolved_name_ = m_config_->attachParamRegistry(requestedName, this);
}

ParamRegistry::~ParamRegistry()
{
    if (!m_detached_ && m_config_) {
        m_config_->detachParamRegistry(m_resolved_name_);
        m_detached_ = true;
    }
}

// --- Registration -----------------------------------------------------------------------------------

void ParamRegistry::addDef(ParamDef def)
{
    std::lock_guard<std::mutex> lock(m_mutex_);
    const auto dup = std::find_if(m_params_.begin(), m_params_.end(),
                                  [&](const ParamDef& d) { return d.key == def.key; });
    assert(dup == m_params_.end() && "ParamRegistry: key registered twice");
    if (dup != m_params_.end()) *dup = std::move(def);
    else                        m_params_.push_back(std::move(def));
}

void ParamRegistry::registerNumeric(const std::string& key, double default_value, double min_val, double max_val,
                                    NumericMeta::SubType subtype, const std::string& group,
                                    const std::string& tooltip, const ParamOpts& opts)
{
    WINEROSE_ASSERT_NOT_REALTIME();
    assert(min_val <= max_val);
    const ParamMeta meta = NumericMeta{subtype, min_val, max_val, opts.curve, opts.curve_param, opts.unit};
    const double def = clampPlain(meta, default_value);

    const std::string ns = namespacedKey(key);
    const double hydrated = clampPlain(meta, m_config_->get<double>(ns, def));
    m_config_->set<double>(ns, hydrated);

    addDef(ParamDef{key, group, tooltip, formatPlain(meta, def), meta, opts.automatable, opts.vst3_id});
}

void ParamRegistry::registerInt(const std::string& key, int default_value, int min_val, int max_val,
                                const std::string& group, const std::string& tooltip, const ParamOpts& opts)
{
    registerNumeric(key, default_value, min_val, max_val, NumericMeta::SubType::INT, group, tooltip, opts);
}

void ParamRegistry::registerFloat(const std::string& key, float default_value, float min_val, float max_val,
                                  const std::string& group, const std::string& tooltip, const ParamOpts& opts)
{
    registerNumeric(key, default_value, min_val, max_val, NumericMeta::SubType::FLOAT, group, tooltip, opts);
}

void ParamRegistry::registerDouble(const std::string& key, double default_value, double min_val, double max_val,
                                   const std::string& group, const std::string& tooltip, const ParamOpts& opts)
{
    registerNumeric(key, default_value, min_val, max_val, NumericMeta::SubType::DOUBLE, group, tooltip, opts);
}

void ParamRegistry::registerDuration(const std::string& key, std::chrono::milliseconds default_value,
                                     std::chrono::milliseconds min_val, std::chrono::milliseconds max_val,
                                     const std::string& group, const std::string& tooltip, const ParamOpts& opts)
{
    ParamOpts o = opts;
    if (o.unit.empty()) o.unit = "ms";
    registerNumeric(key, static_cast<double>(default_value.count()), static_cast<double>(min_val.count()),
                    static_cast<double>(max_val.count()), NumericMeta::SubType::INT, group, tooltip, o);
}

void ParamRegistry::registerString(const std::string& key, const std::string& default_value,
                                   const std::string& group, const std::string& tooltip, const ParamOpts& opts)
{
    WINEROSE_ASSERT_NOT_REALTIME();
    const std::string ns = namespacedKey(key);
    const std::string hydrated = m_config_->get<std::string>(ns, default_value);
    m_config_->set<std::string>(ns, hydrated);
    addDef(ParamDef{key, group, tooltip, default_value, StringMeta{}, /*automatable=*/false, opts.vst3_id});
}

void ParamRegistry::registerBool(const std::string& key, bool default_value,
                                 const std::string& group, const std::string& tooltip, const ParamOpts& opts)
{
    WINEROSE_ASSERT_NOT_REALTIME();
    const ParamMeta meta = EnumMeta{{{0, "Off"}, {1, "On"}}, /*is_bool=*/true};
    const double def = default_value ? 1.0 : 0.0;
    const std::string ns = namespacedKey(key);
    const double hydrated = clampPlain(meta, m_config_->get<double>(ns, def));
    m_config_->set<double>(ns, hydrated);
    addDef(ParamDef{key, group, tooltip, formatPlain(meta, def), meta, opts.automatable, opts.vst3_id});
}

void ParamRegistry::registerEnum(const std::string& key, std::vector<EnumChoice> choices, int default_value,
                                 const std::string& group, const std::string& tooltip, const ParamOpts& opts)
{
    WINEROSE_ASSERT_NOT_REALTIME();
    assert(!choices.empty() && "registerEnum needs at least one choice");
    const ParamMeta meta = EnumMeta{std::move(choices), false};
    const double def = clampPlain(meta, default_value);
    const std::string ns = namespacedKey(key);
    const double hydrated = clampPlain(meta, m_config_->get<double>(ns, def));
    m_config_->set<double>(ns, hydrated);
    addDef(ParamDef{key, group, tooltip, formatPlain(meta, def), meta, opts.automatable, opts.vst3_id});
}

// --- Live values ------------------------------------------------------------------------------------

std::optional<ParamMeta> ParamRegistry::metaFor(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(m_mutex_);
    for (const auto& d : m_params_)
        if (d.key == key) return d.meta;
    return std::nullopt;
}

void ParamRegistry::setPlain(const std::string& key, const ParamMeta& meta, double plain)
{
    m_config_->set<double>(namespacedKey(key), clampPlain(meta, plain));
}

bool ParamRegistry::modify(const std::string& key, const std::string& rawValue)
{
    WINEROSE_ASSERT_NOT_REALTIME();
    const auto meta = metaFor(key);
    if (!meta) return false;

    if (std::holds_alternative<StringMeta>(*meta)) {
        m_config_->set<std::string>(namespacedKey(key), rawValue);
        return true;
    }
    const auto parsed = parsePlain(*meta, rawValue);
    if (!parsed) return false;
    setPlain(key, *meta, *parsed);
    return true;
}

void ParamRegistry::resetToDefaults()
{
    WINEROSE_ASSERT_NOT_REALTIME();
    std::vector<std::pair<std::string, std::string>> defaults;
    {
        std::lock_guard<std::mutex> lock(m_mutex_);
        for (const auto& d : m_params_) defaults.emplace_back(d.key, d.default_value);
    }
    m_config_->beginBatch();
    for (const auto& [key, value] : defaults) modify(key, value);
    m_config_->endBatch();
}

// --- Realtime / normalization -----------------------------------------------------------------------

ParamHandle ParamRegistry::handle(const std::string& key) const
{
    const auto meta = metaFor(key);
    if (!meta || std::holds_alternative<StringMeta>(*meta)) return {};
    return ParamHandle{m_config_->numericSlot(namespacedKey(key))};
}

double ParamRegistry::toNormalized(const std::string& key, double plain) const
{
    const auto meta = metaFor(key);
    return meta ? winerose::toNormalized(*meta, plain) : 0.0;
}

double ParamRegistry::fromNormalized(const std::string& key, double normalized) const
{
    const auto meta = metaFor(key);
    return meta ? winerose::fromNormalized(*meta, normalized) : 0.0;
}

// --- Query / validation -----------------------------------------------------------------------------

const std::vector<ParamDef>& ParamRegistry::getAll() const
{
    return m_params_;
}

std::optional<ParamDef> ParamRegistry::find(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(m_mutex_);
    for (const auto& d : m_params_)
        if (d.key == key) return d;
    return std::nullopt;
}

std::vector<ValidationError> ParamRegistry::validate() const
{
    WINEROSE_ASSERT_NOT_REALTIME();
    std::vector<ParamDef> defs;
    {
        std::lock_guard<std::mutex> lock(m_mutex_);
        defs = m_params_;
    }
    std::vector<ValidationError> errors;
    for (const auto& d : defs) {
        const std::string ns = namespacedKey(d.key);
        if (!m_config_->contains(ns)) {
            errors.push_back({ns, "missing value"});
            continue;
        }
        if (std::holds_alternative<StringMeta>(d.meta)) continue;
        const double v = m_config_->get<double>(ns, 0.0);
        if (!nearlyEqual(clampPlain(d.meta, v), v)) {
            if (std::holds_alternative<EnumMeta>(d.meta))
                errors.push_back({ns, "value " + formatNumber(v) + " is not a valid choice"});
            else
                errors.push_back({ns, "value " + formatNumber(v) + " is out of range"});
        }
    }
    return errors;
}

bool ParamRegistry::clampAndApply(const ParamDef& def)
{
    WINEROSE_ASSERT_NOT_REALTIME();
    if (std::holds_alternative<StringMeta>(def.meta)) return false;
    const std::string ns = namespacedKey(def.key);
    const double v = m_config_->get<double>(ns, 0.0);
    const double c = clampPlain(def.meta, v);
    if (nearlyEqual(c, v) && m_config_->contains(ns)) return false;
    m_config_->set<double>(ns, c);
    return true;
}

} // namespace winerose

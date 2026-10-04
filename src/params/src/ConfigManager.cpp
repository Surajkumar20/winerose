#include "params/ConfigManager.h"
#include "params/ParamRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>

namespace winerose {

namespace {

constexpr const char* kStateFormat  = "winerose.state";
constexpr int         kStateVersion = 1;

std::optional<double> parseNumber(const std::string& s)
{
    if (s.empty()) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return std::nullopt;
    return v;
}

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

} // namespace

ConfigManager::~ConfigManager() = default;

// --- Values -----------------------------------------------------------------------------------------

bool ConfigManager::contains(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(m_mutex_);
    return m_numeric_.count(key) != 0 || m_text_.count(key) != 0;
}

const std::atomic<float>* ConfigManager::numericSlot(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(m_mutex_);
    const auto it = m_numeric_.find(key);
    return it == m_numeric_.end() ? nullptr : &m_slots_[it->second];
}

std::atomic<float>* ConfigManager::hostWritableSlot(const std::string& key)
{
    std::lock_guard<std::mutex> lock(m_mutex_);
    const auto it = m_numeric_.find(key);
    return it == m_numeric_.end() ? nullptr : &m_slots_[it->second];
}

std::optional<double> ConfigManager::numericValue(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(m_mutex_);
    if (const auto it = m_numeric_.find(key); it != m_numeric_.end())
        return static_cast<double>(m_slots_[it->second].load(std::memory_order_relaxed));
    if (const auto it = m_text_.find(key); it != m_text_.end())
        return parseNumber(it->second);
    return std::nullopt;
}

std::optional<std::string> ConfigManager::textValue(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(m_mutex_);
    if (const auto it = m_text_.find(key); it != m_text_.end())
        return it->second;
    if (const auto it = m_numeric_.find(key); it != m_numeric_.end())
        return formatNumber(m_slots_[it->second].load(std::memory_order_relaxed));
    return std::nullopt;
}

void ConfigManager::setNumeric(const std::string& key, double value)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex_);
        const auto it = m_numeric_.find(key);
        if (it != m_numeric_.end()) {
            m_slots_[it->second].store(static_cast<float>(value), std::memory_order_relaxed);
        } else {
            m_slots_.emplace_back(static_cast<float>(value));
            m_numeric_.emplace(key, m_slots_.size() - 1);
            m_text_.erase(key);   // a restored-but-unclaimed value is now claimed by a numeric param
        }
    }
    fireChanged(key);
}

void ConfigManager::setText(const std::string& key, const std::string& value)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex_);
        const auto it = m_numeric_.find(key);
        if (it != m_numeric_.end()) {
            const auto num = parseNumber(value);
            if (!num) return;   // non-numeric text can't go into a numeric slot — ignore
            m_slots_[it->second].store(static_cast<float>(*num), std::memory_order_relaxed);
        } else {
            m_text_[key] = value;
        }
    }
    fireChanged(key);
}

void ConfigManager::clear_all()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex_);
        for (auto& slot : m_slots_) slot.store(0.0f, std::memory_order_relaxed);
        m_text_.clear();
    }
    beginBatch();
    m_batch_dirty_ = true;
    endBatch();
}

void ConfigManager::erasePrefix(const std::string& prefix)
{
    bool erased = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex_);
        for (auto it = m_text_.begin(); it != m_text_.end();) {
            if (it->first.rfind(prefix, 0) == 0) { it = m_text_.erase(it); erased = true; }
            else ++it;
        }
        for (auto it = m_numeric_.begin(); it != m_numeric_.end();) {
            if (it->first.rfind(prefix, 0) == 0) { it = m_numeric_.erase(it); erased = true; }
            else ++it;
        }
    }
    if (erased) {
        beginBatch();
        m_batch_dirty_ = true;
        endBatch();
    }
}

void ConfigManager::notifyChanged(const std::string& key)
{
    fireChanged(key);
}

// --- Batching / listeners ---------------------------------------------------------------------------

void ConfigManager::beginBatch()
{
    ++m_batch_depth_;
}

void ConfigManager::endBatch()
{
    if (m_batch_depth_.load() <= 0) return;
    if (--m_batch_depth_ == 0 && m_batch_dirty_.exchange(false))
        fireBatchEnd();
}

void ConfigManager::addListener(IParamListener* listener)
{
    if (listener == nullptr) return;
    std::lock_guard<std::mutex> lock(m_listener_mutex_);
    if (std::find(m_listeners_.begin(), m_listeners_.end(), listener) == m_listeners_.end())
        m_listeners_.push_back(listener);
}

void ConfigManager::removeListener(IParamListener* listener)
{
    std::lock_guard<std::mutex> lock(m_listener_mutex_);
    m_listeners_.erase(std::remove(m_listeners_.begin(), m_listeners_.end(), listener), m_listeners_.end());
}

void ConfigManager::fireChanged(const std::string& key)
{
    if (m_batch_depth_.load() > 0) {
        m_batch_dirty_ = true;
        return;
    }
    std::vector<IParamListener*> listeners;
    {
        std::lock_guard<std::mutex> lock(m_listener_mutex_);
        listeners = m_listeners_;
    }
    for (auto* l : listeners) l->onParamChanged(key);   // no lock held: listeners may call back in
}

void ConfigManager::fireBatchEnd()
{
    std::vector<IParamListener*> listeners;
    {
        std::lock_guard<std::mutex> lock(m_listener_mutex_);
        listeners = m_listeners_;
    }
    for (auto* l : listeners) l->onBatchEnd();
}

// --- State ------------------------------------------------------------------------------------------

std::string ConfigManager::serialize() const
{
    WINEROSE_ASSERT_NOT_REALTIME();
    nlohmann::ordered_json values = nlohmann::ordered_json::object();
    {
        std::lock_guard<std::mutex> lock(m_mutex_);
        // Sorted keys → byte-identical output for identical state (SPEC §5.8 round-trip requirement).
        std::map<std::string, double> numeric;
        for (const auto& [key, idx] : m_numeric_)
            numeric.emplace(key, static_cast<double>(m_slots_[idx].load(std::memory_order_relaxed)));
        std::map<std::string, nlohmann::ordered_json> merged;
        for (const auto& [key, v] : numeric) merged.emplace(key, v);
        for (const auto& [key, v] : m_text_) merged.emplace(key, v);
        for (auto& [key, v] : merged) values[key] = std::move(v);
    }
    nlohmann::ordered_json doc;
    doc["format"]  = kStateFormat;
    doc["version"] = kStateVersion;
    doc["values"]  = std::move(values);
    return doc.dump();
}

bool ConfigManager::isValidState(const std::string& blob)
{
    const auto doc = nlohmann::json::parse(blob, nullptr, /*allow_exceptions=*/false);
    return doc.is_object()
        && doc.value("format", std::string{}) == kStateFormat
        && doc.contains("values") && doc["values"].is_object();
}

bool ConfigManager::restore(const std::string& blob)
{
    WINEROSE_ASSERT_NOT_REALTIME();
    if (!isValidState(blob)) return false;
    const auto doc = nlohmann::json::parse(blob);

    beginBatch();
    for (const auto& [key, v] : doc["values"].items()) {
        if (v.is_number() || v.is_boolean()) {
            const double d = v.is_boolean() ? (v.get<bool>() ? 1.0 : 0.0) : v.get<double>();
            bool isTextParam = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex_);
                isTextParam = m_numeric_.count(key) == 0 && m_text_.count(key) != 0;
            }
            if (isTextParam) setText(key, formatNumber(d));
            else             setNumeric(key, d);
        } else if (v.is_string()) {
            setText(key, v.get<std::string>());
        }
        // other JSON types: not produced by serialize() — ignored
    }
    m_batch_dirty_ = true;   // a restore always counts as a change, even if every value was identical
    endBatch();
    return true;
}

// --- Registry bookkeeping ---------------------------------------------------------------------------

std::string ConfigManager::attachParamRegistry(const std::string& requestedName, ParamRegistry* registry)
{
    std::lock_guard<std::mutex> lock(m_registry_mutex_);
    std::string candidate = requestedName;
    int suffix = 2;
    while (m_attached_registries_.count(candidate))
        candidate = requestedName + "_" + std::to_string(suffix++);
    m_attached_registries_[candidate] = registry;
    return candidate;
}

void ConfigManager::detachParamRegistry(const std::string& resolvedName)
{
    std::lock_guard<std::mutex> lock(m_registry_mutex_);
    m_attached_registries_.erase(resolvedName);
}

std::map<std::string, ParamRegistry*> ConfigManager::getAttachedParamRegistries() const
{
    std::lock_guard<std::mutex> lock(m_registry_mutex_);
    return m_attached_registries_;
}

ParamRegistry* ConfigManager::findParamRegistry(const std::string& resolvedName) const
{
    std::lock_guard<std::mutex> lock(m_registry_mutex_);
    const auto it = m_attached_registries_.find(resolvedName);
    return it == m_attached_registries_.end() ? nullptr : it->second;
}

// --- Schema -----------------------------------------------------------------------------------------

bool ConfigManager::writeParamSchema(const std::filesystem::path& rootDir,
                                     const std::string& resolvedName,
                                     const std::vector<ParamDef>& defs) const
{
    std::error_code ec;
    const auto dir = rootDir / resolvedName;
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;

    std::ofstream out(dir / "schema.ini", std::ios::trunc);
    if (!out) return false;

    for (const auto& def : defs) {
        out << '[' << def.key << "]\n";
        out << "type=" << typeName(def.meta) << '\n';
        out << "group=" << def.group << '\n';
        out << "tooltip=" << def.tooltip << '\n';
        out << "default=" << def.default_value << '\n';
        out << "automatable=" << (def.automatable ? "true" : "false") << '\n';
        if (def.vst3_id >= 0) out << "vst3_id=" << def.vst3_id << '\n';
        std::visit(overloaded{
            [&](const NumericMeta& m) {
                out << "min=" << formatNumber(m.min_val) << '\n';
                out << "max=" << formatNumber(m.max_val) << '\n';
                out << "curve=" << curveName(m.curve) << '\n';
                if (m.curve == NumericMeta::Curve::Power) out << "curve_param=" << formatNumber(m.curve_param) << '\n';
                if (!m.unit.empty()) out << "unit=" << m.unit << '\n';
            },
            [&](const EnumMeta& m) {
                // Same comma-list convention as the example: choices=0:Off,1:On,2:Auto
                out << "choices=";
                for (std::size_t i = 0; i < m.choices.size(); ++i)
                    out << (i ? "," : "") << m.choices[i].value << ':' << m.choices[i].label;
                out << '\n';
            },
            [](const StringMeta&) {},
        }, def.meta);
        out << '\n';
    }
    return static_cast<bool>(out);
}

} // namespace winerose

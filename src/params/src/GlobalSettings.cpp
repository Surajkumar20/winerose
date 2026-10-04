#include "params/GlobalSettings.h"

#include <fstream>

namespace winerose {

namespace {

std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

} // namespace

GlobalSettings::GlobalSettings(std::filesystem::path file)
    : m_file_(std::move(file))
{
    load();
}

bool GlobalSettings::load()
{
    std::lock_guard<std::mutex> lock(m_mutex_);
    m_data_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(m_file_, ec)) return true;
    std::ifstream in(m_file_);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        m_data_[trim(t.substr(0, eq))] = trim(t.substr(eq + 1));
    }
    return true;
}

bool GlobalSettings::save() const
{
    std::lock_guard<std::mutex> lock(m_mutex_);
    std::error_code ec;
    if (m_file_.has_parent_path()) std::filesystem::create_directories(m_file_.parent_path(), ec);
    std::ofstream out(m_file_, std::ios::trunc);
    if (!out) return false;
    for (const auto& [k, v] : m_data_) out << k << " = " << v << '\n';
    return static_cast<bool>(out);
}

std::optional<std::string> GlobalSettings::getRaw(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(m_mutex_);
    const auto it = m_data_.find(key);
    if (it == m_data_.end()) return std::nullopt;
    return it->second;
}

void GlobalSettings::setRaw(const std::string& key, const std::string& value)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex_);
        m_data_[key] = value;
    }
    save();
}

} // namespace winerose

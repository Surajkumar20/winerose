#include "presets/Sfz.h"

#include <algorithm>
#include <cctype>

namespace winerose::presets {

namespace {

std::string stripComments(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '/' && i + 1 < in.size() && in[i + 1] == '/') {
            while (i < in.size() && in[i] != '\n') ++i;
            out.push_back('\n');
        } else if (in[i] == '/' && i + 1 < in.size() && in[i + 1] == '*') {
            i += 2;
            while (i + 1 < in.size() && !(in[i] == '*' && in[i + 1] == '/')) ++i;
            ++i;
            out.push_back(' ');
        } else {
            out.push_back(in[i] == '\r' ? '\n' : in[i]);
        }
    }
    return out;
}

std::string trim(const std::string& s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool isOpcodeChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$'; }

} // namespace

std::optional<int> parseSfzNote(const std::string& raw)
{
    const std::string v = trim(raw);
    if (v.empty()) return std::nullopt;
    if (std::isdigit(static_cast<unsigned char>(v[0])) || v[0] == '-') {
        try { return std::stoi(v); } catch (...) { return std::nullopt; }
    }
    static const int kBase[] = {9, 11, 0, 2, 4, 5, 7};   // a b c d e f g
    const char letter = static_cast<char>(std::tolower(static_cast<unsigned char>(v[0])));
    if (letter < 'a' || letter > 'g') return std::nullopt;
    int note = kBase[letter - 'a'];
    std::size_t i = 1;
    if (i < v.size() && (v[i] == '#' || v[i] == 's')) { ++note; ++i; }
    else if (i < v.size() && v[i] == 'b' && i + 1 < v.size() && (std::isdigit(static_cast<unsigned char>(v[i + 1])) || v[i + 1] == '-')) { --note; ++i; }
    try {
        const int octave = std::stoi(v.substr(i));
        return std::clamp(note + (octave + 1) * 12, 0, 127);
    } catch (...) {
        return std::nullopt;
    }
}

SfzFile SfzFile::parse(const std::string& source)
{
    SfzFile file;
    std::string text = stripComments(source);

    // #define $NAME value (applied textually, longest names first so $A doesn't eat $AB).
    std::vector<std::pair<std::string, std::string>> defines;
    {
        std::string kept;
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t end = std::min(text.find('\n', start), text.size());
            const std::string line = trim(text.substr(start, end - start));
            if (line.rfind("#define", 0) == 0) {
                const std::string rest = trim(line.substr(7));
                const auto sp = rest.find_first_of(" \t");
                if (sp != std::string::npos) defines.emplace_back(rest.substr(0, sp), trim(rest.substr(sp)));
            } else if (line.rfind("#include", 0) == 0) {
                file.warnings.push_back("#include is not supported: " + line);
            } else {
                kept += text.substr(start, end - start);
            }
            kept.push_back('\n');
            start = end + 1;
        }
        text = kept;
        std::sort(defines.begin(), defines.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
        for (const auto& [name, value] : defines)
            for (std::size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + value.size()))
                text.replace(at, name.size(), value);
    }

    enum class Level { None, Control, Global, Master, Group, Region, Ignored };
    Level level = Level::None;
    std::map<std::string, std::string> global, master, group, region;
    bool inRegion = false;

    auto flush = [&] {
        if (!inRegion) return;
        Region r;
        r.opcodes = global;
        for (const auto& kv : master) r.opcodes[kv.first] = kv.second;
        for (const auto& kv : group) r.opcodes[kv.first] = kv.second;
        for (const auto& kv : region) r.opcodes[kv.first] = kv.second;
        file.regions.push_back(std::move(r));
        region.clear();
        inRegion = false;
    };

    auto assign = [&](const std::string& key, const std::string& value) {
        switch (level) {
            case Level::Control: if (key == "default_path") file.defaultPath = value; break;
            case Level::Global:  global[key] = value; break;
            case Level::Master:  master[key] = value; break;
            case Level::Group:   group[key] = value; break;
            case Level::Region:  region[key] = value; break;
            case Level::None:    region[key] = value; break;   // opcodes before any header: treat as region-level
            case Level::Ignored: break;
        }
    };

    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        if (std::isspace(static_cast<unsigned char>(text[i]))) { ++i; continue; }
        if (text[i] == '<') {
            const auto close = text.find('>', i);
            if (close == std::string::npos) break;
            const std::string header = text.substr(i + 1, close - i - 1);
            i = close + 1;
            flush();
            if (header == "region") { level = Level::Region; inRegion = true; }
            else if (header == "group") { level = Level::Group; group.clear(); }
            else if (header == "master") { level = Level::Master; master.clear(); group.clear(); }
            else if (header == "global") { level = Level::Global; global.clear(); master.clear(); group.clear(); }
            else if (header == "control") level = Level::Control;
            else level = Level::Ignored;   // <curve>, <effect>, <midi>, ...
            continue;
        }
        // opcode=value; the value runs to the next "name=" token, header or line end.
        std::size_t k = i;
        while (k < n && isOpcodeChar(text[k])) ++k;
        if (k == i || k >= n || text[k] != '=') {   // junk: skip the token
            while (i < n && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            continue;
        }
        const std::string key = text.substr(i, k - i);
        std::size_t v = k + 1, end = v;
        while (end < n && text[end] != '\n' && text[end] != '<') {
            if (std::isspace(static_cast<unsigned char>(text[end]))) {
                std::size_t look = end;
                while (look < n && (text[look] == ' ' || text[look] == '\t')) ++look;
                std::size_t name = look;
                while (name < n && isOpcodeChar(text[name])) ++name;
                if (name > look && name < n && text[name] == '=') break;   // next opcode starts here
            }
            ++end;
        }
        std::string value = trim(text.substr(v, end - v));
        if (key == "sample" || key == "default_path") std::replace(value.begin(), value.end(), '\\', '/');
        assign(key, value);
        if (level == Level::None) inRegion = true;   // header-less files: one implicit region
        i = end;
    }
    flush();
    if (file.regions.empty()) file.warnings.push_back("no <region> found");
    return file;
}

} // namespace winerose::presets

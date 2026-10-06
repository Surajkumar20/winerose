#include "engine/midi/Midi.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace winerose::midi {

std::uint16_t scaleMask(Scale s) noexcept
{
    auto bits = [](std::initializer_list<int> degrees) {
        std::uint16_t m = 0;
        for (int d : degrees) m = static_cast<std::uint16_t>(m | (1u << d));
        return m;
    };
    switch (s) {
        case Scale::Chromatic:       return 0x0FFF;
        case Scale::Major:           return bits({0, 2, 4, 5, 7, 9, 11});
        case Scale::Minor:           return bits({0, 2, 3, 5, 7, 8, 10});
        case Scale::HarmonicMinor:   return bits({0, 2, 3, 5, 7, 8, 11});
        case Scale::MelodicMinor:    return bits({0, 2, 3, 5, 7, 9, 11});
        case Scale::Dorian:          return bits({0, 2, 3, 5, 7, 9, 10});
        case Scale::Phrygian:        return bits({0, 1, 3, 5, 7, 8, 10});
        case Scale::Lydian:          return bits({0, 2, 4, 6, 7, 9, 11});
        case Scale::Mixolydian:      return bits({0, 2, 4, 5, 7, 9, 10});
        case Scale::Locrian:         return bits({0, 1, 3, 5, 6, 8, 10});
        case Scale::MajorPentatonic: return bits({0, 2, 4, 7, 9});
        case Scale::MinorPentatonic: return bits({0, 3, 5, 7, 10});
        case Scale::Blues:           return bits({0, 3, 5, 6, 7, 10});
        case Scale::WholeTone:       return bits({0, 2, 4, 6, 8, 10});
        case Scale::Count:           break;
    }
    return 0x0FFF;
}

int quantize(int note, int key, Scale scale, int transpose) noexcept
{
    const int n = note + transpose;
    const std::uint16_t mask = scaleMask(scale);
    auto inScale = [&](int x) { return (mask >> (((x - key) % 12 + 12) % 12)) & 1u; };
    for (int d = 0; d < 12; ++d) {
        if (inScale(n - d)) return std::clamp(n - d, 0, 127);
        if (inScale(n + d)) return std::clamp(n + d, 0, 127);
    }
    return std::clamp(n, 0, 127);
}

Clip Clip::parse(const std::string& text, double lengthBeats)
{
    Clip clip;
    clip.lengthBeats = std::max(0.25, lengthBeats);
    std::stringstream all(text);
    std::string item;
    while (std::getline(all, item, ';')) {
        double v[4];
        int got = 0;
        std::stringstream fields(item);
        std::string f;
        while (got < 4 && std::getline(fields, f, ',')) {
            char* end = nullptr;
            v[got] = std::strtod(f.c_str(), &end);
            if (end == f.c_str()) break;
            ++got;
        }
        if (got < 3) continue;
        Note n;
        n.start = std::clamp(v[0], 0.0, clip.lengthBeats);
        n.length = std::clamp(v[1], 1.0 / 64.0, 64.0);
        n.note = std::clamp(static_cast<int>(v[2]), 0, 127);
        n.velocity = got >= 4 ? std::clamp(static_cast<int>(v[3]), 1, 127) : 100;
        if (n.start < clip.lengthBeats) clip.notes.push_back(n);
    }
    std::stable_sort(clip.notes.begin(), clip.notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
    return clip;
}

std::string Clip::serialize() const
{
    std::string out;
    char buf[96];
    for (const auto& n : notes) {
        std::snprintf(buf, sizeof(buf), "%s%.6g,%.6g,%d,%d", out.empty() ? "" : ";", n.start, n.length, n.note, n.velocity);
        out += buf;
    }
    return out;
}

} // namespace winerose::midi

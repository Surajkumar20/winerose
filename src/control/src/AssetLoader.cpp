#include "AssetLoader.h"

#include "presets/AudioFile.h"
#include "presets/WavetableWav.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <cmath>
#include <map>

namespace winerose::control {

namespace {

double number(const presets::SfzFile::Region& r, const std::string& key, double fallback)
{
    const std::string v = r.get(key);
    if (v.empty()) return fallback;
    try { return std::stod(v); } catch (...) { return fallback; }
}

int note(const presets::SfzFile::Region& r, const std::string& key, int fallback)
{
    const std::string v = r.get(key);
    if (v.empty()) return fallback;
    return presets::parseSfzNote(v).value_or(fallback);
}

std::shared_ptr<const dsp::SampleData> toSampleData(const presets::AudioFile& a, std::string name)
{
    std::vector<float> left = a.channels.empty() ? std::vector<float>{} : a.channels[0];
    std::vector<float> right = a.channels.size() >= 2 ? a.channels[1] : std::vector<float>{};
    return dsp::SampleData::build(std::move(left), std::move(right), a.sampleRate, a.rootKey >= 0 ? a.rootKey : 60,
                                  a.loopStart, a.loopEnd, std::move(name));
}

} // namespace

std::shared_ptr<const dsp::SampleData> loadSampleFile(const std::filesystem::path& file, std::string& error)
{
    const auto audio = presets::AudioFile::readFile(file, error);
    if (!audio || audio->frames() == 0) {
        if (error.empty()) error = "empty audio file";
        return nullptr;
    }
    return toSampleData(*audio, file.stem().string());
}

bool isWavetableFile(const std::filesystem::path& file)
{
    std::string error;
    const auto audio = presets::AudioFile::readFile(file, error);
    return audio && audio->clm.find("<!>") != std::string::npos;
}

std::shared_ptr<const dsp::WavetableBank> loadWavetableFile(const std::filesystem::path& file, std::string& error)
{
    const auto wt = presets::Wavetable::readFile(file, error);
    if (!wt) return nullptr;
    return dsp::WavetableBank::build(wt->samples, wt->frameSize, wt->name);
}

namespace {
constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string encodeWavetable(const std::vector<float>& samples, int frameSize)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(samples.size() * 2);
    for (float v : samples) {
        const auto s = static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
        bytes.push_back(static_cast<std::uint8_t>(s & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((s >> 8) & 0xFF));
    }
    std::string out = "wt1:" + std::to_string(frameSize) + ":";
    out.reserve(out.size() + (bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16)
                              | (i + 1 < bytes.size() ? static_cast<std::uint32_t>(bytes[i + 1]) << 8 : 0u)
                              | (i + 2 < bytes.size() ? static_cast<std::uint32_t>(bytes[i + 2]) : 0u);
        out.push_back(kB64[(n >> 18) & 63]);
        out.push_back(kB64[(n >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? kB64[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < bytes.size() ? kB64[n & 63] : '=');
    }
    return out;
}

std::shared_ptr<const dsp::WavetableBank> decodeWavetable(const std::string& text, const std::string& name, std::string& error)
{
    if (text.rfind("wt1:", 0) != 0) { error = "unknown embedded wavetable format"; return nullptr; }
    const auto colon = text.find(':', 4);
    if (colon == std::string::npos) { error = "malformed embedded wavetable"; return nullptr; }
    const int frameSize = std::atoi(text.substr(4, colon - 4).c_str());
    if (frameSize < 16 || frameSize > 8192) { error = "bad frame size"; return nullptr; }
    int lookup[256];
    std::fill(std::begin(lookup), std::end(lookup), -1);
    for (int i = 0; i < 64; ++i) lookup[static_cast<unsigned char>(kB64[i])] = i;
    std::vector<std::uint8_t> bytes;
    std::uint32_t acc = 0;
    int bits = 0;
    for (std::size_t i = colon + 1; i < text.size(); ++i) {
        const int v = lookup[static_cast<unsigned char>(text[i])];
        if (v < 0) continue;   // '=' padding / whitespace
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; bytes.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFF)); }
    }
    std::vector<float> samples(bytes.size() / 2);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float>(static_cast<std::int16_t>(bytes[2 * i] | (bytes[2 * i + 1] << 8))) / 32767.0f;
    if (samples.size() < static_cast<std::size_t>(frameSize)) { error = "embedded wavetable is empty"; return nullptr; }
    samples.resize(samples.size() / static_cast<std::size_t>(frameSize) * static_cast<std::size_t>(frameSize));
    return dsp::WavetableBank::build(samples, frameSize, name);
}

std::shared_ptr<const dsp::Multisample> buildMultisample(const presets::SfzFile& sfz, const std::filesystem::path& baseDir,
                                                         std::vector<std::string>& warnings)
{
    auto ms = std::make_shared<dsp::Multisample>();
    std::map<std::string, std::shared_ptr<const dsp::SampleData>> cache;   // each file decoded once
    warnings.insert(warnings.end(), sfz.warnings.begin(), sfz.warnings.end());

    for (const auto& r : sfz.regions) {
        const std::string samplePath = r.get("sample");
        if (samplePath.empty()) { warnings.push_back("region without sample= skipped"); continue; }
        const auto full = (baseDir / std::filesystem::path(sfz.defaultPath) / std::filesystem::path(samplePath)).lexically_normal();
        auto& sample = cache[full.string()];
        if (!sample) {
            std::string error;
            sample = loadSampleFile(full, error);
            if (!sample) { warnings.push_back("cannot load " + full.string() + ": " + error); continue; }
        }

        dsp::Multisample::Region reg;
        reg.sample = sample;
        const int key = note(r, "key", -1);
        reg.loKey = note(r, "lokey", key >= 0 ? key : 0);
        reg.hiKey = note(r, "hikey", key >= 0 ? key : 127);
        reg.keyCenter = note(r, "pitch_keycenter", key >= 0 ? key : sample->rootKey());
        reg.loVel = static_cast<int>(number(r, "lovel", 1));
        reg.hiVel = static_cast<int>(number(r, "hivel", 127));
        reg.keytrack = static_cast<float>(number(r, "pitch_keytrack", 100.0));
        reg.tuneCents = static_cast<float>(number(r, "tune", 0.0) + 100.0 * number(r, "transpose", 0.0));
        reg.gain = static_cast<float>(std::pow(10.0, number(r, "volume", 0.0) / 20.0));
        reg.pan = static_cast<float>(std::clamp(number(r, "pan", 0.0) / 100.0, -1.0, 1.0));
        reg.offset = static_cast<std::int64_t>(number(r, "offset", 0.0));
        const double end = number(r, "end", -1.0);
        reg.end = end >= 0.0 ? static_cast<std::int64_t>(end) + 1 : -1;   // SFZ end is inclusive

        const std::string mode = r.get("loop_mode", r.get("loopmode"));
        const bool fileLoop = sample->loopStart() >= 0;
        if (mode == "loop_continuous") reg.loop = dsp::SamplePlayer::Loop::Forward;
        else if (mode == "loop_sustain") reg.loop = dsp::SamplePlayer::Loop::Sustain;
        else if (mode == "one_shot") { reg.loop = dsp::SamplePlayer::Loop::Off; reg.oneShot = true; }
        else if (mode == "no_loop") reg.loop = dsp::SamplePlayer::Loop::Off;
        else reg.loop = fileLoop ? dsp::SamplePlayer::Loop::Forward : dsp::SamplePlayer::Loop::Off;   // SFZ default
        const double ls = number(r, "loop_start", number(r, "loopstart", -1.0));
        const double le = number(r, "loop_end", number(r, "loopend", -1.0));
        reg.loopStart = ls >= 0.0 ? static_cast<std::int64_t>(ls) : -1;
        reg.loopEnd = le >= 0.0 ? static_cast<std::int64_t>(le) + 1 : -1;   // inclusive in SFZ
        reg.loopXfadeSeconds = number(r, "loop_crossfade", 0.0);

        // amp_velcurve_N points (linear between them); default is the SFZ velocity curve (v/127)^2.
        std::map<int, float> points;
        for (const auto& [k, v] : r.opcodes) {
            if (k.rfind("amp_velcurve_", 0) != 0) continue;
            try { points[std::clamp(std::stoi(k.substr(13)), 0, 127)] = std::stof(v); } catch (...) {}
        }
        for (int vel = 0; vel < 128; ++vel) {
            float g;
            if (points.empty()) {
                g = static_cast<float>(vel * vel) / (127.0f * 127.0f);
            } else {
                if (points.find(0) == points.end()) points[0] = 0.0f;
                if (points.find(127) == points.end()) points[127] = 1.0f;
                auto hi = points.lower_bound(vel);
                if (hi->first == vel) g = hi->second;
                else {
                    auto lo = std::prev(hi);
                    g = lo->second + (hi->second - lo->second) * static_cast<float>(vel - lo->first) / static_cast<float>(hi->first - lo->first);
                }
            }
            reg.velCurve[static_cast<std::size_t>(vel)] = g;
        }
        reg.seqLength = std::max(1, static_cast<int>(number(r, "seq_length", 1)));
        reg.seqPosition = std::clamp(static_cast<int>(number(r, "seq_position", 1)), 1, reg.seqLength);
        reg.trigger = r.get("trigger") == "release" ? dsp::Multisample::Trigger::Release : dsp::Multisample::Trigger::Attack;
        if (const std::string t = r.get("trigger"); !t.empty() && t != "attack" && t != "release")
            warnings.push_back("trigger=" + t + " treated as attack");
        ms->regions.push_back(std::move(reg));
    }
    return ms;
}

std::shared_ptr<const dsp::Multisample> loadSfzFile(const std::filesystem::path& file, std::string& error,
                                                    std::vector<std::string>& warnings)
{
    const auto bytes = presets::readBytes(file);
    if (!bytes) { error = "cannot open " + file.string(); return nullptr; }
    const auto sfz = presets::SfzFile::parse(std::string(bytes->begin(), bytes->end()));
    auto ms = buildMultisample(sfz, file.parent_path(), warnings);
    if (ms->regions.empty()) { error = "no playable regions"; return nullptr; }
    auto named = std::const_pointer_cast<dsp::Multisample>(ms);
    named->name = file.stem().string();
    return ms;
}

} // namespace winerose::control

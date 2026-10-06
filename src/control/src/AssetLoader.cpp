#include "AssetLoader.h"

#include "presets/AudioFile.h"
#include "presets/WavetableWav.h"

#include <algorithm>
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

// Phase 7 through the engine (SPEC §5.4): Sample / Multisample (SFZ) / Granular / Spectral oscillators,
// key/velocity mapping, arpeggiator, clip sequencer, key/scale, asset reload with the patch.

#include "EngineRig.h"

#include "control/Controller.h"
#include "engine/dsp/SampleData.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

using namespace winerose;
using winerose::test::midiEvent;
using winerose::test::Rig;
using Catch::Approx;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double freq, double seconds, double sr = 48000.0, double amp = 0.5)
{
    std::vector<float> x(static_cast<std::size_t>(seconds * sr));
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) / sr));
    return x;
}

// Frequency from positive-going zero crossings over a steady stretch.
double pitchOf(const std::vector<float>& x, std::size_t from, std::size_t to, double sr = 48000.0)
{
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (std::size_t i = from + 1; i < to && i < x.size(); ++i) {
        if (x[i - 1] <= 0.0f && x[i] > 0.0f) {
            const double t = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0.0) first = t;
            else ++crossings;
            last = t;
        }
    }
    return crossings > 0 ? sr * crossings / (last - first) : 0.0;
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double e = 0.0;
    for (std::size_t i = from; i < to && i < x.size(); ++i) e += static_cast<double>(x[i]) * x[i];
    return std::sqrt(e / static_cast<double>(std::max<std::size_t>(1, to - from)));
}

void writeWav(const std::filesystem::path& file, const std::vector<float>& x, int rootKey = -1, std::int64_t loopStart = -1, std::int64_t loopEnd = -1)
{
    std::vector<std::uint8_t> b;
    auto tag = [&](const char* t) { b.insert(b.end(), t, t + 4); };
    auto u32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); };
    auto u16 = [&](std::uint16_t v) { b.push_back(static_cast<std::uint8_t>(v)); b.push_back(static_cast<std::uint8_t>(v >> 8)); };
    const bool smpl = rootKey >= 0;
    const auto dataBytes = static_cast<std::uint32_t>(x.size() * 4);
    tag("RIFF");
    u32(4 + 24 + (smpl ? 8 + 60 : 0) + 8 + dataBytes);
    tag("WAVE");
    tag("fmt "); u32(16); u16(3); u16(1); u32(48000); u32(48000 * 4); u16(4); u16(32);
    if (smpl) {
        tag("smpl"); u32(60);
        for (int i = 0; i < 3; ++i) u32(0);
        u32(static_cast<std::uint32_t>(rootKey));
        for (int i = 0; i < 3; ++i) u32(0);
        u32(loopStart >= 0 ? 1 : 0); u32(0);
        u32(0); u32(0);
        u32(static_cast<std::uint32_t>(std::max<std::int64_t>(0, loopStart)));
        u32(static_cast<std::uint32_t>(std::max<std::int64_t>(0, loopEnd - 1)));
        u32(0); u32(0);
    }
    tag("data"); u32(dataBytes);
    for (float v : x) { std::uint8_t c[4]; std::memcpy(c, &v, 4); b.insert(b.end(), c, c + 4); }
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

struct Collect final : MidiEventSink {
    std::vector<std::pair<std::int64_t, MidiEvent>> events;
    std::int64_t base = 0;
    void push(const MidiEvent& e) noexcept override { events.emplace_back(base + e.sampleOffset, e); }
};

std::filesystem::path tempDir(const char* name)
{
    auto d = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

void setOsc(Rig& rig, const std::string& key, double v) { rig.reg("Oscillator0").set<double>(key, v); }

} // namespace

// --- Sample oscillator -----------------------------------------------------------------------------------

TEST_CASE("sample oscillator: keytracked pitch from the root key, and key/velocity mapping", "[phase7][sample]")
{
    Rig rig;
    rig.engine.setOscillatorSample(0, dsp::SampleData::build(sine(261.6256, 2.0), {}, 48000.0, 60));
    setOsc(rig, "type", static_cast<double>(modules::OscType::Sample));
    rig.reg("Filter0").set<bool>("enabled", false);
    const auto out = rig.renderNote(72, 0.5);
    CHECK(pitchOf(out, 4800, 24000) == Approx(523.25).epsilon(0.002));

    Rig mapped;
    mapped.engine.setOscillatorSample(0, dsp::SampleData::build(sine(261.6256, 2.0), {}, 48000.0, 60));
    mapped.reg("Oscillator0").set<double>("type", static_cast<double>(modules::OscType::Sample));
    mapped.reg("Oscillator0").set<int>("velHi", 64);   // our notes are velocity 100
    const auto silent = mapped.renderNote(72, 0.2);
    CHECK(rms(silent, 0, silent.size()) < 1e-6);
}

TEST_CASE("sample oscillator: one-shot ends, forward loop keeps sounding", "[phase7][sample]")
{
    for (const bool looping : {false, true}) {
        Rig rig;
        rig.engine.setOscillatorSample(0, dsp::SampleData::build(sine(440.0, 0.1), {}, 48000.0, 69));
        setOsc(rig, "type", static_cast<double>(modules::OscType::Sample));
        if (looping) {
            setOsc(rig, "smpLoop", static_cast<double>(dsp::SamplePlayer::Loop::Forward));
            setOsc(rig, "smpLoopStart", 0.2);
            setOsc(rig, "smpLoopEnd", 0.8);
            setOsc(rig, "smpXfade", 0.1);
        }
        const auto out = rig.renderNote(69, 0.5);
        const double tail = rms(out, 12000, 24000);   // well after the 0.1 s sample would have ended
        if (looping) CHECK(tail > 0.05);
        else         CHECK(tail < 1e-6);
    }
}

// --- SFZ multisample -------------------------------------------------------------------------------------

TEST_CASE("SFZ suite: key ranges, velocity layers, round robin, release triggers, loops", "[phase7][sfz]")
{
    const auto dir = tempDir("winerose_sfz");
    std::filesystem::create_directories(dir / "samples");
    writeWav(dir / "samples" / "low.wav", sine(200.0, 1.0));
    writeWav(dir / "samples" / "high soft.wav", sine(1000.0, 1.0));
    writeWav(dir / "samples" / "high loud.wav", sine(1500.0, 1.0));
    writeWav(dir / "samples" / "rr1.wav", sine(300.0, 1.0));
    writeWav(dir / "samples" / "rr2.wav", sine(350.0, 1.0));
    writeWav(dir / "samples" / "rel.wav", sine(2000.0, 1.0));
    writeWav(dir / "samples" / "loop.wav", sine(500.0, 0.2), 80, 2400, 7200);   // smpl root 80 + loop
    {
        std::ofstream sfz(dir / "suite.sfz");
        sfz << "// test instrument\n<control> default_path=samples/\n#define $LOW 48\n"
               "<global> volume=0\n"
               "<group> lokey=0 hikey=$LOW\n<region> sample=low.wav pitch_keycenter=48\n"
               "<group> lokey=c4 hikey=c5\n"
               "<region> sample=high soft.wav lovel=1 hivel=64 pitch_keycenter=c4\n"
               "<region> sample=high loud.wav lovel=65 hivel=127 pitch_keycenter=c4\n"
               "/* round robin */ <group> key=90 seq_length=2\n"
               "<region> sample=rr1.wav seq_position=1\n<region> sample=rr2.wav seq_position=2\n"
               "<group>\n<region> key=100 sample=rel.wav trigger=release pitch_keycenter=100\n"
               "<region> key=100 sample=low.wav pitch_keycenter=100\n"
               "<region> key=80 sample=loop.wav loop_mode=loop_continuous\n";
    }

    std::shared_ptr<ConfigManager> cm = std::make_shared<ConfigManager>();
    Engine engine(cm);
    control::Controller ctl(engine, cm);
    engine.prepare(48000.0, 512);
    const auto r = ctl.loadOscillatorFile(0, (dir / "suite.sfz").string());
    INFO(r.error << r.message);
    REQUIRE(r.ok);
    CHECK(ctl.get("Oscillator0.type").number() == static_cast<double>(modules::OscType::Multisample));
    ctl.set("Filter0.enabled", 0.0);
    ctl.set("Env0.release", 1.0);

    auto play = [&](int note, int velocity, double holdSeconds, double tailSeconds) {
        std::vector<float> out;
        float l[512], rr[512];
        float* ch[] = {l, rr};
        const int hold = static_cast<int>(holdSeconds * 48000 / 512), total = hold + static_cast<int>(tailSeconds * 48000 / 512);
        for (int b = 0; b < total; ++b) {
            std::vector<MidiEvent> ev;
            if (b == 0) ev.push_back(midiEvent(0, 0x90, static_cast<std::uint8_t>(note), static_cast<std::uint8_t>(velocity)));
            if (b == hold) ev.push_back(midiEvent(0, 0x80, static_cast<std::uint8_t>(note), 0));
            engine.process(ch, 2, 512, ev.data(), static_cast<int>(ev.size()), TransportInfo{});
            out.insert(out.end(), l, l + 512);
        }
        // let it die out before the next note
        for (int b = 0; b < 200; ++b) engine.process(ch, 2, 512, nullptr, 0, TransportInfo{});
        return out;
    };

    // Key ranges + keycenter: note 46 plays low.wav two semitones down.
    CHECK(pitchOf(play(46, 100, 0.3, 0.0), 2400, 14000) == Approx(200.0 * std::exp2(-2.0 / 12.0)).epsilon(0.003));
    // Velocity layers (note names: c4 = 60).
    CHECK(pitchOf(play(60, 40, 0.3, 0.0), 2400, 14000) == Approx(1000.0).epsilon(0.003));
    CHECK(pitchOf(play(60, 110, 0.3, 0.0), 2400, 14000) == Approx(1500.0).epsilon(0.003));
    // Round robin alternates.
    const double rr1 = pitchOf(play(90, 100, 0.3, 0.0), 2400, 14000);
    const double rr2 = pitchOf(play(90, 100, 0.3, 0.0), 2400, 14000);
    CHECK(std::abs(rr1 - rr2) > 30.0);
    CHECK(((std::abs(rr1 - 300.0) < 3.0 && std::abs(rr2 - 350.0) < 3.0) || (std::abs(rr1 - 350.0) < 3.0 && std::abs(rr2 - 300.0) < 3.0)));
    // Release trigger: rel.wav (2 kHz) only sounds after the note-off.
    const auto rel = play(100, 100, 0.3, 0.4);
    const std::size_t off = static_cast<std::size_t>(0.3 * 48000 / 512) * 512;
    CHECK(pitchOf(rel, 2400, off - 512) == Approx(200.0).epsilon(0.003));
    CHECK(pitchOf(rel, off + 1024, off + 6000) > 1500.0);
    // Loop from the smpl chunk keeps a 0.2 s sample going.
    const auto loop = play(80, 100, 1.0, 0.0);
    CHECK(rms(loop, 24000, 46000) > 0.05);
    std::filesystem::remove_all(dir);
}

// --- Granular / Spectral through the engine --------------------------------------------------------------

TEST_CASE("granular and spectral oscillators play the loaded sample at the note's pitch", "[phase7][granular][spectral]")
{
    for (const auto type : {modules::OscType::Granular, modules::OscType::Spectral}) {
        Rig rig;
        rig.engine.setOscillatorSample(0, dsp::SampleData::build(sine(220.0, 2.0), {}, 48000.0, 57));
        setOsc(rig, "type", static_cast<double>(type));
        rig.reg("Filter0").set<bool>("enabled", false);
        if (type == modules::OscType::Granular) {
            setOsc(rig, "grnSize", 100.0);
            setOsc(rig, "grnDensity", 40.0);
        }
        const auto out = rig.renderNote(69, 1.0);   // A4 = sample root + 12
        for (float v : out) REQUIRE(std::isfinite(v));
        INFO(modules::kOscTypeNames[static_cast<int>(type)]);
        CHECK(rms(out, 12000, 48000) > 0.02);
        CHECK(pitchOf(out, 12000, 48000) == Approx(440.0).epsilon(0.01));
    }
}

// --- Arp / scale / clip ----------------------------------------------------------------------------------

TEST_CASE("arpeggiator: Up pattern on the 1/16 grid, sample-accurate MIDI out", "[phase7][arp]")
{
    Rig rig;
    auto& arp = rig.reg("Arp0");
    arp.set<bool>("enabled", true);
    arp.set<double>("gate", 0.5);
    Collect out;
    std::vector<MidiEvent> chord = {midiEvent(100, 0x90, 64, 90), midiEvent(100, 0x90, 60, 90), midiEvent(100, 0x90, 67, 90)};
    for (int b = 0; b < 60; ++b) {
        out.base = b * 512;
        rig.block(b == 0 ? chord : std::vector<MidiEvent>{});
        rig.engine.drainMidiOut(out);
    }
    std::vector<std::pair<std::int64_t, int>> ons;
    for (const auto& [t, e] : out.events) if ((e.data[0] & 0xF0) == 0x90) ons.emplace_back(t, e.data[1]);
    REQUIRE(ons.size() >= 5);
    // 120 BPM: a sixteenth = 0.125 s = 6000 samples; the free clock starts at the first key (sample 100).
    const int expected[] = {60, 64, 67, 60, 64};
    for (int i = 0; i < 5; ++i) {
        CHECK(ons[static_cast<std::size_t>(i)].second == expected[i]);
        CHECK(ons[static_cast<std::size_t>(i)].first == 100 + i * 6000);
    }
    // Gate 50%: each note-off lands 3000 samples after its note-on.
    for (const auto& [t, e] : out.events)
        if ((e.data[0] & 0xF0) == 0x80 && e.data[1] == 60) { CHECK(t == 100 + 3000); break; }
}

TEST_CASE("arp output is identical whatever the host block size", "[phase7][arp]")
{
    auto render = [](int blockSize) {
        Rig rig;
        rig.reg("Arp0").set<bool>("enabled", true);
        rig.reg("Arp0").set<double>("mode", static_cast<double>(midi::ArpMode::UpDown));
        rig.reg("Arp0").set<int>("octaves", 2);
        std::vector<float> all;
        std::vector<float> l(static_cast<std::size_t>(blockSize)), r(static_cast<std::size_t>(blockSize));
        float* ch[] = {l.data(), r.data()};
        for (int pos = 0; pos < 48000; pos += blockSize) {
            std::vector<MidiEvent> ev;
            if (pos == 0) { ev.push_back(midiEvent(7, 0x90, 60, 100)); ev.push_back(midiEvent(7, 0x90, 63, 100)); }
            rig.engine.process(ch, 2, blockSize, ev.data(), static_cast<int>(ev.size()), TransportInfo{});
            all.insert(all.end(), l.begin(), l.end());
        }
        all.resize(48000);
        return all;
    };
    const auto a = render(512), b = render(97);
    CHECK(a == b);
    CHECK(rms(a, 0, a.size()) > 0.01);
}

TEST_CASE("key/scale: notes snap into the scale and release cleanly", "[phase7][scale]")
{
    Rig rig;
    auto& q = rig.reg("PitchQuantizer0");
    q.set<bool>("enabled", true);
    q.set<double>("scale", static_cast<double>(midi::Scale::Major));
    CHECK(midi::quantize(61, 0, midi::Scale::Major, 0) == 60);   // tie goes down
    CHECK(midi::quantize(66, 0, midi::Scale::Major, 0) == 65);
    CHECK(midi::quantize(63, 2, midi::Scale::Dorian, 0) == 62);  // D dorian: D E F G A B C
    rig.reg("Filter0").set<bool>("enabled", false);
    rig.block({midiEvent(0, 0x90, 61, 100)});
    for (int b = 0; b < 10; ++b) rig.block();
    CHECK(rig.engine.activeVoiceCount() == 1);
    rig.block({midiEvent(0, 0x80, 61, 0)});
    for (int b = 0; b < 400; ++b) rig.block();
    CHECK(rig.engine.activeVoiceCount() == 0);   // the quantized note was released
}

TEST_CASE("clip sequencer: key-triggered and transposed, loops, stops on release", "[phase7][clip]")
{
    Rig rig;
    rig.reg("MidiClip0").set<std::string>("notes", "0,0.5,60,100;1,0.5,64,100");
    rig.reg("MidiClip0").set<double>("length", 2.0);
    rig.engine.publishSnapshot();
    rig.reg("ClipPlayer").set<bool>("enabled", true);
    Collect out;
    for (int b = 0; b < 200; ++b) {   // 102,400 samples > two loops (2 beats = 48,000 samples)
        out.base = b * 512;
        std::vector<MidiEvent> ev;
        if (b == 0) ev.push_back(midiEvent(0, 0x90, 62, 127));
        if (b == 150) ev.push_back(midiEvent(0, 0x80, 62, 0));
        rig.block(ev);
        rig.engine.drainMidiOut(out);
    }
    std::vector<std::pair<std::int64_t, int>> ons;
    for (const auto& [t, e] : out.events) if ((e.data[0] & 0xF0) == 0x90) ons.emplace_back(t, e.data[1]);
    REQUIRE(ons.size() >= 4);
    CHECK(ons[0] == std::pair<std::int64_t, int>{0, 62});
    CHECK(ons[1] == std::pair<std::int64_t, int>{24000, 66});
    CHECK(ons[2] == std::pair<std::int64_t, int>{48000, 62});
    CHECK(ons[3] == std::pair<std::int64_t, int>{72000, 66});
    for (const auto& on : ons) CHECK(on.first < 150 * 512);   // nothing after the key went up
    for (int b = 0; b < 300; ++b) rig.block();
    CHECK(rig.engine.activeVoiceCount() == 0);
}

TEST_CASE("clip sequencer: host-synced playback follows the transport position", "[phase7][clip]")
{
    Rig rig;
    rig.reg("MidiClip3").set<std::string>("notes", "0.5,0.25,70,100");
    rig.reg("MidiClip3").set<double>("length", 1.0);
    rig.engine.publishSnapshot();
    auto& player = rig.reg("ClipPlayer");
    player.set<bool>("enabled", true);
    player.set<int>("clip", 3);
    player.set<double>("trigger", static_cast<double>(midi::ClipTrigger::Host));
    Collect out;
    float l[512], r[512];
    float* ch[] = {l, r};
    for (int b = 0; b < 120; ++b) {
        TransportInfo t;
        t.isPlaying = true;
        t.bpm = 120.0;
        t.ppqPosition = 10.0 + b * 512.0 / 24000.0;   // starts at beat 10
        out.base = b * 512;
        rig.engine.process(ch, 2, 512, nullptr, 0, t);
        rig.engine.drainMidiOut(out);
    }
    std::vector<std::int64_t> ons;
    for (const auto& [time, e] : out.events) if ((e.data[0] & 0xF0) == 0x90) ons.push_back(time);
    REQUIRE(ons.size() >= 2);
    CHECK(ons[0] == 12000);   // beat 10.5
    CHECK(ons[1] == 36000);   // beat 11.5
}

// --- Assets persist with the patch -----------------------------------------------------------------------

TEST_CASE("oscillator files are stored in the patch and reloaded with it", "[phase7][assets]")
{
    const auto dir = tempDir("winerose_assets7");
    writeWav(dir / "pad.wav", sine(330.0, 0.5));
    std::shared_ptr<ConfigManager> cm = std::make_shared<ConfigManager>();
    Engine engine(cm);
    control::Controller ctl(engine, cm);
    const auto r = ctl.loadOscillatorFile(1, (dir / "pad.wav").string());
    REQUIRE(r.ok);
    CHECK(ctl.get("Oscillator1.type").number() == static_cast<double>(modules::OscType::Sample));
    REQUIRE(engine.oscillatorSample(1) != nullptr);
    const std::string state = ctl.saveState();

    std::shared_ptr<ConfigManager> cm2 = std::make_shared<ConfigManager>();
    Engine engine2(cm2);
    control::Controller ctl2(engine2, cm2);
    REQUIRE(ctl2.loadState(state).ok);
    REQUIRE(engine2.oscillatorSample(1) != nullptr);
    CHECK(engine2.oscillatorSample(1)->frames() == 24000);

    // A patch without the file clears it again.
    std::shared_ptr<ConfigManager> blankCm = std::make_shared<ConfigManager>();
    Engine blank(blankCm);
    control::Controller blankCtl(blank, blankCm);
    REQUIRE(ctl2.loadState(blankCtl.saveState()).ok);
    CHECK(engine2.oscillatorSample(1) == nullptr);
    std::filesystem::remove_all(dir);
}

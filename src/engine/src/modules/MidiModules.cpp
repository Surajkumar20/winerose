#include "engine/modules/MidiModules.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace winerose::modules {

namespace {

template <std::size_t N>
std::vector<EnumChoice> choices(const char* const (&names)[N])
{
    std::vector<EnumChoice> c;
    for (std::size_t i = 0; i < N; ++i) c.push_back({static_cast<int>(i), names[i]});
    return c;
}

int asInt(float v) noexcept { return static_cast<int>(std::lround(v)); }
bool asBool(float v) noexcept { return v >= 0.5f; }
template <typename E>
E asEnum(float v, E count) noexcept { return static_cast<E>(std::clamp(asInt(v), 0, static_cast<int>(count) - 1)); }

} // namespace

MidiModules::MidiModules(std::shared_ptr<ConfigManager> config)
    : m_scale(std::make_unique<ParamRegistry>(config, scale_keys::module))
    , m_arp(std::make_unique<ParamRegistry>(config, arp_keys::module))
    , m_clipPlayer(std::make_unique<ParamRegistry>(config, clip_keys::module))
{
    const std::string gs = "Key & Scale", ga = "Arp", gc = "Clip";
    auto& s = *m_scale;
    s.registerBool(scale_keys::enabled, false, gs, "Quantize incoming and generated notes to a scale");
    s.registerEnum(scale_keys::key, choices(midi::kKeyNames), 0, gs, "Key (root of the scale)");
    s.registerEnum(scale_keys::scale, choices(midi::kScaleNames), 0, gs, "Scale");
    s.registerInt (scale_keys::transpose, 0, -24, 24, gs, "Transpose before quantizing", ParamOpts{.unit = "st"});

    auto& a = *m_arp;
    a.registerBool (arp_keys::enabled, false, ga, "Arpeggiator on/off");
    a.registerEnum (arp_keys::mode, choices(midi::kArpModeNames), 0, ga, "Pattern");
    a.registerEnum (arp_keys::rate, choices(midi::kDivisionNames), static_cast<int>(midi::Division::D16), ga, "Step length (tempo-synced)");
    a.registerInt  (arp_keys::octaves, 1, 1, 4, ga, "Octave range");
    a.registerFloat(arp_keys::gate, 0.5f, 0.05f, 1.0f, ga, "Note length (fraction of a step; 100% = legato)");
    a.registerFloat(arp_keys::swing, 0.0f, 0.0f, 1.0f, ga, "Swing (delays every other step)");
    a.registerFloat(arp_keys::chance, 1.0f, 0.0f, 1.0f, ga, "Probability that a step plays");
    a.registerEnum (arp_keys::velocityMode, choices(midi::kVelocityModeNames), 0, ga, "Velocity source");
    a.registerInt  (arp_keys::velocity, 100, 1, 127, ga, "Velocity (Fixed / Ramp peak)");
    a.registerBool (arp_keys::latch, false, ga, "Keep playing after the keys are released");
    a.registerInt  (arp_keys::transpose, 0, -24, 24, ga, "Transpose the arpeggio", ParamOpts{.unit = "st"});

    auto& c = *m_clipPlayer;
    c.registerBool (clip_keys::enabled, false, gc, "Clip sequencer on/off (takes priority over the arp)");
    c.registerInt  (clip_keys::clip, 0, 0, midi::kClipSlots - 1, gc, "Active clip slot");
    c.registerEnum (clip_keys::trigger, choices(midi::kClipTriggerNames), 0, gc, "Start the clip from a key (transposed) or from the host transport");
    c.registerFloat(clip_keys::swing, 0.0f, 0.0f, 1.0f, gc, "Swing on odd sixteenths");
    for (int i = 0; i < midi::kClipSlots; ++i) {
        auto& r = m_clips[static_cast<std::size_t>(i)];
        r = std::make_unique<ParamRegistry>(config, "MidiClip" + std::to_string(i));
        r->registerString(clip_keys::notes, "", gc, "Notes: start,length,note,velocity;... (beats)");
        r->registerFloat(clip_keys::length, 4.0f, 0.25f, 64.0f, gc, "Loop length", ParamOpts{.unit = "beats", .automatable = false});
    }

    m_scaleOn = s.handle(scale_keys::enabled);       m_key = s.handle(scale_keys::key);
    m_scaleType = s.handle(scale_keys::scale);       m_transpose = s.handle(scale_keys::transpose);
    m_arpOn = a.handle(arp_keys::enabled);           m_arpMode = a.handle(arp_keys::mode);
    m_arpRate = a.handle(arp_keys::rate);            m_arpOctaves = a.handle(arp_keys::octaves);
    m_arpGate = a.handle(arp_keys::gate);            m_arpSwing = a.handle(arp_keys::swing);
    m_arpChance = a.handle(arp_keys::chance);        m_arpVelMode = a.handle(arp_keys::velocityMode);
    m_arpVelocity = a.handle(arp_keys::velocity);    m_arpLatch = a.handle(arp_keys::latch);
    m_arpTranspose = a.handle(arp_keys::transpose);
    m_clipOn = c.handle(clip_keys::enabled);         m_clipSlot = c.handle(clip_keys::clip);
    m_clipTrigger = c.handle(clip_keys::trigger);    m_clipSwing = c.handle(clip_keys::swing);
}

midi::Settings MidiModules::read() const noexcept
{
    midi::Settings s;
    s.scaleOn = asBool(m_scaleOn.load());
    s.key = std::clamp(asInt(m_key.load()), 0, 11);
    s.scale = asEnum(m_scaleType.load(), midi::Scale::Count);
    s.transpose = asInt(m_transpose.load());
    s.arpOn = asBool(m_arpOn.load());
    s.arpMode = asEnum(m_arpMode.load(), midi::ArpMode::Count);
    s.arpStepBeats = midi::kDivisionBeats[static_cast<std::size_t>(asEnum(m_arpRate.load(), midi::Division::Count))];
    s.arpOctaves = std::clamp(asInt(m_arpOctaves.load()), 1, 4);
    s.arpGate = m_arpGate.load();
    s.arpSwing = m_arpSwing.load();
    s.arpChance = m_arpChance.load();
    s.arpVelocityMode = asEnum(m_arpVelMode.load(), midi::VelocityMode::Count);
    s.arpVelocity = asInt(m_arpVelocity.load());
    s.arpLatch = asBool(m_arpLatch.load());
    s.arpTranspose = asInt(m_arpTranspose.load());
    s.clipOn = asBool(m_clipOn.load());
    s.clip = std::clamp(asInt(m_clipSlot.load()), 0, midi::kClipSlots - 1);
    s.clipTrigger = asEnum(m_clipTrigger.load(), midi::ClipTrigger::Count);
    s.clipSwing = m_clipSwing.load();
    return s;
}

std::string MidiModules::clipText(int slot) const
{
    return m_clips[static_cast<std::size_t>(slot)]->get<std::string>(clip_keys::notes);
}

double MidiModules::clipLength(int slot) const
{
    return m_clips[static_cast<std::size_t>(slot)]->get<double>(clip_keys::length);
}

} // namespace winerose::modules

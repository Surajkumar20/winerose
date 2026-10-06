#include "Pages.h"

#include "Theme.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace winerose::ui {

namespace {

void drawPanel(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
{
    const auto r = area.toFloat().reduced(2.0f);
    g.setColour(colours::panel);
    g.fillRoundedRectangle(r, 6.0f);
    g.setColour(colours::edge);
    g.drawRoundedRectangle(r, 6.0f, 1.0f);
    if (title.isNotEmpty()) {
        g.setColour(colours::panelHi);
        g.fillRoundedRectangle(r.withHeight(26.0f), 6.0f);
        g.setColour(colours::gold);
        g.setFont(juce::FontOptions(13.5f, juce::Font::bold));
        g.drawText(title, area.withHeight(28).withTrimmedLeft(12), juce::Justification::centredLeft);
    }
}

// Oscillator controls per type (keys within OscillatorN), 5 per row.
std::vector<std::string> oscKeys(int type)
{
    const std::vector<std::string> common = {"level", "pan", "octave", "semi", "fine"};
    std::vector<std::string> extra;
    switch (type) {
        case 1: extra = {"smpStart", "smpEnd", "smpLoop", "smpLoopStart", "smpLoopEnd", "smpXfade", "smpFileLoop", "coarse", "route", "filterBalance"}; break;
        case 2: extra = {"keyLo", "keyHi", "velLo", "velHi", "coarse", "route", "filterBalance", "bus1Send", "bus2Send"}; break;
        case 3: extra = {"grnPos", "grnScan", "grnSize", "grnDensity", "grnWindow", "grnPosRand", "grnPitchRand", "grnPanRand", "grnWindowAmt", "route"}; break;
        case 4: extra = {"spcPos", "spcScan", "spcTimbre", "spcFormant", "spcTransients", "spcLowCut", "spcHighCut", "coarse", "route", "filterBalance"}; break;
        default: extra = {"wtPos", "unison", "uniDetune", "uniBlend", "uniWidth", "warp1Mode", "warp1Amount", "warp2Mode", "warp2Amount", "route"}; break;
    }
    std::vector<std::string> keys = common;
    keys.insert(keys.end(), extra.begin(), extra.end());
    return keys;
}

} // namespace

// --- HSlider ---------------------------------------------------------------------------------------------

HSlider::HSlider(ParamHub& hub, std::string nsKey) : ParamControl(hub, std::move(nsKey))
{
    m_slider.setSliderStyle(juce::Slider::LinearHorizontal);
    m_slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    m_slider.setRange(0.0, 1.0, 0.0);
    m_slider.setScrollWheelEnabled(false);
    m_slider.addListener(this);
    if (m_schema != nullptr) {
        m_slider.setDoubleClickReturnValue(true, m_hub.controller().toNormalized(m_key, m_schema->defaultValue.number()));
        m_slider.getProperties().set("bipolar", m_schema->min < 0.0 && m_schema->max > 0.0);
    }
    addAndMakeVisible(m_slider);
    m_value.setFont(juce::FontOptions(11.5f));
    m_value.setJustificationType(juce::Justification::centredRight);
    m_value.setEditable(false, true, false);
    m_value.onTextChange = [this] { m_hub.controller().modify(m_key, m_value.getText().toStdString()); paramChanged(); };
    addAndMakeVisible(m_value);
    paramChanged();
}

void HSlider::resized()
{
    auto r = getLocalBounds();
    m_value.setBounds(r.removeFromRight(std::min(54, r.getWidth() / 3)));
    m_slider.setBounds(r);
}

void HSlider::paramChanged()
{
    m_updating = true;
    m_slider.setValue(m_hub.controller().toNormalized(m_key, m_hub.number(m_key)), juce::dontSendNotification);
    m_updating = false;
    m_value.setText(m_hub.controller().format(m_key, m_hub.number(m_key)), juce::dontSendNotification);
}

void HSlider::sliderValueChanged(juce::Slider*)
{
    if (!m_updating) m_hub.controller().set(m_key, m_hub.controller().fromNormalized(m_key, m_slider.getValue()));
}

// --- OscPanel --------------------------------------------------------------------------------------------

OscPanel::OscPanel(ParamHub& hub, int index, std::function<void(const juce::String&)> status)
    : ModulePanel(hub, juce::String("OSC ") + juce::String::charToString(static_cast<juce::juce_wchar>('A' + index)), "Oscillator" + std::to_string(index), 5)
    , m_index(index)
    , m_view(hub, index)
    , m_status(std::move(status))
{
    m_curve.setTooltip("Edit the Remap curve used by the Remap warps");
    m_curve.onClick = [this] {
        auto editor = std::make_unique<CurveEditor>(m_hub, m_module + ".remapCurve");
        editor->setSize(260, 180);
        juce::CallOutBox::launchAsynchronously(std::move(editor), m_curve.getScreenBounds(), nullptr);
    };
    addAndMakeVisible(m_curve);
    m_type = std::make_unique<Choice>(hub, m_module + ".type", false);
    addAndMakeVisible(*m_type);
    m_load.setTooltip("Load a wavetable, sample (.wav) or SFZ instrument onto this oscillator");
    m_load.onClick = [this] { chooseFile(); };
    addAndMakeVisible(m_load);
    m_file.setFont(juce::FontOptions(11.5f));
    m_file.setColour(juce::Label::textColourId, colours::textDim);
    m_file.setMinimumHorizontalScale(0.7f);
    addAndMakeVisible(m_file);
    m_hub.add(m_module + ".type", this);
    m_anyId = m_hub.onAny([this](const control::ParamChange& c) {
        if (c.everything || c.nsKey.rfind(m_module + ".", 0) == 0) updateFileName();
    });
    setTop(&m_view, 44);   // after the header widgets exist (it lays the panel out)
    handleAsyncUpdate();
}

OscPanel::~OscPanel()
{
    cancelPendingUpdate();
    m_hub.remove(m_module + ".type", this);
    m_hub.removeAny(m_anyId);
}

void OscPanel::handleAsyncUpdate()
{
    setKeys(oscKeys(static_cast<int>(std::lround(m_hub.number(m_module + ".type")))));
    updateFileName();
}

void OscPanel::updateFileName()
{
    const int type = static_cast<int>(std::lround(m_hub.number(m_module + ".type")));
    const char* key = type == 0 ? ".wavetablePath" : (type == 2 ? ".multisamplePath" : ".samplePath");
    const juce::File f(juce::String(m_hub.text(m_module + key)));
    m_file.setText(f.getFullPathName().isEmpty() ? juce::String(type == 0 ? "built-in" : "no file") : f.getFileNameWithoutExtension(),
                   juce::dontSendNotification);
}

void OscPanel::layoutHeader(juce::Rectangle<int>& header)
{
    m_type->setBounds(header.removeFromLeft(100).reduced(0, 1));
    header.removeFromLeft(4);
    m_load.setBounds(header.removeFromRight(44).reduced(0, 2));
    const bool wavetable = std::lround(m_hub.number(m_module + ".type")) == 0;
    m_curve.setVisible(wavetable);
    if (wavetable) m_curve.setBounds(header.removeFromRight(44).reduced(1, 2));
    m_file.setBounds(header.reduced(2, 0));
}

void OscPanel::chooseFile()
{
    m_chooser = std::make_unique<juce::FileChooser>("Load onto " + m_title, juce::File(), "*.wav;*.sfz");
    m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
        const auto file = fc.getResult();
        if (!file.existsAsFile()) return;
        const auto r = m_hub.controller().loadOscillatorFile(m_index, file.getFullPathName().toStdString());
        if (m_status) m_status(r.ok ? juce::String(r.message) + " on " + m_title : "Could not load " + file.getFileName() + ": " + juce::String(r.error));
        updateFileName();
    });
}

// --- TabbedModulePanel -----------------------------------------------------------------------------------

TabbedModulePanel::TabbedModulePanel(ParamHub& hub, juce::String title, std::string prefix, int count, int columns, std::vector<std::string> exclude)
    : ModulePanel(hub, std::move(title), prefix + "0", columns), m_prefix(std::move(prefix)), m_exclude(std::move(exclude))
{
    juce::StringArray names;
    for (int i = 0; i < count; ++i) names.add(juce::String(i + 1));
    m_tabs.setTabs(names);
    m_tabs.onSelect = [this](int i) { select(i); };
    addAndMakeVisible(m_tabs);
    select(0);
}

void TabbedModulePanel::select(int i)
{
    std::vector<std::string> keys;
    for (const auto& k : m_hub.keysOf(m_prefix + std::to_string(i)))
        if (std::find(m_exclude.begin(), m_exclude.end(), k) == m_exclude.end()) keys.push_back(k);
    m_module = m_prefix + std::to_string(i);
    setKeys(keys);
    if (onSelected) onSelected(i);
}

void TabbedModulePanel::layoutHeader(juce::Rectangle<int>& header)
{
    m_tabs.setBounds(header.reduced(0, 2));
}

// --- MacroPanel ------------------------------------------------------------------------------------------

MacroPanel::MacroPanel(ParamHub& hub) : m_hub(hub)
{
    for (int i = 0; i < 8; ++i) {
        auto k = std::make_unique<Knob>(hub, "Macro" + std::to_string(i) + ".value");
        addAndMakeVisible(*k);
        m_knobs.push_back(std::move(k));
    }
    auto names = [this] {
        for (int i = 0; i < 8; ++i) {
            const std::string n = m_hub.text("Macro" + std::to_string(i) + ".name");
            m_knobs[static_cast<std::size_t>(i)]->setCaption(n.empty() ? juce::String("Macro ") + juce::String(i + 1) : juce::String(n));
        }
    };
    names();
    m_anyId = m_hub.onAny([names](const control::ParamChange& c) {
        if (c.everything || (c.nsKey.rfind("Macro", 0) == 0 && c.nsKey.find(".name") != std::string::npos)) names();
    });
}

void MacroPanel::paint(juce::Graphics& g) { drawPanel(g, getLocalBounds(), "MACROS"); }

void MacroPanel::resized()
{
    auto r = getLocalBounds().reduced(8, 4);
    r.removeFromTop(26);
    const int w = r.getWidth() / 8;
    for (auto& k : m_knobs) k->setBounds(r.removeFromLeft(w));
}

// --- SoundPage -------------------------------------------------------------------------------------------

SoundPage::SoundPage(ParamHub& hub, std::function<void(const juce::String&)> status)
{
    auto keep = [this](auto ptr) { auto* raw = ptr.get(); addAndMakeVisible(*raw); m_parts.push_back(std::move(ptr)); return raw; };
    m_oscA = keep(std::make_unique<OscPanel>(hub, 0, status));
    m_oscB = keep(std::make_unique<OscPanel>(hub, 1, status));
    m_oscC = keep(std::make_unique<OscPanel>(hub, 2, status));
    m_noise = keep(std::make_unique<ModulePanel>(hub, "NOISE", "Oscillator3", 5));
    m_noise->setKeys({"type", "level", "pan", "pitch", "route"});
    m_sub = keep(std::make_unique<ModulePanel>(hub, "SUB", "Oscillator4", 5));
    m_sub->setKeys({"shape", "octave", "level", "pan", "route"});
    m_f1 = keep(std::make_unique<ModulePanel>(hub, "FILTER 1", "Filter0", 5));
    m_f2 = keep(std::make_unique<ModulePanel>(hub, "FILTER 2", "Filter1", 5));
    const std::vector<std::string> filterKeys = {"type", "cutoff", "resonance", "drive", "var", "mix", "keytrack", "level", "stereo", "output", "clean", "x", "y"};
    m_f1->setKeys(filterKeys);
    m_f2->setKeys(filterKeys);
    m_env = keep(std::make_unique<TabbedModulePanel>(hub, "ENV", "Env", 4, 4));
    m_lfo = keep(std::make_unique<TabbedModulePanel>(hub, "LFO", "LFO", 10, 5, std::vector<std::string>{"path"}));
    m_envView = std::make_unique<EnvelopeView>(hub);
    m_env->setTop(m_envView.get(), 82);
    m_env->onSelected = [this](int i) { m_envView->setModule("Env" + std::to_string(i)); };
    m_lfoPath = std::make_unique<CurveEditor>(hub, "LFO0.path");
    m_lfoPath->inactiveNote = "Set Shape to Path to draw this LFO";
    m_lfoPath->setTooltip("Drag points; double-click to add or remove; Alt-drag a segment to bend it; Shift snaps");
    m_lfoPath->isActive = [this, &hub] { return std::lround(hub.number("LFO" + std::to_string(m_lfo->selected()) + ".shape")) == 0; };
    m_lfo->setTop(m_lfoPath.get(), 92);
    m_lfo->onSelected = [this](int i) { m_lfoPath->setKey("LFO" + std::to_string(i) + ".path"); };
    m_macros = keep(std::make_unique<MacroPanel>(hub));
    m_global = keep(std::make_unique<ModulePanel>(hub, "VOICE", "Global", 4));
    m_global->setKeys({"polyphony", "quality", "bendUp", "bendDown"});
    // Filter routing sits in the global strip: it belongs to both filters.
    auto routing = std::make_unique<Choice>(hub, "Routing.filterRouting");
    routing->setCaption("Filter Routing");
    m_parts.push_back(std::move(routing));
    addAndMakeVisible(*m_parts.back());
}

void SoundPage::resized()
{
    auto r = getLocalBounds().reduced(4);
    auto top = r.removeFromTop(282);
    const int oscW = (top.getWidth() - 300) / 3;
    m_oscA->setBounds(top.removeFromLeft(oscW));
    m_oscB->setBounds(top.removeFromLeft(oscW));
    m_oscC->setBounds(top.removeFromLeft(oscW));
    m_noise->setBounds(top.removeFromTop(top.getHeight() / 2));
    m_sub->setBounds(top);

    auto mid = r.removeFromTop(282);
    const int w = mid.getWidth() / 4;
    m_f1->setBounds(mid.removeFromLeft(w));
    m_f2->setBounds(mid.removeFromLeft(w));
    m_env->setBounds(mid.removeFromLeft(w));
    m_lfo->setBounds(mid);

    auto bottom = r;
    m_macros->setBounds(bottom.removeFromLeft(bottom.getWidth() * 3 / 5));
    auto glob = bottom;
    m_parts.back()->setBounds(glob.removeFromRight(130).reduced(6, 30));
    m_global->setBounds(glob);
}

// --- MatrixPage ------------------------------------------------------------------------------------------

namespace {

class DestinationButton final : public ParamControl {
public:
    DestinationButton(ParamHub& hub, std::string nsKey) : ParamControl(hub, std::move(nsKey))
    {
        m_button.onClick = [this] { showMenu(); };
        addAndMakeVisible(m_button);
        paramChanged();
    }
    void resized() override { m_button.setBounds(getLocalBounds()); }
    void paramChanged() override
    {
        const std::string dest = m_hub.text(m_key);
        m_button.setButtonText(dest.empty() ? juce::String("(none)") : juce::String(dest.substr(0, dest.find('.'))) + "  " + juce::String(m_hub.label(dest)));
    }

private:
    void showMenu()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "(none)");
        std::map<std::string, juce::PopupMenu> byModule;
        std::vector<std::string> order;
        m_targets.clear();
        for (const auto& s : m_hub.all()) {
            if (!s.modulatable) continue;
            if (byModule.find(s.module) == byModule.end()) order.push_back(s.module);
            m_targets.push_back(s.nsKey);
            byModule[s.module].addItem(static_cast<int>(m_targets.size()) + 1, m_hub.label(s.nsKey), true, m_hub.text(m_key) == s.nsKey);
        }
        for (const auto& m : order) menu.addSubMenu(m, byModule[m]);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&m_button), [this](int id) {
            if (id == 1) m_hub.controller().set(m_key, std::string());
            else if (id >= 2 && id - 2 < static_cast<int>(m_targets.size())) m_hub.controller().set(m_key, m_targets[static_cast<std::size_t>(id - 2)]);
        });
    }
    juce::TextButton m_button;
    std::vector<std::string> m_targets;
};

class SlotRow final : public juce::Component {
public:
    SlotRow(ParamHub& hub, int slot) : m_slot(slot)
    {
        const std::string m = "ModSlot" + std::to_string(slot) + ".";
        add(std::make_unique<Switch>(hub, m + "bypass", false));
        add(std::make_unique<Choice>(hub, m + "source", false));
        add(std::make_unique<DestinationButton>(hub, m + "destination"));
        add(std::make_unique<HSlider>(hub, m + "amount"));
        add(std::make_unique<Switch>(hub, m + "bipolar", false));
        add(std::make_unique<HSlider>(hub, m + "curve"));
        add(std::make_unique<Choice>(hub, m + "aux", false));
        add(std::make_unique<HSlider>(hub, m + "auxAmount"));
        add(std::make_unique<HSlider>(hub, m + "output"));
    }
    void paint(juce::Graphics& g) override
    {
        g.setColour(m_slot % 2 == 0 ? colours::panel : colours::panelHi.withAlpha(0.5f));
        g.fillRect(getLocalBounds());
        g.setColour(colours::textDim);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(juce::String(m_slot + 1), getLocalBounds().removeFromLeft(30), juce::Justification::centred);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced(2, 3);
        r.removeFromLeft(30);
        static constexpr int widths[] = {40, 150, 250, 170, 40, 120, 150, 130, 0};
        for (std::size_t i = 0; i < m_parts.size(); ++i) {
            const int w = widths[i] == 0 ? r.getWidth() : widths[i];
            m_parts[i]->setBounds(r.removeFromLeft(w).reduced(3, 0));
        }
    }

private:
    void add(std::unique_ptr<juce::Component> c) { addAndMakeVisible(*c); m_parts.push_back(std::move(c)); }
    int m_slot;
    std::vector<std::unique_ptr<juce::Component>> m_parts;
};

} // namespace

MatrixPage::MatrixPage(ParamHub& hub) : m_hub(hub)
{
    for (int s = 0; s < 64; ++s) {
        if (m_hub.schema("ModSlot" + std::to_string(s) + ".source") == nullptr) break;
        auto row = std::make_unique<SlotRow>(hub, s);
        m_rows.addAndMakeVisible(*row);
        m_slots.push_back(std::move(row));
    }
    m_view.setViewedComponent(&m_rows, false);
    m_view.setScrollBarsShown(true, false);
    m_view.setScrollBarThickness(10);
    addAndMakeVisible(m_view);
}

void MatrixPage::paint(juce::Graphics& g)
{
    drawPanel(g, getLocalBounds().reduced(4), "MOD MATRIX");
    auto r = getLocalBounds().reduced(10, 0).withY(34).withHeight(18);
    g.setColour(colours::textDim);
    g.setFont(juce::FontOptions(12.0f));
    r.removeFromLeft(32);
    const char* heads[] = {"Off", "Source", "Destination", "Amount", "Bi", "Curve", "Aux source", "Aux amount", "Output"};
    const int widths[] = {40, 150, 250, 170, 40, 120, 150, 130, 140};
    for (int i = 0; i < 9; ++i) g.drawText(heads[i], r.removeFromLeft(widths[i]).reduced(4, 0), juce::Justification::centredLeft);
}

void MatrixPage::resized()
{
    m_view.setBounds(getLocalBounds().reduced(8).withTrimmedTop(48));
    const int rowH = 32;
    m_rows.setSize(m_view.getWidth() - 12, rowH * static_cast<int>(m_slots.size()));
    for (std::size_t i = 0; i < m_slots.size(); ++i) m_slots[i]->setBounds(0, static_cast<int>(i) * rowH, m_rows.getWidth(), rowH);
}

// --- FxPage ----------------------------------------------------------------------------------------------

namespace {

class FxSlotRow final : public juce::Component {
public:
    FxSlotRow(ParamHub& hub, int rack, int slot, std::function<void()> select, bool selected)
        : m_enable(hub, "FXRack" + std::to_string(rack) + "Slot" + std::to_string(slot) + ".enabled", false)
        , m_type(hub, "FXRack" + std::to_string(rack) + "Slot" + std::to_string(slot) + ".type", false)
        , m_slot(slot), m_selected(selected)
    {
        m_edit.setButtonText(juce::String(slot + 1));
        m_edit.setToggleState(selected, juce::dontSendNotification);
        m_edit.onClick = std::move(select);
        addAndMakeVisible(m_edit);
        addAndMakeVisible(m_enable);
        addAndMakeVisible(m_type);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced(4, 4);
        m_edit.setBounds(r.removeFromLeft(34));
        r.removeFromLeft(6);
        m_enable.setBounds(r.removeFromLeft(40));
        m_type.setBounds(r);
    }

private:
    juce::TextButton m_edit;
    Switch m_enable;
    Choice m_type;
    int m_slot;
    bool m_selected;
};

} // namespace

FxPage::FxPage(ParamHub& hub) : m_hub(hub)
{
    m_racks.setTabs({"Main", "Bus 1", "Bus 2"});
    m_racks.onSelect = [this](int r) { showRack(r); };
    addAndMakeVisible(m_racks);
    m_mixer = std::make_unique<ModulePanel>(hub, "MIX", "Mixer", 3);
    m_mixer->setKeys(m_hub.keysOf("Mixer"));
    addAndMakeVisible(*m_mixer);
    m_anyId = m_hub.onAny([this](const control::ParamChange& c) {
        const std::string prefix = "FXRack" + std::to_string(m_rack) + "Slot" + std::to_string(m_slot) + ".type";
        if (c.everything || c.nsKey == prefix) triggerAsyncUpdate();
    });
    showRack(0);
}

FxPage::~FxPage()
{
    cancelPendingUpdate();
    m_hub.removeAny(m_anyId);
}

void FxPage::handleAsyncUpdate() { showSlot(m_slot); }

void FxPage::showRack(int rack)
{
    m_rack = rack;
    m_slotRows.clear();
    for (int s = 0; s < 8; ++s) {
        auto row = std::make_unique<FxSlotRow>(m_hub, rack, s, [this, s] { showSlot(s); }, s == m_slot);
        addAndMakeVisible(*row);
        m_slotRows.push_back(std::move(row));
    }
    showSlot(m_slot);
}

void FxPage::showSlot(int slot)
{
    m_slot = slot;
    const std::string module = "FXRack" + std::to_string(m_rack) + "Slot" + std::to_string(slot);
    const int type = static_cast<int>(std::lround(m_hub.number(module + ".type")));
    const auto* schema = m_hub.schema(module + ".type");
    juce::String typeName = "Empty";
    if (schema != nullptr)
        for (const auto& [v, n] : schema->choices) if (v == type) typeName = n;
    m_detail = std::make_unique<ModulePanel>(m_hub, juce::String("SLOT ") + juce::String(slot + 1) + "  " + typeName.toUpperCase(), module, 5);
    std::vector<std::string> keys;
    if (type != 0) {
        keys.push_back("mix");
        for (int p = 0; p < 8; ++p) {
            // Only the knobs this effect uses (unused ones are labelled "-").
            const std::string k = "p" + std::to_string(p);
            if (m_hub.controller().label(module + "." + k).find("(unused)") == std::string::npos) keys.push_back(k);
        }
    }
    m_detail->setKeys(keys);
    addAndMakeVisible(*m_detail);
    for (std::size_t i = 0; i < m_slotRows.size(); ++i)
        if (auto* row = dynamic_cast<FxSlotRow*>(m_slotRows[i].get())) {
            juce::ignoreUnused(row);
        }
    resized();
    repaint();
}

void FxPage::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().reduced(4);
    drawPanel(g, r.removeFromLeft(360), "FX RACK");
}

void FxPage::resized()
{
    auto r = getLocalBounds().reduced(4);
    auto left = r.removeFromLeft(360).reduced(8);
    left.removeFromTop(26);
    m_racks.setBounds(left.removeFromTop(28));
    left.removeFromTop(6);
    for (auto& row : m_slotRows) row->setBounds(left.removeFromTop(40));
    r.removeFromLeft(6);
    if (m_mixer) m_mixer->setBounds(r.removeFromBottom(120));
    if (m_detail) m_detail->setBounds(r.removeFromTop(std::min(r.getHeight(), 260)));
}

// --- ClipEditor ------------------------------------------------------------------------------------------

ClipEditor::ClipEditor(ParamHub& hub) : m_hub(hub)
{
    load();
    m_anyId = m_hub.onAny([this](const control::ParamChange& c) {
        if (c.everything || c.nsKey == "ClipPlayer.clip" || c.nsKey.rfind("MidiClip", 0) == 0) { load(); repaint(); }
    });
}

ClipEditor::~ClipEditor() { m_hub.removeAny(m_anyId); }

std::string ClipEditor::slotKey(const char* key) const { return "MidiClip" + std::to_string(m_slot) + "." + key; }

double ClipEditor::beats() const { return std::max(0.25, m_hub.number(slotKey("length"))); }

void ClipEditor::load()
{
    m_slot = std::clamp(static_cast<int>(std::lround(m_hub.number("ClipPlayer.clip"))), 0, 11);
    m_notes.clear();
    std::stringstream all(m_hub.text(slotKey("notes")));
    std::string item;
    while (std::getline(all, item, ';')) {
        Note n {0.0, 0.25, 60, 100};
        std::replace(item.begin(), item.end(), ',', ' ');
        std::istringstream fields(item);
        if (fields >> n.start >> n.length >> n.note) {
            if (!(fields >> n.velocity)) n.velocity = 100;
            m_notes.push_back(n);
        }
    }
}

void ClipEditor::store()
{
    std::string text;
    char buf[96];
    std::sort(m_notes.begin(), m_notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
    for (const auto& n : m_notes) {
        std::snprintf(buf, sizeof(buf), "%s%.6g,%.6g,%d,%d", text.empty() ? "" : ";", n.start, n.length, n.note, n.velocity);
        text += buf;
    }
    m_hub.controller().set(slotKey("notes"), text);
}

juce::Rectangle<float> ClipEditor::noteRect(const Note& n) const
{
    const auto area = getLocalBounds().toFloat().reduced(1.0f).withTrimmedLeft(34.0f);
    const float rowH = area.getHeight() / static_cast<float>(kHigh - kLow);
    const float beatW = area.getWidth() / static_cast<float>(beats());
    return {area.getX() + static_cast<float>(n.start) * beatW, area.getY() + static_cast<float>(kHigh - 1 - n.note) * rowH,
            std::max(3.0f, static_cast<float>(n.length) * beatW), rowH};
}

void ClipEditor::paint(juce::Graphics& g)
{
    g.fillAll(colours::panel);
    const auto area = getLocalBounds().toFloat().reduced(1.0f).withTrimmedLeft(34.0f);
    const float rowH = area.getHeight() / static_cast<float>(kHigh - kLow);
    for (int note = kLow; note < kHigh; ++note) {
        const float y = area.getY() + static_cast<float>(kHigh - 1 - note) * rowH;
        const bool black = juce::MidiMessage::isMidiNoteBlack(note);
        g.setColour(black ? colours::background : colours::panelHi.withAlpha(0.6f));
        g.fillRect(area.getX(), y, area.getWidth(), rowH);
        if (note % 12 == 0) {
            g.setColour(colours::textDim);
            g.setFont(juce::FontOptions(10.5f));
            g.drawText(juce::MidiMessage::getMidiNoteName(note, true, true, 4), juce::Rectangle<float>(0.0f, y, 32.0f, rowH), juce::Justification::centredRight);
        }
    }
    const double total = beats();
    const float beatW = area.getWidth() / static_cast<float>(total);
    for (int s = 0; s <= static_cast<int>(total * 4.0); ++s) {
        const float x = area.getX() + static_cast<float>(s) * beatW / 4.0f;
        g.setColour(s % 4 == 0 ? colours::edge.brighter(0.4f) : colours::edge);
        g.drawVerticalLine(static_cast<int>(x), area.getY(), area.getBottom());
    }
    for (const auto& n : m_notes) {
        if (n.note < kLow || n.note >= kHigh) continue;
        const auto r = noteRect(n).reduced(0.5f, 1.0f);
        g.setColour(colours::accent.withAlpha(0.5f + 0.5f * static_cast<float>(n.velocity) / 127.0f));
        g.fillRoundedRectangle(r, 2.0f);
        g.setColour(colours::gold);
        g.drawRoundedRectangle(r, 2.0f, 1.0f);
    }
}

void ClipEditor::mouseDown(const juce::MouseEvent& e)
{
    const auto p = e.position;
    for (std::size_t i = 0; i < m_notes.size(); ++i)
        if (noteRect(m_notes[i]).contains(p)) {
            m_notes.erase(m_notes.begin() + static_cast<long>(i));
            store();
            repaint();
            return;
        }
    const auto area = getLocalBounds().toFloat().reduced(1.0f).withTrimmedLeft(34.0f);
    if (!area.contains(p)) return;
    const float rowH = area.getHeight() / static_cast<float>(kHigh - kLow);
    const int note = kHigh - 1 - static_cast<int>((p.y - area.getY()) / rowH);
    const double beat = std::floor((p.x - area.getX()) / area.getWidth() * beats() * 4.0) / 4.0;
    m_notes.push_back({beat, 0.25, note, 100});
    m_dragging = static_cast<int>(m_notes.size()) - 1;
    repaint();
}

void ClipEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (m_dragging < 0) return;
    const auto area = getLocalBounds().toFloat().reduced(1.0f).withTrimmedLeft(34.0f);
    auto& n = m_notes[static_cast<std::size_t>(m_dragging)];
    const double end = std::ceil((e.position.x - area.getX()) / area.getWidth() * beats() * 4.0) / 4.0;
    n.length = std::clamp(end - n.start, 0.25, beats() - n.start);
    repaint();
}

void ClipEditor::mouseUp(const juce::MouseEvent&)
{
    if (m_dragging >= 0) store();
    m_dragging = -1;
}

// --- MidiPage --------------------------------------------------------------------------------------------

MidiPage::MidiPage(ParamHub& hub)
    : m_scale(hub, "KEY & SCALE", "PitchQuantizer0", 3)
    , m_arp(hub, "ARPEGGIATOR", "Arp0", 5)
    , m_clip(hub, "CLIP SEQUENCER", "ClipPlayer", 3)
    , m_clipSlot(hub, "CLIP", "MidiClip0", 1)
    , m_editor(hub)
    , m_hub(hub)
{
    m_scale.setKeys(hub.keysOf("PitchQuantizer0"));
    m_arp.setKeys(hub.keysOf("Arp0"));
    m_clip.setKeys(hub.keysOf("ClipPlayer"));
    m_clipSlot.setKeys({"length"});
    for (auto* c : std::initializer_list<juce::Component*>{&m_scale, &m_arp, &m_clip, &m_clipSlot, &m_editor, &m_hint}) addAndMakeVisible(c);
    m_hint.setText("Click the grid to add a note, drag to lengthen, click a note to delete. Key-triggered clips play from C4.",
                   juce::dontSendNotification);
    m_hint.setColour(juce::Label::textColourId, colours::textDim);
    m_hint.setFont(juce::FontOptions(12.0f));
    m_anyId = m_hub.onAny([this](const control::ParamChange& c) {
        if (c.everything || c.nsKey == "ClipPlayer.clip") {
            const int slot = std::clamp(static_cast<int>(std::lround(m_hub.number("ClipPlayer.clip"))), 0, 11);
            juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MidiPage>(this), slot] {
                if (safe) safe->m_clipSlot.setModule("MidiClip" + std::to_string(slot), "CLIP " + juce::String(slot + 1));
            });
        }
    });
}

void MidiPage::resized()
{
    auto r = getLocalBounds().reduced(4);
    auto top = r.removeFromTop(270);
    m_scale.setBounds(top.removeFromLeft(260));
    m_clip.setBounds(top.removeFromRight(300));
    m_arp.setBounds(top);
    auto bottom = r;
    m_hint.setBounds(bottom.removeFromBottom(22).reduced(8, 0));
    m_clipSlot.setBounds(bottom.removeFromLeft(110));
    m_editor.setBounds(bottom.reduced(4));
}

// --- TablePage -------------------------------------------------------------------------------------------

namespace {
class ValueCell final : public juce::Label {
public:
    explicit ValueCell(ParamHub& hub) : m_hub(hub)
    {
        setEditable(false, true, false);
        setFont(juce::FontOptions(13.0f));
        setColour(juce::Label::textColourId, colours::gold);
        onTextChange = [this] {
            if (m_key.empty()) return;
            const auto* s = m_hub.schema(m_key);
            if (s != nullptr && s->type == "string") m_hub.controller().set(m_key, getText().toStdString());
            else m_hub.controller().modify(m_key, getText().toStdString());
            show();
        };
    }
    void setKey(const std::string& k) { m_key = k; show(); }
    void show()
    {
        const auto* s = m_hub.schema(m_key);
        if (s == nullptr) return;
        setText(s->type == "string" ? juce::String(m_hub.text(m_key)) : juce::String(m_hub.controller().format(m_key, m_hub.number(m_key))),
                juce::dontSendNotification);
    }

private:
    ParamHub& m_hub;
    std::string m_key;
};
}

TablePage::TablePage(ParamHub& hub, std::function<void()> onSave) : m_hub(hub)
{
    m_search.setTextToShowWhenEmpty("Search parameters (module, name or description)...", colours::textDim);
    m_search.onTextChange = [this] { filter(); };
    addAndMakeVisible(m_search);
    auto& h = m_table.getHeader();
    h.addColumn("Module", 1, 150);
    h.addColumn("Parameter", 2, 170);
    h.addColumn("Value (double-click to edit)", 3, 220);
    h.addColumn("Default", 4, 120);
    h.addColumn("Range", 5, 150);
    h.addColumn("Description", 6, 400);
    m_table.setModel(this);
    m_table.setRowHeight(24);
    addAndMakeVisible(m_table);
    m_save.onClick = std::move(onSave);
    m_save.setColour(juce::TextButton::buttonColourId, colours::accent);
    addAndMakeVisible(m_save);   // floating, over the table
    m_anyId = m_hub.onAny([this](const control::ParamChange&) { m_table.updateContent(); m_table.repaint(); });
    filter();
}

TablePage::~TablePage() { m_hub.removeAny(m_anyId); }

void TablePage::filter()
{
    const auto q = m_search.getText().toLowerCase().toStdString();
    m_rows.clear();
    for (const auto& s : m_hub.all()) {
        if (!q.empty()) {
            const std::string hay = juce::String(s.nsKey + " " + s.group + " " + s.tooltip).toLowerCase().toStdString();
            if (hay.find(q) == std::string::npos) continue;
        }
        m_rows.push_back(&s);
    }
    m_table.updateContent();
    m_table.repaint();
}

void TablePage::resized()
{
    auto r = getLocalBounds().reduced(8);
    m_search.setBounds(r.removeFromTop(28));
    r.removeFromTop(6);
    m_table.setBounds(r);
    m_save.setBounds(r.getRight() - 150, r.getBottom() - 46, 130, 32);
}

void TablePage::paintRowBackground(juce::Graphics& g, int row, int, int, bool selected)
{
    g.fillAll(selected ? colours::accentDim : (row % 2 == 0 ? colours::panel : colours::panelHi.withAlpha(0.5f)));
}

void TablePage::paintCell(juce::Graphics& g, int row, int column, int w, int h, bool)
{
    if (row < 0 || row >= static_cast<int>(m_rows.size())) return;
    const auto& s = *m_rows[static_cast<std::size_t>(row)];
    juce::String text;
    switch (column) {
        case 1: text = s.module; break;
        case 2: text = s.key; break;
        case 4: text = s.type == "string" ? juce::String(s.defaultValue.text()) : juce::String(m_hub.controller().format(s.nsKey, s.defaultValue.number())); break;
        case 5: text = s.type == "string" ? juce::String("text") : juce::String(m_hub.controller().format(s.nsKey, s.min)) + " .. " + juce::String(m_hub.controller().format(s.nsKey, s.max)); break;
        case 6: text = s.tooltip; break;
        default: break;
    }
    g.setColour(column == 6 ? colours::textDim : colours::text);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(text, 4, 0, w - 8, h, juce::Justification::centredLeft, true);
}

juce::Component* TablePage::refreshComponentForCell(int row, int column, bool, juce::Component* existing)
{
    if (column != 3) { delete existing; return nullptr; }
    auto* cell = dynamic_cast<ValueCell*>(existing);
    if (cell == nullptr) { delete existing; cell = new ValueCell(m_hub); }
    if (row >= 0 && row < static_cast<int>(m_rows.size())) cell->setKey(m_rows[static_cast<std::size_t>(row)]->nsKey);
    return cell;
}

// --- PresetBrowser ---------------------------------------------------------------------------------------

juce::File PresetBrowser::userFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Winerose").getChildFile("Presets");
}

PresetBrowser::PresetBrowser(ParamHub& hub, std::function<void(const juce::File&)> load, juce::PropertiesFile* settings)
    : m_hub(hub), m_load(std::move(load)), m_settings(settings)
{
    m_roots.setTabs({"Winerose", "Serum 2", "Serum 1", "My folders"});
    m_roots.onSelect = [this](int i) { m_root = i; rescan(); };
    addAndMakeVisible(m_roots);
    m_addFolder.setTooltip("Add a folder of presets (e.g. your Serum 2 preset library) to \"My folders\"");
    m_addFolder.onClick = [this] { addFolder(); };
    m_clearFolders.onClick = [this] {
        if (m_settings != nullptr) { m_settings->setValue("presetFolders", juce::String()); m_settings->saveIfNeeded(); }
        rescan();
    };
    addAndMakeVisible(m_addFolder);
    addAndMakeVisible(m_clearFolders);
    m_search.setTextToShowWhenEmpty("Search presets...", colours::textDim);
    m_search.onTextChange = [this] { filter(); };
    addAndMakeVisible(m_search);
    m_list.setRowHeight(24);
    addAndMakeVisible(m_list);
    rescan();
}

juce::StringArray PresetBrowser::folders() const
{
    juce::StringArray list;
    if (m_settings != nullptr) list.addTokens(m_settings->getValue("presetFolders"), "|", "");
    list.removeEmptyStrings();
    return list;
}

void PresetBrowser::addFolder()
{
    m_chooser = std::make_unique<juce::FileChooser>("Add a preset folder",
                                                    juce::File::getSpecialLocation(juce::File::userDocumentsDirectory));
    m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this](const juce::FileChooser& fc) {
        const auto dir = fc.getResult();
        if (!dir.isDirectory() || m_settings == nullptr) return;
        auto list = folders();
        list.addIfNotAlreadyThere(dir.getFullPathName());
        m_settings->setValue("presetFolders", list.joinIntoString("|"));
        m_settings->saveIfNeeded();
        m_roots.setSelected(3);
    });
}

void PresetBrowser::rescan()
{
    const auto docs = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    juce::Array<juce::File> roots;
    juce::String pattern;
    switch (m_root) {
        case 1: roots.add(docs.getChildFile("Xfer/Serum 2 Presets")); pattern = "*.SerumPreset"; break;
        case 2: roots.add(docs.getChildFile("Xfer/Serum Presets")); pattern = "*.fxp;*.fxb"; break;
        case 3:
            for (const auto& f : folders()) roots.add(juce::File(f));
            pattern = "*.SerumPreset;*.fxp;*.fxb;*.wrpreset";
            break;
        default: roots.add(userFolder()); pattern = "*.wrpreset;*.json"; break;
    }
    m_all.clear();
    for (const auto& root : roots) {
        if (!root.isDirectory()) continue;
        for (const auto& entry : juce::RangedDirectoryIterator(root, true, pattern, juce::File::findFiles)) {
            m_all.add(entry.getFile());
            if (m_all.size() >= 20000) break;
        }
    }
    std::sort(m_all.begin(), m_all.end(), [](const juce::File& a, const juce::File& b) {
        return a.getFileNameWithoutExtension().compareIgnoreCase(b.getFileNameWithoutExtension()) < 0;
    });
    filter();
}

void PresetBrowser::filter()
{
    const auto q = m_search.getText();
    m_shown.clear();
    for (const auto& f : m_all)
        if (q.isEmpty() || f.getRelativePathFrom(f.getParentDirectory().getParentDirectory()).containsIgnoreCase(q)) m_shown.add(f);
    m_list.setVisible(!m_all.isEmpty());   // otherwise the panel shows where to find / add presets
    m_list.updateContent();
    m_list.repaint();
    repaint();
}

juce::File PresetBrowser::step(int delta)
{
    if (m_shown.isEmpty()) return {};
    int i = m_shown.indexOf(m_current);
    i = i < 0 ? (delta > 0 ? 0 : m_shown.size() - 1) : (i + delta + m_shown.size()) % m_shown.size();
    m_current = m_shown[i];
    m_list.selectRow(i);
    return m_current;
}

void PresetBrowser::paint(juce::Graphics& g)
{
    drawPanel(g, getLocalBounds().reduced(4), "PRESETS");
    if (m_all.isEmpty()) {
        g.setColour(colours::textDim);
        g.setFont(juce::FontOptions(14.0f));
        juce::String msg;
        switch (m_root) {
            case 0: msg = "No Winerose presets yet in " + userFolder().getFullPathName() + ".\nUse Save in the header to create one."; break;
            case 1: msg = "No Serum 2 presets in Documents/Xfer/Serum 2 Presets.\nUse \"Import...\" in the header for single files, or \"Add folder...\" for a preset library anywhere on disk."; break;
            case 2: msg = "No Serum 1 presets in Documents/Xfer/Serum Presets.\nUse \"Import...\" or \"Add folder...\"."; break;
            default: msg = folders().isEmpty() ? juce::String("No folders added. Click \"Add folder...\" and pick a folder of .SerumPreset / .fxp / .wrpreset files.")
                                               : juce::String("No presets found in the added folders."); break;
        }
        g.drawFittedText(msg, getLocalBounds().reduced(40, 140), juce::Justification::centredTop, 4);
    }
}

void PresetBrowser::resized()
{
    auto r = getLocalBounds().reduced(12);
    r.removeFromTop(26);
    auto bar = r.removeFromTop(28);
    m_clearFolders.setBounds(bar.removeFromRight(110));
    bar.removeFromRight(6);
    m_addFolder.setBounds(bar.removeFromRight(110));
    m_roots.setBounds(bar.removeFromLeft(560));
    r.removeFromTop(6);
    m_search.setBounds(r.removeFromTop(28));
    r.removeFromTop(6);
    m_list.setBounds(r);
}

void PresetBrowser::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= m_shown.size()) return;
    const auto& f = m_shown[row];
    g.fillAll(selected || f == m_current ? colours::accentDim : (row % 2 == 0 ? colours::panel : colours::panelHi.withAlpha(0.4f)));
    g.setColour(colours::text);
    g.setFont(juce::FontOptions(13.5f));
    g.drawText(f.getFileNameWithoutExtension(), 8, 0, w / 2, h, juce::Justification::centredLeft, true);
    g.setColour(colours::textDim);
    g.setFont(juce::FontOptions(12.0f));
    g.drawText(f.getParentDirectory().getFileName(), w / 2, 0, w / 2 - 8, h, juce::Justification::centredRight, true);
}

void PresetBrowser::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    if (row < 0 || row >= m_shown.size()) return;
    m_current = m_shown[row];
    if (m_load) m_load(m_current);
    m_list.repaint();
}

} // namespace winerose::ui

#include "Widgets.h"

#include "Theme.h"

#include <algorithm>
#include <cctype>

namespace winerose::ui {

// --- ParamHub --------------------------------------------------------------------------------------------

ParamHub::ParamHub(control::IController& c) : m_controller(c), m_schema(c.schema())
{
    for (std::size_t i = 0; i < m_schema.size(); ++i) m_index[m_schema[i].nsKey] = i;
    std::weak_ptr<bool> alive = m_alive;
    m_subscription = m_controller.onChange([this, alive](const control::ParamChange& change) {
        if (juce::MessageManager::getInstance()->isThisTheMessageThread()) {
            dispatch(change);
            return;
        }
        juce::MessageManager::callAsync([this, alive, change] {
            if (alive.lock()) dispatch(change);
        });
    });
}

ParamHub::~ParamHub()
{
    m_subscription.reset();
    m_alive.reset();
}

const control::ParamSchema* ParamHub::schema(const std::string& nsKey) const
{
    const auto it = m_index.find(nsKey);
    return it == m_index.end() ? nullptr : &m_schema[it->second];
}

std::vector<std::string> ParamHub::keysOf(const std::string& module) const
{
    std::vector<std::string> keys;
    for (const auto& s : m_schema) if (s.module == module) keys.push_back(s.key);
    return keys;
}

void ParamHub::add(const std::string& nsKey, Listener* l) { m_listeners.emplace(nsKey, l); }

void ParamHub::remove(const std::string& nsKey, Listener* l)
{
    const auto range = m_listeners.equal_range(nsKey);
    for (auto it = range.first; it != range.second; ++it)
        if (it->second == l) { m_listeners.erase(it); return; }
}

int ParamHub::onAny(std::function<void(const control::ParamChange&)> f)
{
    m_any.emplace(m_nextAny, std::move(f));
    return m_nextAny++;
}

void ParamHub::removeAny(int id) { m_any.erase(id); }

void ParamHub::dispatch(const control::ParamChange& change)
{
    // Copy first: a callback may add/remove listeners (panels rebuild asynchronously, but be safe).
    std::vector<Listener*> targets;
    if (change.everything) {
        for (const auto& [k, l] : m_listeners) targets.push_back(l);
    } else {
        const auto range = m_listeners.equal_range(change.nsKey);
        for (auto it = range.first; it != range.second; ++it) targets.push_back(it->second);
    }
    for (auto* l : targets) {
        // Skip listeners removed by an earlier callback in this loop.
        bool present = false;
        for (const auto& [k, x] : m_listeners) if (x == l) { present = true; break; }
        if (present) l->paramChanged();
    }
    std::vector<std::function<void(const control::ParamChange&)>> any;
    for (const auto& [id, f] : m_any) any.push_back(f);
    for (const auto& f : any) f(change);
}

std::string ParamHub::label(const std::string& nsKey) const
{
    static const std::map<std::string, std::string> kShort = {
        {"level", "Level"}, {"pan", "Pan"}, {"octave", "Oct"}, {"semi", "Semi"}, {"fine", "Fine"}, {"coarse", "Pitch"},
        {"wtPos", "WT Pos"}, {"unison", "Unison"}, {"uniDetune", "Detune"}, {"uniBlend", "Blend"}, {"uniWidth", "Width"},
        {"uniRange", "Range"}, {"uniStack", "Stack"}, {"uniMode", "Detune Mode"}, {"uniSpan", "Span"},
        {"warp1Mode", "Warp 1"}, {"warp1Amount", "Warp 1"}, {"warp2Mode", "Warp 2"}, {"warp2Amount", "Warp 2"},
        {"route", "Route"}, {"filterBalance", "F1 <> F2"}, {"bus1Send", "Bus 1"}, {"bus2Send", "Bus 2"},
        {"cutoff", "Cutoff"}, {"resonance", "Res"}, {"drive", "Drive"}, {"keytrack", "Key Trk"}, {"mix", "Mix"},
        {"attack", "Attack"}, {"hold", "Hold"}, {"decay", "Decay"}, {"sustain", "Sustain"}, {"release", "Release"},
        {"smpStart", "Start"}, {"smpEnd", "End"}, {"smpLoop", "Loop"}, {"smpLoopStart", "Loop In"}, {"smpLoopEnd", "Loop Out"},
        {"smpXfade", "X-Fade"}, {"smpFileLoop", "File Loop"}, {"grnPos", "Position"}, {"grnScan", "Scan"},
        {"grnSize", "Size"}, {"grnDensity", "Density"}, {"grnPosRand", "Pos Rnd"}, {"grnPitchRand", "Pitch Rnd"},
        {"grnPanRand", "Pan Rnd"}, {"grnWindow", "Window"}, {"grnWindowAmt", "Win Amt"}, {"spcPos", "Position"},
        {"spcScan", "Scan"}, {"spcTimbre", "Timbre"}, {"spcFormant", "Formant"}, {"spcLowCut", "Low Cut"},
        {"spcHighCut", "High Cut"}, {"spcTransients", "Transients"}, {"keyLo", "Key Lo"}, {"keyHi", "Key Hi"},
        {"velLo", "Vel Lo"}, {"velHi", "Vel Hi"}, {"masterVolume", "Master"}, {"bendUp", "Bend Up"}, {"bendDown", "Bend Dn"},
    };
    const auto dot = nsKey.find('.');
    const std::string key = dot == std::string::npos ? nsKey : nsKey.substr(dot + 1);
    // FX slots: the controller knows the effect's own names for p0..p7.
    if (nsKey.rfind("FXRack", 0) == 0 && key.size() == 2 && key[0] == 'p') return m_controller.label(nsKey);
    if (const auto it = kShort.find(key); it != kShort.end()) return it->second;
    std::string out;   // camelCase → "Camel Case"
    for (std::size_t i = 0; i < key.size(); ++i) {
        const char c = key[i];
        if (i == 0) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        else if (std::isupper(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(key[i - 1]))) { out.push_back(' '); out.push_back(c); }
        else out.push_back(c);
    }
    return out;
}

// --- ParamControl ----------------------------------------------------------------------------------------

ParamControl::ParamControl(ParamHub& hub, std::string nsKey) : m_hub(hub), m_key(std::move(nsKey)), m_schema(hub.schema(m_key))
{
    m_hub.add(m_key, this);
    if (m_schema != nullptr) setTooltip(m_schema->tooltip);
}

ParamControl::~ParamControl() { m_hub.remove(m_key, this); }

void ParamControl::refreshCaption()
{
    if (!m_hasCaption) m_caption = m_hub.label(m_key);
    captionChanged();
}

// --- Knob ------------------------------------------------------------------------------------------------

Knob::Knob(ParamHub& hub, std::string nsKey) : ParamControl(hub, std::move(nsKey))
{
    m_slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    m_slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    m_slider.setRotaryParameters(juce::MathConstants<float>::pi * 1.2f, juce::MathConstants<float>::pi * 2.8f, true);
    m_slider.setRange(0.0, 1.0, 0.0);
    m_slider.setScrollWheelEnabled(false);   // the page scrolls; knobs don't steal the wheel
    m_slider.setVelocityBasedMode(false);
    m_slider.setMouseDragSensitivity(220);
    m_slider.addListener(this);
    if (m_schema != nullptr) {
        m_slider.setDoubleClickReturnValue(true, m_hub.controller().toNormalized(m_key, m_schema->defaultValue.number()));
        m_slider.getProperties().set("bipolar", m_schema->min < 0.0 && m_schema->max > 0.0);
        if (m_schema->type == "int") {
            const double steps = m_schema->max - m_schema->min;
            if (steps > 0.0 && steps <= 128.0) m_slider.setRange(0.0, 1.0, 1.0 / steps);
        }
    }
    addAndMakeVisible(m_slider);

    m_name.setJustificationType(juce::Justification::centred);
    m_name.setFont(juce::FontOptions(11.5f));
    m_name.setColour(juce::Label::textColourId, colours::textDim);
    m_name.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(m_name);

    m_value.setJustificationType(juce::Justification::centred);
    m_value.setFont(juce::FontOptions(11.5f));
    m_value.setEditable(false, true, false);
    m_value.onTextChange = [this] {
        m_hub.controller().modify(m_key, m_value.getText().toStdString());
        showValue();
    };
    addAndMakeVisible(m_value);
    refreshCaption();
    paramChanged();
}

void Knob::captionChanged() { m_name.setText(m_caption, juce::dontSendNotification); }

void Knob::resized()
{
    auto r = getLocalBounds();
    m_name.setBounds(r.removeFromTop(14));
    m_value.setBounds(r.removeFromBottom(14));
    m_slider.setBounds(r.withSizeKeepingCentre(std::min(r.getWidth(), r.getHeight() + 6), r.getHeight()));
}

void Knob::paramChanged()
{
    m_updating = true;
    const double plain = m_hub.number(m_key);
    m_slider.setValue(m_hub.controller().toNormalized(m_key, plain), juce::dontSendNotification);
    m_updating = false;
    showValue();
    if (!m_hasCaption && m_key.rfind("FXRack", 0) == 0) refreshCaption();   // effect knob names follow the type
}

void Knob::showValue()
{
    m_value.setText(m_hub.controller().format(m_key, m_hub.number(m_key)), juce::dontSendNotification);
}

void Knob::sliderValueChanged(juce::Slider*)
{
    if (m_updating) return;
    m_hub.controller().set(m_key, m_hub.controller().fromNormalized(m_key, m_slider.getValue()));
}

void Knob::sliderDragStarted(juce::Slider*) { m_hub.controller().beginGesture(m_key); }
void Knob::sliderDragEnded(juce::Slider*) { m_hub.controller().endGesture(m_key); }

// --- Choice ----------------------------------------------------------------------------------------------

Choice::Choice(ParamHub& hub, std::string nsKey, bool showCaption) : ParamControl(hub, std::move(nsKey)), m_showCaption(showCaption)
{
    if (m_schema != nullptr) {
        if (m_schema->type == "bool") {
            m_box.addItem("Off", 1);
            m_box.addItem("On", 2);
        } else {
            for (const auto& [value, name] : m_schema->choices) m_box.addItem(name, value + 1);
        }
    }
    m_box.onChange = [this] {
        const int id = m_box.getSelectedId();
        if (id > 0 && static_cast<double>(id - 1) != m_hub.number(m_key)) m_hub.controller().set(m_key, static_cast<double>(id - 1));
    };
    addAndMakeVisible(m_box);
    m_name.setJustificationType(juce::Justification::centred);
    m_name.setFont(juce::FontOptions(11.5f));
    m_name.setColour(juce::Label::textColourId, colours::textDim);
    if (m_showCaption) addAndMakeVisible(m_name);
    refreshCaption();
    paramChanged();
}

void Choice::resized()
{
    auto r = getLocalBounds();
    if (m_showCaption) m_name.setBounds(r.removeFromTop(14));
    m_box.setBounds(r.withSizeKeepingCentre(r.getWidth() - 2, std::min(22, r.getHeight())));
}

void Choice::paramChanged()
{
    m_box.setSelectedId(static_cast<int>(std::lround(m_hub.number(m_key))) + 1, juce::dontSendNotification);
}

// --- Switch ----------------------------------------------------------------------------------------------

Switch::Switch(ParamHub& hub, std::string nsKey, bool showCaption) : ParamControl(hub, std::move(nsKey)), m_showCaption(showCaption)
{
    m_button.onClick = [this] { m_hub.controller().set(m_key, m_button.getToggleState() ? 1.0 : 0.0); };
    addAndMakeVisible(m_button);
    m_name.setJustificationType(juce::Justification::centred);
    m_name.setFont(juce::FontOptions(11.5f));
    m_name.setColour(juce::Label::textColourId, colours::textDim);
    m_name.setMinimumHorizontalScale(0.7f);
    if (m_showCaption) addAndMakeVisible(m_name);
    refreshCaption();
    paramChanged();
}

void Switch::resized()
{
    auto r = getLocalBounds();
    if (m_showCaption) {   // like a knob: caption on top, the switch centred below
        m_name.setBounds(r.removeFromTop(14));
        m_button.setBounds(r.withSizeKeepingCentre(32, std::min(20, r.getHeight())));
    } else {
        m_button.setBounds(r.withSizeKeepingCentre(std::min(r.getWidth(), 32), std::min(20, r.getHeight())));
    }
}

void Switch::paramChanged() { m_button.setToggleState(m_hub.number(m_key) >= 0.5, juce::dontSendNotification); }

std::unique_ptr<ParamControl> makeControl(ParamHub& hub, const std::string& nsKey)
{
    const auto* s = hub.schema(nsKey);
    if (s == nullptr || s->type == "string") return nullptr;
    if (s->type == "enum") return std::make_unique<Choice>(hub, nsKey);
    if (s->type == "bool") return std::make_unique<Switch>(hub, nsKey);
    return std::make_unique<Knob>(hub, nsKey);
}

// --- ModulePanel -----------------------------------------------------------------------------------------

ModulePanel::ModulePanel(ParamHub& hub, juce::String title, std::string module, int columns)
    : m_hub(hub), m_title(std::move(title)), m_module(std::move(module)), m_columns(columns)
{
    setModule(m_module, m_title);
}

void ModulePanel::setModule(std::string module, juce::String title)
{
    m_module = std::move(module);
    m_title = std::move(title);
    m_enable.reset();
    if (m_hub.schema(m_module + ".enabled") != nullptr) {
        m_enable = std::make_unique<Switch>(m_hub, m_module + ".enabled", false);
        addAndMakeVisible(*m_enable);
    }
    setKeys(m_keys);
    repaint();
}

void ModulePanel::setKeys(const std::vector<std::string>& keys)
{
    m_keys = keys;
    m_controls.clear();
    for (const auto& k : m_keys) {
        if (k == "enabled") continue;
        if (auto c = makeControl(m_hub, m_module + "." + k)) {
            addAndMakeVisible(*c);
            m_controls.push_back(std::move(c));
        }
    }
    resized();
}

void ModulePanel::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced(2.0f);
    g.setColour(colours::panel);
    g.fillRoundedRectangle(r, 6.0f);
    g.setColour(colours::edge);
    g.drawRoundedRectangle(r, 6.0f, 1.0f);
    g.setColour(colours::panelHi);
    g.fillRoundedRectangle(r.withHeight(static_cast<float>(kHeader)), 6.0f);
    g.setColour(colours::gold);
    g.setFont(juce::FontOptions(13.5f, juce::Font::bold));
    g.drawText(m_title, juce::Rectangle<int>(m_enable ? 46 : 12, 2, 220, kHeader), juce::Justification::centredLeft);
}

void ModulePanel::resized()
{
    auto r = getLocalBounds().reduced(6, 4);
    auto header = r.removeFromTop(kHeader - 2);
    if (m_enable) m_enable->setBounds(header.removeFromLeft(36).reduced(2, 3));
    header.removeFromLeft(static_cast<int>(juce::GlyphArrangement::getStringWidth(juce::FontOptions(13.5f, juce::Font::bold), m_title)) + 18);
    layoutHeader(header);
    r.removeFromTop(4);
    if (m_top != nullptr) {
        m_top->setBounds(r.removeFromTop(m_topHeight));
        r.removeFromTop(2);
    }
    if (m_controls.empty()) return;
    const int cols = std::max(1, m_columns);
    const int rows = (static_cast<int>(m_controls.size()) + cols - 1) / cols;
    const int cellW = r.getWidth() / cols;
    const int cellH = std::min(78, r.getHeight() / std::max(1, rows));
    for (std::size_t i = 0; i < m_controls.size(); ++i) {
        const int col = static_cast<int>(i) % cols, row = static_cast<int>(i) / cols;
        m_controls[i]->setBounds(r.getX() + col * cellW, r.getY() + row * cellH, cellW, cellH);
    }
}

// --- TabStrip --------------------------------------------------------------------------------------------

void TabStrip::setTabs(const juce::StringArray& names, int selected)
{
    m_buttons.clear();
    for (int i = 0; i < names.size(); ++i) {
        auto* b = m_buttons.add(new juce::TextButton(names[i]));
        b->setClickingTogglesState(false);
        b->setConnectedEdges(juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);   // less padding: "10" fits
        b->onClick = [this, i] { setSelected(i); };
        addAndMakeVisible(b);
    }
    setSelected(selected, false);
    resized();
}

void TabStrip::setSelected(int index, bool notify)
{
    m_selected = std::clamp(index, 0, std::max(0, m_buttons.size() - 1));
    for (int i = 0; i < m_buttons.size(); ++i) m_buttons[i]->setToggleState(i == m_selected, juce::dontSendNotification);
    if (notify && onSelect) onSelect(m_selected);
}

void TabStrip::resized()
{
    if (m_buttons.isEmpty()) return;
    auto r = getLocalBounds();
    const int w = r.getWidth() / m_buttons.size();
    for (auto* b : m_buttons) b->setBounds(r.removeFromLeft(w).reduced(1, 0));
}

} // namespace winerose::ui

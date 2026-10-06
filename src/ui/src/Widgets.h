#pragma once

#include "control/IController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace winerose::ui {

/**
 * @brief The UI's single connection to IController: schema cache, one change subscription, and fan-out of
 *        changes to the controls bound to each key. Every callback runs on the message thread; a change
 *        arriving on another thread is re-posted. Controls never call IController directly for observation.
 */
class ParamHub {
public:
    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void paramChanged() = 0;   // the bound key changed (or everything did)
    };

    explicit ParamHub(control::IController& c);
    ~ParamHub();

    control::IController& controller() { return m_controller; }
    const control::ParamSchema* schema(const std::string& nsKey) const;
    const std::vector<control::ParamSchema>& all() const { return m_schema; }
    std::vector<std::string> keysOf(const std::string& module) const;   // registration order

    void add(const std::string& nsKey, Listener* l);
    void remove(const std::string& nsKey, Listener* l);
    /** Called after every change (any key) — headers, undo buttons, tables. Returns an id for removal. */
    int  onAny(std::function<void(const control::ParamChange&)> f);
    void removeAny(int id);

    double number(const std::string& nsKey) const { return m_controller.get(nsKey).number(); }
    std::string text(const std::string& nsKey) const { return m_controller.get(nsKey).text(); }

    /** Display label: a short human name for a key ("uniDetune" → "Detune"), or the FX-aware label. */
    std::string label(const std::string& nsKey) const;

private:
    void dispatch(const control::ParamChange& change);

    control::IController& m_controller;
    std::vector<control::ParamSchema> m_schema;
    std::map<std::string, std::size_t> m_index;
    std::multimap<std::string, Listener*> m_listeners;
    std::map<int, std::function<void(const control::ParamChange&)>> m_any;
    int m_nextAny = 1;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    control::Subscription m_subscription;
};

/** Base for a control bound to one parameter. */
class ParamControl : public juce::Component, public juce::SettableTooltipClient, protected ParamHub::Listener {
public:
    ParamControl(ParamHub& hub, std::string nsKey);
    ~ParamControl() override;
    const std::string& key() const { return m_key; }
    void setCaption(const juce::String& c) { m_caption = c; m_hasCaption = true; refreshCaption(); }

protected:
    void refreshCaption();
    virtual void captionChanged() {}
    ParamHub& m_hub;
    std::string m_key;
    const control::ParamSchema* m_schema = nullptr;
    juce::String m_caption;
    bool m_hasCaption = false;
};

/** Rotary knob: caption above, value below (double-click the value to type one; double-click the knob for default). */
class Knob final : public ParamControl, private juce::Slider::Listener {
public:
    Knob(ParamHub& hub, std::string nsKey);
    void resized() override;
    void paramChanged() override;

private:
    void captionChanged() override;
    void sliderValueChanged(juce::Slider*) override;
    void sliderDragStarted(juce::Slider*) override;
    void sliderDragEnded(juce::Slider*) override;
    void showValue();

    juce::Slider m_slider;
    juce::Label  m_name, m_value;
    bool m_updating = false;
};

/** Enum (or bool shown as a menu): caption above a combo box. */
class Choice final : public ParamControl {
public:
    Choice(ParamHub& hub, std::string nsKey, bool showCaption = true);
    void resized() override;
    void paramChanged() override;

private:
    void captionChanged() override { m_name.setText(m_caption, juce::dontSendNotification); }
    juce::ComboBox m_box;
    juce::Label m_name;
    bool m_showCaption;
};

/** Bool as a switch. */
class Switch final : public ParamControl {
public:
    Switch(ParamHub& hub, std::string nsKey, bool showCaption = true);
    void resized() override;
    void paramChanged() override;

private:
    void captionChanged() override { m_name.setText(m_caption, juce::dontSendNotification); }
    juce::ToggleButton m_button;
    juce::Label m_name;
    bool m_showCaption;
};

/** Picks Knob / Choice / Switch from the schema; nullptr for strings or unknown keys. */
std::unique_ptr<ParamControl> makeControl(ParamHub& hub, const std::string& nsKey);

/**
 * @brief A titled panel: header (title + the module's on/off switch when it has "enabled"), then a grid of
 *        controls for a list of keys. setKeys() rebuilds the grid (deferred-safe: call from the message loop).
 */
class ModulePanel : public juce::Component {
public:
    ModulePanel(ParamHub& hub, juce::String title, std::string module, int columns = 5);
    void setKeys(const std::vector<std::string>& keys);   // keys within the module ("cutoff")
    void setModule(std::string module, juce::String title);
    void setColumns(int c) { m_columns = c; resized(); }
    /** A display (graph, editor) shown between the header and the controls. Not owned. */
    void setTop(juce::Component* c, int height) { m_top = c; m_topHeight = height; if (c != nullptr) addAndMakeVisible(c); resized(); }
    void paint(juce::Graphics&) override;
    void resized() override;
    juce::Rectangle<int> headerExtra() const { return m_extra; }   // free header space for subclasses
    const std::string& module() const { return m_module; }

protected:
    virtual void layoutHeader(juce::Rectangle<int>& header) { m_extra = header; }
    ParamHub& m_hub;
    juce::String m_title;
    std::string m_module;
    std::vector<std::string> m_keys;
    std::vector<std::unique_ptr<ParamControl>> m_controls;
    std::unique_ptr<Switch> m_enable;
    int m_columns;
    juce::Rectangle<int> m_extra;
    juce::Component* m_top = nullptr;
    int m_topHeight = 0;
    static constexpr int kHeader = 26;
};

/** Row of buttons selecting one of N (tabs for Env 1-4, LFO 1-10, FX racks, pages). */
class TabStrip final : public juce::Component {
public:
    std::function<void(int)> onSelect;
    void setTabs(const juce::StringArray& names, int selected = 0);
    void setSelected(int index, bool notify = true);
    int selected() const { return m_selected; }
    void resized() override;

private:
    juce::OwnedArray<juce::TextButton> m_buttons;
    int m_selected = 0;
};

} // namespace winerose::ui

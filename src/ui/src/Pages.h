#pragma once

#include "Widgets.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace winerose::ui {

/** Horizontal slider bound to a key (matrix amounts and other compact rows). */
class HSlider final : public ParamControl, private juce::Slider::Listener {
public:
    HSlider(ParamHub& hub, std::string nsKey);
    void resized() override;
    void paramChanged() override;

private:
    void sliderValueChanged(juce::Slider*) override;
    void sliderDragStarted(juce::Slider*) override { m_hub.controller().beginGesture(m_key); }
    void sliderDragEnded(juce::Slider*) override { m_hub.controller().endGesture(m_key); }
    juce::Slider m_slider;
    juce::Label m_value;
    bool m_updating = false;
};

/** Oscillator A/B/C: type selector, file loader, and the controls of the current type. */
class OscPanel final : public ModulePanel, private ParamHub::Listener, private juce::AsyncUpdater {
public:
    OscPanel(ParamHub& hub, int index, std::function<void(const juce::String&)> status);
    ~OscPanel() override;
    void paramChanged() override { triggerAsyncUpdate(); }

private:
    void handleAsyncUpdate() override;
    void layoutHeader(juce::Rectangle<int>& header) override;
    void chooseFile();
    void updateFileName();

    int m_index;
    std::unique_ptr<Choice> m_type;
    juce::TextButton m_load {"Load"};
    juce::Label m_file;
    std::unique_ptr<juce::FileChooser> m_chooser;
    std::function<void(const juce::String&)> m_status;
    int m_anyId = 0;
};

/** A ModulePanel whose module is chosen by tabs (Env 1-4, LFO 1-10). */
class TabbedModulePanel final : public ModulePanel {
public:
    TabbedModulePanel(ParamHub& hub, juce::String title, std::string prefix, int count, int columns, std::vector<std::string> exclude = {});

private:
    void layoutHeader(juce::Rectangle<int>& header) override;
    void select(int i);
    std::string m_prefix;
    std::vector<std::string> m_exclude;
    TabStrip m_tabs;
};

class MacroPanel final : public juce::Component {
public:
    explicit MacroPanel(ParamHub& hub);
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    ParamHub& m_hub;
    std::vector<std::unique_ptr<Knob>> m_knobs;
    int m_anyId = 0;
};

class SoundPage final : public juce::Component {
public:
    SoundPage(ParamHub& hub, std::function<void(const juce::String&)> status);
    void resized() override;

private:
    std::vector<std::unique_ptr<juce::Component>> m_parts;
    OscPanel *m_oscA, *m_oscB, *m_oscC;
    ModulePanel *m_noise, *m_sub, *m_f1, *m_f2, *m_global;
    TabbedModulePanel *m_env, *m_lfo;
    MacroPanel* m_macros;
};

class MatrixPage final : public juce::Component {
public:
    explicit MatrixPage(ParamHub& hub);
    void resized() override;
    void paint(juce::Graphics&) override;

private:
    ParamHub& m_hub;
    juce::Viewport m_view;
    juce::Component m_rows;
    std::vector<std::unique_ptr<juce::Component>> m_slots;
};

class FxPage final : public juce::Component, private juce::AsyncUpdater {
public:
    explicit FxPage(ParamHub& hub);
    ~FxPage() override;
    void resized() override;
    void paint(juce::Graphics&) override;

private:
    void handleAsyncUpdate() override;
    void showRack(int rack);
    void showSlot(int slot);
    ParamHub& m_hub;
    TabStrip m_racks;
    int m_rack = 0, m_slot = 0;
    std::vector<std::unique_ptr<juce::Component>> m_slotRows;
    std::unique_ptr<ModulePanel> m_detail;
    std::unique_ptr<ModulePanel> m_mixer;
    int m_anyId = 0;
};

/** Piano roll for MidiClip{n}.notes on a 1/16 grid (click to add, drag to lengthen, click a note to delete). */
class ClipEditor final : public juce::Component {
public:
    explicit ClipEditor(ParamHub& hub);
    ~ClipEditor() override;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

private:
    struct Note { double start, length; int note, velocity; };
    std::string slotKey(const char* key) const;
    void load();
    void store();
    juce::Rectangle<float> noteRect(const Note& n) const;
    double beats() const;

    ParamHub& m_hub;
    std::vector<Note> m_notes;
    int m_slot = 0;
    int m_dragging = -1;
    int m_anyId = 0;
    static constexpr int kLow = 48, kHigh = 84;   // C3 .. C6 (exclusive)
};

class MidiPage final : public juce::Component {
public:
    explicit MidiPage(ParamHub& hub);
    void resized() override;

private:
    ModulePanel m_scale, m_arp, m_clip, m_clipSlot;
    ClipEditor m_editor;
    juce::Label m_hint;
    int m_anyId = 0;
    ParamHub& m_hub;
};

/** Every parameter in a searchable table; double-click a value to type a new one. */
class TablePage final : public juce::Component, private juce::TableListBoxModel {
public:
    TablePage(ParamHub& hub, std::function<void()> onSave);
    ~TablePage() override;
    void resized() override;

private:
    int getNumRows() override { return static_cast<int>(m_rows.size()); }
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int column, int w, int h, bool selected) override;
    juce::Component* refreshComponentForCell(int row, int column, bool selected, juce::Component* existing) override;
    void filter();

    ParamHub& m_hub;
    juce::TextEditor m_search;
    juce::TableListBox m_table;
    juce::TextButton m_save {"Save preset..."};
    std::vector<const control::ParamSchema*> m_rows;
    int m_anyId = 0;
};

/** Preset files: Winerose presets plus the user's Serum preset folders (imported on click). */
class PresetBrowser final : public juce::Component, private juce::ListBoxModel {
public:
    PresetBrowser(ParamHub& hub, std::function<void(const juce::File&)> load, juce::PropertiesFile* settings);
    void resized() override;
    void paint(juce::Graphics&) override;
    void rescan();
    juce::File step(int delta);   // previous / next in the current list (wraps); invalid if empty
    void setCurrent(const juce::File& f) { m_current = f; m_list.repaint(); }
    static juce::File userFolder();

private:
    int getNumRows() override { return m_shown.size(); }
    void paintListBoxItem(int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void filter();
    juce::StringArray folders() const;
    void addFolder();

    ParamHub& m_hub;
    std::function<void(const juce::File&)> m_load;
    juce::PropertiesFile* m_settings;
    TabStrip m_roots;
    juce::TextButton m_addFolder {"Add folder..."}, m_clearFolders {"Clear folders"};
    std::unique_ptr<juce::FileChooser> m_chooser;
    juce::TextEditor m_search;
    juce::ListBox m_list {"presets", this};
    juce::Array<juce::File> m_all, m_shown;
    juce::File m_current;
    int m_root = 0;
};

} // namespace winerose::ui

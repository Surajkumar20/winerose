#pragma once

#include "control/IController.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <string>
#include <vector>

namespace winerose::ui {

/**
 * @class WineroseEditor
 * @brief Placeholder editor: one generic control per parameter, built entirely from IController::schema().
 *
 * It knows nothing about the engine — only parameter IDs ("Global.masterVolume") and the control API —
 * which is the rule every UI in this project follows (PLAN.md §1.4). feature/UI replaces it with the
 * original Winerose design plus the ParamTableView; a later web UI replaces it with a WebBrowserComponent.
 */
class WineroseEditor final : public juce::AudioProcessorEditor {
public:
    WineroseEditor(juce::AudioProcessor& processor, control::IController& controller);
    ~WineroseEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    struct Row {
        control::ParamSchema              schema;
        std::unique_ptr<juce::Label>      section;    // module header, on the first row of each module
        std::unique_ptr<juce::Label>      label;
        std::unique_ptr<juce::Slider>     slider;     // numeric params (value = normalized 0..1)
        std::unique_ptr<juce::ComboBox>   combo;      // enum / bool params
        std::unique_ptr<juce::TextEditor> text;       // string params
        bool                              dragging = false;
    };

    void buildRows();
    void layoutRows();
    void refresh(Row& row);
    void refreshAll();
    Row* findRow(const std::string& nsKey);
    void commit(Row& row, const control::ParamValue& value);

    control::IController& m_controller;
    juce::Label           m_title;
    juce::TextButton      m_undo { "Undo" };
    juce::TextButton      m_redo { "Redo" };
    juce::Viewport        m_viewport;
    juce::Component       m_content;     // holds every row; scrolled by m_viewport
    std::vector<std::unique_ptr<Row>> m_rows;
    control::Subscription m_subscription;   // declared last: unsubscribes before rows are destroyed

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WineroseEditor)
};

} // namespace winerose::ui

#pragma once

#include "control/IController.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace winerose::ui {

class Theme;
class ParamHub;
class Content;

/**
 * @class WineroseEditor
 * @brief The Winerose plugin window (feature/UI, SPEC Phase 8): pages for Sound, Matrix, FX, MIDI, Presets and
 *        All Params, a header with the preset bar, undo/redo and master, and an on-screen keyboard.
 *
 * Talks to the synth only through IController (PLAN.md §1.4); the keyboard state is the one JUCE object the
 * processor shares. The whole UI is designed at one size and scaled (aspect-locked), so it stays crisp and
 * consistent at any window size.
 */
class WineroseEditor final : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget {
public:
    static constexpr int kDesignWidth = 1200;
    static constexpr int kDesignHeight = 820;

    WineroseEditor(juce::AudioProcessor& processor, control::IController& controller, juce::MidiKeyboardState* keyboard = nullptr);
    ~WineroseEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

private:
    std::unique_ptr<Theme> m_theme;
    std::unique_ptr<ParamHub> m_hub;
    std::unique_ptr<Content> m_content;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WineroseEditor)
};

} // namespace winerose::ui

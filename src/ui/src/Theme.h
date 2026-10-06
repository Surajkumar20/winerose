#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace winerose::ui {

// Winerose's own look (not Serum's): deep plum panels, wine-red accents, warm gold highlights.
namespace colours {
inline const juce::Colour background {0xff151017};
inline const juce::Colour panel      {0xff211923};
inline const juce::Colour panelHi    {0xff2b2030};
inline const juce::Colour edge       {0xff3c2c43};
inline const juce::Colour accent     {0xffb8375e};   // wine
inline const juce::Colour accentDim  {0xff6e2a40};
inline const juce::Colour gold       {0xffe2ac6b};
inline const juce::Colour text       {0xffeadde8};
inline const juce::Colour textDim    {0xff9d8b9b};
inline const juce::Colour track      {0xff3a2d3f};
}

class Theme final : public juce::LookAndFeel_V4 {
public:
    Theme();

    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end, juce::Slider&) override;
    void drawLinearSlider(juce::Graphics&, int x, int y, int w, int h, float pos, float min, float max,
                          juce::Slider::SliderStyle, juce::Slider&) override;
    void drawComboBox(juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override { return juce::FontOptions(12.5f); }
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont(juce::TextButton&, int height) override { return juce::FontOptions(std::min(13.0f, static_cast<float>(height) * 0.62f)); }
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    juce::Font getLabelFont(juce::Label& l) override { return l.getFont(); }
    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int w, int h, bool vertical, int thumbStart,
                       int thumbSize, bool over, bool down) override;
};

} // namespace winerose::ui

#include "Theme.h"

namespace winerose::ui {

Theme::Theme()
{
    using namespace colours;
    setColour(juce::ResizableWindow::backgroundColourId, background);
    setColour(juce::Label::textColourId, text);
    setColour(juce::Slider::textBoxTextColourId, text);
    setColour(juce::ComboBox::backgroundColourId, panelHi);
    setColour(juce::ComboBox::textColourId, text);
    setColour(juce::ComboBox::outlineColourId, edge);
    setColour(juce::ComboBox::arrowColourId, gold);
    setColour(juce::PopupMenu::backgroundColourId, panel);
    setColour(juce::PopupMenu::textColourId, text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, accentDim);
    setColour(juce::PopupMenu::highlightedTextColourId, text);
    setColour(juce::PopupMenu::headerTextColourId, gold);
    setColour(juce::TextButton::buttonColourId, panelHi);
    setColour(juce::TextButton::buttonOnColourId, accent);
    setColour(juce::TextButton::textColourOffId, text);
    setColour(juce::TextButton::textColourOnId, text);
    setColour(juce::TextEditor::backgroundColourId, background);
    setColour(juce::TextEditor::textColourId, text);
    setColour(juce::TextEditor::outlineColourId, edge);
    setColour(juce::TextEditor::focusedOutlineColourId, gold);
    setColour(juce::TextEditor::highlightColourId, accentDim);
    setColour(juce::CaretComponent::caretColourId, gold);
    setColour(juce::ListBox::backgroundColourId, panel);
    setColour(juce::ListBox::outlineColourId, edge);
    setColour(juce::TableHeaderComponent::backgroundColourId, panelHi);
    setColour(juce::TableHeaderComponent::textColourId, gold);
    setColour(juce::TableHeaderComponent::outlineColourId, edge);
    setColour(juce::ScrollBar::thumbColourId, accentDim);
    setColour(juce::AlertWindow::backgroundColourId, panel);
    setColour(juce::AlertWindow::textColourId, text);
    setColour(juce::AlertWindow::outlineColourId, edge);
    setColour(juce::MidiKeyboardComponent::keyDownOverlayColourId, accent);
    setColour(juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, gold.withAlpha(0.35f));
    setColour(juce::MidiKeyboardComponent::whiteNoteColourId, juce::Colour(0xffe9e1e7));
    setColour(juce::MidiKeyboardComponent::blackNoteColourId, juce::Colour(0xff241b26));
    setColour(juce::MidiKeyboardComponent::keySeparatorLineColourId, juce::Colour(0xff6c5a6a));
    setColour(juce::MidiKeyboardComponent::shadowColourId, juce::Colours::black.withAlpha(0.25f));
}

void Theme::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end, juce::Slider& s)
{
    const auto bounds = juce::Rectangle<int>(x, y, w, h).toFloat().reduced(3.0f);
    const float radius = std::min(bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float lineW = std::max(2.5f, radius * 0.16f);
    const float arcR = radius - lineW * 0.5f;
    const float angle = start + pos * (end - start);
    const bool bipolar = s.getProperties().getWithDefault("bipolar", false);

    juce::Path track;
    track.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f, start, end, true);
    g.setColour(colours::track);
    g.strokePath(track, juce::PathStrokeType(lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float from = bipolar ? (start + end) * 0.5f : start;
    if (std::abs(angle - from) > 0.001f) {
        juce::Path value;
        value.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f, std::min(from, angle), std::max(from, angle), true);
        g.setColour(s.isEnabled() ? colours::accent : colours::accentDim);
        g.strokePath(value, juce::PathStrokeType(lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    const float knobR = arcR - lineW * 1.1f;
    g.setColour(colours::panelHi.brighter(s.isMouseOverOrDragging() ? 0.15f : 0.05f));
    g.fillEllipse(centre.x - knobR, centre.y - knobR, knobR * 2.0f, knobR * 2.0f);
    g.setColour(colours::edge);
    g.drawEllipse(centre.x - knobR, centre.y - knobR, knobR * 2.0f, knobR * 2.0f, 1.0f);
    const juce::Point<float> tip = centre.getPointOnCircumference(knobR * 0.85f, angle);
    const juce::Point<float> base = centre.getPointOnCircumference(knobR * 0.25f, angle);
    g.setColour(colours::gold);
    g.drawLine({base, tip}, std::max(1.6f, lineW * 0.6f));
}

void Theme::drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                             juce::Slider::SliderStyle style, juce::Slider& s)
{
    if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearBar) {
        LookAndFeel_V4::drawLinearSlider(g, x, y, w, h, pos, 0, 0, style, s);
        return;
    }
    const auto r = juce::Rectangle<int>(x, y, w, h).toFloat().reduced(1.0f, static_cast<float>(h) * 0.3f);
    g.setColour(colours::track);
    g.fillRoundedRectangle(r, r.getHeight() * 0.5f);
    const bool bipolar = s.getProperties().getWithDefault("bipolar", false);
    const float mid = r.getX() + r.getWidth() * 0.5f;
    const float a = bipolar ? std::min(mid, pos) : r.getX();
    const float b = bipolar ? std::max(mid, pos) : pos;
    g.setColour(colours::accent);
    g.fillRoundedRectangle(juce::Rectangle<float>(a, r.getY(), std::max(2.0f, b - a), r.getHeight()), r.getHeight() * 0.5f);
}

void Theme::drawComboBox(juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)).reduced(0.5f);
    g.setColour(box.findColour(juce::ComboBox::backgroundColourId).brighter(box.isMouseOver() ? 0.08f : 0.0f));
    g.fillRoundedRectangle(r, 4.0f);
    g.setColour(colours::edge);
    g.drawRoundedRectangle(r, 4.0f, 1.0f);
    juce::Path arrow;
    const float ax = static_cast<float>(w) - 10.0f, ay = static_cast<float>(h) * 0.5f;
    arrow.addTriangle(ax - 4.0f, ay - 2.0f, ax + 4.0f, ay - 2.0f, ax, ay + 3.0f);
    g.setColour(colours::gold);
    g.fillPath(arrow);
}

void Theme::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(4, 0, box.getWidth() - 20, box.getHeight());
    label.setFont(getComboBoxFont(box));
    label.setMinimumHorizontalScale(0.55f);   // narrow cells squeeze the text rather than cut it
}

void Theme::drawToggleButton(juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    const auto r = b.getLocalBounds().toFloat();
    const float h = std::min(r.getHeight(), 16.0f), w = h * 1.8f;
    const auto pill = juce::Rectangle<float>(r.getX() + 1.0f, r.getCentreY() - h * 0.5f, w, h);
    const bool on = b.getToggleState();
    g.setColour(on ? colours::accent : colours::track.brighter(highlighted ? 0.15f : 0.0f));
    g.fillRoundedRectangle(pill, h * 0.5f);
    g.setColour(colours::text);
    const float d = h - 4.0f;
    g.fillEllipse(on ? pill.getRight() - d - 2.0f : pill.getX() + 2.0f, pill.getY() + 2.0f, d, d);
    if (b.getButtonText().isNotEmpty()) {
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(12.5f));
        g.drawText(b.getButtonText(), r.withTrimmedLeft(w + 6.0f), juce::Justification::centredLeft);
    }
}

void Theme::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour& bg, bool highlighted, bool down)
{
    const auto r = b.getLocalBounds().toFloat().reduced(0.5f);
    auto c = b.getToggleState() ? b.findColour(juce::TextButton::buttonOnColourId) : bg;
    if (down) c = c.brighter(0.2f);
    else if (highlighted) c = c.brighter(0.08f);
    g.setColour(c);
    g.fillRoundedRectangle(r, 4.0f);
    g.setColour(colours::edge);
    g.drawRoundedRectangle(r, 4.0f, 1.0f);
}

void Theme::drawButtonText(juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    g.setFont(getTextButtonFont(b, b.getHeight()));
    g.setColour(b.findColour(b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                    .withMultipliedAlpha(b.isEnabled() ? 1.0f : 0.4f));
    g.drawFittedText(b.getButtonText(), b.getLocalBounds().reduced(2, 1), juce::Justification::centred, 1, 0.7f);
}

void Theme::drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical, int thumbStart,
                          int thumbSize, bool over, bool)
{
    auto thumb = vertical ? juce::Rectangle<int>(x, thumbStart, w, thumbSize) : juce::Rectangle<int>(thumbStart, y, thumbSize, h);
    g.setColour(over ? colours::accent : colours::accentDim);
    g.fillRoundedRectangle(thumb.toFloat().reduced(2.0f), 3.0f);
}

} // namespace winerose::ui

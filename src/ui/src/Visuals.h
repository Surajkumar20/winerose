#pragma once

#include "Widgets.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <string>
#include <vector>

namespace winerose::ui {

/**
 * @brief Drawable breakpoint curve bound to a curve-string parameter ("x,y,c;..." — LFO paths, warp Remap).
 *
 * Drag a point to move it (end points move vertically only), double-click empty space to add a point,
 * double-click a point to remove it, Alt-drag (or right-drag) inside a segment to bend it. Each gesture is
 * written once on release, so it is one undo step. Shape maths mirror the engine: segment y = a + (b-a)·bend(t, c),
 * bend(t, c) = (e^{10ct} - 1)/(e^{10c} - 1).
 */
class CurveEditor final : public juce::Component, public juce::SettableTooltipClient, private ParamHub::Listener {
public:
    CurveEditor(ParamHub& hub, std::string nsKey, juce::String placeholder = {});
    ~CurveEditor() override;
    void setKey(std::string nsKey);
    /** When set and returning false, the editor is dimmed with this note (e.g. LFO shape isn't Path). */
    std::function<bool()> isActive;
    juce::String inactiveNote;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void paramChanged() override;

private:
    struct Point { double x, y, c; };
    void load();
    void store();
    double evaluate(double x) const;
    juce::Rectangle<float> area() const { return getLocalBounds().toFloat().reduced(6.0f); }
    juce::Point<float> toScreen(double x, double y) const;
    int hitPoint(juce::Point<float> p) const;

    ParamHub& m_hub;
    std::string m_key;
    std::vector<Point> m_points;
    int m_drag = -1;
    int m_bendSegment = -1;
    double m_bendStartC = 0.0;
    float m_bendStartY = 0.0f;
    bool m_changed = false;
};

/** ADSR(+hold) graph of an Env module, redrawn whenever one of its parameters changes. */
class EnvelopeView final : public juce::Component {
public:
    explicit EnvelopeView(ParamHub& hub);
    ~EnvelopeView() override;
    void setModule(std::string module) { m_module = std::move(module); repaint(); }
    void paint(juce::Graphics&) override;

private:
    ParamHub& m_hub;
    std::string m_module = "Env0";
    int m_anyId = 0;
};

/** Oscillator display: the current wavetable frame, or the loaded sample's overview. */
class OscView final : public juce::Component, private juce::AsyncUpdater {
public:
    OscView(ParamHub& hub, int oscillator);
    ~OscView() override;
    void paint(juce::Graphics&) override;

private:
    void handleAsyncUpdate() override;
    ParamHub& m_hub;
    int m_osc;
    std::vector<float> m_data;
    bool m_isSample = false;
    int m_anyId = 0;
};

} // namespace winerose::ui

#include "Visuals.h"

#include "Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace winerose::ui {

namespace {

double bend(double t, double c)
{
    const double k = 10.0 * c;
    if (std::abs(k) < 1e-6) return t;
    return (std::exp(k * t) - 1.0) / (std::exp(k) - 1.0);
}

void drawBackground(juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour(colours::background);
    g.fillRoundedRectangle(r, 4.0f);
    g.setColour(colours::edge);
    for (int i = 1; i < 4; ++i) {
        g.drawHorizontalLine(static_cast<int>(r.getY() + r.getHeight() * i / 4.0f), r.getX(), r.getRight());
        g.drawVerticalLine(static_cast<int>(r.getX() + r.getWidth() * i / 4.0f), r.getY(), r.getBottom());
    }
    g.drawRoundedRectangle(r, 4.0f, 1.0f);
}

} // namespace

// --- CurveEditor -----------------------------------------------------------------------------------------

CurveEditor::CurveEditor(ParamHub& hub, std::string nsKey, juce::String placeholder) : m_hub(hub), m_key(std::move(nsKey))
{
    inactiveNote = std::move(placeholder);
    m_hub.add(m_key, this);
    load();
}

CurveEditor::~CurveEditor() { m_hub.remove(m_key, this); }

void CurveEditor::setKey(std::string nsKey)
{
    m_hub.remove(m_key, this);
    m_key = std::move(nsKey);
    m_hub.add(m_key, this);
    load();
}

void CurveEditor::paramChanged()
{
    if (m_drag < 0 && m_bendSegment < 0) load();   // never yank points from under the mouse
    repaint();
}

void CurveEditor::load()
{
    m_points.clear();
    std::stringstream all(m_hub.text(m_key));
    std::string item;
    while (std::getline(all, item, ';')) {
        std::replace(item.begin(), item.end(), ',', ' ');
        std::istringstream f(item);
        Point p {0, 0, 0};
        if (f >> p.x >> p.y) {
            if (!(f >> p.c)) p.c = 0.0;
            m_points.push_back(p);
        }
    }
    if (m_points.size() < 2) m_points = {{0, 0, 0}, {0.5, 1, 0}, {1, 0, 0}};
    std::sort(m_points.begin(), m_points.end(), [](const Point& a, const Point& b) { return a.x < b.x; });
    repaint();
}

void CurveEditor::store()
{
    std::string text;
    char buf[80];
    for (const auto& p : m_points) {
        std::snprintf(buf, sizeof(buf), "%s%.4g,%.4g,%.3g", text.empty() ? "" : ";", p.x, p.y, p.c);
        text += buf;
    }
    m_hub.controller().set(m_key, text);
}

double CurveEditor::evaluate(double x) const
{
    for (std::size_t i = 1; i < m_points.size(); ++i) {
        const auto& a = m_points[i - 1];
        const auto& b = m_points[i];
        if (x <= b.x) {
            const double span = b.x - a.x;
            if (span <= 0.0) return b.y;
            return a.y + (b.y - a.y) * bend((x - a.x) / span, a.c);
        }
    }
    return m_points.back().y;
}

juce::Point<float> CurveEditor::toScreen(double x, double y) const
{
    const auto r = area();
    return {r.getX() + static_cast<float>(x) * r.getWidth(), r.getBottom() - static_cast<float>(y) * r.getHeight()};
}

int CurveEditor::hitPoint(juce::Point<float> p) const
{
    for (std::size_t i = 0; i < m_points.size(); ++i)
        if (toScreen(m_points[i].x, m_points[i].y).getDistanceFrom(p) < 7.0f) return static_cast<int>(i);
    return -1;
}

void CurveEditor::paint(juce::Graphics& g)
{
    const auto r = area();
    drawBackground(g, r);
    const bool active = !isActive || isActive();
    juce::Path path;
    const int n = std::max(32, static_cast<int>(r.getWidth()));
    for (int i = 0; i <= n; ++i) {
        const double x = static_cast<double>(i) / n;
        const auto p = toScreen(x, evaluate(x));
        if (i == 0) path.startNewSubPath(p);
        else path.lineTo(p);
    }
    juce::Path fill = path;
    fill.lineTo(r.getRight(), r.getBottom());
    fill.lineTo(r.getX(), r.getBottom());
    fill.closeSubPath();
    g.setColour(colours::accent.withAlpha(active ? 0.18f : 0.06f));
    g.fillPath(fill);
    g.setColour(active ? colours::accent : colours::accentDim);
    g.strokePath(path, juce::PathStrokeType(2.0f));
    if (active) {
        for (const auto& p : m_points) {
            const auto s = toScreen(p.x, p.y);
            g.setColour(colours::gold);
            g.fillEllipse(s.x - 4.0f, s.y - 4.0f, 8.0f, 8.0f);
        }
    } else if (inactiveNote.isNotEmpty()) {
        g.setColour(colours::textDim);
        g.setFont(juce::FontOptions(12.0f));
        g.drawFittedText(inactiveNote, getLocalBounds().reduced(10), juce::Justification::centred, 2);
    }
}

void CurveEditor::mouseDown(const juce::MouseEvent& e)
{
    if (isActive && !isActive()) return;
    m_changed = false;
    m_drag = hitPoint(e.position);
    m_bendSegment = -1;
    if (m_drag < 0 && (e.mods.isAltDown() || e.mods.isPopupMenu())) {
        const auto r = area();
        const double x = (e.position.x - r.getX()) / r.getWidth();
        for (std::size_t i = 1; i < m_points.size(); ++i)
            if (x >= m_points[i - 1].x && x <= m_points[i].x) { m_bendSegment = static_cast<int>(i) - 1; break; }
        if (m_bendSegment >= 0) {
            m_bendStartC = m_points[static_cast<std::size_t>(m_bendSegment)].c;
            m_bendStartY = e.position.y;
        }
    }
}

void CurveEditor::mouseDrag(const juce::MouseEvent& e)
{
    const auto r = area();
    if (m_drag >= 0) {
        auto& p = m_points[static_cast<std::size_t>(m_drag)];
        const bool endPoint = m_drag == 0 || m_drag == static_cast<int>(m_points.size()) - 1;
        if (!endPoint) {
            const double lo = m_points[static_cast<std::size_t>(m_drag - 1)].x, hi = m_points[static_cast<std::size_t>(m_drag + 1)].x;
            p.x = std::clamp(static_cast<double>((e.position.x - r.getX()) / r.getWidth()), lo, hi);
        }
        p.y = std::clamp(static_cast<double>((r.getBottom() - e.position.y) / r.getHeight()), 0.0, 1.0);
        if (e.mods.isShiftDown()) { p.y = std::round(p.y * 8.0) / 8.0; if (!endPoint) p.x = std::round(p.x * 16.0) / 16.0; }
        m_changed = true;
        repaint();
    } else if (m_bendSegment >= 0) {
        auto& a = m_points[static_cast<std::size_t>(m_bendSegment)];
        const auto& b = m_points[static_cast<std::size_t>(m_bendSegment + 1)];
        const double dir = b.y >= a.y ? 1.0 : -1.0;   // dragging up always bends the curve upward
        a.c = std::clamp(m_bendStartC - dir * (e.position.y - m_bendStartY) / 120.0, -1.0, 1.0);
        m_changed = true;
        repaint();
    }
}

void CurveEditor::mouseUp(const juce::MouseEvent&)
{
    if (m_changed) store();
    m_drag = -1;
    m_bendSegment = -1;
    m_changed = false;
}

void CurveEditor::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (isActive && !isActive()) return;
    const int hit = hitPoint(e.position);
    if (hit > 0 && hit < static_cast<int>(m_points.size()) - 1) {
        m_points.erase(m_points.begin() + hit);
    } else if (hit < 0) {
        const auto r = area();
        const double x = std::clamp(static_cast<double>((e.position.x - r.getX()) / r.getWidth()), 0.001, 0.999);
        const double y = std::clamp(static_cast<double>((r.getBottom() - e.position.y) / r.getHeight()), 0.0, 1.0);
        m_points.push_back({x, y, 0.0});
        std::sort(m_points.begin(), m_points.end(), [](const Point& a, const Point& b) { return a.x < b.x; });
    } else {
        return;
    }
    store();
    repaint();
}

// --- EnvelopeView ----------------------------------------------------------------------------------------

EnvelopeView::EnvelopeView(ParamHub& hub) : m_hub(hub)
{
    m_anyId = m_hub.onAny([this](const control::ParamChange& c) {
        if (c.everything || c.nsKey.rfind(m_module + ".", 0) == 0) repaint();
    });
}

EnvelopeView::~EnvelopeView() { m_hub.removeAny(m_anyId); }

void EnvelopeView::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced(6.0f);
    drawBackground(g, r);
    auto num = [this](const char* k) { return m_hub.number(m_module + "." + k); };
    const double a = num("attack"), h = num("hold"), d = num("decay"), s = std::clamp(num("sustain"), 0.0, 1.0), rel = num("release");
    const double sustainTime = std::max(0.25, (a + h + d + rel) * 0.25);
    const double total = std::max(1e-3, a + h + d + sustainTime + rel);
    auto shape = [](double x, double c) {
        if (std::abs(c) < 1e-4) return x;
        return (std::exp(c * x) - 1.0) / (std::exp(c) - 1.0);
    };
    juce::Path path;
    auto point = [&](double t, double level) {
        return juce::Point<float>(r.getX() + static_cast<float>(t / total) * r.getWidth(), r.getBottom() - static_cast<float>(level) * r.getHeight());
    };
    path.startNewSubPath(point(0.0, 0.0));
    auto stage = [&](double t0, double dur, double from, double to, double curve) {
        const int steps = 40;
        for (int i = 1; i <= steps; ++i) {
            const double x = static_cast<double>(i) / steps;
            path.lineTo(point(t0 + dur * x, from + (to - from) * shape(x, curve)));
        }
    };
    double t = 0.0;
    stage(t, a, 0.0, 1.0, num("attackCurve")); t += a;
    path.lineTo(point(t + h, 1.0)); t += h;
    stage(t, d, 1.0, s, num("decayCurve")); t += d;
    path.lineTo(point(t + sustainTime, s)); t += sustainTime;
    stage(t, rel, s, 0.0, num("releaseCurve"));
    juce::Path fill = path;
    fill.lineTo(r.getRight(), r.getBottom());
    fill.closeSubPath();
    g.setColour(colours::accent.withAlpha(0.18f));
    g.fillPath(fill);
    g.setColour(colours::accent);
    g.strokePath(path, juce::PathStrokeType(2.0f));
    g.setColour(colours::textDim);
    g.setFont(juce::FontOptions(10.5f));
    g.drawText(juce::String(total - sustainTime, 2) + " s + sustain", r.reduced(4.0f), juce::Justification::topRight);
}

// --- OscView ---------------------------------------------------------------------------------------------

OscView::OscView(ParamHub& hub, int oscillator) : m_hub(hub), m_osc(oscillator)
{
    const std::string prefix = "Oscillator" + std::to_string(oscillator) + ".";
    m_anyId = m_hub.onAny([this, prefix](const control::ParamChange& c) {
        if (c.everything || c.nsKey.rfind(prefix, 0) == 0) triggerAsyncUpdate();
    });
    handleAsyncUpdate();
}

OscView::~OscView()
{
    cancelPendingUpdate();
    m_hub.removeAny(m_anyId);
}

void OscView::handleAsyncUpdate()
{
    const auto type = static_cast<int>(std::lround(m_hub.number("Oscillator" + std::to_string(m_osc) + ".type")));
    m_isSample = type != 0;
    m_data = m_hub.controller().oscillatorPreview(m_osc, m_isSample ? 256 : 512);
    repaint();
}

void OscView::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced(4.0f, 2.0f);
    g.setColour(colours::background);
    g.fillRoundedRectangle(r, 4.0f);
    g.setColour(colours::edge);
    g.drawHorizontalLine(static_cast<int>(r.getCentreY()), r.getX(), r.getRight());
    if (m_data.empty()) {
        g.setColour(colours::textDim);
        g.setFont(juce::FontOptions(11.5f));
        g.drawText(m_isSample ? "Load a sample or SFZ" : "", r, juce::Justification::centred);
        return;
    }
    const bool on = m_hub.number("Oscillator" + std::to_string(m_osc) + ".enabled") >= 0.5;
    const auto colour = on ? colours::accent : colours::accentDim;
    const float half = r.getHeight() * 0.45f;
    if (m_isSample) {
        g.setColour(colour.withAlpha(0.8f));
        const float w = r.getWidth() / static_cast<float>(m_data.size());
        for (std::size_t i = 0; i < m_data.size(); ++i) {
            const float h = std::max(0.5f, m_data[i] * half);
            g.fillRect(r.getX() + static_cast<float>(i) * w, r.getCentreY() - h, std::max(1.0f, w - 0.5f), 2.0f * h);
        }
        return;
    }
    juce::Path p;
    for (std::size_t i = 0; i < m_data.size(); ++i) {
        const float x = r.getX() + r.getWidth() * static_cast<float>(i) / static_cast<float>(m_data.size() - 1);
        const float y = r.getCentreY() - std::clamp(m_data[i], -1.0f, 1.0f) * half;
        if (i == 0) p.startNewSubPath(x, y);
        else p.lineTo(x, y);
    }
    g.setColour(colour);
    g.strokePath(p, juce::PathStrokeType(1.8f));
}

} // namespace winerose::ui

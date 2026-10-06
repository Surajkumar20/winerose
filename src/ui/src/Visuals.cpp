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

OscView::OscView(ParamHub& hub, int oscillator, bool large) : m_hub(hub), m_osc(oscillator), m_large(large)
{
    const std::string prefix = "Oscillator" + std::to_string(oscillator) + ".";
    m_anyId = m_hub.onAny([this, prefix](const control::ParamChange& c) {
        if (c.everything) { m_tableDirty = true; triggerAsyncUpdate(); return; }
        if (c.nsKey.rfind(prefix, 0) != 0) return;
        const std::string key = c.nsKey.substr(prefix.size());
        // Moving the position only needs the current frame; anything that can swap the table needs all of it.
        if (key == "type" || key == "wavetablePath" || key == "wavetableData" || key == "samplePath" || key == "multisamplePath")
            m_tableDirty = true;
        if (key == "wtPos" || m_tableDirty || key == "enabled") triggerAsyncUpdate();
    });
    setTooltip(large ? juce::String() : juce::String("Click 2D/3D to switch views; double-click to enlarge"));
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
    const int points = m_large ? 512 : 256;
    m_current = m_hub.controller().oscillatorPreview(m_osc, m_isSample ? points / 2 : points);
    if (!m_isSample && m_tableDirty) m_table = m_hub.controller().wavetablePreview(m_osc, m_large ? 96 : 48, m_large ? 256 : 128);
    m_tableDirty = false;
    repaint();
}

juce::Rectangle<float> OscView::modeTag() const
{
    const auto r = getLocalBounds().toFloat().reduced(4.0f, 2.0f);
    return {r.getRight() - 26.0f, r.getY() + 2.0f, 24.0f, 14.0f};
}

void OscView::mouseUp(const juce::MouseEvent& e)
{
    if (!m_isSample && modeTag().expanded(3.0f).contains(e.position) && e.getNumberOfClicks() == 1) setThreeD(!m_threeD);
}

void OscView::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (m_large || modeTag().expanded(3.0f).contains(e.position)) return;
    auto big = std::make_unique<OscView>(m_hub, m_osc, true);
    big->setThreeD(m_threeD);
    big->setSize(600, 340);
    juce::CallOutBox::launchAsynchronously(std::move(big), getScreenBounds(), nullptr);
}

void OscView::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced(4.0f, 2.0f);
    g.setColour(colours::background);
    g.fillRoundedRectangle(r, 4.0f);
    const bool on = m_large || m_hub.number("Oscillator" + std::to_string(m_osc) + ".enabled") >= 0.5;

    if (m_current.empty()) {
        g.setColour(colours::textDim);
        g.setFont(juce::FontOptions(11.5f));
        g.drawText(m_isSample ? "Load a sample or SFZ" : "", r, juce::Justification::centred);
        return;
    }
    if (m_isSample) {
        const auto colour = on ? colours::accent : colours::accentDim;
        const float half = r.getHeight() * 0.45f;
        g.setColour(colour.withAlpha(0.8f));
        const float w = r.getWidth() / static_cast<float>(m_current.size());
        for (std::size_t i = 0; i < m_current.size(); ++i) {
            const float h = std::max(0.5f, m_current[i] * half);
            g.fillRect(r.getX() + static_cast<float>(i) * w, r.getCentreY() - h, std::max(1.0f, w - 0.5f), 2.0f * h);
        }
        return;
    }

    const auto inner = r.reduced(m_large ? 14.0f : 3.0f, m_large ? 12.0f : 3.0f);
    if (m_threeD && m_table.frames.size() > 1) paintStack(g, inner, on);
    else paintFlat(g, inner, on);

    // Mode tag, and the frame readout in the large view.
    const auto tag = modeTag();
    g.setColour(colours::panelHi);
    g.fillRoundedRectangle(tag, 3.0f);
    g.setColour(colours::gold);
    g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
    g.drawText(m_threeD ? "3D" : "2D", tag, juce::Justification::centred);
    if (m_large && m_table.totalFrames > 0) {
        const double pos = m_hub.number("Oscillator" + std::to_string(m_osc) + ".wtPos");
        const int frame = static_cast<int>(std::lround(pos * (m_table.totalFrames - 1))) + 1;
        g.setColour(colours::textDim);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText("Frame " + juce::String(frame) + " / " + juce::String(m_table.totalFrames), r.reduced(10.0f, 6.0f),
                   juce::Justification::topLeft);
    }
}

void OscView::paintFlat(juce::Graphics& g, juce::Rectangle<float> r, bool on)
{
    g.setColour(colours::edge);
    g.drawHorizontalLine(static_cast<int>(r.getCentreY()), r.getX(), r.getRight());
    juce::Path p;
    const float half = r.getHeight() * 0.48f;
    for (std::size_t i = 0; i < m_current.size(); ++i) {
        const float x = r.getX() + r.getWidth() * static_cast<float>(i) / static_cast<float>(m_current.size() - 1);
        const float y = r.getCentreY() - std::clamp(m_current[i], -1.0f, 1.0f) * half;
        if (i == 0) p.startNewSubPath(x, y);
        else p.lineTo(x, y);
    }
    g.setColour(on ? colours::accent : colours::accentDim);
    g.strokePath(p, juce::PathStrokeType(m_large ? 2.4f : 1.8f));
}

void OscView::paintStack(juce::Graphics& g, juce::Rectangle<float> r, bool on)
{
    // Frame 1 at the front-left bottom, the last frame at the back-right top (an oblique projection).
    const float depthX = r.getWidth() * 0.26f, depthY = r.getHeight() * 0.42f;
    const float waveW = r.getWidth() - depthX;
    const float amp = (r.getHeight() - depthY) * 0.46f;
    const int last = std::max(1, m_table.totalFrames - 1);
    auto framePath = [&](const std::vector<float>& data, float t) {
        juce::Path p;
        const float x0 = r.getX() + t * depthX;
        const float yc = r.getBottom() - amp - t * depthY;
        for (std::size_t i = 0; i < data.size(); ++i) {
            const float x = x0 + waveW * static_cast<float>(i) / static_cast<float>(data.size() - 1);
            const float y = yc - std::clamp(data[i], -1.0f, 1.0f) * amp;
            if (i == 0) p.startNewSubPath(x, y);
            else p.lineTo(x, y);
        }
        return p;
    };
    const auto base = on ? colours::gold : colours::textDim;
    for (int j = static_cast<int>(m_table.frames.size()) - 1; j >= 0; --j) {
        const float t = static_cast<float>(m_table.frameIndex[static_cast<std::size_t>(j)]) / static_cast<float>(last);
        g.setColour(base.withAlpha(0.10f + 0.30f * (1.0f - t)));   // nearer frames read stronger
        g.strokePath(framePath(m_table.frames[static_cast<std::size_t>(j)], t), juce::PathStrokeType(m_large ? 1.1f : 0.8f));
    }
    // The frame being played, at its depth.
    const float t = static_cast<float>(std::clamp(m_hub.number("Oscillator" + std::to_string(m_osc) + ".wtPos"), 0.0, 1.0));
    const auto current = framePath(m_current, t);
    g.setColour((on ? colours::accent : colours::accentDim).withAlpha(0.35f));
    g.strokePath(current, juce::PathStrokeType(m_large ? 6.0f : 4.0f));
    g.setColour(on ? colours::accent.brighter(0.3f) : colours::accentDim);
    g.strokePath(current, juce::PathStrokeType(m_large ? 2.4f : 1.7f));
}

} // namespace winerose::ui

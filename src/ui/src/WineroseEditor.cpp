#include "ui/WineroseEditor.h"

namespace winerose::ui {

namespace {
constexpr int kRowHeight     = 30;
constexpr int kSectionHeight = 34;
constexpr int kHeader        = 56;
constexpr int kMargin        = 16;
constexpr int kScrollbar     = 12;
}

WineroseEditor::WineroseEditor(juce::AudioProcessor& processor, control::IController& controller)
    : juce::AudioProcessorEditor(processor)
    , m_controller(controller)
{
    m_title.setText("Winerose", juce::dontSendNotification);
    m_title.setFont(juce::FontOptions(22.0f, juce::Font::bold));
    addAndMakeVisible(m_title);

    m_undo.onClick = [this] { m_controller.undo(); };
    m_redo.onClick = [this] { m_controller.redo(); };
    addAndMakeVisible(m_undo);
    addAndMakeVisible(m_redo);

    m_viewport.setViewedComponent(&m_content, false);
    m_viewport.setScrollBarsShown(true, false);
    m_viewport.setScrollBarThickness(kScrollbar);
    addAndMakeVisible(m_viewport);

    buildRows();
    refreshAll();

    m_subscription = m_controller.onChange([this](const control::ParamChange& change) {
        if (change.everything) { refreshAll(); return; }
        if (auto* row = findRow(change.nsKey)) refresh(*row);
        // A type change renames the module's generic knobs (FX slots): refresh its labels and readouts.
        const auto dot = change.nsKey.find('.');
        if (dot != std::string::npos && change.nsKey.compare(dot + 1, std::string::npos, "type") == 0)
            refreshModule(change.nsKey.substr(0, dot));
        m_undo.setEnabled(m_controller.canUndo());
        m_redo.setEnabled(m_controller.canRedo());
    });

    setResizable(true, true);
    setResizeLimits(420, 240, 2400, 2400);
    setSize(600, 760);
}

WineroseEditor::~WineroseEditor()
{
    m_subscription.reset();
}

void WineroseEditor::buildRows()
{
    std::string currentModule;
    for (auto& schema : m_controller.schema()) {
        auto row = std::make_unique<Row>();
        row->schema = std::move(schema);
        Row* r = row.get();
        const std::string nsKey = r->schema.nsKey;

        if (r->schema.module != currentModule) {
            currentModule = r->schema.module;
            r->section = std::make_unique<juce::Label>();
            r->section->setText(currentModule, juce::dontSendNotification);
            r->section->setFont(juce::FontOptions(16.0f, juce::Font::bold));
            r->section->setColour(juce::Label::textColourId, juce::Colour(0xffe8a0b8));
            m_content.addAndMakeVisible(*r->section);
        }

        r->label = std::make_unique<juce::Label>();
        r->label->setText(m_controller.label(nsKey), juce::dontSendNotification);
        r->label->setTooltip(r->schema.tooltip);
        m_content.addAndMakeVisible(*r->label);

        if (r->schema.type == "enum" || r->schema.type == "bool") {
            r->combo = std::make_unique<juce::ComboBox>();
            for (const auto& [value, label] : r->schema.choices)
                r->combo->addItem(label, value + 1);   // ComboBox IDs must be non-zero
            r->combo->onChange = [this, r] {
                commit(*r, control::ParamValue(static_cast<double>(r->combo->getSelectedId() - 1)));
            };
            m_content.addAndMakeVisible(*r->combo);
        } else if (r->schema.type == "string") {
            r->text = std::make_unique<juce::TextEditor>();
            r->text->onReturnKey = [this, r] { commit(*r, control::ParamValue(r->text->getText().toStdString())); };
            r->text->onFocusLost = r->text->onReturnKey;
            m_content.addAndMakeVisible(*r->text);
        } else {
            r->slider = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
            r->slider->setRange(0.0, 1.0, 0.0);
            r->slider->setScrollWheelEnabled(false);   // the wheel scrolls the list, not the knob under it
            r->slider->setDoubleClickReturnValue(true, m_controller.toNormalized(nsKey, r->schema.defaultValue.number()));
            r->slider->textFromValueFunction = [this, nsKey](double n) {
                return juce::String(m_controller.format(nsKey, m_controller.fromNormalized(nsKey, n)));
            };
            r->slider->valueFromTextFunction = [this, nsKey](const juce::String& t) {
                return m_controller.toNormalized(nsKey, t.getDoubleValue());
            };
            r->slider->onDragStart = [this, r] { r->dragging = true; m_controller.beginGesture(r->schema.nsKey); };
            r->slider->onDragEnd   = [this, r] { m_controller.endGesture(r->schema.nsKey); r->dragging = false; };
            r->slider->onValueChange = [this, r] {
                const double plain = m_controller.fromNormalized(r->schema.nsKey, r->slider->getValue());
                commit(*r, control::ParamValue(plain));
            };
            m_content.addAndMakeVisible(*r->slider);
        }
        m_rows.push_back(std::move(row));
    }
}

void WineroseEditor::commit(Row& row, const control::ParamValue& value)
{
    // A drag is already bracketed by onDragStart/onDragEnd; one-off edits get their own gesture so the
    // host records an automation point and undo gets one step.
    if (row.dragging) {
        m_controller.set(row.schema.nsKey, value);
        return;
    }
    m_controller.beginGesture(row.schema.nsKey);
    m_controller.set(row.schema.nsKey, value);
    m_controller.endGesture(row.schema.nsKey);
}

WineroseEditor::Row* WineroseEditor::findRow(const std::string& nsKey)
{
    for (auto& r : m_rows)
        if (r->schema.nsKey == nsKey) return r.get();
    return nullptr;
}

void WineroseEditor::refresh(Row& row)
{
    const auto value = m_controller.get(row.schema.nsKey);
    if (row.slider) {
        if (!row.dragging)
            row.slider->setValue(m_controller.toNormalized(row.schema.nsKey, value.number()), juce::dontSendNotification);
        row.slider->updateText();
    } else if (row.combo) {
        row.combo->setSelectedId(static_cast<int>(value.number()) + 1, juce::dontSendNotification);
    } else if (row.text && !row.text->hasKeyboardFocus(true)) {
        row.text->setText(value.text(), juce::dontSendNotification);
    }
}

void WineroseEditor::refreshModule(const std::string& module)
{
    for (auto& r : m_rows) {
        if (r->schema.module != module) continue;
        r->label->setText(m_controller.label(r->schema.nsKey), juce::dontSendNotification);
        refresh(*r);
    }
}

void WineroseEditor::refreshAll()
{
    for (auto& r : m_rows) {
        r->label->setText(m_controller.label(r->schema.nsKey), juce::dontSendNotification);
        refresh(*r);
    }
    m_undo.setEnabled(m_controller.canUndo());
    m_redo.setEnabled(m_controller.canRedo());
}

void WineroseEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1b1520));
    g.setColour(juce::Colour(0xff6b2a45));
    g.fillRect(0, kHeader - 2, getWidth(), 2);
}

void WineroseEditor::resized()
{
    auto area = getLocalBounds().reduced(kMargin, 0);
    auto header = area.removeFromTop(kHeader).reduced(0, 10);
    m_redo.setBounds(header.removeFromRight(64));
    header.removeFromRight(8);
    m_undo.setBounds(header.removeFromRight(64));
    m_title.setBounds(header);

    m_viewport.setBounds(getLocalBounds().withTrimmedTop(kHeader));
    layoutRows();
}

void WineroseEditor::layoutRows()
{
    const int width = m_viewport.getWidth() - kScrollbar;
    int y = kMargin / 2;
    for (auto& r : m_rows) {
        if (r->section) {
            r->section->setBounds(kMargin, y + 6, width - 2 * kMargin, kSectionHeight - 6);
            y += kSectionHeight;
        }
        auto line = juce::Rectangle<int>(kMargin, y, width - 2 * kMargin, kRowHeight).reduced(0, 3);
        r->label->setBounds(line.removeFromLeft(160));
        if (r->slider) r->slider->setBounds(line);
        if (r->combo)  r->combo->setBounds(line.removeFromLeft(200));
        if (r->text)   r->text->setBounds(line);
        y += kRowHeight;
    }
    m_content.setSize(width, y + kMargin);
}

} // namespace winerose::ui

#include "ui/WineroseEditor.h"

#include "Pages.h"
#include "Theme.h"
#include "Widgets.h"

namespace winerose::ui {

namespace {

class ClickLabel final : public juce::Label {
public:
    std::function<void()> onClick;
    void mouseUp(const juce::MouseEvent&) override { if (onClick) onClick(); }
};

class Meter final : public juce::Component, private juce::Timer {
public:
    explicit Meter(control::IController& c) : m_controller(c) { startTimerHz(30); }
    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const float h = r.getHeight() / 2.0f - 1.0f;
        for (int ch = 0; ch < 2; ++ch) {
            const auto bar = juce::Rectangle<float>(r.getX(), r.getY() + static_cast<float>(ch) * (h + 2.0f), r.getWidth(), h);
            g.setColour(colours::track);
            g.fillRoundedRectangle(bar, 2.0f);
            const float db = juce::Decibels::gainToDecibels(m_level[ch], -60.0f);
            const float frac = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
            g.setColour(m_level[ch] >= 1.0f ? juce::Colours::red : (db > -6.0f ? colours::gold : colours::accent));
            g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * frac), 2.0f);
        }
    }

private:
    void timerCallback() override
    {
        const auto m = m_controller.meters();
        m_level[0] = std::max(m.peakLeft, m_level[0] * 0.85f);
        m_level[1] = std::max(m.peakRight, m_level[1] * 0.85f);
        repaint();
    }
    control::IController& m_controller;
    float m_level[2] = {};
};

} // namespace

/** Everything inside the window, laid out at the design size and scaled by the editor. */
class Content final : public juce::Component, private juce::Timer {
public:
    Content(ParamHub& hub, juce::MidiKeyboardState* keyboard)
        : m_hub(hub), m_meter(hub.controller()), m_master(hub, "Global.masterVolume")
    {
        m_logo.setText("WINEROSE", juce::dontSendNotification);
        m_logo.setFont(juce::FontOptions(24.0f, juce::Font::bold));
        m_logo.setColour(juce::Label::textColourId, colours::gold);
        addAndMakeVisible(m_logo);

        m_pages.setTabs({"Sound", "Matrix", "FX", "MIDI", "Presets", "All Params"});
        m_pages.onSelect = [this](int i) { showPage(i); };
        addAndMakeVisible(m_pages);

        m_prev.onClick = [this] { stepPreset(-1); };
        m_next.onClick = [this] { stepPreset(+1); };
        m_name.onClick = [this] { m_pages.setSelected(4); };
        m_name.setTooltip("Browse presets");
        m_save.onClick = [this] { savePreset(); };
        m_import.setTooltip("Import a preset file: .SerumPreset, .fxp/.fxb (Serum 1) or a Winerose preset");
        m_import.onClick = [this] { importFile(); };
        addAndMakeVisible(m_import);
        m_status.onClick = [this] { showImportReport(); };
        m_status.setMouseCursor(juce::MouseCursor::PointingHandCursor);
        m_undo.onClick = [this] { m_hub.controller().undo(); };
        m_redo.onClick = [this] { m_hub.controller().redo(); };
        for (auto* b : {&m_prev, &m_next, &m_name, &m_save, &m_undo, &m_redo}) addAndMakeVisible(b);
        m_name.setButtonText("Init");
        addAndMakeVisible(m_master);
        addAndMakeVisible(m_meter);
        m_status.setFont(juce::FontOptions(12.0f));
        m_status.setColour(juce::Label::textColourId, colours::textDim);
        m_status.setMinimumHorizontalScale(0.6f);
        addAndMakeVisible(m_status);

        auto status = [this](const juce::String& s) { setStatus(s); };
        m_sound = std::make_unique<SoundPage>(hub, status);
        m_matrix = std::make_unique<MatrixPage>(hub);
        m_fx = std::make_unique<FxPage>(hub);
        m_midi = std::make_unique<MidiPage>(hub);
        juce::PropertiesFile::Options options;
        options.applicationName = "Winerose";
        options.folderName = "Winerose";
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        m_settings = std::make_unique<juce::PropertiesFile>(options);
        m_browser = std::make_unique<PresetBrowser>(hub, [this](const juce::File& f) { loadFile(f); }, m_settings.get());
        m_table = std::make_unique<TablePage>(hub, [this] { savePreset(); });
        for (juce::Component* p : pages()) addChildComponent(p);

        if (keyboard != nullptr) {
            m_keyboard = std::make_unique<juce::MidiKeyboardComponent>(*keyboard, juce::MidiKeyboardComponent::horizontalKeyboard);
            m_keyboard->setAvailableRange(24, 108);
            m_keyboard->setOctaveForMiddleC(4);
            m_keyboard->setKeyWidth(23.0f);
            addAndMakeVisible(*m_keyboard);
        }

        m_anyId = m_hub.onAny([this](const control::ParamChange&) { updateUndo(); });
        updateUndo();
        showPage(0);
    }

    ~Content() override { m_hub.removeAny(m_anyId); }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(colours::background);
        g.setColour(colours::panel);
        g.fillRect(getLocalBounds().removeFromTop(kHeader));
        g.setColour(colours::accent);
        g.fillRect(0, kHeader - 2, getWidth(), 2);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto header = r.removeFromTop(kHeader).reduced(10, 4);
        m_master.setBounds(header.removeFromRight(62));
        header.removeFromRight(6);
        auto top = header.removeFromTop(30);
        m_logo.setBounds(top.removeFromLeft(140));
        m_pages.setBounds(top.removeFromLeft(460).reduced(0, 2));
        top.removeFromLeft(10);
        m_prev.setBounds(top.removeFromLeft(26).reduced(0, 2));
        m_name.setBounds(top.removeFromLeft(170).reduced(2, 2));
        m_next.setBounds(top.removeFromLeft(26).reduced(0, 2));
        top.removeFromLeft(4);
        m_save.setBounds(top.removeFromLeft(50).reduced(0, 2));
        top.removeFromLeft(4);
        m_import.setBounds(top.removeFromLeft(64).reduced(0, 2));
        top.removeFromLeft(10);
        m_undo.setBounds(top.removeFromLeft(50).reduced(0, 2));
        m_redo.setBounds(top.removeFromLeft(54).reduced(0, 2).withTrimmedLeft(4));
        m_meter.setBounds(top.reduced(6, 7));
        m_status.setBounds(header.withTrimmedLeft(140));

        if (m_keyboard) m_keyboard->setBounds(r.removeFromBottom(kKeyboard));
        for (juce::Component* p : pages()) p->setBounds(r);
    }

    void loadFile(const juce::File& f)
    {
        juce::MemoryBlock data;
        if (!f.loadFileAsData(data)) { setStatus("Cannot read " + f.getFileName()); return; }
        const auto r = m_hub.controller().loadPreset(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data.getData()), data.getSize()));
        if (r.ok) {
            m_name.setButtonText(f.getFileNameWithoutExtension());
            m_browser->setCurrent(f);
            const bool serum = f.hasFileExtension("SerumPreset;fxp;fxb");
            setStatus(r.message.empty() ? "Loaded " + f.getFileName()
                                        : juce::String(r.message) + (serum ? "  (click for details)" : ""));
        } else {
            setStatus("Could not load " + f.getFileName() + ": " + juce::String(r.error));
        }
    }

    void loadOntoOscA(const juce::File& f)
    {
        const auto r = m_hub.controller().loadOscillatorFile(0, f.getFullPathName().toStdString());
        setStatus(r.ok ? juce::String(r.message) + " on OSC A" : "Could not load " + f.getFileName() + ": " + juce::String(r.error));
    }

private:
    static constexpr int kHeader = 64;
    static constexpr int kKeyboard = 72;

    std::vector<juce::Component*> pages() { return {m_sound.get(), m_matrix.get(), m_fx.get(), m_midi.get(), m_browser.get(), m_table.get()}; }

    void showPage(int i)
    {
        auto list = pages();
        for (std::size_t p = 0; p < list.size(); ++p) list[p]->setVisible(static_cast<int>(p) == i);
        if (i == 4) m_browser->rescan();
    }

    void stepPreset(int delta)
    {
        const auto f = m_browser->step(delta);
        if (f.existsAsFile()) loadFile(f);
        else setStatus("No presets in the current browser folder");
    }

    void importFile()
    {
        m_chooser = std::make_unique<juce::FileChooser>("Import a preset", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
                                                        "*.SerumPreset;*.fxp;*.fxb;*.wrpreset;*.json");
        m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
            if (fc.getResult().existsAsFile()) loadFile(fc.getResult());
        });
    }

    void showImportReport()
    {
        const auto report = juce::JSON::parse(juce::String(m_hub.controller().importReport()));
        if (!report.isObject() || report["format"].toString().isEmpty()) return;
        juce::String text;
        text << report["format"].toString() << "  \"" << report["name"].toString() << "\"";
        if (report["author"].toString().isNotEmpty()) text << " by " << report["author"].toString();
        text << "\n\nMapped onto Winerose controls: " << (static_cast<int>(report["mappedExplicit"]) + static_cast<int>(report["mappedBySynonym"]))
             << " (" << static_cast<int>(report["mappedBySynonym"]) << " by name, unverified)\n";
        const auto* unmapped = report["unmapped"].getArray();
        text << "Kept but not yet mapped: " << (unmapped != nullptr ? unmapped->size() : 0)
             << "  (stored in the patch; they start working when the Serum tables are filled in)\n";
        if (const auto* w = report["wavetables"].getArray()) text << "Wavetables loaded: " << w->size() << "\n";
        if (const auto* warnings = report["warnings"].getArray(); warnings != nullptr && !warnings->isEmpty()) {
            text << "\nWarnings:\n";
            for (const auto& w : *warnings) text << "  - " << w.toString() << "\n";
        }
        if (unmapped != nullptr && !unmapped->isEmpty()) {
            text << "\nUnmapped settings (first 40):\n";
            for (int i = 0; i < std::min(40, unmapped->size()); ++i) text << "  " << (*unmapped)[i].toString() << "\n";
        }
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Import report", text, "OK", this);
    }

    void savePreset()
    {
        auto* w = new juce::AlertWindow("Save preset", "Name (saved in " + PresetBrowser::userFolder().getFullPathName() + "):",
                                        juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor("name", m_name.getButtonText() == "Init" ? juce::String("My preset") : m_name.getButtonText());
        w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int result) {
            const auto name = juce::File::createLegalFileName(w->getTextEditorContents("name").trim());
            if (result != 1 || name.isEmpty()) return;
            const auto folder = PresetBrowser::userFolder();
            folder.createDirectory();
            const auto file = folder.getChildFile(name + ".wrpreset");
            if (file.replaceWithText(juce::String(m_hub.controller().saveState()))) {
                m_name.setButtonText(name);
                m_browser->rescan();
                m_browser->setCurrent(file);
                setStatus("Saved " + file.getFullPathName());
            } else {
                setStatus("Could not write " + file.getFullPathName());
            }
        }), true);
    }

    void setStatus(const juce::String& s)
    {
        m_status.setText(s, juce::dontSendNotification);
        m_status.setTooltip(s);
        startTimer(8000);
    }

    void timerCallback() override { m_status.setText({}, juce::dontSendNotification); stopTimer(); }

    void updateUndo()
    {
        m_undo.setEnabled(m_hub.controller().canUndo());
        m_redo.setEnabled(m_hub.controller().canRedo());
    }

    ParamHub& m_hub;
    juce::TooltipWindow m_tooltips {this, 600};
    juce::Label m_logo;
    ClickLabel m_status;
    juce::TextButton m_import {"Import..."};
    std::unique_ptr<juce::FileChooser> m_chooser;
    std::unique_ptr<juce::PropertiesFile> m_settings;
    TabStrip m_pages;
    juce::TextButton m_prev {"<"}, m_next {">"}, m_name, m_save {"Save"}, m_undo {"Undo"}, m_redo {"Redo"};
    Meter m_meter;
    Knob m_master;
    std::unique_ptr<SoundPage> m_sound;
    std::unique_ptr<MatrixPage> m_matrix;
    std::unique_ptr<FxPage> m_fx;
    std::unique_ptr<MidiPage> m_midi;
    std::unique_ptr<PresetBrowser> m_browser;
    std::unique_ptr<TablePage> m_table;
    std::unique_ptr<juce::MidiKeyboardComponent> m_keyboard;
    int m_anyId = 0;
};

WineroseEditor::WineroseEditor(juce::AudioProcessor& processor, control::IController& controller, juce::MidiKeyboardState* keyboard)
    : juce::AudioProcessorEditor(processor)
    , m_theme(std::make_unique<Theme>())
    , m_hub(std::make_unique<ParamHub>(controller))
{
    setLookAndFeel(m_theme.get());
    m_content = std::make_unique<Content>(*m_hub, keyboard);
    addAndMakeVisible(*m_content);
    m_content->setBounds(0, 0, kDesignWidth, kDesignHeight);

    setResizable(true, true);
    getConstrainer()->setFixedAspectRatio(static_cast<double>(kDesignWidth) / kDesignHeight);
    setResizeLimits(kDesignWidth / 2, kDesignHeight / 2, kDesignWidth * 2, kDesignHeight * 2);
    setSize(kDesignWidth * 9 / 10, kDesignHeight * 9 / 10);
}

WineroseEditor::~WineroseEditor()
{
    m_content.reset();
    m_hub.reset();
    setLookAndFeel(nullptr);
}

void WineroseEditor::paint(juce::Graphics& g) { g.fillAll(colours::background); }

void WineroseEditor::resized()
{
    const float scale = static_cast<float>(getWidth()) / static_cast<float>(kDesignWidth);
    m_content->setTransform(juce::AffineTransform::scale(scale));
}

bool WineroseEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files)
        if (f.endsWithIgnoreCase(".SerumPreset") || f.endsWithIgnoreCase(".fxp") || f.endsWithIgnoreCase(".fxb")
            || f.endsWithIgnoreCase(".wrpreset") || f.endsWithIgnoreCase(".json") || f.endsWithIgnoreCase(".wav") || f.endsWithIgnoreCase(".sfz"))
            return true;
    return false;
}

void WineroseEditor::filesDropped(const juce::StringArray& files, int, int)
{
    if (files.isEmpty()) return;
    const juce::File f(files[0]);
    if (f.hasFileExtension("wav;sfz")) m_content->loadOntoOscA(f);
    else m_content->loadFile(f);
}

} // namespace winerose::ui

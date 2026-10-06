// ui_snapshot — renders the Winerose editor offscreen, one PNG per page, for reviewing the UI without a DAW.
//
//   ui_snapshot <outDir> [scale]
//
// Builds a real Engine + Controller (no audio device) behind a stub AudioProcessor, opens the editor, clicks
// each page tab and writes <outDir>/<page>.png. Optional: --sample <wav> loads it on oscillator B in Sample
// mode first, so the per-type oscillator layouts show.

#include "control/Controller.h"
#include "engine/Engine.h"
#include "params/ConfigManager.h"
#include "ui/WineroseEditor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>

namespace {

class StubProcessor final : public juce::AudioProcessor {
public:
    StubProcessor() : juce::AudioProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true)) {}
    const juce::String getName() const override { return "Winerose"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return true; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
};

juce::TextButton* findButton(juce::Component& root, const juce::String& text)
{
    if (auto* b = dynamic_cast<juce::TextButton*>(&root); b != nullptr && b->getButtonText() == text) return b;
    for (auto* child : root.getChildren())
        if (auto* found = findButton(*child, text)) return found;
    return nullptr;
}

void pump()
{
    for (int i = 0; i < 20; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: ui_snapshot <outDir> [scale] [--sample file.wav]\n";
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]);
    out.createDirectory();
    float scale = 1.0f;
    juce::String sample;
    for (int i = 2; i < argc; ++i) {
        const juce::String a(argv[i]);
        if (a == "--sample" && i + 1 < argc) sample = argv[++i];
        else scale = a.getFloatValue() > 0.0f ? a.getFloatValue() : 1.0f;
    }

    auto config = std::make_shared<winerose::ConfigManager>();
    winerose::Engine engine(config);
    winerose::control::Controller controller(engine, config);
    engine.prepare(48000.0, 512);
    if (sample.isNotEmpty()) {
        const auto r = controller.loadOscillatorFile(1, sample.toStdString());
        std::cout << "sample: " << (r.ok ? r.message : r.error) << "\n";
    }
    controller.set("Oscillator2.type", 3.0);   // granular, to show a third layout
    controller.set("FXRack0Slot0.type", 9.0);  // an EQ in slot 1
    controller.set("FXRack0Slot0.enabled", 1.0);
    controller.set("ModSlot0.source", 1.0);
    controller.set("ModSlot0.destination", std::string("Filter0.cutoff"));
    controller.set("ModSlot0.amount", 0.4);
    controller.set("MidiClip0.notes", std::string("0,0.5,60,100;0.5,0.25,63,90;1,0.5,67,110;2,1,72,100;3,0.25,70,80"));

    juce::MidiKeyboardState keyboard;
    StubProcessor processor;
    winerose::ui::WineroseEditor editor(processor, controller, &keyboard);
    editor.setSize(static_cast<int>(winerose::ui::WineroseEditor::kDesignWidth * scale),
                   static_cast<int>(winerose::ui::WineroseEditor::kDesignHeight * scale));
    editor.setVisible(true);
    pump();

    const char* pages[] = {"Sound", "Matrix", "FX", "MIDI", "Presets", "All Params"};
    for (const char* page : pages) {
        if (auto* b = findButton(editor, page)) b->onClick();
        pump();
        const auto image = editor.createComponentSnapshot(editor.getLocalBounds(), true, 1.0f);
        const auto file = out.getChildFile(juce::String(page).replace(" ", "_") + ".png");
        file.deleteFile();
        juce::FileOutputStream stream(file);
        juce::PNGImageFormat().writeImageToStream(image, stream);
        std::cout << file.getFullPathName() << "\n";
    }
    return 0;
}

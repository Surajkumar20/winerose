// measure_host — black-box measurement of a VST3 synth (SPEC §2.3, §5.7). Run it against your own
// licensed Serum 2 to recover value curves, enum lists and parameter/state mappings. No decompilation:
// it only uses what any host sees (parameter names, getText(), state chunks).
//
//   measure_host list  <plugin.vst3> [--out params.csv]
//   measure_host sweep <plugin.vst3> [--steps 1025] [--match <substring>] [--out sweep.csv]
//   measure_host state <plugin.vst3> --out state.bin [--set <index>=<normalized>]...
//
//   list   one row per parameter: index, id, name, default, steps, discrete, boolean, label
//   sweep  for each parameter, getText() at <steps> evenly spaced normalized values (SPEC step 2)
//   state  set parameters, run a few silent blocks so the processor applies them, then save the plugin's
//          state chunk. Diffing chunks saved with one value changed links state keys to parameters
//          (SPEC step 3; the .SerumPreset/CBOR side of the diff comes with feature/presets).
//
// Probe renders and comparison against Winerose (SPEC step 4) are added once the engine makes sound.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <iostream>
#include <memory>

namespace {

int usage()
{
    std::cerr <<
        "usage:\n"
        "  measure_host list  <plugin.vst3> [--out params.csv]\n"
        "  measure_host sweep <plugin.vst3> [--steps 1025] [--match <substring>] [--out sweep.csv]\n"
        "  measure_host state <plugin.vst3> --out state.bin [--set <index>=<normalized>]...\n";
    return 2;
}

juce::String csv(const juce::String& field)
{
    if (field.containsAnyOf(",\"\n\r")) return "\"" + field.replace("\"", "\"\"") + "\"";
    return field;
}

juce::String parameterId(juce::AudioProcessorParameter& p)
{
    if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*>(&p)) return hosted->getParameterID();
    if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(&p)) return withId->paramID;
    return juce::String(p.getParameterIndex());
}

std::unique_ptr<juce::AudioPluginInstance> loadPlugin(const juce::String& path, juce::String& error)
{
    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager(formats);

    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* format : formats.getFormats())
        if (format->fileMightContainThisPluginType(path))
            format->findAllTypesForFile(types, path);

    if (types.isEmpty()) {
        error = "no plugin found in " + path;
        return nullptr;
    }
    return formats.createPluginInstance(*types[0], 48000.0, 512, error);
}

/** Output sink: a file if --out was given, else stdout. */
class Output {
public:
    explicit Output(const juce::String& path)
    {
        if (path.isNotEmpty()) {
            juce::File file = juce::File::getCurrentWorkingDirectory().getChildFile(path);
            file.getParentDirectory().createDirectory();
            file.deleteFile();
            m_stream = file.createOutputStream();
        }
    }
    bool ok() const { return m_stream == nullptr || m_stream->openedOk(); }
    void line(const juce::String& text)
    {
        if (m_stream) *m_stream << text << "\n";
        else          std::cout << text << "\n";
    }
private:
    std::unique_ptr<juce::FileOutputStream> m_stream;
};

int list(juce::AudioPluginInstance& plugin, Output& out)
{
    out.line("index,id,name,default,steps,discrete,boolean,label");
    for (auto* p : plugin.getParameters()) {
        out.line(juce::String(p->getParameterIndex()) + "," + csv(parameterId(*p)) + "," + csv(p->getName(256)) + ","
                 + juce::String(p->getDefaultValue(), 6) + "," + juce::String(p->getNumSteps()) + ","
                 + (p->isDiscrete() ? "1" : "0") + "," + (p->isBoolean() ? "1" : "0") + "," + csv(p->getLabel()));
    }
    return 0;
}

int sweep(juce::AudioPluginInstance& plugin, Output& out, int steps, const juce::String& match)
{
    if (steps < 2) return usage();
    out.line("index,id,name,normalized,text");
    for (auto* p : plugin.getParameters()) {
        const juce::String name = p->getName(256);
        if (match.isNotEmpty() && !name.containsIgnoreCase(match)) continue;
        const juce::String prefix = juce::String(p->getParameterIndex()) + "," + csv(parameterId(*p)) + "," + csv(name) + ",";
        for (int i = 0; i < steps; ++i) {
            const float v = static_cast<float>(i) / static_cast<float>(steps - 1);
            out.line(prefix + juce::String(v, 6) + "," + csv(p->getText(v, 1024)));
        }
    }
    return 0;
}

int state(juce::AudioPluginInstance& plugin, const juce::String& outPath, const juce::StringArray& sets)
{
    if (outPath.isEmpty()) return usage();
    const auto& params = plugin.getParameters();
    for (const auto& s : sets) {
        const int index = s.upToFirstOccurrenceOf("=", false, false).getIntValue();
        const float value = s.fromFirstOccurrenceOf("=", false, false).getFloatValue();
        if (!s.contains("=") || index < 0 || index >= params.size()) {
            std::cerr << "measure_host: bad --set '" << s << "'\n";
            return 2;
        }
        params[index]->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
    }

    // VST3 processors pick up parameter changes inside process(): run a few silent blocks first.
    plugin.setRateAndBufferSizeDetails(48000.0, 512);
    plugin.prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> buffer(juce::jmax(2, plugin.getTotalNumOutputChannels()), 512);
    juce::MidiBuffer midi;
    for (int i = 0; i < 8; ++i) {
        buffer.clear();
        midi.clear();
        plugin.processBlock(buffer, midi);
    }
    plugin.releaseResources();

    juce::MemoryBlock chunk;
    plugin.getStateInformation(chunk);

    // JUCE's VST3 host wraps the plugin's own state as <VST3PluginState><IComponent>base64</IComponent>…
    // Unwrap it: the IComponent bytes are what the plugin itself wrote (for Serum 2, the XferJson
    // container), which is what gets diffed. The JUCE wrapper is kept alongside as <out>.host.xml.
    juce::MemoryBlock component;
    if (auto xml = juce::AudioProcessor::getXmlFromBinary(chunk.getData(), static_cast<int>(chunk.getSize()));
        xml != nullptr && xml->hasTagName("VST3PluginState")) {
        if (auto* comp = xml->getChildByName("IComponent"))
            component.fromBase64Encoding(comp->getAllSubText().trim());
    }

    juce::File file = juce::File::getCurrentWorkingDirectory().getChildFile(outPath);
    file.getParentDirectory().createDirectory();
    const juce::MemoryBlock& primary = component.isEmpty() ? chunk : component;
    if (!file.replaceWithData(primary.getData(), primary.getSize())) {
        std::cerr << "measure_host: cannot write " << file.getFullPathName() << "\n";
        return 1;
    }
    if (!component.isEmpty())
        file.withFileExtension(file.getFileExtension() + ".host.xml").replaceWithData(chunk.getData(), chunk.getSize());

    std::cout << "measure_host: wrote " << primary.getSize() << " bytes of "
              << (component.isEmpty() ? "host state" : "plugin component state") << " to " << file.getFullPathName() << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;

    if (argc < 3) return usage();
    const juce::String command = argv[1];
    const juce::String pluginPath = juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]).getFullPathName();

    juce::String outPath, match;
    juce::StringArray sets;
    int steps = 1025;
    for (int i = 3; i < argc; ++i) {
        const juce::String arg = argv[i];
        if (i + 1 >= argc) return usage();
        if      (arg == "--out")   outPath = argv[++i];
        else if (arg == "--steps") steps = juce::String(argv[++i]).getIntValue();
        else if (arg == "--match") match = argv[++i];
        else if (arg == "--set")   sets.add(argv[++i]);
        else return usage();
    }

    juce::String error;
    auto plugin = loadPlugin(pluginPath, error);
    if (plugin == nullptr) {
        std::cerr << "measure_host: " << error << "\n";
        return 1;
    }
    std::cerr << "measure_host: loaded " << plugin->getName() << " (" << plugin->getParameters().size() << " parameters)\n";

    if (command == "state") return state(*plugin, outPath, sets);

    Output out(outPath);
    if (!out.ok()) {
        std::cerr << "measure_host: cannot write " << outPath << "\n";
        return 1;
    }
    if (command == "list")  return list(*plugin, out);
    if (command == "sweep") return sweep(*plugin, out, steps, match);
    return usage();
}

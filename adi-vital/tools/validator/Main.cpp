/*
  VitalValidator -- headless VST3 smoke test.

  Loads a .vst3 over the real VST3 ABI, instantiates it, plays a note and
  checks the audio that comes out. Built to answer "did I just break the
  plugin?" without needing a DAW, and cheap enough to run on every build.

  Usage:  VitalValidator <path-to.vst3> [--seconds N] [--note N]

  Exit code 0 = all checks passed, 1 = something failed.
*/

#include <JuceHeader.h>
#include <cmath>

namespace
{
int failures = 0;
int checks   = 0;

void check (bool condition, const juce::String& what, const juce::String& detail = {})
{
    ++checks;
    if (! condition)
        ++failures;

    juce::String line;
    line << (condition ? "  [ OK ] " : "  [FAIL] ") << what;
    if (detail.isNotEmpty())
        line << "  --  " << detail;

    std::cout << line << std::endl;
}

void heading (const juce::String& text)
{
    std::cout << std::endl << text << std::endl
              << juce::String::repeatedString ("-", text.length()) << std::endl;
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::String (argv[i]));

    if (args.isEmpty())
    {
        std::cerr << "usage: VitalValidator <path-to.vst3> [--seconds N] [--note N]" << std::endl;
        return 1;
    }

    const juce::File pluginFile (args[0]);
    double       seconds = 1.0;
    int          note    = 60;
    juce::String dumpStatePath;
    juce::String dumpParamsPath;

    for (int i = 1; i < args.size() - 1; ++i)
    {
        if (args[i] == "--seconds")     seconds        = args[i + 1].getDoubleValue();
        if (args[i] == "--note")        note           = args[i + 1].getIntValue();
        if (args[i] == "--dump-state")  dumpStatePath  = args[i + 1];
        if (args[i] == "--dump-params") dumpParamsPath = args[i + 1];
    }

    constexpr double sampleRate = 44100.0;
    constexpr int    blockSize  = 512;

    std::cout << "VitalValidator" << std::endl
              << "  plugin : " << pluginFile.getFullPathName() << std::endl
              << "  format : " << sampleRate << " Hz, " << blockSize << " frames/block"
              << std::endl;

    heading ("Module");
    check (pluginFile.exists(), "file exists");
    if (! pluginFile.exists())
        return 1;

    // ---- scan -------------------------------------------------------------
    juce::AudioPluginFormatManager formats;
    auto* vst3 = new juce::VST3PluginFormat();
    formats.addFormat (vst3);

    juce::OwnedArray<juce::PluginDescription> found;
    juce::KnownPluginList list;
    list.scanAndAddFile (pluginFile.getFullPathName(), true, found, *vst3);

    check (found.size() > 0, "VST3 scan found at least one plugin",
           juce::String (found.size()) + " description(s)");
    if (found.isEmpty())
    {
        std::cout << std::endl << "FAILED: nothing to instantiate." << std::endl;
        return 1;
    }

    auto& desc = *found[0];
    heading ("Description");
    std::cout << "  name        : " << desc.name           << std::endl
              << "  vendor      : " << desc.manufacturerName << std::endl
              << "  version     : " << desc.version        << std::endl
              << "  category    : " << desc.category       << std::endl
              // NB: JUCE 6.0.5 spells this `uid`; `uniqueId` only arrives in 6.1.
              << "  uid         : " << juce::String::toHexString (desc.uid) << std::endl;

    check (desc.name.isNotEmpty(), "reports a name");
    check (desc.isInstrument, "declares itself an instrument (synth)");

    // ---- instantiate ------------------------------------------------------
    heading ("Instantiation");
    juce::String error;
    std::unique_ptr<juce::AudioPluginInstance> instance (
        formats.createPluginInstance (desc, sampleRate, blockSize, error));

    check (instance != nullptr, "instantiated", error.isEmpty() ? juce::String ("no error") : error);
    if (instance == nullptr)
    {
        std::cout << std::endl << "FAILED: could not instantiate." << std::endl;
        return 1;
    }

    // Buses must be enabled before channel counts mean anything: a freshly
    // created VST3 instance reports 0/0 until its layout is activated.
    instance->enableAllBuses();

    const int numParams = instance->getParameters().size();
    std::cout << "  parameters  : " << numParams << std::endl
              << "  in / out    : " << instance->getTotalNumInputChannels()
              << " / " << instance->getTotalNumOutputChannels() << std::endl
              << "  accepts MIDI: " << (instance->acceptsMidi() ? "yes" : "no") << std::endl
              << "  tail (s)    : " << instance->getTailLengthSeconds() << std::endl;

    check (numParams > 0, "exposes automatable parameters",
           juce::String (numParams) + " parameters");

    // --dump-params lists every parameter the host can see, in registration
    // order. For Vital this is the ground truth for which ValueDetails entries
    // the engine actually registered: SynthPlugin's constructor skips any entry
    // with no matching control_map key (synth_plugin.cpp:28-30), and
    // ValueBridge::getName() returns that entry's display_name. Everything
    // after Vital's own parameters is JUCE's MIDI-CC emulation block.
    if (dumpParamsPath.isNotEmpty())
    {
        juce::StringArray lines;
        const auto& params = instance->getParameters();
        for (int i = 0; i < params.size(); ++i)
            lines.add (juce::String (i) + "\t" + params[i]->getName (512));

        juce::File out (dumpParamsPath);
        out.replaceWithText (lines.joinIntoString ("\n"));
        std::cout << "  dumped " << params.size() << " parameter names -> "
                  << out.getFullPathName() << std::endl;
    }
    check (instance->getTotalNumOutputChannels() >= 2, "has at least a stereo output",
           juce::String (instance->getTotalNumOutputChannels()) + " output channels");
    check (instance->acceptsMidi(), "accepts MIDI input");

    // ---- prepare and render ----------------------------------------------
    heading ("Rendering");
    instance->prepareToPlay (sampleRate, blockSize);
    check (true, "prepareToPlay survived");

    const int numChannels = juce::jmax (2, instance->getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer (numChannels, blockSize);

    // A few silent blocks first: anything audible here would be a bug.
    double silentPeak = 0.0;
    for (int b = 0; b < 4; ++b)
    {
        buffer.clear();
        juce::MidiBuffer midi;
        instance->processBlock (buffer, midi);
        silentPeak = juce::jmax (silentPeak, (double) buffer.getMagnitude (0, blockSize));
    }
    check (silentPeak < 1.0e-4, "silent before any note-on",
           "peak " + juce::String (silentPeak, 8));

    // Note on, then render.
    const int totalBlocks = (int) std::ceil ((seconds * sampleRate) / blockSize);
    double peak = 0.0;
    double sumSquares = 0.0;
    juce::int64 sampleCount = 0;
    bool sawNonFinite = false;

    for (int b = 0; b < totalBlocks; ++b)
    {
        buffer.clear();
        juce::MidiBuffer midi;
        if (b == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);

        instance->processBlock (buffer, midi);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const float* d = buffer.getReadPointer (ch);
            for (int i = 0; i < blockSize; ++i)
            {
                const float v = d[i];
                if (! std::isfinite (v))
                    sawNonFinite = true;
                peak = juce::jmax (peak, (double) std::abs (v));
                sumSquares += (double) v * (double) v;
                ++sampleCount;
            }
        }
    }

    const double rms = sampleCount > 0 ? std::sqrt (sumSquares / (double) sampleCount) : 0.0;

    std::cout << "  rendered    : " << totalBlocks << " blocks ("
              << juce::String (seconds, 2) << " s), MIDI note " << note << std::endl
              << "  peak        : " << juce::String (peak, 6) << std::endl
              << "  rms         : " << juce::String (rms, 6) << std::endl;

    check (! sawNonFinite, "no NaN or Inf in the output");
    check (peak > 1.0e-3, "produced audible output after note-on",
           "peak " + juce::String (peak, 6));
    check (peak <= 1.5, "output is not clipping badly",
           "peak " + juce::String (peak, 6));

    // ---- state round-trip -------------------------------------------------
    heading ("State");
    juce::MemoryBlock state;
    instance->getStateInformation (state);
    check (state.getSize() > 0, "getStateInformation returned data",
           juce::String (state.getSize()) + " bytes");

    if (state.getSize() > 0)
    {
        instance->setStateInformation (state.getData(), (int) state.getSize());
        check (true, "setStateInformation round-tripped without crashing");
    }

    // --dump-state writes the raw preset JSON out. stateToJson() builds it by
    // iterating the engine's live control_map, so its "settings" keys are the
    // ground truth for which parameters actually exist -- as opposed to the
    // ValueDetails table, which contains entries the engine never registers.
    if (dumpStatePath.isNotEmpty() && state.getSize() > 0)
    {
        // Write the raw bytes. getStateInformation() wrote the JSON with
        // MemoryOutputStream::writeString, which appends a null terminator --
        // strip it, but do not try to re-decode the block as a stream.
        auto size = state.getSize();
        const char* raw = static_cast<const char*> (state.getData());
        while (size > 0 && raw[size - 1] == '\0')
            --size;

        juce::File out (dumpStatePath);
        out.deleteFile();
        out.appendData (raw, size);
        std::cout << "  dumped state -> " << out.getFullPathName()
                  << " (" << size << " bytes)" << std::endl;
    }

    instance->releaseResources();
    instance.reset();

    heading ("Result");
    std::cout << "  " << (checks - failures) << " / " << checks << " checks passed" << std::endl;
    if (failures > 0)
        std::cout << "  FAILED" << std::endl;
    else
        std::cout << "  PASSED" << std::endl;

    return failures > 0 ? 1 : 0;
}

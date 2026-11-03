// Dev tool: renders a MIDI file through ilanaSynth with a factory preset, or
// with the Init patch and osc 1 switched to the Modal Piano exciter (as
// clicking its icon does), to compare against a render from a host.
//
//   ilanaMidiRender <file.mid> <out.wav> <factory preset name | init-modal> [sample rate] [block]

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <iostream>
#include "PluginProcessor.h"

int main (int argc, char** argv)
{
    if (argc < 4) { std::cerr << "usage: ilanaMidiRender file.mid out.wav preset [rate] [block]\n"; return 1; }
    juce::ScopedJuceInitialiser_GUI gui;
    const double rate = argc > 4 ? std::atof (argv[4]) : 48000.0;
    const int blockSize = argc > 5 ? std::atoi (argv[5]) : 256;

    juce::MidiFile file;
    juce::FileInputStream in (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    if (! in.openedOk() || ! file.readFrom (in)) { std::cerr << "can't read midi\n"; return 1; }
    file.convertTimestampTicksToSeconds();
    juce::MidiMessageSequence seq;
    for (int t = 0; t < file.getNumTracks(); ++t)
        seq.addSequence (*file.getTrack (t), 0.0);
    seq.updateMatchedPairs();

    IlanaSynthAudioProcessor processor;
    const juce::String preset (argv[3]);
    if (preset != "init-modal")
    {
        const auto names = processor.getFactoryPresetNames();
        const auto index = names.indexOf (preset);
        if (index < 0) { std::cerr << "no preset " << argv[3] << "\n"; return 1; }
        processor.loadFactoryPreset (index);
    }
    else if (auto* p = processor.apvts.getParameter ("osc1_excite"))
    {
        if (auto* mode = processor.apvts.getParameter ("osc1_mode"))
            mode->setValueNotifyingHost (mode->convertTo0to1 (1.0f)); // Physical
        p->setValueNotifyingHost (p->convertTo0to1 (11.0f));
    }
    // ILANA_SET="id=value,id=value": parameters set after the preset loads.
    for (auto pair : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("ILANA_SET", ""), ",", ""))
        if (auto* p = processor.apvts.getParameter (pair.upToFirstOccurrenceOf ("=", false, false).trim()))
            p->setValueNotifyingHost (p->convertTo0to1 (pair.fromFirstOccurrenceOf ("=", false, false).getFloatValue()));
    processor.setPlayConfigDetails (0, 2, rate, blockSize);
    processor.prepareToPlay (rate, blockSize);

    // ILANA_WARMUP=1: first play 32 high notes at once and let them die out
    // (voices with a history, as in a live session), then render.
    if (juce::SystemStats::getEnvironmentVariable ("ILANA_WARMUP", "").isNotEmpty())
    {
        juce::AudioBuffer<float> scratch (2, blockSize);
        for (int b = 0; b < (int) (20.0 * rate / blockSize); ++b)
        {
            scratch.clear();
            juce::MidiBuffer midi;
            if (b == 0) for (int n = 0; n < 32; ++n) midi.addEvent (juce::MidiMessage::noteOn (1, 76 + n, (juce::uint8) 100), 0);
            if (b == 10) for (int n = 0; n < 32; ++n) midi.addEvent (juce::MidiMessage::noteOff (1, 76 + n), 0);
            processor.processBlock (scratch, midi);
        }
    }
    const auto seconds = seq.getEndTime() + 4.0;
    const auto blocks = (int) std::ceil (seconds * rate / blockSize);
    juce::AudioBuffer<float> out (2, blocks * blockSize), buffer (2, blockSize);
    int next = 0;
    for (int b = 0; b < blocks; ++b)
    {
        buffer.clear();
        juce::MidiBuffer midi;
        const auto start = (long long) b * blockSize;
        while (next < seq.getNumEvents())
        {
            const auto& m = seq.getEventPointer (next)->message;
            const auto at = (long long) std::llround (m.getTimeStamp() * rate);
            if (at >= start + blockSize) break;
            if (! m.isMetaEvent()) midi.addEvent (m, (int) juce::jmax (0LL, at - start));
            ++next;
        }
        processor.processBlock (buffer, midi);
        for (int c = 0; c < 2; ++c) out.copyFrom (c, (int) start, buffer, c, 0, blockSize);
    }
    juce::File outFile = juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]);
    outFile.deleteFile();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (new juce::FileOutputStream (outFile), rate, 2, 24, {}, 0));
    writer->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
    std::cout << "wrote " << outFile.getFullPathName() << " " << seconds << " s\n";
    return 0;
}

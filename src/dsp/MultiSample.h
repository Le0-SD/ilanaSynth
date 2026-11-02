#pragma once

// SoundFont 2 (.sf2) and SFZ (.sfz) instruments for the Sample oscillator:
// each is read into a SampleData whose zones hold the regions (key and
// velocity ranges, root key, tuning, loop, level). One instrument per file:
// an SF2 preset (by default the first: lowest bank, then program); every region of an SFZ.
// Envelopes, filters and modulators in the files are ignored: the synth's own
// do that job.

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "SamplePlayer.h"

namespace MultiSample
{
inline bool isMultiSampleFile (const juce::File& file)
{
    return file.hasFileExtension ("sf2;sfz");
}

// The region under middle C (else the first) goes in the SampleData's own
// buffer, for the displays.
inline void finish (SampleData& data)
{
    // Step 36: each zone knows its place in the file, for saved edits.
    for (size_t i = 0; i < data.zones.size(); ++i)
        if (data.zones[i].file.isEmpty())
            data.zones[i].origin = (int) i;
    const auto* shown = data.zoneFor (60, 100);
    if (shown != nullptr && shown->data != nullptr)
    {
        data.buffer.makeCopyOf (shown->data->buffer);
        data.sampleRate = shown->data->sampleRate;
    }
}

// ---------------------------------------------------------------- SFZ

// "c4" / "C#3" / "60" -> 60 (middle C is c4, as SFZ has it).
inline int parseNote (const juce::String& text)
{
    const auto trimmed = text.trim().toLowerCase();
    if (trimmed.isEmpty())
        return -1;
    if (juce::CharacterFunctions::isDigit (trimmed[0]) || trimmed[0] == '-')
        return trimmed.getIntValue() > 127 ? -1 : trimmed.getIntValue();
    static const int semis[] { 9, 11, 0, 2, 4, 5, 7 }; // a..g
    const auto letter = trimmed[0];
    if (letter < 'a' || letter > 'g')
        return -1;
    auto note = semis[letter - 'a'];
    auto rest = trimmed.substring (1);
    if (rest.startsWithChar ('#')) { ++note; rest = rest.substring (1); }
    else if (rest.startsWithChar ('b')) { --note; rest = rest.substring (1); }
    return juce::jlimit (0, 127, note + (rest.getIntValue() + 1) * 12);
}

// What an SFZ asked for (portable sample presets, roadmap step 31): every
// audio file its regions name, as written and as found, and the ones that
// are not on disk. scanOnly reads the text without decoding any audio.
struct SfzReport
{
    struct Entry
    {
        juce::String written; // the sample= value, slashes normalised
        juce::File file;
        bool found = false;
    };
    std::vector<Entry> files;
    bool scanOnly = false;

    std::vector<juce::File> missing() const
    {
        std::vector<juce::File> out;
        for (const auto& entry : files)
            if (! entry.found)
                out.push_back (entry.file);
        return out;
    }
};

inline std::shared_ptr<SampleData> loadSfz (const juce::File& file, juce::String& error, SfzReport* report = nullptr)
{
    const auto text = file.loadFileAsString();
    if (text.isEmpty())
    {
        error = "empty or unreadable file";
        return nullptr;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::map<juce::String, std::shared_ptr<SampleData>> loaded;
    auto defaultPath = file.getParentDirectory();

    // All the regions' audio together: past this the rest is left out (the
    // decode happens on the message thread, in memory).
    constexpr juce::int64 maxBytes = (juce::int64) 768 * 1024 * 1024;
    juce::int64 totalBytes = 0;
    auto overBudget = false;
    // Step 43: what was left out, said once the instrument is in.
    juce::StringArray missingFiles;
    auto cutFiles = 0;

    const auto loadAudio = [&] (const juce::String& relative) -> std::shared_ptr<SampleData>
    {
        const auto path = relative.replaceCharacter ('\\', '/');
        const auto audioFile = juce::File::isAbsolutePath (path) ? juce::File (path) : defaultPath.getChildFile (path);
        const auto key = audioFile.getFullPathName();
        if (const auto found = loaded.find (key); found != loaded.end())
            return found->second;
        if (report != nullptr)
            report->files.push_back ({ path, audioFile, audioFile.existsAsFile() });
        std::shared_ptr<SampleData> data;
        if (report != nullptr && report->scanOnly)
        {
            data = std::make_shared<SampleData>(); // a stand-in: only the names matter
            data->buffer.setSize (1, 2);
            data->buffer.clear();
            loaded[key] = data;
            return data;
        }
        if (std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (audioFile) }; reader != nullptr && reader->lengthInSamples > 1)
        {
            const auto length = (int) juce::jmin (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 120.0));
            if (length < reader->lengthInSamples)
                ++cutFiles;
            const auto bytes = (juce::int64) length * juce::jlimit (1, 2, (int) reader->numChannels) * (juce::int64) sizeof (float);
            if (totalBytes + bytes > maxBytes)
            {
                overBudget = true;
                loaded[key] = nullptr;
                return nullptr;
            }
            totalBytes += bytes;
            data = std::make_shared<SampleData>();
            data->sampleRate = reader->sampleRate;
            data->name = audioFile.getFileNameWithoutExtension();
            data->buffer.setSize (juce::jlimit (1, 2, (int) reader->numChannels), length);
            if (! reader->read (&data->buffer, 0, length, 0, true, true))
                data.reset();
            else
                scrubAudio (data->buffer);
        }
        if (data == nullptr)
            missingFiles.add (audioFile.getFileName());
        loaded[key] = data;
        return data;
    };

    // Strip comments, then walk headers and opcodes. Values may contain
    // spaces (sample paths): a value runs to the next "name=" or header.
    auto uncommented = text;
    for (auto open = uncommented.indexOf ("/*"); open >= 0; open = uncommented.indexOf ("/*"))
    {
        const auto close = uncommented.indexOf (open + 2, "*/");
        uncommented = uncommented.substring (0, open) + (close < 0 ? juce::String() : uncommented.substring (close + 2));
    }
    juce::String clean;
    for (auto line : juce::StringArray::fromLines (uncommented))
    {
        // "//" starts a comment at a line's start or after a space, not inside a //server/share path.
        auto cut = -1;
        for (auto at = line.indexOf ("//"); at >= 0; at = line.indexOf (at + 1, "//"))
            if (at == 0 || juce::CharacterFunctions::isWhitespace (line[at - 1]))
            {
                cut = at;
                break;
            }
        clean << (cut >= 0 ? line.substring (0, cut) : line) << "\n";
    }

    using Opcodes = std::map<juce::String, juce::String>;
    Opcodes global, master, group, region;
    juce::String header;
    auto data = std::make_shared<SampleData>();
    data->name = file.getFileNameWithoutExtension();

    const auto flushRegion = [&]
    {
        if (header != "region")
            return;
        Opcodes merged = global;
        for (const auto& [k, v] : master) merged[k] = v;
        for (const auto& [k, v] : group) merged[k] = v;
        for (const auto& [k, v] : region) merged[k] = v;
        const auto get = [&merged] (const char* name, const juce::String& fallback = {})
        {
            const auto found = merged.find (name);
            return found != merged.end() ? found->second : fallback;
        };
        if (get ("sample").isEmpty())
            return;
        // Release-trigger regions are noises played on key-up; as ordinary notes they would win zoneFor.
        if (const auto trigger = get ("trigger"); trigger == "release" || trigger == "release_key")
            return;
        SampleZone zone;
        zone.data = loadAudio (get ("sample"));
        if (zone.data == nullptr)
            return;
        if (const auto key = parseNote (get ("key")); key >= 0)
            zone.loKey = zone.hiKey = key, zone.rootNote = key;
        if (const auto lo = parseNote (get ("lokey")); lo >= 0) zone.loKey = lo;
        if (const auto hi = parseNote (get ("hikey")); hi >= 0) zone.hiKey = hi;
        if (const auto centre = parseNote (get ("pitch_keycenter")); centre >= 0) zone.rootNote = centre;
        zone.loVel = juce::jlimit (1, 127, get ("lovel", "1").getIntValue());
        zone.hiVel = juce::jlimit (1, 127, get ("hivel", "127").getIntValue());
        zone.rootNote -= get ("tune", "0").getDoubleValue() / 100.0 + get ("transpose", "0").getDoubleValue();
        zone.gain = juce::Decibels::decibelsToGain (get ("volume", "0").getFloatValue());
        // Step 37: a round-robin alternate's place among the regions sharing its keys.
        zone.sequence = juce::jmax (0, get ("seq_position", "0").getIntValue());
        const auto length = zone.data->getNumSamples();
        const auto mode = get ("loop_mode", get ("loopmode"));
        zone.loopStart = juce::jlimit (0, length, get ("loop_start", get ("loopstart", "0")).getIntValue());
        zone.loopEnd = juce::jlimit (0, length, get ("loop_end", get ("loopend", juce::String (length))).getIntValue() + 1);
        zone.loop = (mode == "loop_continuous" || mode == "loop_sustain"
                     || (mode.isEmpty() && merged.count ("loop_start") + merged.count ("loopstart") > 0))
                    && zone.loopEnd - zone.loopStart > 16;

        // offset / end trim the region's audio (in frames of the file).
        const auto trimStart = juce::jlimit (0, length - 1, get ("offset").getIntValue());
        const auto trimEnd = get ("end").isEmpty() ? length : juce::jlimit (trimStart, length, get ("end").getIntValue() + 1);
        if (get ("end").isNotEmpty() && get ("end").getIntValue() < 0)
            return; // end=-1 turns the region off
        if (trimStart > 0 || trimEnd < length)
        {
            if (trimEnd - trimStart <= 1)
                return;
            auto trimmed = std::make_shared<SampleData>();
            trimmed->sampleRate = zone.data->sampleRate;
            trimmed->name = zone.data->name;
            trimmed->buffer.setSize (zone.data->getNumChannels(), trimEnd - trimStart);
            for (int c = 0; c < trimmed->getNumChannels(); ++c)
                trimmed->buffer.copyFrom (c, 0, zone.data->buffer, c, trimStart, trimEnd - trimStart);
            zone.data = trimmed;
            zone.loopStart = juce::jlimit (0, trimEnd - trimStart, zone.loopStart - trimStart);
            zone.loopEnd = juce::jlimit (0, trimEnd - trimStart, zone.loopEnd - trimStart);
            zone.loop = zone.loop && zone.loopEnd - zone.loopStart > 16;
        }
        data->zones.push_back (zone);
    };

    // std::string, so the walk is linear.
    const auto source = clean.toStdString();
    size_t pos = 0;
    const auto isSpace = [] (char c) { return c == ' ' || c == '\t' || c == '\r'; };
    const auto isWord = [] (char c) { return std::isalnum ((unsigned char) c) != 0 || c == '_'; };
    while (pos < source.size())
    {
        while (pos < source.size() && (isSpace (source[pos]) || source[pos] == '\n'))
            ++pos;
        if (pos >= source.size())
            break;
        if (source[pos] == '<')
        {
            flushRegion();
            const auto close = source.find ('>', pos);
            if (close == std::string::npos)
                break;
            header = juce::String (source.substr (pos + 1, close - pos - 1)).trim().toLowerCase();
            pos = close + 1;
            if (header == "region") region.clear();
            else if (header == "group") { group.clear(); }
            else if (header == "master") { master.clear(); group.clear(); }
            else if (header == "global") { global.clear(); master.clear(); group.clear(); }
            continue;
        }
        const auto equals = source.find ('=', pos);
        if (equals == std::string::npos)
            break;
        const auto name = juce::String (source.substr (pos, equals - pos)).trim().toLowerCase();
        // The value ends at the line's end, a header or the next "word=".
        auto end = equals + 1;
        while (end < source.size() && source[end] != '\n' && source[end] != '<')
        {
            if (isSpace (source[end]))
            {
                auto k = end;
                while (k < source.size() && isSpace (source[k]))
                    ++k;
                const auto wordStart = k;
                while (k < source.size() && isWord (source[k]))
                    ++k;
                if (k > wordStart && k < source.size() && source[k] == '=')
                    break;
            }
            ++end;
        }
        const auto value = juce::String (source.substr (equals + 1, end - equals - 1)).trim();
        pos = end;
        if (header == "control" && name == "default_path")
            defaultPath = file.getParentDirectory().getChildFile (value.replaceCharacter ('\\', '/'));
        else if (header == "region") region[name] = value;
        else if (header == "group") group[name] = value;
        else if (header == "master") master[name] = value;
        else if (header == "global") global[name] = value;
    }
    flushRegion();

    if (data->zones.empty())
    {
        error = overBudget ? "the samples are too large (limit 768 MB)"
                : missingFiles.isEmpty() ? "no playable regions"
                                         : "none of its sample files could be read (first: " + missingFiles[0] + ")";
        return nullptr;
    }
    juce::StringArray notes;
    if (! missingFiles.isEmpty())
        notes.add (juce::String (missingFiles.size()) + (missingFiles.size() == 1 ? " sample file" : " sample files")
                   + " could not be read (" + missingFiles[0] + (missingFiles.size() > 1 ? ", ..." : "") + ").");
    if (overBudget)
        notes.add ("Some regions were left out: the samples together pass 768 MB.");
    if (cutFiles > 0)
        notes.add (juce::String (cutFiles) + (cutFiles == 1 ? " sample was" : " samples were") + " cut at 120 seconds.");
    data->loadWarning = notes.joinIntoString (" ");
    finish (*data);
    return data;
}

// ---------------------------------------------------------------- SF2

namespace Sf2
{
struct Chunk
{
    juce::String id;
    const char* data = nullptr;
    juce::uint32 size = 0;
};

inline juce::uint16 u16 (const char* p) { return (juce::uint16) ((juce::uint8) p[0] | ((juce::uint8) p[1] << 8)); }
inline juce::uint32 u32 (const char* p) { return (juce::uint32) u16 (p) | ((juce::uint32) u16 (p + 2) << 16); }

// The chunks inside a LIST (or the RIFF form), after its 4-byte type.
inline std::vector<Chunk> children (const char* data, juce::uint32 size)
{
    std::vector<Chunk> chunks;
    // 64-bit, so a chunk size near 4 GB cannot wrap the offset and loop forever.
    juce::uint64 offset = 4;
    while (offset + 8 <= (juce::uint64) size)
    {
        Chunk chunk;
        chunk.id = juce::String (data + offset, 4);
        chunk.size = u32 (data + offset + 4);
        chunk.data = data + offset + 8;
        if (offset + 8 + (juce::uint64) chunk.size > (juce::uint64) size)
            break;
        chunks.push_back (chunk);
        offset += 8 + (juce::uint64) chunk.size + (chunk.size & 1);
    }
    return chunks;
}

inline juce::String listType (const Chunk& chunk) { return chunk.size >= 4 ? juce::String (chunk.data, 4) : juce::String(); }

enum Gen
{
    startAddrsOffset = 0, endAddrsOffset = 1, startloopAddrsOffset = 2, endloopAddrsOffset = 3, startAddrsCoarseOffset = 4,
    endAddrsCoarseOffset = 12, instrument = 41, keyRange = 43, velRange = 44, startloopAddrsCoarseOffset = 45,
    initialAttenuation = 48, endloopAddrsCoarseOffset = 50, coarseTune = 51, fineTune = 52, sampleID = 53, sampleModes = 54,
    overridingRootKey = 58
};

struct Generators
{
    std::map<int, juce::uint16> values;
    bool has (int gen) const { return values.count (gen) > 0; }
    int get (int gen, int fallback = 0) const
    {
        const auto found = values.find (gen);
        return found != values.end() ? (int) (juce::int16) found->second : fallback;
    }
    int lo (int gen, int fallback) const { const auto f = values.find (gen); return f != values.end() ? (f->second & 0xff) : fallback; }
    int hi (int gen, int fallback) const { const auto f = values.find (gen); return f != values.end() ? (f->second >> 8) : fallback; }
};

// The generator lists of the zones bag[first..last).
// numBags is the bag chunk's size / 4; the last bag is the terminator.
inline std::vector<Generators> zonesOf (const char* bags, juce::uint32 numBags, const char* gens, juce::uint32 numGens, int first, int last)
{
    std::vector<Generators> zones;
    last = juce::jmin (last, (int) juce::jmin<juce::uint32> (numBags, 65536u) - 1);
    for (int b = first; b < last; ++b)
    {
        Generators zone;
        const auto from = u16 (bags + b * 4), to = u16 (bags + (b + 1) * 4);
        for (juce::uint32 g = from; g < to && g < numGens; ++g)
            zone.values[u16 (gens + g * 4)] = u16 (gens + g * 4 + 2);
        zones.push_back (std::move (zone));
    }
    return zones;
}

// A file read into memory with its sample data and preset-data chunks found.
// The chunk pointers point into block, so keep the two together.
struct Parsed
{
    juce::MemoryBlock block;
    const char* smpl = nullptr;
    juce::uint32 smplSize = 0;
    std::map<juce::String, Chunk> pdta;
    int numPresets = 0; // without the terminator record
};

// Reads and checks the file. Returns an empty string, or what is wrong with it.
inline juce::String parse (const juce::File& file, Parsed& parsed)
{
    auto& block = parsed.block;
    if (! file.loadFileAsData (block) || block.getSize() < 12)
        return "unreadable file";
    const auto* raw = static_cast<const char*> (block.getData());
    if (juce::String (raw, 4) != "RIFF" || juce::String (raw + 8, 4) != "sfbk")
        return "not a SoundFont";

    for (const auto& list : children (raw + 8, (juce::uint32) std::min<size_t> (u32 (raw + 4), block.getSize() - 8)))
    {
        if (list.id != "LIST")
            continue;
        for (const auto& chunk : children (list.data, list.size))
        {
            if (listType (list) == "sdta" && chunk.id == "smpl")
                parsed.smpl = chunk.data, parsed.smplSize = chunk.size;
            else if (listType (list) == "pdta")
                parsed.pdta[chunk.id] = chunk;
        }
    }
    for (const char* id : { "phdr", "pbag", "pgen", "inst", "ibag", "igen", "shdr" })
        if (parsed.pdta.count (id) == 0)
            return juce::String ("missing ") + id;
    if (parsed.smpl == nullptr)
        return "no sample data";

    parsed.numPresets = (int) (parsed.pdta["phdr"].size / 38) - 1; // the last is the terminator
    if (parsed.numPresets < 1)
        return "no presets";
    return {};
}

inline int presetBank (const Parsed& parsed, int index) { return u16 (parsed.pdta.at ("phdr").data + index * 38 + 22); }
inline int presetProgram (const Parsed& parsed, int index) { return u16 (parsed.pdta.at ("phdr").data + index * 38 + 20); }
} // namespace Sf2

struct Sf2Preset
{
    int bank = 0, program = 0;
    juce::String name;
};

// Every preset of a SoundFont, by bank then program. Empty if the file is unusable.
inline std::vector<Sf2Preset> listSf2Presets (const juce::File& file)
{
    Sf2::Parsed parsed;
    std::vector<Sf2Preset> presets;
    if (Sf2::parse (file, parsed).isNotEmpty())
        return presets;
    for (int p = 0; p < parsed.numPresets; ++p)
        presets.push_back ({ Sf2::presetBank (parsed, p), Sf2::presetProgram (parsed, p),
                             juce::String (parsed.pdta.at ("phdr").data + p * 38, 20).trim() });
    // Stable, so presets sharing a bank and program keep the file's order.
    std::stable_sort (presets.begin(), presets.end(), [] (const Sf2Preset& a, const Sf2Preset& b)
                      { return a.bank != b.bank ? a.bank < b.bank : a.program < b.program; });
    return presets;
}

// bank and program of -1 (or a pair the file lacks) pick the first preset:
// lowest bank, then lowest program.
inline std::shared_ptr<SampleData> loadSf2 (const juce::File& file, juce::String& error, int bank = -1, int program = -1)
{
    using namespace Sf2;
    Parsed parsed;
    error = parse (file, parsed);
    if (error.isNotEmpty())
        return nullptr;

    const auto* smpl = parsed.smpl;
    const auto smplSize = parsed.smplSize;
    auto& pdta = parsed.pdta;
    const auto& phdr = pdta["phdr"];
    const auto numPresets = parsed.numPresets;
    auto chosen = -1;
    for (int p = 0; p < numPresets && chosen < 0; ++p)
        if (presetBank (parsed, p) == bank && presetProgram (parsed, p) == program)
            chosen = p;
    if (chosen < 0)
    {
        chosen = 0;
        for (int p = 1; p < numPresets; ++p)
        {
            const auto* a = phdr.data + p * 38;
            const auto* b = phdr.data + chosen * 38;
            if (u16 (a + 22) < u16 (b + 22) || (u16 (a + 22) == u16 (b + 22) && u16 (a + 20) < u16 (b + 20)))
                chosen = p;
        }
    }
    const auto presetName = juce::String (phdr.data + chosen * 38, 20).trim();
    const auto presetZones = zonesOf (pdta["pbag"].data, pdta["pbag"].size / 4, pdta["pgen"].data, pdta["pgen"].size / 4,
                                      u16 (phdr.data + chosen * 38 + 24), u16 (phdr.data + (chosen + 1) * 38 + 24));

    const auto& inst = pdta["inst"];
    const auto& shdr = pdta["shdr"];
    const auto numInstruments = (int) (inst.size / 22) - 1;
    const auto numSampleHeaders = (int) (shdr.size / 46) - 1;
    const auto numFrames = smplSize / 2;
    std::map<std::tuple<int, int, int>, std::shared_ptr<SampleData>> audio; // by sample header and range

    const auto readSample = [&] (int header, int start, int end) -> std::shared_ptr<SampleData>
    {
        const auto* h = shdr.data + header * 46;
        const auto key = std::make_tuple (header, start, end);
        if (const auto found = audio.find (key); found != audio.end())
            return found->second;
        std::shared_ptr<SampleData> data;
        const auto length = end - start;
        if (start >= 0 && end <= (int) numFrames && length > 1)
        {
            // A left sample linked to a right one plays as a stereo pair.
            const auto type = u16 (h + 44);
            const auto link = (int) u16 (h + 42);
            const auto stereo = (type & 4) != 0 && link < numSampleHeaders;
            const auto partnerStart = stereo ? (int) u32 (shdr.data + link * 46 + 20) : 0;
            data = std::make_shared<SampleData>();
            data->sampleRate = juce::jmax (1000.0, (double) u32 (h + 36));
            data->name = juce::String (h, 20).trim();
            data->buffer.setSize (stereo ? 2 : 1, length);
            for (int channel = 0; channel < data->buffer.getNumChannels(); ++channel)
            {
                const auto offset = channel == 0 ? start : partnerStart + (start - (int) u32 (h + 20));
                auto* out = data->buffer.getWritePointer (channel);
                for (int i = 0; i < length; ++i)
                {
                    const auto frame = offset + i;
                    out[i] = frame >= 0 && frame < (int) numFrames ? (float) (juce::int16) u16 (smpl + frame * 2) / 32768.0f : 0.0f;
                }
            }
        }
        audio[key] = data;
        return data;
    };

    auto data = std::make_shared<SampleData>();
    data->name = presetName.isNotEmpty() ? presetName : file.getFileNameWithoutExtension();

    // The preset's global zone (no instrument) applies to all its zones.
    Generators presetGlobal;
    if (! presetZones.empty() && ! presetZones.front().has (instrument))
        presetGlobal = presetZones.front();

    for (const auto& presetZone : presetZones)
    {
        if (! presetZone.has (instrument))
            continue;
        const auto instrumentIndex = presetZone.get (instrument);
        if (instrumentIndex < 0 || instrumentIndex >= numInstruments)
            continue;
        const auto pLoKey = presetZone.lo (keyRange, presetGlobal.lo (keyRange, 0));
        const auto pHiKey = presetZone.hi (keyRange, presetGlobal.hi (keyRange, 127));
        const auto pLoVel = presetZone.lo (velRange, presetGlobal.lo (velRange, 0));
        const auto pHiVel = presetZone.hi (velRange, presetGlobal.hi (velRange, 127));
        const auto presetTune = presetZone.get (coarseTune, presetGlobal.get (coarseTune)) * 100
                                + presetZone.get (fineTune, presetGlobal.get (fineTune));
        const auto presetAttenuation = presetZone.get (initialAttenuation, presetGlobal.get (initialAttenuation));

        const auto instZones = zonesOf (pdta["ibag"].data, pdta["ibag"].size / 4, pdta["igen"].data, pdta["igen"].size / 4,
                                        u16 (inst.data + instrumentIndex * 22 + 20), u16 (inst.data + (instrumentIndex + 1) * 22 + 20));
        Generators instGlobal;
        if (! instZones.empty() && ! instZones.front().has (sampleID))
            instGlobal = instZones.front();

        for (const auto& zoneGens : instZones)
        {
            if (! zoneGens.has (sampleID))
                continue;
            const auto get = [&] (int gen, int fallback = 0) { return zoneGens.has (gen) ? zoneGens.get (gen) : instGlobal.get (gen, fallback); };
            const auto header = get (sampleID);
            if (header < 0 || header >= numSampleHeaders)
                continue;
            const auto* h = shdr.data + header * 46;
            const auto type = u16 (h + 44);
            if ((type & 0x8000) != 0) // ROM samples
                continue;
            // The right half of a stereo pair is played with its left one.
            if ((type & 2) != 0)
            {
                const auto link = (int) u16 (h + 42);
                auto hasLeft = false;
                for (const auto& other : instZones)
                    hasLeft = hasLeft || (other.has (sampleID) && other.get (sampleID) == link);
                if (hasLeft)
                    continue;
            }

            const auto start = (int) u32 (h + 20) + get (startAddrsOffset) + get (startAddrsCoarseOffset) * 32768;
            const auto end = (int) u32 (h + 24) + get (endAddrsOffset) + get (endAddrsCoarseOffset) * 32768;
            const auto loopStart = (int) u32 (h + 28) + get (startloopAddrsOffset) + get (startloopAddrsCoarseOffset) * 32768;
            const auto loopEnd = (int) u32 (h + 32) + get (endloopAddrsOffset) + get (endloopAddrsCoarseOffset) * 32768;

            SampleZone zone;
            zone.data = readSample (header, start, end);
            if (zone.data == nullptr)
                continue;
            const auto lo = [&] (int gen, int fallback) { return zoneGens.has (gen) ? zoneGens.lo (gen, fallback) : instGlobal.lo (gen, fallback); };
            const auto hi = [&] (int gen, int fallback) { return zoneGens.has (gen) ? zoneGens.hi (gen, fallback) : instGlobal.hi (gen, fallback); };
            zone.loKey = juce::jmax (pLoKey, lo (keyRange, 0));
            zone.hiKey = juce::jmin (pHiKey, hi (keyRange, 127));
            zone.loVel = juce::jmax (1, juce::jmax (pLoVel, lo (velRange, 0)));
            zone.hiVel = juce::jmin (pHiVel, hi (velRange, 127));
            if (zone.loKey > zone.hiKey || zone.loVel > zone.hiVel)
                continue;
            const auto overriding = get (overridingRootKey, -1);
            const auto originalPitch = (int) (juce::uint8) h[40];
            zone.rootNote = (double) (overriding >= 0 ? overriding : (originalPitch <= 127 ? originalPitch : 60))
                            - (double) (juce::int8) h[41] / 100.0
                            - (double) (get (coarseTune) * 100 + get (fineTune) + presetTune) / 100.0;
            // Attenuation in centibels (SoundFont players scale it by 0.4).
            zone.gain = juce::Decibels::decibelsToGain (-0.04f * (float) (get (initialAttenuation) + presetAttenuation));
            const auto modes = get (sampleModes) & 3;
            zone.loop = (modes == 1 || modes == 3) && loopEnd - loopStart > 16;
            zone.loopStart = juce::jlimit (0, zone.data->getNumSamples(), loopStart - start);
            zone.loopEnd = juce::jlimit (0, zone.data->getNumSamples(), loopEnd - start);
            data->zones.push_back (zone);
        }
    }

    if (data->zones.empty())
    {
        error = "this preset has no playable zones";
        return nullptr;
    }
    finish (*data);
    return data;
}

inline std::shared_ptr<SampleData> load (const juce::File& file, juce::String& error, SfzReport* report = nullptr,
                                         int bank = -1, int program = -1)
{
    // The bank and program only choose among an SF2's presets (step 38).
    return file.hasFileExtension ("sfz") ? loadSfz (file, error, report) : loadSf2 (file, error, bank, program);
}
} // namespace MultiSample

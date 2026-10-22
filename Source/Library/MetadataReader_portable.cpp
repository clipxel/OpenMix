// Tags on Windows, Linux and Android (macOS and iOS use MetadataReader_mac.mm): ID3v2 frames read directly from
// MP3 / AIFF files, RIFF INFO and FLAC / Ogg comments through JUCE's readers, and the duration
// from the decoder. Read-only.

#include "MetadataReader.h"

#if ! (JUCE_MAC || JUCE_IOS)

#include <juce_audio_formats/juce_audio_formats.h>

namespace om::Metadata
{

static juce::String decodeText (const juce::uint8* p, int n)
{
    if (n <= 0) return {};
    const int enc = p[0];
    ++p; --n;
    auto trimNul = [] (juce::String s) { return s.upToFirstOccurrenceOf (juce::String::charToString (0), false, false).trim(); };

    if (enc == 1 || enc == 2)                       // UTF-16 with BOM / UTF-16BE
    {
        bool bigEndian = enc == 2;
        if (n >= 2 && enc == 1)
        {
            bigEndian = p[0] == 0xFE && p[1] == 0xFF;
            if ((p[0] == 0xFF && p[1] == 0xFE) || bigEndian) { p += 2; n -= 2; }
        }
        juce::String out;
        for (int i = 0; i + 1 < n; i += 2)
        {
            auto at = [&] (int k) { return (juce::juce_wchar) (bigEndian ? (p[k] << 8 | p[k + 1]) : (p[k + 1] << 8 | p[k])); };
            auto c = at (i);
            if (c == 0) break;
            if (c >= 0xd800 && c < 0xdc00 && i + 3 < n && at (i + 2) >= 0xdc00 && at (i + 2) < 0xe000)
            {
                c = 0x10000 + ((c - 0xd800) << 10) + (at (i + 2) - 0xdc00);      // surrogate pair (emoji…)
                i += 2;
            }
            out += juce::String::charToString (c);
        }
        return out.trim();
    }
    if (enc == 3)                                   // UTF-8
        return trimNul (juce::String::fromUTF8 ((const char*) p, n));

    juce::String out;                               // ISO-8859-1
    for (int i = 0; i < n && p[i] != 0; ++i)
        out += juce::String::charToString ((juce::juce_wchar) p[i]);
    return out.trim();
}

/** ID3v2.2 / 2.3 / 2.4 text frames (title, artist, album, genre, comment, key, BPM). */
static void readId3 (juce::InputStream& in, FileTags& tags)
{
    juce::uint8 h[10];
    if (in.read (h, 10) != 10 || h[0] != 'I' || h[1] != 'D' || h[2] != '3')
        return;
    const int version = h[3];
    const int size = (h[6] & 0x7f) << 21 | (h[7] & 0x7f) << 14 | (h[8] & 0x7f) << 7 | (h[9] & 0x7f);
    if (size <= 0 || size > 16 * 1024 * 1024)
        return;
    juce::HeapBlock<juce::uint8> data ((size_t) size);
    if (in.read (data, size) != size)
        return;

    const int idLen = version == 2 ? 3 : 4, headLen = version == 2 ? 6 : 10;
    int pos = 0;
    if (version >= 3 && (h[5] & 0x40) != 0 && size >= 4)        // extended header
        pos += version == 4 ? ((data[0] & 0x7f) << 21 | (data[1] & 0x7f) << 14 | (data[2] & 0x7f) << 7 | (data[3] & 0x7f))
                            : (data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3]) + 4;

    while (pos + headLen <= size && data[pos] != 0)
    {
        const juce::String id ((const char*) data.get() + pos, (size_t) idLen);
        int len;
        if (version == 2)      len = data[pos + 3] << 16 | data[pos + 4] << 8 | data[pos + 5];
        else if (version == 4) len = (data[pos + 4] & 0x7f) << 21 | (data[pos + 5] & 0x7f) << 14 | (data[pos + 6] & 0x7f) << 7 | (data[pos + 7] & 0x7f);
        else                   len = data[pos + 4] << 24 | data[pos + 5] << 16 | data[pos + 6] << 8 | data[pos + 7];
        pos += headLen;
        if (len <= 0 || pos + len > size)
            break;
        const auto* body = data.get() + pos;

        auto text = [&] { return decodeText (body, len); };
        if      (id == "TIT2" || id == "TT2") tags.title  = text();
        else if (id == "TPE1" || id == "TP1") tags.artist = text();
        else if (id == "TALB" || id == "TAL") tags.album  = text();
        else if (id == "TCON" || id == "TCO")
        {
            auto g = text();                                     // "(17)Rock" → "Rock"
            if (g.startsWith ("(") && g.fromFirstOccurrenceOf (")", false, false).isNotEmpty())
                g = g.fromFirstOccurrenceOf (")", false, false);
            tags.genre = g;
        }
        else if (id == "TKEY" || id == "TKE") tags.key    = text();
        else if (id == "TBPM" || id == "TBP") tags.bpm    = text().getDoubleValue();
        else if ((id == "COMM" || id == "COM") && len > 4 && tags.comment.isEmpty())
        {
            // encoding, language (3), short description (NUL-terminated), text
            int p = 4;
            const int enc = body[0];
            const bool wide = enc == 1 || enc == 2;
            while (p < len)
            {
                if (! wide && body[p] == 0) { ++p; break; }
                if (wide && p + 1 < len && body[p] == 0 && body[p + 1] == 0) { p += 2; break; }
                p += wide ? 2 : 1;
            }
            juce::HeapBlock<juce::uint8> tmp ((size_t) (len - p + 1));
            tmp[0] = (juce::uint8) enc;
            std::memcpy (tmp + 1, body + p, (size_t) (len - p));
            tags.comment = decodeText (tmp, len - p + 1);
        }
        pos += len;
    }
}

FileTags read (const juce::File& file)
{
    FileTags tags;

    if (auto in = file.createInputStream())
        readId3 (*in, tags);

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    if (std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file)); r != nullptr)
    {
        if (r->sampleRate > 0)
            tags.durationSeconds = (double) r->lengthInSamples / r->sampleRate;

        auto meta = [&r] (std::initializer_list<const char*> keys)
        {
            for (auto* k : keys)
                for (auto& key : r->metadataValues.getAllKeys())
                    if (key.equalsIgnoreCase (k))
                        if (auto v = r->metadataValues[key].trim(); v.isNotEmpty())
                            return v;
            return juce::String();
        };
        if (tags.title.isEmpty())   tags.title   = meta ({ "INAM", "TITLE" });
        if (tags.artist.isEmpty())  tags.artist  = meta ({ "IART", "ARTIST" });
        if (tags.album.isEmpty())   tags.album   = meta ({ "IPRD", "ALBUM" });
        if (tags.genre.isEmpty())   tags.genre   = meta ({ "IGNR", "GENRE" });
        if (tags.comment.isEmpty()) tags.comment = meta ({ "ICMT", "COMMENT", "DESCRIPTION" });
        if (tags.key.isEmpty())     tags.key     = meta ({ "INITIALKEY", "KEY" });
        if (tags.bpm <= 0.0)        tags.bpm     = meta ({ "BPM", "TBPM" }).getDoubleValue();
    }
    return tags;
}

} // namespace om::Metadata

#endif

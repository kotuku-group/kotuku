/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

*********************************************************************************************************************/

// Included by mp3.cpp to share the frame-header parser and scanner with playback.

static void append_codepoint(std::string &Text, uint32_t Code)
{
   if ((Code < 32 and Code != 9 and Code != 10 and Code != 13) or
       (Code >= 0xd800 and Code <= 0xdfff) or Code > 0x10ffff or Code IS 0xfffe or Code IS 0xffff) Code = 0xfffd;
   if (Code < 0x80) Text += char(Code);
   else if (Code < 0x800) {
      Text += char(0xc0 | (Code >> 6));
      Text += char(0x80 | (Code & 63));
   }
   else if (Code < 0x10000) {
      Text += char(0xe0 | (Code >> 12));
      Text += char(0x80 | ((Code >> 6) & 63));
      Text += char(0x80 | (Code & 63));
   }
   else {
      Text += char(0xf0 | (Code >> 18));
      Text += char(0x80 | ((Code >> 12) & 63));
      Text += char(0x80 | ((Code >> 6) & 63));
      Text += char(0x80 | (Code & 63));
   }
}

//********************************************************************************************************************
// ID3 text encodings: Latin-1, UTF-16 with BOM, UTF-16BE and UTF-8.  Only the first value is returned.

static std::string tag_text(std::span<const uint8_t> Data, int Encoding)
{
   std::string text;
   size_t pos = 0;
   bool little = false;

   if (Encoding IS 1) {
      if (Data.size() < 2) return text;
      if (Data[0] IS 0xff and Data[1] IS 0xfe) little = true;
      else if (Data[0] != 0xfe or Data[1] != 0xff) return text;
      pos = 2;
   }
   while (pos < Data.size()) {
      uint32_t code = Data[pos++];
      if (Encoding IS 1 or Encoding IS 2) {
         if (pos >= Data.size()) break;
         code = little ? code | (uint32_t(Data[pos]) << 8) : (code << 8) | Data[pos];
         pos++;

         if (code >= 0xd800 and code <= 0xdbff) {
            if (pos + 1 >= Data.size()) { append_codepoint(text, 0xfffd); break; }
            uint32_t low = little ? Data[pos] | (uint32_t(Data[pos+1]) << 8) :
               (uint32_t(Data[pos]) << 8) | Data[pos+1];

            if (low >= 0xdc00 and low <= 0xdfff) {
               code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
               pos += 2;
            }
            else code = 0xfffd;
         }
      }
      else if (Encoding IS 3 and code >= 0x80) {
         int count = code >= 0xc2 and code <= 0xdf ? 1 :
            code >= 0xe0 and code <= 0xef ? 2 : code >= 0xf0 and code <= 0xf4 ? 3 : 0;
         uint32_t minimum = count IS 1 ? 0x80 : count IS 2 ? 0x800 : 0x10000;

         if (!count or pos + count > Data.size()) code = 0xfffd;
         else {
            code &= (1 << (6 - count)) - 1;
            bool valid = true;
            for (int i = 0; i < count; i++) {
               if ((Data[pos] & 0xc0) != 0x80) { valid = false; break; }
               code = (code << 6) | (Data[pos++] & 63);
            }
            if (!valid or code < minimum) code = 0xfffd;
         }
      }

      if (!code) break;
      append_codepoint(text, code);
   }

   while (!text.empty() and text.back() IS ' ') text.pop_back();
   return text;
}

//********************************************************************************************************************

static void append_tag(std::string &XML, std::string_view Name, const std::string &Text)
{
   if (Text.empty()) return;

   XML += "<";
   XML += Name;
   XML += ">";
   for (char ch : Text) {
      if (ch IS '&') XML += "&amp;";
      else if (ch IS '<') XML += "&lt;";
      else if (ch IS '>') XML += "&gt;";
      else XML += ch;
   }
   XML += "</";
   XML += Name;
   XML += ">";
}

//********************************************************************************************************************

static bool synchsafe(const uint8_t *Data, uint32_t &Value)
{
   if ((Data[0] | Data[1] | Data[2] | Data[3]) & 0x80) return false;
   Value = (uint32_t(Data[0]) << 21) | (uint32_t(Data[1]) << 14) | (uint32_t(Data[2]) << 7) | Data[3];
   return true;
}

//********************************************************************************************************************

static void remove_unsync(std::vector<uint8_t> &Data)
{
   size_t out = 0;
   for (size_t i = 0; i < Data.size(); i++) {
      Data[out++] = Data[i];
      if (Data[i] IS 0xff and i + 1 < Data.size() and !Data[i+1]) i++;
   }
   Data.resize(out);
}

//********************************************************************************************************************

static std::string parse_text_tags(std::vector<uint8_t> Data, int Version, uint8_t Flags)
{
   std::string tags;

   if (Version < 2 or Version > 4) return tags;
   if (Version IS 2 and (Flags & 0x40)) return tags; // Compressed v2.2 tags are unsupported.
   if (Version < 4 and (Flags & 0x80)) remove_unsync(Data);

   size_t pos = 0;

   if (Version >= 3 and (Flags & 0x40)) {
      if (Data.size() < 4) return tags;
      uint32_t size = read_be32(Data.data());

      if (Version IS 4) {
         if (!synchsafe(Data.data(), size) or size < 6) return tags;
      }
      else {
         if (size < 6 or size > Data.size() - 4) return tags;
         size += 4;
      }

      pos = size;
   }

   const size_t header = Version IS 2 ? 6 : 10;
   while (pos <= Data.size() and Data.size() - pos >= header) {
      const uint8_t *frame = Data.data() + pos;

      if (!frame[0]) break;

      const size_t name_size = Version IS 2 ? 3 : 4;
      std::string_view id((const char *)frame, name_size);

      uint32_t size;
      if (Version IS 2) size = (uint32_t(frame[3]) << 16) | (uint32_t(frame[4]) << 8) | frame[5];
      else if (Version IS 4) { if (!synchsafe(frame + 4, size)) break; }
      else size = read_be32(frame + 4);

      pos += header;
      if (size > Data.size() - pos) break;
      pos += size;
      if (!size) continue;

      // Skip compressed, encrypted or grouped frames rather than interpreting their prefixes as text.

      if (Version IS 3 and (frame[9] & 0xe0)) continue;
      if (Version IS 4 and (frame[9] & 0x4c)) continue;

      std::string_view name;
      if (id IS "TIT2" or id IS "TT2") name = "title";
      else if (id IS "TPE1" or id IS "TP1") name = "artist";
      else if (id IS "TALB" or id IS "TAL") name = "album";
      else if (id IS "TRCK" or id IS "TRK") name = "track";
      else if (id IS "TDRC" or id IS "TYER" or id IS "TYE") name = "year";
      else if (id IS "TCON" or id IS "TCO") name = "genre";
      else continue;

      std::vector<uint8_t> value(frame + header, frame + header + size);

      if (Version IS 4 and ((Flags & 0x80) or (frame[9] & 2))) remove_unsync(value);

      if (Version IS 4 and (frame[9] & 1)) {
         uint32_t data_length;
         if (value.size() < 4 or !synchsafe(value.data(), data_length)) continue;
         value.erase(value.begin(), value.begin() + 4);
         if (data_length != value.size()) continue;
      }

      if (value.empty() or value[0] > 3) continue;

      append_tag(tags, name, tag_text(std::span<const uint8_t>(value).subspan(1), value[0]));
   }
   return tags;
}

/*********************************************************************************************************************

-FUNCTION-
GetMetadata: Queries MP3 stream properties and optional text tags without creating a Sound object.

GetMetadata() opens the file at `Path` for reading and inspects the first MPEG Layer III frame that follows any ID3v2
tag.  The audio is not decoded, so the call is considerably cheaper than creating an @MP3 object when only the stream
properties are needed.  The result is returned in `XML` as a single document in the following form:

<pre>
&lt;mp3 sampleRate="44100" channels="2" bitrate="128000"&gt;
  &lt;duration seconds="215.432000" source="xing" estimated="false"/&gt;
  &lt;tags&gt;
    &lt;title&gt;...&lt;/title&gt;&lt;artist&gt;...&lt;/artist&gt;&lt;album&gt;...&lt;/album&gt;
    &lt;track&gt;...&lt;/track&gt;&lt;year&gt;...&lt;/year&gt;&lt;genre&gt;...&lt;/genre&gt;
  &lt;/tags&gt;
&lt;/mp3&gt;
</pre>

The output is not indented in practice.  The `sampleRate`, `channels` and `bitrate` attributes are taken from the
first audio frame; `bitrate` is expressed in bits per second and does not represent the average rate of a VBR stream.

The `duration` element reports the playback length in seconds, with `source` indicating how it was obtained:

<list type="bullet">
<li><b>xing</b>: Read from a Xing or Info header in the first frame.  This source is always used when available,
regardless of `Flags`, and is corrected for the encoder delay and padding recorded in a LAME extension.</li>
<li><b>scan</b>: Calculated by reading every frame header in the file.  Requires `SCAN` and is accurate for both CBR
and VBR streams, but the cost is proportional to the file size.</li>
<li><b>estimate</b>: Derived from the compressed audio size and the bitrate of the first frame.  Requires `ESTIMATE`
and is unreliable for VBR streams.  The `estimated` attribute is `true` only for this source.</li>
</list>

If no source is available, the `duration` element is omitted.

When `TAGS` is set, the `tags` element lists the title, artist, album, track, year and genre values that are present.
Values from ID3v2.2, ID3v2.3 and ID3v2.4 text frames take precedence, and an ID3v1 trailer fills any values that remain
missing, including the ID3v1.1 track number and the ID3v1 genre name.  Text is converted to UTF-8 and XML-escaped;
only the first value of a multi-value frame is reported, trailing spaces are removed and empty values are omitted.
ID3v2 values such as `TCON` genre references and `TDRC` timestamps are reported as stored.  Compressed or encrypted
frames are ignored, and tag parsing is skipped if the ID3v2 block exceeds 1 MiB.  The `tags` element is omitted if no
values are found.

!MPF

`XML` is cleared on entry and is only populated if the call succeeds.  Errors raised by the underlying @File
operations are returned to the caller unchanged.

-INPUT-
strview Path: Path to the MP3 file.
flags(MPF) Flags: Optional flags that enable tag parsing and more costly duration calculations.
^&string XML: Receives the XML document.

-ERRORS-
Okay
NullArgs
Args: `Path` is empty or contains a null byte, or `Flags` contains an unrecognised value.
CreateObject: The file could not be opened for reading.
NoSupport: The file is too small or no MPEG Layer III frame was found where the audio data begins.
InvalidData: The ID3v2 tag is malformed, the Xing header gives a non-positive length, or a scan lost frame sync.

-TAGS-
blocking, mutates-input
-END-

*********************************************************************************************************************/

namespace mp3 {

ERR GetMetadata(const std::string_view &Path, MPF Flags, std::string *XML)
{
   if (!XML) return ERR::NullArgs;

   XML->clear();

   if (Path.empty() or Path.find('\0') != std::string_view::npos or
       (Flags & ~(MPF::TAGS|MPF::ESTIMATE|MPF::SCAN)) != MPF::NIL) return ERR::Args;

   auto file = kt::make_unique_object(objFile::create::local(fl::Path(Path), fl::Flags(FL::READ|FL::APPROXIMATE)));
   if (!file) return ERR::CreateObject;

   int64_t filesize;
   if (auto error = file->getSize(filesize); error != ERR::Okay) return error;
   if (filesize < 4) return ERR::NoSupport;

   int trailer = 0;
   std::array<uint8_t, 128> id3{};
   if (filesize >= 128) {
      if (auto error = file->seekStart(filesize - 128); error != ERR::Okay) return error;
      int count = 0;
      if (auto error = file->read(std::span<int8_t>((int8_t *)id3.data(), id3.size()), &count);
          error != ERR::Okay) return error;
      if (count IS 128 and !memcmp(id3.data(), "TAG", 3)) trailer = 128;
   }

   if (auto error = file->seekStart(0); error != ERR::Okay) return error;
   std::array<uint8_t, 16 * 1024> input{};
   int count = 0;

   if (auto error = file->read(std::span<int8_t>((int8_t *)input.data(), 10), &count);
       error != ERR::Okay) return error;

   int64_t offset = 0;
   std::string tags;
   if (count >= 10 and !memcmp(input.data(), "ID3", 3)) {
      uint32_t size;
      if (not synchsafe(input.data() + 6, size)) return ERR::InvalidData;
      offset = 10 + int64_t(size) + ((input[3] IS 4 and (input[5] & 0x10)) ? 10 : 0);
      if (offset > filesize - trailer) return ERR::InvalidData;
      if ((Flags & MPF::TAGS) != MPF::NIL and size <= 1024 * 1024) {
         std::vector<uint8_t> data(size);
         int read = 0;
         if (size) {
            if (auto error = file->read(std::span<int8_t>((int8_t *)data.data(), data.size()), &read);
                error != ERR::Okay) return error;
         }
         if (read != int(size)) return ERR::InvalidData;
         tags = parse_text_tags(std::move(data), input[3], input[5]);
      }
   }

   if (auto error = file->seekStart(offset); error != ERR::Okay) return error;

   int available = int(std::min<int64_t>(input.size(), filesize - trailer - offset));

   if (available <= 0) return ERR::NoSupport;

   if (auto error = file->read(std::span<int8_t>((int8_t *)input.data(), available), &count);
       error != ERR::Okay) return error;

   mp3dec_t decoder{};
   mp3dec_init(&decoder);

   mp3dec_frame_info_t info{};
   int samples = mp3dec_decode_frame(&decoder, input.data(), count, nullptr, &info);
   if (!samples or info.layer != 3 or info.hz <= 0 or info.channels <= 0) return ERR::NoSupport;

   offset += info.frame_offset;
   info.frame_bytes -= info.frame_offset;

   xing_metadata xing;
   double seconds = 0;
   const char *source = nullptr;

   if (parse_xing(input.data() + info.frame_offset, info, xing)) {
      int64_t total = int64_t(xing.Frames) * samples - xing.Delay - xing.Padding;
      if (total <= 0) return ERR::InvalidData;
      seconds = double(total) / info.hz;
      source = "xing";
   }
   else if ((Flags & MPF::SCAN) != MPF::NIL) {
      int64_t length = 0, stream_size = 0;
      int frames = 0;
      bool vbr = false;
      if (auto error = scan_frames(file.get(), offset, trailer, &length, frames, vbr, stream_size);
          error != ERR::Okay) return error;
      seconds = double(length) / (info.channels * sizeof(int16_t)) / info.hz;
      source = "scan";
   }
   else if ((Flags & MPF::ESTIMATE) != MPF::NIL and info.bitrate_kbps > 0) {
      seconds = double(filesize - trailer - offset) * 8 / (info.bitrate_kbps * 1000);
      source = "estimate";
   }

   if ((Flags & MPF::TAGS) != MPF::NIL and trailer) {
      auto fallback = [&](std::string_view Name, size_t Start, size_t Size) {
         if (tags.find(std::string("<") + std::string(Name) + ">") IS std::string::npos) {
            append_tag(tags, Name, tag_text(std::span<const uint8_t>(id3).subspan(Start, Size), 0));
         }
      };

      fallback("title", 3, 30);
      fallback("artist", 33, 30);
      fallback("album", 63, 30);
      fallback("year", 93, 4);

      if (!id3[125] and id3[126] and tags.find("<track>") IS std::string::npos) {
         append_tag(tags, "track", std::to_string(id3[126]));
      }

      if (id3[127] < genre_table.size() and tags.find("<genre>") IS std::string::npos) {
         append_tag(tags, "genre", genre_table[id3[127]]);
      }
   }

   std::string xml = "<mp3 sampleRate=\"" + std::to_string(info.hz) + "\" channels=\"" +
      std::to_string(info.channels) + "\" bitrate=\"" + std::to_string(info.bitrate_kbps * 1000) + "\">";

   if (source) {
      xml += "<duration seconds=\"" + std::to_string(seconds) + "\" source=\"" + source + "\" estimated=\"" +
         (std::string_view(source) IS "estimate" ? "true" : "false") + "\"/>";
   }

   if (not tags.empty()) xml += "<tags>" + tags + "</tags>";

   xml += "</mp3>";
   *XML = std::move(xml);
   return ERR::Okay;
}

} // namespace mp3

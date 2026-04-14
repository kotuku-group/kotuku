/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

MP3: Sound class extension

*********************************************************************************************************************/

#include <kotuku/main.h>
#include <kotuku/modules/audio.h>
#include <kotuku/strings.hpp>

extern "C" {
//#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_IMPLEMENTATION
#include "minimp3/minimp3.h"
}

#include <array>

using namespace pf;

#define VER_MP3 1.0

JUMPTABLE_CORE
JUMPTABLE_AUDIO

static OBJECTPTR modAudio = nullptr;
static OBJECTPTR clMP3 = nullptr;

const int COMMENT_TRACK = 29;

#define MPF_MPEG1     (1<<19)
#define MPF_PAD       (1<<9)
#define MPF_COPYRIGHT (1<<3)
#define MPF_ORIGINAL  (1<<2)

const int MAX_FRAME_BYTES = MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t);

struct prvMP3 {
   std::array<uint8_t, 16 * 1024> Input;  // For incoming MP3 data.  Also needs to be big enough to accomodate the ID3v2 header.
   std::array<uint8_t, 100> TOC; // Xing Table of Contents
   std::array<uint8_t, MAX_FRAME_BYTES> Overflow;
   mp3dec_t mp3d;              // Decoder information
   mp3dec_frame_info_t info;   // Retains info on the most recently decoded frame.
   objFile *File;              // Source MP3 file
   // frame_bytes, frame_offset, channels, hz, layer, bitrate_kbps
   int   OverflowPos;         // Overflow read position
   int   OverflowSize;        // Number of bytes used in the overflow buffer.
   int   SamplesPerFrame;     // Last known frame size, measured in samples: 384, 576 or 1152
   int   SeekOffset;          // Offset to apply when performing seek operations.
   int64_t  WriteOffset;         // Current stream offset in bytes, relative to Sound.Length.
   int64_t  ReadOffset;          // Current seek position for the Read() action.  Max value is Sound.Length
   int   CompressedOffset;    // Next byte position for reading compressed input
   int   FramesProcessed;     // Count of frames processed by the decoder.
   int   TotalFrames;         // Total frames for the entire stream (known if CBR data, or VBR header is present).
   int   TotalSamples;        // Total samples for the entire stream.  Adjusted for null padding at either end of the stream.
   int   PaddingStart;        // Total samples at the start of the decoded stream that can be skipped.
   int   PaddingEnd;          // Total samples at the end of the decoded stream that can be ignored.
   int   StreamSize;          // Compressed stream length, if defined by VBR header.
   bool   EndOfFile;           // True if all incoming data has been read.
   bool   VBR;                 // True if VBR detected, otherwise CBR
   bool   XingChecked;         // True if Xing header has been checked
   bool   TOCLoaded;           // True if the Table of Contents has been defined.

   void reset() { // Reset the decoder.  Necessary for seeking.
      CompressedOffset = 0;
      ReadOffset       = 0;
      WriteOffset      = 0;
      FramesProcessed  = 0;
      SamplesPerFrame  = 1152;
      OverflowPos      = 0;
      OverflowSize     = 0;
      EndOfFile        = false;
   }
};

struct id3tag {
   char tag[3];
   char title[30];
   char artist[30];
   char album[30];
   char year[4];
   char comment[30]; // Byte 30 may contain a track number instead of a null terminator
   uint8_t genre;
};

static int find_frame(objSound *, uint8_t *, int);

//********************************************************************************************************************

static const std::vector<CSTRING> genre_table = {
   "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge",
   "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B",
   "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
   "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop",
   "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental", "Acid",
   "House", "Game", "Sound Clip", "Gospel", "Noise", "AlternRock", "Bass",
   "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock",
   "Ethnic", "Gothic", "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk",
   "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40",
   "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
   "Psychadelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk",
   "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock", "Folk",
   "Folk/Rock", "National folk", "Swing", "Fast-fusion", "Bebob", "Latin",
   "Revival", "Celtic", "Bluegrass", "Avantgarde", "Gothic Rock", "Progressive Rock",
   "Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Big Band", "Chorus",
   "Easy Listening", "Acoustic", "Humour", "Speech", "Chanson", "Opera", "Chamber Music",
   "Sonata", "Symphony", "Booty Bass", "Primus", "Porn Groove", "Satire",
   "Slow Jam", "Club", "Tango", "Samba", "Folklore", "Ballad", "Powder Ballad",
   "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock", "Drum Solo", "A Capella",
   "Euro-House", "Dance Hall", "Goa", "Drum & Bass", "Club House", "Hardcore",
   "Terror", "Indie", "BritPop", "NegerPunk", "Polsk Punk", "Beat",
   "Christian Gangsta", "Heavy Metal", "Black Metal", "Crossover", "Contemporary C",
   "Christian Rock", "Merengue", "Salsa", "Thrash Metal", "Anime", "JPop",
   "SynthPop"
};

// Determine the decoded byte length of the entire MP3 sample

static const int bitrate_table[5][15] = {
   // MPEG-1
   { 0, 32000,  64000,  96000, 128000, 160000, 192000, 224000, 256000, 288000, 320000, 352000, 384000, 416000, 448000 }, // Layer I
   { 0, 32000,  48000,  56000,  64000,  80000,  96000, 112000, 128000, 160000, 192000, 224000, 256000, 320000, 384000 }, // Layer II
   { 0, 32000,  40000,  48000,  56000,  64000,  80000,  96000, 112000, 128000, 160000, 192000, 224000, 256000, 320000 }, // Layer III
   // MPEG-2 LSF
   { 0, 32000,  48000,  56000,  64000,  80000,  96000, 112000, 128000, 144000, 160000, 176000, 192000, 224000, 256000 }, // Layer I
   { 0,  8000,  16000,  24000,  32000,  40000,  48000,  56000, 64000,  80000,  96000,  112000, 128000, 144000, 160000 }  // Layers II & III
};

static const int samplerate_table[3] = { 44100, 48000, 32000 };

static int64_t calc_length(objSound *, int);

//********************************************************************************************************************
// The ID3v1 tag can be located at the end of the MP3 file.
// There may also be an 'Enhanced TAG' just prior to the ID3v1 header - this code does not support it yet.

static bool parse_id3v1(objSound *Self)
{
   pf::Log log(__FUNCTION__);

   auto prv = (prvMP3 *)Self->ChildPrivate;
   bool processed = false;

   id3tag id3;
   prv->File->seekEnd(sizeof(id3));

   int result;
   if ((prv->File->read(&id3, sizeof(id3), &result) IS ERR::Okay) and (result IS sizeof(id3))) {
      if (!strncmp("TAG", (STRING)&id3, 3)) {
         char buffer[sizeof(id3)];

         log.detail("ID3v1 tag found.");

         std::string title(id3.title);
         pf::ltrim(title, " ");
         acSetKey(Self, "Title", title.c_str());

         std::string artist(id3.artist);
         pf::ltrim(artist, " ");
         acSetKey(Self, "Author", artist.c_str());

         std::string album(id3.album);
         pf::ltrim(album, " ");
         acSetKey(Self, "Album", album.c_str());

         std::string comment(id3.comment);
         pf::ltrim(comment, " ");
         acSetKey(Self, "Description", comment.c_str());

         if (id3.genre <= genre_table.size()) {
            acSetKey(Self, "Genre", genre_table[id3.genre]);
         }
         else acSetKey(Self, "Genre", "Unknown");

         if (id3.comment[COMMENT_TRACK] > 0) {
            strcopy(std::to_string(id3.comment[COMMENT_TRACK]), buffer, sizeof(buffer));
            acSetKey(Self, "Track", buffer);
         }

         processed = true;
      }
   }

   prv->File->seekStart(0);

   return processed;
}

//********************************************************************************************************************

static int detect_id3v2(const char *Buffer)
{
   if (!strncmp(Buffer, "ID3", 3)) {
      if (!((Buffer[5] & 15) or (Buffer[6] & 0x80) or (Buffer[7] & 0x80) or (Buffer[8] & 0x80) or (Buffer[9] & 0x80))) {
         int id3v2size = (((Buffer[6] & 0x7f) << 21) | ((Buffer[7] & 0x7f) << 14) | ((Buffer[8] & 0x7f) << 7) | (Buffer[9] & 0x7f)) + 10;
         if ((Buffer[5] & 16)) id3v2size += 10; // footer
         return id3v2size;
      }
   }

   return 0;
}

//********************************************************************************************************************
// Check for the Xing/Info tag.  Ideally this is always present for VBR files, and is also useful for CBR files.

const int XING_FRAMES       = 1; // Total number of frames is defined.
const int XING_STREAM_SIZE  = 2; // The compressed audio stream size in bytes is defined.  Excludes ID3vX, Xing etc.
const int XING_TOC          = 4; // TOC entries are defined.
const int XING_SCALE        = 8; // VBR quality is indicated from 0 (best) to 100 (worst).

static int check_xing(objSound *Self, const uint8_t *Frame)
{
   pf::Log log;

   auto prv = (prvMP3 *)Self->ChildPrivate;

   if (prv->XingChecked) return 1;
   else prv->XingChecked = true;

   bs_t bs[1];
   L3_gr_info_t gr_info[4];
   bs_init(bs, Frame + HDR_SIZE, prv->info.frame_bytes - HDR_SIZE);
   if (HDR_IS_CRC(Frame)) get_bits(bs, 16);
   if (L3_read_side_info(bs, gr_info, Frame) < 0) return 0; // side info corrupted

   const uint8_t *tag = Frame + HDR_SIZE + bs->pos / 8;
   if ((!strncmp("Xing", (CSTRING)tag, 4)) and (!strncmp("Info", (CSTRING)tag, 4))) return 0;
   const int flags = tag[7];
   if (!(flags & XING_FRAMES)) return -1;
   tag += 8;

   prv->TotalFrames = int((tag[0] << 24) | (tag[1] << 16) | (tag[2] << 8) | tag[3]);
   //prv->TotalFrames--; // The VBR frame doesn't count as audio data.
   tag += 4;

   if (flags & XING_STREAM_SIZE) {
      // This value is used for TOC seek calculations.
      prv->StreamSize = int((tag[0] << 24) | (tag[1] << 16) | (tag[2] << 8) | tag[3]);
      tag += 4;
   }

   if (flags & XING_TOC) {
      copymem(tag, prv->TOC.data(), 100);
      tag += 100;
      prv->TOCLoaded = true;
   }

   if (flags & XING_SCALE) {
      int quality = int((tag[0] << 24) | (tag[1] << 16) | (tag[2] << 8) | tag[3]);
      acSetKey(Self, "Quality", std::to_string(quality).c_str());
      tag += 4;
   }

   int delay = 0; // Typically the first 528 samples are padding set to zero and can be skipped.
   int padding = 0; // Padding tells you the number of samples at the end of the file that are empty.

   if (*tag) { // Optional extension, LAME, Lavc, etc. Should be the same structure.
       tag += 21;
       if (tag - Frame + 14 >= prv->info.frame_bytes);
       else {
          delay   = ((tag[0] << 4) | (tag[1] >> 4)) + (528 + 1);
          padding = (((tag[1] & 0xF) << 8) | tag[2]) - (528 + 1);
       }
   }

   prv->PaddingEnd   = padding;
   prv->PaddingStart = delay;

   // Calculate the total no of samples for the entire stream, adjusted for padding at both the start and end.

   int64_t detected_samples = prv->info.samples * (int64_t)prv->TotalFrames;
   if (detected_samples >= prv->PaddingStart) detected_samples -= prv->PaddingStart;
   if (detected_samples >= prv->PaddingEnd) detected_samples -= prv->PaddingEnd;

   prv->TotalSamples = detected_samples;

   const double seconds_len = double(detected_samples) / double(prv->info.hz);

   // Compute byte length with adjustment for padding at the end, but not the start.

   int64_t len = prv->TotalFrames * prv->SamplesPerFrame * prv->info.channels * sizeof(int16_t);
   len -= int64_t(prv->PaddingEnd * prv->info.channels) * sizeof(int16_t);
   Self->setLength(len);

   log.msg("Info header detected.  Total Frames: %d, Samples: %d, Track Time: %.2fs, Byte Length: %" PRId64 ", Padding: %d/%d",
      prv->TotalFrames, prv->TotalSamples, seconds_len, int64_t(len), prv->PaddingStart, prv->PaddingEnd);

   return 1;
}


//********************************************************************************************************************

static ERR MP3_Free(objSound *Self)
{
   prvMP3 *prv;
   if (!(prv = (prvMP3 *)Self->ChildPrivate)) return ERR::Okay;

   if (prv->File) { FreeResource(prv->File); prv->File = nullptr; }

   return ERR::Okay;
}

//********************************************************************************************************************
// Playback is managed by Sound.acActivate()

static ERR MP3_Init(objSound *Self)
{
   pf::Log log;

   CSTRING location = nullptr;
   Self->get(FID_Path, location);

   if ((!location) or ((Self->Flags & SDF::NEW) != SDF::NIL)) {
      // If no location has been specified, assume that the sound is being
      // created from scratch (e.g. to record an mp3 file to disk).

      return ERR::Okay;
   }

   prvMP3 *prv;
   if (AllocMemory(sizeof(prvMP3), MEM::DATA, &Self->ChildPrivate) IS ERR::Okay) {
      prv = (prvMP3 *)Self->ChildPrivate;
      new (prv) prvMP3;
   }
   else return ERR::AllocMemory;

   mp3dec_init(&prv->mp3d);
   prv->reset();

   // Fill the buffer with audio information and parse any MP3 header that is present.  This will also prove whether
   // or not this is really an mp3 file.

   if (!prv->File) {
      if (!(prv->File = objFile::create::local(fl::Path(location), fl::Flags(FL::READ|FL::APPROXIMATE)))) {
         return log.warning(ERR::CreateObject);
      }
   }
   else prv->File->seekStart(0);

   // Read the ID3v1 file header, if there is one.

   int reduce = 0;
   if (parse_id3v1(Self)) reduce += sizeof(struct id3tag);

   // Process ID3V2 and Xing VBR headers if present.

   int result;
   if (prv->File->read(prv->Input.data(), prv->Input.size(), &result) IS ERR::Okay) {
      if (auto id3size = detect_id3v2((const char *)prv->Input.data())) {
         log.msg("Detected ID3v2 header of %d bytes.", id3size);
         prv->SeekOffset = id3size;
         prv->File->seekStart(prv->SeekOffset);
         prv->File->read(prv->Input.data(), prv->Input.size(), &result);
      }
      else {
         log.msg("No ID3v2 header found.");
         prv->SeekOffset = 0;
      }

      if (find_frame(Self, prv->Input.data(), result) != -1) {
         if (check_xing(Self, prv->Input.data())) {
            prv->SeekOffset += prv->info.frame_bytes;
         }
         else log.detail("No VBR header found.");
      }
   }
   else {
      FreeResource(Self->ChildPrivate); Self->ChildPrivate = nullptr;
      return ERR::NoSupport;
   }

   prv->File->seekStart(prv->SeekOffset);

   if (prv->info.channels IS 2) Self->Flags |= SDF::STEREO;
   if (Self->Stream != STREAM::NEVER) Self->Flags |= SDF::STREAM;

   Self->BytesPerSecond = int(prv->info.hz * prv->info.channels * sizeof(int16_t));
   Self->BitsPerSample  = 16;
   Self->Frequency      = prv->info.hz;
   Self->Playback       = Self->Frequency;

   if (Self->Length <= 0) {
      Self->Length = calc_length(Self, reduce);
      prv->File->seekStart(prv->SeekOffset);
   }

   log.msg("File is MP3.  Stereo: %c, BytesPerSecond: %d, Freq: %d, Byte Length: %d",
      ((Self->Flags & SDF::STEREO) != SDF::NIL) ? 'Y' : 'N', Self->BytesPerSecond, Self->Frequency, Self->Length);

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR MP3_Read(objSound *Self, struct acRead *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   Args->Result = 0;
   if (Args->Length <= 0) return ERR::Okay;

   auto prv = (prvMP3 *)Self->ChildPrivate;

   // Keep decoding until we exhaust space in the output buffer.  Setting the EOF to true indicates that everything
   // has been output, or an error has occurred.

   int pos = 0;
   auto write_offset  = prv->WriteOffset;
   bool no_more_input = false;
   while ((prv->WriteOffset < Self->Length) and (!prv->EndOfFile) and (pos < Args->Length)) {
      // Previously decoded bytes that overflowed have priority.

      if ((prv->OverflowSize) and (prv->OverflowPos < prv->OverflowSize)) {
         int to_copy = prv->OverflowSize - prv->OverflowPos;
         if (pos + to_copy > Args->Length) to_copy = Args->Length - pos;
         copymem(prv->Overflow.data() + prv->OverflowPos, (uint8_t *)Args->Buffer + pos, to_copy);
         prv->OverflowPos += to_copy;
         prv->WriteOffset += to_copy;
         pos += to_copy;
         continue;
      }

      // Read as much input as possible.

      log.trace("Writing %" PF64 " max bytes to %d, Avail. Compressed: %d bytes", (long long)Args->Length-pos, prv->WriteOffset, prv->CompressedOffset);

      if ((prv->CompressedOffset < (int)prv->Input.size()) and (!prv->EndOfFile) and (!no_more_input)) {
         int result;
         if (auto error = prv->File->read(prv->Input.data() + prv->CompressedOffset, prv->Input.size() - prv->CompressedOffset, &result); error != ERR::Okay) {
            log.warning("File read error: %s", GetErrorMsg(error));
            prv->EndOfFile = true;
            break;
         }
         else if (!result) {
            log.detail("Reached end of input file.");
            no_more_input = true; // Don't change the EOF - let the output code do that.
         }

         prv->CompressedOffset += result;
      }

      int in = 0; // Always start from zero

      while ((prv->WriteOffset < Self->Length) and (in < (int)prv->Input.size() - (8 * 1024)) and (pos < Args->Length)) {
         int decoded_samples;

         if (pos + MAX_FRAME_BYTES > Args->Length) {
            // Buffer overflow management - necessary if we need to decode more data than what the output buffer can support.

            int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
            decoded_samples = mp3dec_decode_frame(&prv->mp3d, prv->Input.data() + in, prv->CompressedOffset - in, pcm, &prv->info);
            if (decoded_samples) {
               int decoded_bytes = decoded_samples * sizeof(int16_t) * prv->info.channels;

               if (pos + decoded_bytes > Args->Length) {
                  // We can't write the full amount, store the rest in overflow.  It is presumed that Length
                  // is sample-aligned, i.e. sample_size * channel_size; so usually 4 bytes.
                  prv->OverflowPos  = 0;
                  prv->OverflowSize = pos + decoded_bytes - Args->Length;
                  decoded_bytes = Args->Length - pos;
                  copymem((uint8_t *)pcm + decoded_bytes, prv->Overflow.data(), prv->OverflowSize);
               }

               copymem(pcm, (uint8_t *)Args->Buffer + pos, decoded_bytes);

               prv->FramesProcessed++;
               prv->WriteOffset += decoded_bytes;
               in  += prv->info.frame_bytes;
               pos += decoded_bytes;
            }
         }
         else {
            decoded_samples = mp3dec_decode_frame(&prv->mp3d, prv->Input.data() + in, prv->CompressedOffset - in, (int16_t *)((uint8_t *)Args->Buffer + pos), &prv->info);

            if (decoded_samples) {
               prv->FramesProcessed++;
               const int decoded_bytes = decoded_samples * sizeof(int16_t) * prv->info.channels;
               prv->WriteOffset += decoded_bytes;
               in  += prv->info.frame_bytes;
               pos += decoded_bytes;
            }
         }

         if (prv->WriteOffset >= Self->Length) {
            prv->WriteOffset = Self->Length;
            prv->EndOfFile = true;
         }

         // Decoder results:
         // 0: No MP3 data found; 384: Layer 1; 576: Layer 3; 1152: All others

         if (!decoded_samples) {
            if (prv->info.frame_bytes > 0) {
               // The decoder skipped ID3 or invalid data - do not play this frame
               log.msg("Skipping MP3 frame at offset %d, size %d.", in, prv->CompressedOffset - in);
               in += prv->info.frame_bytes;
            }
            else if (!prv->info.frame_bytes) {
               // Insufficient data (read more to obtain a frame) OR end of file
               if ((!in) or (no_more_input)) prv->EndOfFile = true;
               break;
            }
         }
      }

      // Shift any remaining data that couldn't be decoded.  This will help maintain the minimum 16k of
      // data in the buffer as recommended by minimp3.

      if (!in) break;
      else if (in < prv->CompressedOffset) {
         copymem((uint8_t *)prv->Input.data() + in, prv->Input.data(), prv->CompressedOffset - in);
      }

      prv->CompressedOffset -= in;
   }

   if (prv->EndOfFile) {
      // We now know the exact length of the decoded audio stream and use that to ensure playback stops
      // at the correct position.

      if (Self->Length != prv->WriteOffset) {
         log.detail("Decode complete, changing sample length from %d to %" PF64 " bytes.  Decoded %d frames.", Self->Length, (long long)prv->WriteOffset, prv->FramesProcessed);
         Self->setLength(prv->WriteOffset);
      }
      else log.detail("Decoding of %d MP3 frames complete, output %" PF64 " bytes.", prv->FramesProcessed, (long long)prv->WriteOffset);
   }

   Self->Position = prv->WriteOffset;
   Args->Result = prv->WriteOffset - write_offset;
   return ERR::Okay;
}

//********************************************************************************************************************
// Accuracy when seeking within an MP3 file is not guaranteed.  This means that offsets can be a little too far
// forward or backward relative to the known length.

static ERR MP3_Seek(objSound *Self, struct acSeek *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);
   if (!Self->initialised()) return log.warning(ERR::NotInitialised);

   auto prv = (prvMP3 *)Self->ChildPrivate;

   int64_t offset;
   if (Args->Position IS SEEK::START)         offset = int(Args->Offset);
   else if (Args->Position IS SEEK::END)      offset = Self->Length - int(Args->Offset);
   else if (Args->Position IS SEEK::CURRENT)  offset = prv->ReadOffset + int(Args->Offset);
   else if (Args->Position IS SEEK::RELATIVE) offset = Self->Length * Args->Offset;
   else return log.warning(ERR::Args);

   if (offset IS Self->Position) return ERR::Okay;

   if ((Self->Flags & SDF::STREAM) != SDF::NIL) {
      prv->reset();
      mp3dec_init(&prv->mp3d);

      if (Args->Offset <= 0) {
         log.traceBranch("Resetting play position to zero.");
         prv->File->seekStart(prv->SeekOffset);
      }
      else {
         // Seeking via byte position, where the position is relative to the decoded length.

         if (Self->Length <= 0) {
            log.warning("MP3 stream length unknown, cannot seek.");
            return ERR::Failed;
         }

         double pct = double(offset) / double(Self->Length);

         if (prv->TOCLoaded) {
            // The TOC gives us an approx. frame number for a given location in the compressed stream
            // (relative to TotalFrames).  By knowing the frame number, we can make more accurate calculations
            // as to time and length remaining.

            int idx = int(pct * prv->TOC.size());
            if (idx < 0) idx = 0;
            else if (idx >= (int)prv->TOC.size()) idx = prv->TOC.size() - 1;

            int offset = prv->SeekOffset + ((prv->TOC[idx] * prv->StreamSize) / 256);
            int frame = prv->TOC[idx] * prv->TotalFrames / 256;
            prv->File->seekStart(offset);

            log.detail("Seeking to byte offset %d, frame %d of %d", offset, frame, prv->TotalFrames);

            prv->WriteOffset     = int64_t(frame * prv->SamplesPerFrame * prv->info.channels) * sizeof(int16_t);
            prv->ReadOffset      = prv->WriteOffset;
            prv->FramesProcessed = frame;
            Self->Position = prv->WriteOffset;
         }
         else {
            // Seeking without a TOC has two approaches: 1) Scan from frame 1 until you reach the frame
            // you're looking for; 2) Use the average frame size to make a jump and rely on frame syncing to
            // find the nearest viable frame.  The accuracy of this is largely dependent on the calc_length()
            // computations.

            if (!prv->StreamSize) {
               int64_t size;
               prv->File->get(FID_Size, size);
               prv->StreamSize = size - prv->SeekOffset;
            }

            int frame  = int(prv->TotalFrames * pct);
            int offset = int(prv->StreamSize * pct);
            if (frame < 0) frame = 0;
            if (offset < 0) offset = 0;
            prv->File->seekStart(prv->SeekOffset + offset);

            log.detail("Seeking to byte offset %d, frame %d of %d", offset, frame, prv->TotalFrames);

            prv->WriteOffset     = int64_t(frame * prv->SamplesPerFrame * prv->info.channels) * sizeof(int16_t);
            prv->ReadOffset      = prv->WriteOffset;
            prv->FramesProcessed = frame;
            Self->Position = prv->WriteOffset;
         }
      }

      int active;
      if (Self->get(FID_Active, active) IS ERR::Okay) {
         if (active) {
            log.branch("Resetting state of active sample, seek to byte %" PF64, (long long)prv->WriteOffset);
            Self->deactivate();
            Self->Position = prv->WriteOffset;
            Self->activate();
         }
      }

      return ERR::Okay;
   }
   else {
      // Revert to base-class behaviour for fully buffered samples, since the MP3 is already decoded.
      return ERR::NoAction;
   }
}

//********************************************************************************************************************
// Calculate the approximate decoded length of an MP3 audio stream.  This will normally be unnecessary if the stream
// has defined a Xing header.

#define SIZE_BUFFER     256000  // Load up to this many bytes to determine if the file is in variable bit-rate
#define SIZE_CBR_BUFFER 51200   // Load at least this many bytes to determine if the file is in constant bit-rate

static int64_t calc_length(objSound *Self, int ReduceEnd)
{
   pf::Log log(__FUNCTION__);
   int buffer_size;
   std::array<uint16_t, 16> avg;  // Used to compute the interquartile mean
   std::vector<uint16_t> fsizes;  // List of all compressed frame sizes

   log.branch();

   auto prv = (prvMP3 *)Self->ChildPrivate;

   avg.fill(0);

   int frame_start     = 0;
   int current_bitrate = 0;
   int frame_size      = 0;
   int channels        = 1;
   int frame_samples   = 1152;
   int8_t layer           = 0;

   prv->VBR = false;

   int filesize;
   prv->File->get(FID_Size, filesize);

   uint8_t *buffer;
   if (AllocMemory(SIZE_BUFFER, MEM::DATA|MEM::NO_CLEAR, (APTR *)&buffer, nullptr) IS ERR::Okay) {
      // Load MP3 data from the file

      prv->File->seekStart(prv->SeekOffset);
      prv->File->read(buffer, SIZE_CBR_BUFFER, &buffer_size);

      // Find the start of the frame data

      frame_start = find_frame(Self, buffer, buffer_size);

      if (frame_start IS -1) {
         log.warning("Failed to find the first mp3 frame.");
         FreeResource(buffer);
         return -1;
      }

      int pos = frame_start;
      while (pos < buffer_size - 8) {
         // MP3 frame information consists of a single 32-bit header
         int frame = (buffer[pos]<<24) | (buffer[pos+1]<<16) | (buffer[pos+2]<<8) | buffer[pos+3];

         bool invalid;
         if ((frame & 0xffe00000) IS 0xffe00000) {
            layer = 4 - ((frame & ((1<<17)|(1<<18)))>>17);
            int samplerate = samplerate_table[(frame & 0x0c00)>>10];
            if ((layer > 3) or (!samplerate)) {
               pos++;
               continue;
            }

            int index = (frame & 0x0000f000)>>12;
            frame_samples = hdr_frame_samples(buffer);

            int bitrate;
            if (frame & MPF_MPEG1) bitrate = bitrate_table[layer - 1][index]; // MPEG-1
            else bitrate = bitrate_table[3 + (layer >> 1)][index]; // MPEG-2

            if (!current_bitrate) current_bitrate = bitrate;
            else if (current_bitrate != bitrate) prv->VBR = true;

            int8_t pad = (frame & MPF_PAD) ? 1 : 0;
            channels = HDR_IS_MONO(buffer) ? 1 : 2;

            if (layer IS 1) frame_size = ((12 * bitrate / samplerate) + pad) * 4;
            else frame_size = (144 * bitrate / samplerate) + pad;

            if (frame_size <= 0) { // In case file lacks integrity.  Frame must be at least 1 byte
               pos++;
               continue;
            }

            fsizes.push_back(frame_size);
            pos += frame_size;
            avg[index] = avg[index] + 1;
            invalid = false;
         }
         else invalid = true;

         // Check if we need to load more data into our buffer for VBR analysis

         if (pos >= buffer_size - 8) {
            if ((prv->VBR) and (buffer_size IS SIZE_CBR_BUFFER)) {
               // Read more file data so that we can calculate the vbr more accurately
               int result;
               prv->File->read(buffer + buffer_size, SIZE_BUFFER - buffer_size, &result);
               buffer_size += result;
            }
            else break; // File is CBR, no need to scan more data
         }

         // Check that the frame is valid

         if (invalid) {
            int index = find_frame(Self, buffer + pos, buffer_size - pos);
            if ((index IS -1) or (!index)) {
               log.msg("Failed to find the next frame at position %d.", pos);
               break;
            }
            else pos += index;
         }
      }

      FreeResource(buffer);
   }
   else return -1;

   if (fsizes.empty()) return -1;

   // Calculate average frame length using interquartile mean

   sort(fsizes.begin(), fsizes.end(), std::greater<uint16_t>());
   const int first = fsizes.size() / 4;
   const int last  = int(fsizes.size() * 0.75);
   double avg_frame_len = 0;
   for (int i=first; i < last; i++) avg_frame_len += fsizes[i];
   avg_frame_len /= (last - first);

   log.detail("File Size: %d, %d frames, Average frame length: %.2f bytes, VBR: %c", filesize, (int)fsizes.size(), avg_frame_len, prv->VBR ? 'Y' : 'N');

   if (filesize > buffer_size) {
      if (prv->VBR) {
         prv->TotalFrames = int((filesize - prv->SeekOffset - frame_start - ReduceEnd) / avg_frame_len);
         return int64_t(prv->TotalFrames * frame_samples * channels) * sizeof(int16_t);
      }
      else {
         // For CBR we guess the total frames from the file size.
         prv->File->get(FID_Size, filesize);
         int total_frames = int((filesize - prv->SeekOffset - frame_start - ReduceEnd) / avg_frame_len);
         double seconds = (total_frames * (double)avg_frame_len) / (double(current_bitrate) / 1000.0 * 125.0);
         prv->TotalFrames = total_frames;
         return seconds * Self->BytesPerSecond;
      }
   }
   else if (fsizes.size() > 0) { // The entire file was loaded into the buffer, so we know the exact length.
      return fsizes.size() * frame_size * channels * sizeof(int16_t);
   }
   else { // File has no detectable MP3 audio content
      return -1;
   }
}

//********************************************************************************************************************

static int find_frame(objSound *Self, uint8_t *Buffer, int BufferSize)
{
   pf::Log log(__FUNCTION__);
   int bitrate, frame_size;
   auto prv = (prvMP3 *)Self->ChildPrivate;

   log.traceBranch("Buffer Size: %d", BufferSize);

   for (int pos=0; (pos < BufferSize-8); pos++) {
      if (Buffer[pos] IS 0xff) {
         int frame = (Buffer[pos]<<24) | (Buffer[pos+1]<<16) | (Buffer[pos+2]<<8) | Buffer[pos+3];
         if ((frame & 0xffe00000) IS 0xffe00000) {
            // Frame sync found.  Check its validity by looking for a following frame.

            int layer = 4 - ((frame & ((1<<17)|(1<<18)))>>17);
            int index = (frame & 0x0c00)>>10;
            if (index >= std::ssize(samplerate_table)) continue;
            int samplerate = samplerate_table[index];
            if ((layer < 0) or (layer > 3) or (!samplerate)) continue;

            index = (frame & 0x0000f000)>>12;

            if (frame & MPF_MPEG1) bitrate = bitrate_table[layer - 1][index]; // MPEG-1
            else bitrate = bitrate_table[3 + (layer >> 1)][index]; // MPEG-2

            int8_t pad = (frame & MPF_PAD) ? 1 : 0;

            if (layer IS 1) frame_size = ((12 * bitrate / samplerate) + pad) * 4;
            else frame_size = (144 * bitrate / samplerate) + pad;

            int next = pos + frame_size;
            if (next + 4 < BufferSize) {
               frame = (Buffer[next]<<24) | (Buffer[next+1]<<16) | (Buffer[next+2]<<8) | Buffer[next+3];

               if ((frame & 0xffe00000) != 0xffe00000) continue;

               prv->info.channels    = HDR_IS_MONO(Buffer) ? 1 : 2;
               prv->info.hz          = samplerate;
               prv->info.frame_bytes = frame_size;
               prv->info.samples     = hdr_frame_samples(Buffer);

               log.detail("Frame found at %d, size %d, channels %d, %d samples, %dhz.", pos, prv->info.frame_bytes, prv->info.channels, prv->info.samples, prv->info.hz);

               return pos;
            }
         }
      }
   }

   log.detail("Failed to find a valid frame.");

   return -1;
}

//********************************************************************************************************************

static const struct ActionArray clActions[] = {
   { AC::Free, MP3_Free },
   { AC::Init, MP3_Init },
   { AC::Read, MP3_Read },
   { AC::Seek, MP3_Seek },
   { AC::NIL, nullptr }
};

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   CoreBase = argCoreBase;

   if (objModule::load("audio", &modAudio, &AudioBase) != ERR::Okay) return ERR::InitModule;

   clMP3 = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::SOUND),
      fl::ClassID(CLASSID::MP3),
      fl::ClassVersion(VER_MP3),
      fl::FileExtension("*.mp3"),
      fl::FileDescription("MP3 Audio Stream"),
      fl::Icon("filetypes/audio"),
      fl::Name("MP3"),
      fl::Category(CCF::AUDIO),
      fl::Actions(clActions),
      fl::Path(MOD_PATH));

   return clMP3 ? ERR::Okay : ERR::AddClass;
}

static ERR MODExpunge(void)
{
   if (clMP3) { FreeResource(clMP3); clMP3 = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, nullptr, MODExpunge, nullptr, nullptr, nullptr)
extern "C" struct ModHeader * register_mp3_module() { return &ModHeader; }

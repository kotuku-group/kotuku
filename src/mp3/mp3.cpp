/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

MP3: Sound class extension

*********************************************************************************************************************/

#define PRV_MP3
#include <kotuku/main.h>
#include <kotuku/modules/audio.h>
#include <kotuku/modules/filesystem.h>
#include <kotuku/modules/mp3.h>
#include <kotuku/modules/module.h>
#include <kotuku/strings.hpp>

extern "C" {
//#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_IMPLEMENTATION
#include "minimp3/minimp3.h"
}

#include <array>
#include <cmath>
#include <limits>

using namespace kt;

JUMPTABLE_CORE
JUMPTABLE_AUDIO

static OBJECTPTR modAudio = nullptr;
static OBJECTPTR clMP3 = nullptr;

const int COMMENT_TRACK = 29;

const int MAX_FRAME_BYTES = MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t);

struct prvMP3 {
   std::array<uint8_t, 16 * 1024> Input;  // For incoming MP3 data.  Also needs to be big enough to accomodate the ID3v2 header.
   std::array<uint8_t, 100> TOC; // Xing Table of Contents
   std::array<uint8_t, MAX_FRAME_BYTES> Overflow;
   mp3dec_t mp3d;              // Decoder information
   mp3dec_frame_info_t info;   // Retains info on the most recently decoded frame.
   objFile *File;              // Source MP3 file
   // frame_bytes, frame_offset, channels, hz, layer, bitrate_kbps
   int     OverflowPos;         // Overflow read position
   int     OverflowSize;        // Number of bytes used in the overflow buffer.
   int     SamplesPerFrame;     // Last known frame size, measured in samples: 384, 576 or 1152
   int     SeekOffset;          // Offset to apply when performing seek operations.
   int64_t WriteOffset;         // Current stream offset in bytes, relative to Sound.Length.
   int     CompressedOffset;    // Next byte position for reading compressed input
   int     FramesProcessed;     // Count of frames processed by the decoder.
   int     TotalFrames;         // Total frames for the entire stream (known if CBR data, or VBR header is present).
   int64_t TotalSamples;        // Total samples for the stream, adjusted for null padding at either end.
   int     PaddingStart;        // Total samples at the start of the decoded stream that can be skipped.
   int     PaddingEnd;          // Total samples at the end of the decoded stream that can be ignored.
   int64_t StreamSize;          // Compressed stream length, if defined by VBR header.
   bool    EndOfFile;           // True if all incoming data has been read.
   bool    VBR;                 // True if VBR detected, otherwise CBR
   bool    XingChecked;         // True if Xing header has been checked
   bool    TOCLoaded;           // True if the Table of Contents has been defined.

   void reset() { // Reset the decoder.  Necessary for seeking.
      CompressedOffset = 0;
      WriteOffset      = 0;
      FramesProcessed  = 0;
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

static int find_frame(objMP3 *, const uint8_t *, int);

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

static ERR calc_length(objMP3 *, int, int64_t *);

//********************************************************************************************************************

static uint32_t read_be32(const uint8_t *Buffer)
{
   return (uint32_t(Buffer[0]) << 24) | (uint32_t(Buffer[1]) << 16) |
      (uint32_t(Buffer[2]) << 8) | uint32_t(Buffer[3]);
}

//********************************************************************************************************************
// The ID3v1 tag can be located at the end of the MP3 file.
// There may also be an 'Enhanced TAG' just prior to the ID3v1 header - this code does not support it yet.

static bool parse_id3v1(objMP3 *Self)
{
   kt::Log log(__FUNCTION__);

   auto prv = (prvMP3 *)Self->DerivedPtr;
   bool processed = false;

   id3tag id3;
   prv->File->seekEnd(sizeof(id3));

   int result;
   if ((!prv->File->read(&id3, sizeof(id3), &result)) and (result IS sizeof(id3))) {
      if (!strncmp("TAG", (STRING)&id3, 3)) {
         char buffer[sizeof(id3)];

         log.detail("ID3v1 tag found.");

         std::string title(id3.title, sizeof(id3.title));
         kt::ltrim(title, " ");
         acSetKey(Self, "Title", title.c_str());

         std::string artist(id3.artist, sizeof(id3.artist));
         kt::ltrim(artist, " ");
         acSetKey(Self, "Author", artist.c_str());

         std::string album(id3.album, sizeof(id3.album));
         kt::ltrim(album, " ");
         acSetKey(Self, "Album", album.c_str());

         std::string comment(id3.comment, sizeof(id3.comment));
         kt::ltrim(comment, " ");
         acSetKey(Self, "Description", comment.c_str());

         if (id3.genre < genre_table.size()) {
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

static bool check_xing(objMP3 *Self, const uint8_t *Frame)
{
   kt::Log log;

   auto prv = (prvMP3 *)Self->DerivedPtr;

   if (prv->XingChecked) return false;
   else prv->XingChecked = true;

   if ((prv->info.frame_bytes <= HDR_SIZE) or (HDR_GET_LAYER(Frame) != 1)) return false;

   const uint8_t *frame_end = Frame + prv->info.frame_bytes;
   bs_t bs[1];
   L3_gr_info_t gr_info[4];
   bs_init(bs, Frame + HDR_SIZE, prv->info.frame_bytes - HDR_SIZE);
   if (HDR_IS_CRC(Frame)) get_bits(bs, 16);
   if ((L3_read_side_info(bs, gr_info, Frame) < 0) or (bs->pos > bs->limit)) return false;

   const uint8_t *tag = Frame + HDR_SIZE + bs->pos / 8;
   if ((frame_end - tag < 8) or (strncmp("Xing", (CSTRING)tag, 4) and strncmp("Info", (CSTRING)tag, 4))) {
      return false;
   }

   const uint32_t flags = read_be32(tag + 4);
   if (!(flags & XING_FRAMES)) return false;
   tag += 8;

   if (frame_end - tag < 4) return false;
   const uint32_t total_frames = read_be32(tag);
   if ((!total_frames) or (total_frames > uint32_t(std::numeric_limits<int>::max()))) return false;
   tag += 4;

   int64_t stream_size = 0;
   if (flags & XING_STREAM_SIZE) {
      // This value is used for TOC seek calculations.
      if (frame_end - tag < 4) return false;
      stream_size = read_be32(tag);
      tag += 4;
   }

   std::array<uint8_t, 100> toc;
   bool toc_loaded = false;
   if (flags & XING_TOC) {
      if (frame_end - tag < std::ssize(toc)) return false;
      copymem(tag, toc.data(), toc.size());
      tag += 100;
      toc_loaded = true;
   }

   uint32_t quality = 0;
   bool quality_defined = false;
   if (flags & XING_SCALE) {
      if (frame_end - tag < 4) return false;
      quality = read_be32(tag);
      quality_defined = true;
      tag += 4;
   }

   int delay = 0; // Typically the first 528 samples are padding set to zero and can be skipped.
   int padding = 0; // Padding tells you the number of samples at the end of the file that are empty.

   if ((tag < frame_end) and (*tag) and (frame_end - tag >= 24)) {
      // Optional extension, LAME, Lavc, etc. Should be the same structure.
      tag += 21;
      delay   = ((tag[0] << 4) | (tag[1] >> 4)) + (528 + 1);
      padding = (((tag[1] & 0xF) << 8) | tag[2]) - (528 + 1);
   }

   prv->TotalFrames  = int(total_frames);
   prv->StreamSize   = stream_size;
   prv->TOCLoaded    = toc_loaded;
   prv->PaddingEnd   = padding;
   prv->PaddingStart = delay;
   if (toc_loaded) prv->TOC = toc;
   if (quality_defined) acSetKey(Self, "Quality", std::to_string(quality).c_str());

   // Calculate the total no of samples for the entire stream, adjusted for padding at both the start and end.

   int64_t detected_samples = prv->info.samples * (int64_t)prv->TotalFrames;
   if (detected_samples >= prv->PaddingStart) detected_samples -= prv->PaddingStart;
   if (detected_samples >= prv->PaddingEnd) detected_samples -= prv->PaddingEnd;

   prv->TotalSamples = detected_samples;

   const double seconds_len = double(detected_samples) / double(prv->info.hz);

   // Compute byte length with adjustment for padding at the end, but not the start.

   int64_t len = int64_t(prv->TotalFrames) * prv->info.samples * prv->info.channels * sizeof(int16_t);
   len -= int64_t(prv->PaddingEnd * prv->info.channels) * sizeof(int16_t);
   Self->setLength(len);

   log.msg("Info header detected.  Total Frames: %d, Samples: %" PRId64 ", Track Time: %.2fs, Byte Length: %" PRId64
      ", Padding: %d/%d", prv->TotalFrames, prv->TotalSamples, seconds_len, int64_t(len), prv->PaddingStart,
      prv->PaddingEnd);

   return true;
}

//********************************************************************************************************************
// Playback is managed by Sound.acActivate()

static ERR init_error(objMP3 *Self, ERR Error)
{
   if (auto prv = (prvMP3 *)Self->DerivedPtr) {
      if (prv->File) FreeResource(prv->File);
      prv->~prvMP3();
      free(Self->DerivedPtr);
      Self->DerivedPtr = nullptr;
   }

   return Error;
}

//********************************************************************************************************************

static ERR MP3_Init(objMP3 *Self)
{
   kt::Log log;

   std::string_view location;
   Self->getPath(location);

   if (location.empty() or ((Self->Flags & SDF::NEW) != SDF::NIL)) {
      // If no location has been specified, assume that the sound is being
      // created from scratch (e.g. to record an mp3 file to disk).

      return ERR::Okay;
   }

   prvMP3 *prv;
   if ((Self->DerivedPtr = malloc(sizeof(prvMP3)))) {
      prv = (prvMP3 *)Self->DerivedPtr;
      new (prv) prvMP3{};
   }
   else return ERR::AllocMemory;

   mp3dec_init(&prv->mp3d);
   prv->reset();

   // Fill the buffer with audio information and parse any MP3 header that is present.  This will also prove whether
   // or not this is really an mp3 file.

   if (!prv->File) {
      if (!(prv->File = objFile::create::local(fl::Path(location), fl::Flags(FL::READ|FL::APPROXIMATE)))) {
         return init_error(Self, log.warning(ERR::CreateObject));
      }
   }
   else if (auto error = prv->File->seekStart(0); error != ERR::Okay) return init_error(Self, error);

   // Read the ID3v1 file header, if there is one.

   int reduce = 0;
   if (parse_id3v1(Self)) reduce += sizeof(struct id3tag);

   // Process ID3V2 and Xing VBR headers if present.

   int result = 0;
   if (auto error = prv->File->read(prv->Input.data(), prv->Input.size(), &result); error != ERR::Okay) {
      return init_error(Self, error);
   }

   if (auto id3size = (result >= 10) ? detect_id3v2((const char *)prv->Input.data()) : 0) {
      log.msg("Detected ID3v2 header of %d bytes.", id3size);
      prv->SeekOffset = id3size;
      if (auto error = prv->File->seekStart(prv->SeekOffset); error != ERR::Okay) {
         return init_error(Self, error);
      }
      if (auto error = prv->File->read(prv->Input.data(), prv->Input.size(), &result); error != ERR::Okay) {
         return init_error(Self, error);
      }
   }
   else {
      log.msg("No ID3v2 header found.");
      prv->SeekOffset = 0;
   }

   const int frame_offset = find_frame(Self, prv->Input.data(), result);
   if (frame_offset IS -1) return init_error(Self, log.warning(ERR::NoSupport));

   prv->SeekOffset += frame_offset;
   if (check_xing(Self, prv->Input.data() + frame_offset)) {
      prv->SeekOffset += prv->info.frame_bytes;
   }
   else log.detail("No VBR header found.");

   if (auto error = prv->File->seekStart(prv->SeekOffset); error != ERR::Okay) return init_error(Self, error);

   if (prv->info.channels IS 2) Self->Flags |= SDF::STEREO;
   if (Self->Stream != STREAM::NEVER) Self->Flags |= SDF::STREAM;

   Self->BytesPerSecond = int(prv->info.hz * prv->info.channels * sizeof(int16_t));
   Self->BitsPerSample  = 16;
   Self->Frequency      = prv->info.hz;
   Self->Playback       = Self->Frequency;

   if (Self->Length <= 0) {
      int64_t length;
      if (auto error = calc_length(Self, reduce, &length); error != ERR::Okay) return init_error(Self, error);
      Self->Length = length;
      if (auto error = prv->File->seekStart(prv->SeekOffset); error != ERR::Okay) return init_error(Self, error);
   }

   log.msg("File is MP3.  Stereo: %c, BytesPerSecond: %d, Freq: %d, Byte Length: %d",
      ((Self->Flags & SDF::STEREO) != SDF::NIL) ? 'Y' : 'N', Self->BytesPerSecond, Self->Frequency, Self->Length);

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR MP3_Read(objMP3 *Self, struct acRead *Args)
{
   kt::Log log;

   if ((!Args) or (!Args->Buffer)) return log.warning(ERR::NullArgs);

   Args->Result = 0;
   if (Args->Length <= 0) return ERR::Okay;

   auto prv = (prvMP3 *)Self->DerivedPtr;

   // Keep decoding until we exhaust space in the output buffer.  Setting the EOF to true indicates that everything
   // has been output, or an error has occurred.

   int pos = 0;
   auto write_offset  = prv->WriteOffset;
   bool no_more_input = false;
   ERR read_error = ERR::Okay;
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
            read_error = error;
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
   return read_error;
}

//********************************************************************************************************************
// Accuracy when seeking within an MP3 file is not guaranteed.  This means that offsets can be a little too far
// forward or backward relative to the known length.

static ERR MP3_Seek(objMP3 *Self, struct acSeek *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);
   if (!Self->initialised()) return log.warning(ERR::NotInitialised);

   auto prv = (prvMP3 *)Self->DerivedPtr;

   if (!std::isfinite(Args->Offset)) return log.warning(ERR::OutOfRange);

   int64_t offset;
   if (Args->Position IS SEEK::START) {
      if (Args->Offset <= 0) offset = 0;
      else if (Args->Offset >= double(Self->Length)) offset = Self->Length;
      else offset = int64_t(Args->Offset);
   }
   else if (Args->Position IS SEEK::END) {
      if (Args->Offset <= 0) offset = Self->Length;
      else if (Args->Offset >= double(Self->Length)) offset = 0;
      else offset = Self->Length - int64_t(Args->Offset);
   }
   else if (Args->Position IS SEEK::CURRENT) {
      if (Args->Offset <= -double(Self->Position)) offset = 0;
      else if (Args->Offset >= double(Self->Length - Self->Position)) offset = Self->Length;
      else offset = Self->Position + int64_t(Args->Offset);
   }
   else if (Args->Position IS SEEK::RELATIVE) {
      if (Args->Offset <= 0) offset = 0;
      else if (Args->Offset >= 1.0) offset = Self->Length;
      else offset = int64_t(double(Self->Length) * Args->Offset);
   }
   else return log.warning(ERR::Args);

   if (offset IS Self->Position) return ERR::Okay;

   if ((Self->Flags & SDF::STREAM) != SDF::NIL) {
      int active;
      if (auto error = Self->getActive(active); error != ERR::Okay) return log.warning(error);

      int frame = 0;
      int64_t file_offset = prv->SeekOffset;

      if (offset <= 0) {
         log.traceBranch("Resetting play position to zero.");
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

            file_offset = int64_t(prv->SeekOffset) + ((int64_t(prv->TOC[idx]) * prv->StreamSize) / 256);
            frame = int((int64_t(prv->TOC[idx]) * prv->TotalFrames) / 256);

            log.detail("Seeking to byte offset %" PRId64 ", frame %d of %d", file_offset, frame, prv->TotalFrames);
         }
         else {
            // Seeking without a TOC has two approaches: 1) Scan from frame 1 until you reach the frame
            // you're looking for; 2) Use the average frame size to make a jump and rely on frame syncing to
            // find the nearest viable frame.  The accuracy of this is largely dependent on the calc_length()
            // computations.

            if (!prv->StreamSize) {
               int64_t size = 0;
               if (auto error = prv->File->getSize(size); error != ERR::Okay) return log.warning(error);
               if (size < prv->SeekOffset) return log.warning(ERR::InvalidData);
               prv->StreamSize = size - prv->SeekOffset;
            }

            frame = int(prv->TotalFrames * pct);
            int64_t stream_offset = int64_t(double(prv->StreamSize) * pct);
            if (frame < 0) frame = 0;
            if (stream_offset < 0) stream_offset = 0;
            file_offset = prv->SeekOffset + stream_offset;

            log.detail("Seeking to byte offset %" PRId64 ", frame %d of %d", file_offset, frame, prv->TotalFrames);
         }
      }

      if (auto error = prv->File->seekStart(file_offset); error != ERR::Okay) return log.warning(error);

      prv->reset();
      mp3dec_init(&prv->mp3d);
      prv->WriteOffset     = int64_t(frame) * prv->SamplesPerFrame * prv->info.channels * sizeof(int16_t);
      prv->FramesProcessed = frame;
      Self->Position       = prv->WriteOffset;

      if (active) {
         log.branch("Resetting state of active sample, seek to byte %" PF64, (long long)prv->WriteOffset);
         auto error = Self->deactivate();
         Self->Position = prv->WriteOffset;
         if (error != ERR::Okay) return log.warning(error);
         if (error = Self->activate(); error != ERR::Okay) return log.warning(error);
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

static ERR calc_length(objMP3 *Self, int ReduceEnd, int64_t *Length)
{
   kt::Log log(__FUNCTION__);
   int buffer_size = 0;
   std::vector<uint16_t> fsizes;  // List of all compressed frame sizes

   log.branch();

   if (!Length) return ERR::NullArgs;
   *Length = 0;

   auto prv = (prvMP3 *)Self->DerivedPtr;

   int frame_start     = 0;
   int current_bitrate = 0;
   int64_t decoded_size = 0;

   prv->VBR = false;

   int64_t filesize = 0;
   if (auto error = prv->File->getSize(filesize); error != ERR::Okay) return error;

   {
      // Load MP3 data from the file

      std::vector<uint8_t> buffer(SIZE_BUFFER);
      if (auto error = prv->File->seekStart(prv->SeekOffset); error != ERR::Okay) return error;
      if (auto error = prv->File->read(buffer.data(), SIZE_CBR_BUFFER, &buffer_size); error != ERR::Okay) return error;

      // Find the start of the frame data

      frame_start = find_frame(Self, buffer.data(), buffer_size);

      if (frame_start IS -1) {
         log.warning("Failed to find the first mp3 frame.");
         return ERR::NoSupport;
      }

      const uint8_t *first_header = buffer.data() + frame_start;
      int free_format_bytes = HDR_IS_FREE_FORMAT(first_header) ?
         prv->info.frame_bytes - hdr_padding(first_header) : 0;
      int pos = frame_start;
      while (pos <= buffer_size - HDR_SIZE) {
         const uint8_t *frame_header = buffer.data() + pos;

         if (hdr_valid(frame_header)) {
            const int frame_size = hdr_frame_bytes(frame_header, free_format_bytes) + hdr_padding(frame_header);
            if (frame_size <= 0) {
               pos++;
               continue;
            }

            if (pos + frame_size > buffer_size) {
               if ((prv->VBR) and (buffer_size IS SIZE_CBR_BUFFER)) {
                  int result = 0;
                  if (auto error = prv->File->read(buffer.data() + buffer_size, SIZE_BUFFER - buffer_size, &result);
                      error != ERR::Okay) return error;
                  buffer_size += result;
                  if (pos + frame_size <= buffer_size) continue;
               }
               break;
            }

            const int bitrate = int(hdr_bitrate_kbps(frame_header) * 1000);
            if (!current_bitrate) current_bitrate = bitrate;
            else if ((bitrate) and (current_bitrate != bitrate)) prv->VBR = true;

            const int channels = HDR_IS_MONO(frame_header) ? 1 : 2;
            const int frame_samples = int(hdr_frame_samples(frame_header));
            decoded_size += int64_t(frame_samples) * channels * sizeof(int16_t);
            fsizes.push_back(frame_size);
            pos += frame_size;
         }
         else {
            int index = find_frame(Self, buffer.data() + pos, buffer_size - pos);
            if (index <= 0) {
               log.msg("Failed to find the next frame at position %d.", pos);
               break;
            }

            pos += index;
            const uint8_t *next_header = buffer.data() + pos;
            free_format_bytes = HDR_IS_FREE_FORMAT(next_header) ?
               prv->info.frame_bytes - hdr_padding(next_header) : 0;
         }

         // Check if we need to load more data into our buffer for VBR analysis

         if (pos >= buffer_size - 8) {
            if ((prv->VBR) and (buffer_size IS SIZE_CBR_BUFFER)) {
               // Read more file data so that we can calculate the vbr more accurately
               int result = 0;
               if (auto error = prv->File->read(buffer.data() + buffer_size, SIZE_BUFFER - buffer_size, &result);
                   error != ERR::Okay) return error;
               buffer_size += result;
            }
            else break; // File is CBR, no need to scan more data
         }
      }
   }

   if (fsizes.empty()) return ERR::NoSupport;

   // Calculate average frame length using interquartile mean

   sort(fsizes.begin(), fsizes.end(), std::greater<uint16_t>());
   int first = fsizes.size() / 4;
   int last  = int(fsizes.size() * 0.75);
   if (last <= first) {
      first = 0;
      last = fsizes.size();
   }

   double avg_frame_len = 0;
   for (int i=first; i < last; i++) avg_frame_len += fsizes[i];
   avg_frame_len /= (last - first);

   log.detail("File Size: %" PRId64 ", %d frames, Average frame length: %.2f bytes, VBR: %c", filesize,
      int(fsizes.size()), avg_frame_len, prv->VBR ? 'Y' : 'N');

   const int64_t stream_size = filesize - prv->SeekOffset - frame_start - ReduceEnd;
   if (stream_size <= 0) return ERR::NoSupport;

   if (stream_size > buffer_size) {
      const double estimated_frames = double(stream_size) / avg_frame_len;
      if (estimated_frames > double(std::numeric_limits<int>::max())) return ERR::OutOfRange;
      prv->TotalFrames = int(estimated_frames);
      const double decoded_frame_len = double(decoded_size) / double(fsizes.size());
      *Length = int64_t(estimated_frames * decoded_frame_len);
   }
   else { // The entire file was loaded into the buffer, so we know the exact length.
      if (fsizes.size() > std::size_t(std::numeric_limits<int>::max())) return ERR::OutOfRange;
      prv->TotalFrames = int(fsizes.size());
      *Length = decoded_size;
   }

   return (*Length > 0) ? ERR::Okay : ERR::NoSupport;
}

//********************************************************************************************************************

static int find_frame(objMP3 *Self, const uint8_t *Buffer, int BufferSize)
{
   kt::Log log(__FUNCTION__);
   auto prv = (prvMP3 *)Self->DerivedPtr;

   log.traceBranch("Buffer Size: %d", BufferSize);

   if (BufferSize <= HDR_SIZE) return -1;

   int free_format_bytes = 0;
   int frame_bytes = 0;
   const int pos = mp3d_find_frame(Buffer, BufferSize, &free_format_bytes, &frame_bytes);
   if ((pos >= BufferSize) or (frame_bytes <= 0)) {
      log.detail("Failed to find a valid frame.");
      return -1;
   }

   const uint8_t *frame = Buffer + pos;
   prv->info.frame_bytes  = frame_bytes;
   prv->info.frame_offset = pos;
   prv->info.channels     = HDR_IS_MONO(frame) ? 1 : 2;
   prv->info.hz           = int(hdr_sample_rate_hz(frame));
   prv->info.layer        = 4 - HDR_GET_LAYER(frame);
   prv->info.bitrate_kbps = int(hdr_bitrate_kbps(frame));
   prv->info.samples      = int(hdr_frame_samples(frame));
   prv->SamplesPerFrame   = prv->info.samples;

   log.detail("Frame found at %d, size %d, channels %d, %d samples, %dhz.", pos, prv->info.frame_bytes,
      prv->info.channels, prv->info.samples, prv->info.hz);

   return pos;
}

//********************************************************************************************************************

#include "mp3_def.cpp"

//********************************************************************************************************************

objMP3::~objMP3() {
   prvMP3 *prv;
   if (!(prv = (prvMP3 *)DerivedPtr)) return;
   if (prv->File) FreeResource(prv->File);
}

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   CoreBase = argCoreBase;

   if (objModule::load("audio", &modAudio, &AudioBase) != ERR::Okay) return ERR::InitModule;

   clMP3 = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::SOUND),
      fl::ClassID(CLASSID::MP3),
      fl::ClassVersion(1.0),
      fl::FileExtension("mp3"),
      fl::FileDescription("MP3 Audio Stream"),
      fl::Icon("filetypes/audio"),
      fl::Name("MP3"),
      fl::Size(sizeof(objMP3)),
      fl::Category(CCF::AUDIO),
      fl::Actions(clMP3Actions),
      fl::Path(MOD_PATH));

   if (clMP3) return ERR::Okay;

   FreeResource(modAudio);
   modAudio = nullptr;
   return ERR::AddClass;
}

static ERR MODExpunge(void)
{
   if (clMP3) { FreeResource(clMP3); clMP3 = nullptr; }
   if (modAudio) { FreeResource(modAudio); modAudio = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, nullptr, MODExpunge, nullptr, nullptr, nullptr)
extern "C" struct ModHeader * register_mp3_module() { return &ModHeader; }

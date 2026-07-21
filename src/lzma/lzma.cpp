/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

This module incorporates the LZMA SDK reference decoder by Igor Pavlov, which is released into the public domain.  See
lzmasdk/README.md for provenance and licensing details.

**********************************************************************************************************************

-CLASS-
LZMAStream: Decompresses raw LZMA1 data streams.

The LZMAStream class extends @CompressedStream with support for decoding raw LZMA1 streams.  It is provided by the
separate `LZMA` module so that the Core remains free of an LZMA dependency; only builds that require LZMA pull the
module in.

LZMAStream is decode-only and is instantiated directly rather than being selected through a
@CompressedStream.Format option on the base class.  To decompress a stream, set the @CompressedStream.Input field
with a source object that supports the Read() action (such as a @File) and set the required #Size
field to the exact number of decompressed bytes expected.  Repeatedly reading from the LZMAStream then yields the
decompressed data:

<pre>
lz = obj.new('lzmastream', { input=file, size=body_size })
err, len = lz.acRead(buffer)
</pre>

The class decodes a raw LZMA1 stream consisting of a 5-byte properties header followed by the compressed data.  It
does not parse the `.lzma`/`.alone` 13-byte header or the `.xz` container; callers are responsible for any outer
framing.  Because raw LZMA1 streams may lack a reliable end marker, #Size is used to bound the output:
decoding completes when exactly that many bytes have been produced, and an end marker or exhausted input encountered
before that point is reported as a decompression error.

The inherited @CompressedStream.Format field has no meaning for this class and is ignored.

-END-

*********************************************************************************************************************/

#include <kotuku/main.h>
#include <kotuku/modules/core.h>
#include <kotuku/modules/lzma.h>
#include <kotuku/modules/module.h>

#include <vector>

extern "C" {
#include "LzmaDec.h"
}

#include "lzma_stream.h"

using namespace kt;

JUMPTABLE_CORE

static OBJECTPTR clLZMAStream = nullptr;
static constexpr int INPUT_CHUNK = 16 * 1024; // Size of the input chunk read from the source object on each refill.
static constexpr int PENDING_CHUNK = 32 * 1024; // Size of the internal decode buffer

//********************************************************************************************************************

class extLZMAStream : public objLZMAStream {
   public:

   int64_t Size = 0;     // Required byte count expected from the raw LZMA stream (virtual field).

   LZMADecoder Decoder;
   std::vector<uint8_t> InputBuf;    // Holds compressed input read from the Input object.
   std::vector<uint8_t> PendingBuf;  // Holds decoded output awaiting delivery to the caller.
   int InputOffset   = 0;            // Read position of unconsumed input within InputBuf.
   int InputLength   = 0;            // Count of unconsumed input bytes within InputBuf.
   int PendingOffset = 0;            // Read position of pending output within PendingBuf.
   int PendingLength = 0;            // Count of pending output bytes within PendingBuf.

   extLZMAStream(objMetaClass *ClassPtr, OBJECTID ObjectID) : objLZMAStream(ClassPtr, ObjectID) { }

   void reset() {
      TotalOutput = 0;
      Decoder.reset();
      InputOffset   = 0;
      InputLength   = 0;
      PendingOffset = 0;
      PendingLength = 0;
   }
};

//********************************************************************************************************************

static ERR LZMASTREAM_Init(extLZMAStream *Self)
{
   kt::Log log;

   if (!Self->Input) return log.warning(ERR::FieldNotSet);

   if (Self->Output) { // Compression is not supported, so an Output target is invalid.
      log.warning("LZMAStream is decode-only; the Output field cannot be set.");
      return ERR::InvalidState;
   }

   if (Self->Size <= 0) {
      log.warning("A positive Size value is required for raw LZMA decoding.");
      return ERR::FieldNotSet;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Read: Decompress data from the input stream and write it to the supplied buffer.

The Read() action decodes the raw LZMA1 stream supplied via @CompressedStream.Input, writing up to `Length`
decompressed bytes into the caller's buffer.  Decoding is bounded by #Size; once that many bytes have
been produced, subsequent reads return zero bytes.  Truncated input, a premature end marker or output that would
exceed #Size are reported as `ERR::Decompression`.
-END-
*********************************************************************************************************************/

static ERR LZMASTREAM_Read(extLZMAStream *Self, struct acRead *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Buffer.data())) return log.warning(ERR::NullArgs);
   if (!Self->initialised()) return log.warning(ERR::NotInitialised);

   Args->Result = 0;
   if (Args->Buffer.empty()) return ERR::Okay;
   if (Args->Buffer.size() > size_t(INT_MAX)) return log.warning(ERR::OutOfRange);
   const int length = int(Args->Buffer.size());

   // Nothing left to do once the full output has been both decoded and delivered.
   if ((Self->TotalOutput >= Self->Size) and (Self->PendingLength <= 0)) return ERR::Okay;

   // The first read consumes the 5-byte LZMA properties header and allocates the decoder.

   if (!Self->Decoder.active()) {
      Byte props[LZMA_PROPS_SIZE];
      int result;
      if (acRead(Self->Input, std::span<int8_t>((int8_t *)props, sizeof(props)), &result) != ERR::Okay) {
         return log.warning(ERR::Read);
      }
      if (result != LZMA_PROPS_SIZE) {
         log.warning("Failed to read the %d-byte LZMA properties header (got %d).", LZMA_PROPS_SIZE, result);
         return ERR::Decompression;
      }

      if (Self->Decoder.allocate(props) != SZ_OK) return log.warning(ERR::Decompression);

      Self->InputBuf.resize(INPUT_CHUNK);
      Self->PendingBuf.resize(PENDING_CHUNK);
   }

   auto out      = (uint8_t *)Args->Buffer.data();
   int  copied   = 0;
   bool no_input = false;

   while ((copied < length) and ((Self->PendingLength > 0) or (Self->TotalOutput < Self->Size))) {
      // Deliver any previously decoded bytes that are awaiting collection.

      if (Self->PendingLength > 0) {
         int to_copy = Self->PendingLength;
         if (copied + to_copy > length) to_copy = length - copied;
         copymem(Self->PendingBuf.data() + Self->PendingOffset, out + copied, to_copy);
         Self->PendingOffset += to_copy;
         Self->PendingLength -= to_copy;
         copied += to_copy;
         continue;
      }

      // Refill the input buffer when no unconsumed compressed bytes remain.

      if (Self->InputLength <= 0) {
         int result;
         if (acRead(Self->Input, std::span<int8_t>((int8_t *)Self->InputBuf.data(), INPUT_CHUNK), &result) !=
             ERR::Okay) {
            return log.warning(ERR::Read);
         }

         if (result <= 0) { no_input = true; }
         else {
            Self->InputOffset = 0;
            Self->InputLength = result;
         }
      }

      // Decode the next segment into the pending buffer, bounded by the bytes still owed to the caller.

      int64_t remaining = Self->Size - Self->TotalOutput;
      SizeT dest_len = (remaining < PENDING_CHUNK) ? SizeT(remaining) : SizeT(PENDING_CHUNK);
      SizeT src_len  = SizeT(Self->InputLength);
      ELzmaFinishMode finish = (int64_t(dest_len) IS remaining) ? LZMA_FINISH_END : LZMA_FINISH_ANY;
      ELzmaStatus status;

      SRes res = LzmaDec_DecodeToBuf(&Self->Decoder.dec, Self->PendingBuf.data(), &dest_len,
         Self->InputBuf.data() + Self->InputOffset, &src_len, finish, &status);

      if (res != SZ_OK) {
         log.warning("LZMA decode error %d.", int(res));
         return ERR::Decompression;
      }

      Self->InputOffset += int(src_len);
      Self->InputLength -= int(src_len);
      Self->PendingOffset = 0;
      Self->PendingLength = int(dest_len);
      Self->TotalOutput  += int64_t(dest_len);

      // An end marker before Size is reached indicates a truncated or corrupt stream.

      if ((status IS LZMA_STATUS_FINISHED_WITH_MARK) and (Self->TotalOutput < Self->Size)) {
         log.warning("LZMA stream ended after %" PF64 " of %" PF64 " expected bytes.",
            (long long)Self->TotalOutput, (long long)Self->Size);
         return ERR::Decompression;
      }

      // Reaching the caller-declared output size is only valid if the SDK also considers the stream finished or
      // plausibly finished without an end marker.  LZMA_STATUS_NOT_FINISHED here means Size was too
      // small.

      if ((Self->TotalOutput IS Self->Size) and (status != LZMA_STATUS_FINISHED_WITH_MARK) and
          (status != LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK)) {
         log.warning("LZMA stream is not finished after declared output size %" PF64 ".",
            (long long)Self->Size);
         return ERR::Decompression;
      }

      // Guard against a stall: if the decoder made no progress and there is no more input, the stream is truncated.

      if ((dest_len IS 0) and (src_len IS 0)) {
         if ((no_input) or (status IS LZMA_STATUS_NEEDS_MORE_INPUT)) {
            log.warning("Truncated LZMA stream: %" PF64 " of %" PF64 " bytes decoded.",
               (long long)Self->TotalOutput, (long long)Self->Size);
            return ERR::Decompression;
         }
      }
   }

   // Reset the decoder once the full uncompressed size has been delivered.

   if ((Self->TotalOutput >= Self->Size) and (Self->PendingLength <= 0)) {
      Self->Decoder.reset();
   }

   Args->Result = copied;
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Reset: Reset the state of the stream.

Resetting an LZMAStream returns it to the same state as when first initialised.  This does not affect the state of
the object referenced via @CompressedStream.Input, so the client may need to reset or re-seek that object separately.
-END-
*********************************************************************************************************************/

static ERR LZMASTREAM_Reset(extLZMAStream *Self)
{
   Self->reset();
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Seek: For use in decompressing streams only.  Seeks to a position within the stream.
-END-
*********************************************************************************************************************/

static ERR LZMASTREAM_Seek(extLZMAStream *Self, struct acSeek *Args)
{
   return kt::Log().warning(ERR::NoSupport);
}

/*********************************************************************************************************************
-ACTION-
Write: Not supported.  LZMAStream is decode-only.
-END-
*********************************************************************************************************************/

static ERR LZMASTREAM_Write(extLZMAStream *Self, struct acWrite *Args)
{
   return kt::Log().warning(ERR::NoSupport);
}

/*********************************************************************************************************************
-FIELD-
Size: The exact number of bytes expected from the decompressed stream.

This field is required prior to initialisation.  Raw LZMA1 streams may not carry a reliable end marker, so the value
is used to bound decoding: the stream completes when exactly this many bytes have been produced, and an end marker or
exhausted input encountered beforehand is reported as a decompression error.
-END-
*********************************************************************************************************************/

static ERR LZMASTREAM_GET_Size(extLZMAStream *Self, int64_t *Value)
{
   *Value = Self->Size;
   return ERR::Okay;
}

static ERR LZMASTREAM_SET_Size(extLZMAStream *Self, int64_t Value)
{
   Self->Size = Value;
   return ERR::Okay;
}

//********************************************************************************************************************

#include "lzma_def.cpp"

static const FieldArray clFields[] = {
   { "Size", FDF_VIRTUAL|FDF_INT64|FDF_RI, LZMASTREAM_GET_Size, LZMASTREAM_SET_Size },
   END_FIELD
};

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   CoreBase = argCoreBase;

   clLZMAStream = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::COMPRESSEDSTREAM),
      fl::ClassID(CLASSID::LZMASTREAM),
      fl::ClassVersion(1.0),
      fl::Name("LZMAStream"),
      fl::Category(CCF::DATA),
      fl::Actions(clLZMAStreamActions),
      fl::Fields(clFields),
      fl::Size(sizeof(extLZMAStream)),
      fl::Path(MOD_PATH));

   return clLZMAStream ? ERR::Okay : ERR::AddClass;
}

static ERR MODExpunge(void)
{
   if (clLZMAStream) { FreeResource(clLZMAStream); clLZMAStream = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, nullptr, MODExpunge, nullptr, nullptr, nullptr)
extern "C" struct ModHeader * register_lzma_module() { return &ModHeader; }

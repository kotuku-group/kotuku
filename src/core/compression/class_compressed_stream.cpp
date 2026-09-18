/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
CompressedStream: Acts as a proxy for decompressing and compressing data streams between objects.

Use the CompressedStream class to compress and decompress data on the fly without the need for a temporary storage
area.  The default compression algorithm is DEFLATE with gzip header data.  It is compatible with common command-line
tools such as gzip.

To decompress data, set the #Input field with a source object that supports the Read() action, such as a @File.
Repeatedly reading from the CompressedStream will automatically handle the decompression process.  The #Finished
field becomes true only after the complete compressed stream, including its checksum, has been verified.  The #Size
field remains unknown until that point.

To compress data, set the #Output field with a source object that supports the Write() action, such as a @File.
Repeatedly writing to the CompressedStream with raw data will automatically handle the compression process for you.
Once all of the data has been written, call the #Write() action with a null empty `Buffer` to finalise the stream.  In
Tiri, call `acWrite(nil)`.  Finalisation is mandatory and may report destination or compression errors.  No further
writes are accepted after finalisation or a terminal failure until #Reset() is called.

-END-

*********************************************************************************************************************/

class extCompressedStream : public objCompressedStream {
   public:

   // Note: As PublicSize is defined, these fields are specific to CompressedStream and will otherwise be
   // overwritten for derived classes.
   std::vector<uint8_t> InputBuffer;
   std::vector<uint8_t> OutputBuffer;
   ZStream Stream;
   gz_header Header;
   size_t InputOffset = 0;
   size_t InputLength = 0;
   ERR TerminalError = ERR::Okay;

   ~extCompressedStream();

   extCompressedStream(objMetaClass *ClassPtr, OBJECTID ObjectID) : objCompressedStream(ClassPtr, ObjectID) {
      TotalOutput = 0;
      TotalInput = 0;
      Format = CF::GZIP;
      Finished = false;
      clearmem(&Header, sizeof(Header));
   }

   void reset() {
      Finished = false;
      TotalInput = 0;
      TotalOutput = 0;
      TerminalError = ERR::Okay;
      InputOffset = 0;
      InputLength = 0;

      Stream.reset();
      clearmem(&Header, sizeof(Header));

      InputBuffer.clear();
      InputBuffer.shrink_to_fit();
      OutputBuffer.clear();
      OutputBuffer.shrink_to_fit();
   }

   ERR fail(ERR Error) {
      Stream.reset();
      TerminalError = Error;
      return Error;
   }
};

//********************************************************************************************************************

static ERR COMPRESSEDSTREAM_Init(extCompressedStream *Self)
{
   kt::Log log;

   if ((!Self->Input) and (!Self->Output)) return log.warning(ERR::FieldNotSet);

   if ((Self->Input) and (Self->Output)) {
      log.warning("A CompressedStream can operate in either read or write mode, not both.");
      return ERR::InvalidState;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Read: Decompress data from the input stream and write it to the supplied buffer.
-END-
*********************************************************************************************************************/

#define MIN_OUTPUT_SIZE ((32 * 1024) + 2048)

static ERR COMPRESSEDSTREAM_Read(extCompressedStream *Self, struct acRead *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Buffer.data())) return log.warning(ERR::NullArgs);
   if (!Self->initialised()) return log.warning(ERR::NotInitialised);

   Args->Result = 0;
   if (Self->TerminalError != ERR::Okay) return Self->TerminalError;
   if (Self->Finished) return ERR::Okay;
   if (Args->Buffer.empty()) return ERR::Okay;
   if (Args->Buffer.size() > size_t(INT_MAX)) return log.warning(ERR::OutOfRange);

   if (!Self->Stream.active()) {
      log.trace("Initialising decompression of the stream.");
      switch (Self->Format) {
         case CF::ZLIB:
            if (Self->Stream.inflate_init(MAX_WBITS) != Z_OK) return Self->fail(log.warning(ERR::Decompression));
            break;

         case CF::DEFLATE:
            if (Self->Stream.inflate_init(-MAX_WBITS) != Z_OK) return Self->fail(log.warning(ERR::Decompression));
            break;

         case CF::GZIP:
         default:
            if (Self->Stream.inflate_init(15 + 32) != Z_OK) {
               return Self->fail(log.warning(ERR::Decompression));
            }
            if (inflateGetHeader(Self->Stream.get(), &Self->Header) != Z_OK) {
               return Self->fail(log.warning(ERR::InvalidData));
            }
      }
   }

   if (Self->InputBuffer.empty()) Self->InputBuffer.resize(2048);

   Self->Stream->next_out = (Bytef *)Args->Buffer.data();
   Self->Stream->avail_out = uInt(Args->Buffer.size());

   while (Self->Stream->avail_out > 0) {
      if (Self->InputOffset >= Self->InputLength) {
         int length = 0;
         auto error = acRead(Self->Input,
            std::span<int8_t>((int8_t *)Self->InputBuffer.data(), Self->InputBuffer.size()), &length);
         if (error != ERR::Okay) return Self->fail(error);
         if ((length < 0) or (size_t(length) > Self->InputBuffer.size())) {
            return Self->fail(log.warning(ERR::Read));
         }
         if (length IS 0) return Self->fail(log.warning(ERR::Decompression));

         Self->InputOffset = 0;
         Self->InputLength = size_t(length);
      }

      Self->Stream->next_in = Self->InputBuffer.data() + Self->InputOffset;
      Self->Stream->avail_in = uInt(Self->InputLength - Self->InputOffset);

      const auto previous_input = Self->Stream->total_in;
      const auto previous_output = Self->Stream->total_out;
      const int result = inflate(Self->Stream.get(), Z_NO_FLUSH);
      Self->InputOffset = Self->InputLength - Self->Stream->avail_in;
      Self->TotalInput = int64_t(Self->Stream->total_in);
      Self->TotalOutput = int64_t(Self->Stream->total_out);
      Args->Result = int(Args->Buffer.size() - Self->Stream->avail_out);

      if (result IS Z_STREAM_END) {
         Self->Finished = true;
         Self->Stream.reset();
         return ERR::Okay;
      }

      if (result != Z_OK) return Self->fail(convert_zip_error(Self->Stream.get(), result));
      if ((Self->Stream->total_in IS previous_input) and (Self->Stream->total_out IS previous_output)) {
         return Self->fail(log.warning(ERR::Decompression));
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Reset: Reset the state of the stream.

Resetting a CompressedStream returns it to the same state as that when first initialised.  Note that this does not
affect the state of the object referenced via #Input or #Output, so it may be necessary for the client to reset
referenced objects separately.

*********************************************************************************************************************/

static ERR COMPRESSEDSTREAM_Reset(extCompressedStream *Self)
{
   Self->reset();
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Seek: For use in decompressing streams only.  Seeks to a position within the stream.
-END-
*********************************************************************************************************************/

static ERR COMPRESSEDSTREAM_Seek(extCompressedStream *Self, struct acSeek *Args)
{
   kt::Log log;

   if (!Args) return ERR::NullArgs;

   if (Self->Output) { // Seeking in write mode isn't possible (violates the streaming process).
      return log.warning(ERR::NoSupport);
   }

   if (!Self->Input) return log.warning(ERR::FieldNotSet);

   double position;
   if (Args->Position IS SEEK::START) position = Args->Offset;
   else if (Args->Position IS SEEK::CURRENT) position = double(Self->TotalOutput) + Args->Offset;
   else return log.warning(ERR::Args);

   if ((not std::isfinite(position)) or (position < 0) or (position > double(INT64_MAX))) {
      return log.warning(ERR::OutOfRange);
   }
   const int64_t pos_target = int64_t(position);

   auto error = acSeek(Self->Input, 0, SEEK::START);
   if (error != ERR::Okay) return error;
   Self->reset();

   int64_t pos = pos_target;
   uint8_t buffer[1024];
   while (pos > 0) {
      auto read_size = std::min<size_t>(size_t(pos), sizeof(buffer));
      struct acRead read = { .Buffer = std::span<int8_t>((int8_t *)buffer, read_size) };
      if (auto read_error = Action(AC::Read, Self, &read); read_error != ERR::Okay) return read_error;
      if (read.Result IS 0) return log.warning(ERR::OutOfRange);
      pos -= read.Result;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Write: Compress raw data in a buffer and write it to the Output object.
-END-
*********************************************************************************************************************/

static ERR COMPRESSEDSTREAM_Write(extCompressedStream *Self, struct acWrite *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);
   if (!Self->initialised()) return log.warning(ERR::NotInitialised);
   if (Args->Buffer.size() > size_t(UINT_MAX)) return log.warning(ERR::OutOfRange);

   Args->Result = 0;
   if (Self->TerminalError != ERR::Okay) return Self->TerminalError;
   if (Self->Finished) return log.warning(ERR::Finished);

   const bool finishing = (not Args->Buffer.data()) and Args->Buffer.empty();
   if ((not Args->Buffer.data()) and (not finishing)) return log.warning(ERR::NullArgs);

   if (!Self->Stream.active()) {
      int window_bits;
      switch (Self->Format) {
         case CF::ZLIB:
            window_bits = MAX_WBITS;
            break;

         case CF::DEFLATE:
            window_bits = -MAX_WBITS;
            break;

         case CF::GZIP:
         default:
            window_bits = 15 + 16;
      }

      if (auto result = Self->Stream.deflate_init(9, window_bits); result != Z_OK) {
         log.warning("deflateInit2() failed with zlib error %d.", result);
         return Self->fail(log.warning(ERR::Compression));
      }

      Self->TotalInput = 0;
      Self->TotalOutput = 0;
   }

   if (Self->OutputBuffer.empty()) Self->OutputBuffer.resize(MIN_OUTPUT_SIZE);

   const int mode = finishing ? Z_FINISH : Z_NO_FLUSH;
   Self->Stream->next_in  = (Bytef *)Args->Buffer.data();
   Self->Stream->avail_in = uInt(Args->Buffer.size());

   // If zlib succeeds but sets avail_out to zero, this means that data was written to the output buffer, but the
   // output buffer is not large enough (so keep calling until avail_out > 0).

   int result;
   do {
      Self->Stream->next_out  = Self->OutputBuffer.data();
      Self->Stream->avail_out = MIN_OUTPUT_SIZE;

      result = deflate(Self->Stream.get(), mode);
      Self->TotalInput = int64_t(Self->Stream->total_in);
      if ((result != Z_OK) and (result != Z_STREAM_END)) {
         log.warning("deflate() failed with zlib error %d.", result);
         return Self->fail(log.warning(ERR::Compression));
      }

      const int len = MIN_OUTPUT_SIZE - Self->Stream->avail_out; // Get number of compressed bytes that were output

      if (len > 0) {
         int written = 0;
         auto error = acWrite(Self->Output,
            std::span<const int8_t>((int8_t *)Self->OutputBuffer.data(), len), &written);
         if (error != ERR::Okay) return Self->fail(error);

         Self->TotalOutput += written;
         if (written != len) return Self->fail(log.warning(ERR::Write));
         log.trace("%d bytes (total %" PF64 ") were compressed.", len, Self->TotalOutput);
      }
      else {
         // deflate() may not output anything if it needs more data to fill up a compression frame.  Return ERR::Okay
         // and wait for more data, or for the developer to end the stream.

         //log.trace("No data output on this cycle.");
         if (not finishing) break;
      }
   } while ((Self->Stream->avail_out IS 0) or (finishing and (result != Z_STREAM_END)));

   if (finishing) {
      Self->Finished = true;
      Self->Stream.reset();
   }
   else Args->Result = int(Args->Buffer.size() - Self->Stream->avail_in);

   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Format: The format of the compressed stream.  The default is GZIP.

-FIELD-
Input: An input object that will supply data for decompression.

To create a stream that decompresses data from a compressed source, set the Input field with a reference to an object
that will provide the source data.  It is most common for the source object to be a @File type, however any
class that supports the Read() action is permitted.

The source object must be in a readable state.  The Input field is mutually exclusive to the #Output field.

-FIELD-
Output: A target object that will receive data compressed by the stream.

To create a stream that compresses data to a target object, set the Output field with an object reference.  It is
most common for the target object to be a @File type, however any class that supports the Write() action is
permitted.

The target object must be in a writeable state.  The Output field is mutually exclusive to the #Input field.

-FIELD-
Size: The full uncompressed size of the input source, if known.

The Size field remains unknown while decompression is in progress.  It reports the full decoded byte count only after
the compressed stream and its checksum have been read successfully and #Finished is true.

If the size is unknown, a value of `-1` is returned.

*********************************************************************************************************************/

static ERR COMPRESSEDSTREAM_GET_Size(extCompressedStream *Self, int64_t *Value)
{
   *Value = -1;
   if (Self->Input) {
      if (Self->Finished) *Value = Self->TotalOutput;
      return ERR::Okay;
   }
   else return ERR::InvalidState;
}

/*********************************************************************************************************************
-FIELD-
TotalOutput: A live counter of total bytes that have been output by the stream.
-END-

-FIELD-
TotalInput: A live counter of total bytes consumed by the compression engine.

During decompression this count excludes any trailing bytes that the Input object prefetched into the stream's input
buffer.  The final value is retained after completion or failure and is reset to zero by #Reset().
-END-

-FIELD-
Finished: True when compression has been finalised or decompression has verified the complete input stream.

After decompression finishes, further reads remain at end-of-file.  After compression finishes, further writes return
an error.  Call #Reset() before reusing the CompressedStream.
-END-
*********************************************************************************************************************/

extCompressedStream::~extCompressedStream()
{
   this->reset();
}

//********************************************************************************************************************

#include "class_compressed_stream_def.c"

static const FieldArray clStreamFields[] = {
   { "TotalOutput", FDF_INT64|FDF_R },
   { "TotalInput",  FDF_INT64|FDF_R },
   { "Input",       FDF_OBJECT|FDF_RI },
   { "Output",      FDF_OBJECT|FDF_RI },
   { "Format",      FDF_INT|FDF_LOOKUP|FD_RI, nullptr, nullptr, &clCompressedStreamFormat },
   { "Finished",    FDF_INT|FDF_R },
   // Virtual fields
   { "Size",        FDF_INT64|FDF_R|FDF_PURE, COMPRESSEDSTREAM_GET_Size },
   END_FIELD
};

extern ERR add_compressed_stream_class(void)
{
   glCompressedStreamClass = extMetaClass::create::global(
      fl::BaseClassID(CLASSID::COMPRESSEDSTREAM),
      fl::ClassVersion(1.0),
      fl::Name("CompressedStream"),
      fl::FileDescription("GZip File"),
      fl::Category(CCF::DATA),
      fl::Actions(clCompressedStreamActions),
      fl::Fields(clStreamFields),
      fl::PublicSize(sizeof(objCompressedStream)),
      fl::Size(sizeof(extCompressedStream)),
      fl::Path("modules:core"));

   return glCompressedStreamClass ? ERR::Okay : ERR::AddClass;
}

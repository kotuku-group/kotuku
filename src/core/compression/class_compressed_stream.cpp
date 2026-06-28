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
Repeatedly reading from the CompressedStream will automatically handle the decompression process.  If the
decompressed size of the incoming data is defined in the source header, it will be reflected in the #Size
field.

To compress data, set the #Output field with a source object that supports the Write() action, such as a @File.
Repeatedly writing to the CompressedStream with raw data will automatically handle the compression process for you.
Once all of the data has been written, call the #Write() action with a `Buffer` of `NULL` and `Length` `-1` to
signal an end to the streaming process.

-END-

*********************************************************************************************************************/

class extCompressedStream : public objCompressedStream {
   public:

   // Note: As PublicSize is defined, these fields are specific to CompressedStream and will otherwise be
   // overwritten for derived classes.
   uint8_t *OutputBuffer;
   ZStream Stream;
   gz_header Header;

   ~extCompressedStream();

   extCompressedStream(objMetaClass *ClassPtr, OBJECTID ObjectID) : objCompressedStream(ClassPtr, ObjectID) {
      Format = CF::GZIP;
   }

   void reset() {
      TotalOutput = 0;

      Stream.reset();

      if (OutputBuffer) { FreeResource(OutputBuffer); OutputBuffer = nullptr; }
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

   if ((!Args) or (!Args->Buffer)) return log.warning(ERR::NullArgs);
   if (!Self->initialised()) return log.warning(ERR::NotInitialised);

   Args->Result = 0;
   if (Args->Length <= 0) return ERR::Okay;

   uint8_t inputstream[2048];
   int length;

   if (acRead(Self->Input, inputstream, sizeof(inputstream), &length) != ERR::Okay) return ERR::Read;

   if (length <= 0) return ERR::Okay;

   if (!Self->Stream.active()) {
      log.trace("Initialising decompression of the stream.");
      switch (Self->Format) {
         case CF::ZLIB:
            if (Self->Stream.inflate_init(MAX_WBITS) != Z_OK) return log.warning(ERR::Decompression);
            break;

         case CF::DEFLATE:
            if (Self->Stream.inflate_init(-MAX_WBITS) != Z_OK) return log.warning(ERR::Decompression);
            break;

         case CF::GZIP:
         default:
            if (Self->Stream.inflate_init(15 + 32) != Z_OK) return log.warning(ERR::Decompression);
            // Read the uncompressed size from the gzip header
            if (inflateGetHeader(Self->Stream.get(), &Self->Header) != Z_OK) {
               return log.warning(ERR::InvalidData);
            }
      }
   }

   APTR output = Args->Buffer;
   int outputsize = Args->Length;
   if (outputsize < MIN_OUTPUT_SIZE) {
      // An internal buffer will need to be allocated if the one supplied to Read() is not large enough.
      outputsize = MIN_OUTPUT_SIZE;
      if (!(output = Self->OutputBuffer)) {
         if (AllocMemory(MIN_OUTPUT_SIZE, MEM::DATA|MEM::NO_CLEAR, (APTR *)&Self->OutputBuffer) != ERR::Okay) return ERR::AllocMemory;
         output = Self->OutputBuffer;
      }
   }

   Self->Stream->next_in  = inputstream;
   Self->Stream->avail_in = length;

   ERR error = ERR::Okay;
   int result = Z_OK;
   while ((result IS Z_OK) and (Self->Stream->avail_in > 0) and (outputsize > 0)) {
      Self->Stream->next_out  = (Bytef *)output;
      Self->Stream->avail_out = outputsize;
      result = inflate(Self->Stream.get(), Z_SYNC_FLUSH);

      if ((result) and (result != Z_STREAM_END)) {
         error = convert_zip_error(Self->Stream.get(), result);
         break;
      }

      if (error != ERR::Okay) break;

      Args->Result += outputsize - Self->Stream->avail_out;
      output = (Bytef *)output + outputsize - Self->Stream->avail_out;

      if (result IS Z_STREAM_END) { // Decompression is complete
         Self->TotalOutput = Self->Stream->total_out;
         Self->Stream.reset();
         return ERR::Okay;
      }
   }

   return error;
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

   // Seeking results in a reset of the compression object's state.  It then needs to decompress the stream up to the
   // position requested by the client.

   Self->reset();

   int64_t pos = 0;
   if (Args->Position IS SEEK::START) pos = int(Args->Offset);
   else if (Args->Position IS SEEK::CURRENT) pos = Self->TotalOutput + int(Args->Offset);
   else return log.warning(ERR::Args);

   if (pos < 0) return log.warning(ERR::OutOfRange);

   uint8_t buffer[1024];
   while (pos > 0) {
      struct acRead read = { .Buffer = buffer, .Length = (int)pos };
      if ((size_t)read.Length > sizeof(buffer)) read.Length = sizeof(buffer);
      if (Action(AC::Read, Self, &read) != ERR::Okay) return ERR::Decompression;
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

   if ((!Args) or (!Args->Buffer)) return log.warning(ERR::NullArgs);
   if (!Self->initialised()) return log.warning(ERR::NotInitialised);

   if (!Self->Stream.active()) {
      switch (Self->Format) {
         case CF::ZLIB:
            if (Self->Stream.deflate_init(9, MAX_WBITS)) {
               return log.warning(ERR::Compression);
            }
            break;

         case CF::DEFLATE:
            if (Self->Stream.deflate_init(9, -MAX_WBITS)) {
               return log.warning(ERR::Compression);
            }
            break;

         case CF::GZIP:
         default:
            if (Self->Stream.deflate_init(9, 15 + 32)) {
               return log.warning(ERR::Compression);
            }
      }

      Self->TotalOutput = 0;
   }

   if (!Self->OutputBuffer) {
      if (AllocMemory(MIN_OUTPUT_SIZE, MEM::DATA|MEM::NO_CLEAR, (APTR *)&Self->OutputBuffer) != ERR::Okay) return ERR::AllocMemory;
   }

   Args->Result = 0;
   int mode;
   if (Args->Length IS -1) { // A length of -1 is a signal to complete the compression process.
      mode = Z_FINISH;
      Self->Stream->next_in  = Self->OutputBuffer;
      Self->Stream->avail_in = 0;
   }
   else {
      mode = Z_NO_FLUSH;
      Self->Stream->next_in  = (Bytef *)Args->Buffer;
      Self->Stream->avail_in = Args->Length;
   }

   // If zlib succeeds but sets avail_out to zero, this means that data was written to the output buffer, but the
   // output buffer is not large enough (so keep calling until avail_out > 0).

   Self->Stream->avail_out = 0;
   while (Self->Stream->avail_out IS 0) {
      Self->Stream->next_out  = Self->OutputBuffer;
      Self->Stream->avail_out = MIN_OUTPUT_SIZE;

      if ((deflate(Self->Stream.get(), mode))) {
         Self->Stream.reset();
         return ERR::BufferOverflow;
      }

      const int len = MIN_OUTPUT_SIZE - Self->Stream->avail_out; // Get number of compressed bytes that were output

      if (len > 0) {
         Self->TotalOutput += len;
         log.trace("%d bytes (total %" PF64 ") were compressed.", len, Self->TotalOutput);
         acWrite(Self->Output, Self->OutputBuffer, len, nullptr);
      }
      else {
         // deflate() may not output anything if it needs more data to fill up a compression frame.  Return ERR::Okay
         // and wait for more data, or for the developer to end the stream.

         //log.trace("No data output on this cycle.");
         break;
      }
   }

   if (mode IS Z_FINISH) Self->Stream.reset();

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
Size: The uncompressed size of the input source, if known.

The Size field will reflect the uncompressed size of the input source, if this can be determined from the header.
In the case of GZIP decompression, the size will not be known until the parser has consumed the header.
This means that at least one call to the #Read() action is required before the Size is known.

If the size is unknown, a value of `-1` is returned.

*********************************************************************************************************************/

static ERR COMPRESSEDSTREAM_GET_Size(extCompressedStream *Self, int64_t *Value)
{
   *Value = -1;
   if (Self->Input) {
      if (Self->Header.done) {
         if (Self->Header.extra) *Value = Self->Header.extra_len;
      }

      return ERR::Okay;
   }
   else return ERR::InvalidState;
}

/*********************************************************************************************************************
-FIELD-
TotalOutput: A live counter of total bytes that have been output by the stream.
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
   { "Input",       FDF_OBJECT|FDF_RI },
   { "Output",      FDF_OBJECT|FDF_RI },
   { "Format",      FDF_INT|FDF_LOOKUP|FD_RI, nullptr, nullptr, &clCompressedStreamFormat },
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

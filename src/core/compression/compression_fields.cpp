
/*********************************************************************************************************************

-FIELD-
ArchiveName: Apply an archive name to the object, allowing it to be used as a named object in the file system.

Setting the ArchiveName will allow a Compression object's files to be accessible using standard file system paths.
This is achieved through use of the `archive:` volume, which is a virtual filesystem included in the Core API.

*********************************************************************************************************************/

static ERR SET_ArchiveName(extCompression *Self, std::string_view &Value)
{
   if (not Value.empty()) Self->ArchiveHash = strihash(Value);
   else Self->ArchiveHash = 0;

   if (Self->ArchiveHash) add_archive(Self);
   else remove_archive(Self);

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
CompressionLevel: The compression level to use when compressing data.

The level of compression that is used when compressing data is determined by the value in this field.  Values range
between 0 for no compression and 100 for maximum compression.  The speed of compression decreases with higher values,
but the compression ratio will improve.

*********************************************************************************************************************/

static ERR SET_CompressionLevel(extCompression *Self, int Value)
{
   if (Value < 0) Value = 0;
   else if (Value > 100) Value = 100;
   Self->CompressionLevel = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Feedback: Provides feedback during the de/compression process.

To receive feedback during any de/compression process, set a callback routine in this field. The format for the
callback routine is `ERR Function(*Compression, *CompressionFeedback)`.

For object classes, the object that initiated the de/compression process can be learned by calling the Core's
~Core.CurrentContext() function.

During the processing of multiple files, any individual file can be skipped by returning `ERR::Skip` and the entire
process can be cancelled by returning `ERR::Terminate`.  All other error codes are ignored.

The !CompressionFeedback structure consists of the following fields:

!CompressionFeedback

*********************************************************************************************************************/

static ERR GET_Feedback(extCompression *Self, FUNCTION * &Value)
{
   if (Self->Feedback.defined()) {
      Value = &Self->Feedback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_Feedback(extCompression *Self, FUNCTION *Value)
{
   if (Self->Feedback.defined()) Self->Feedback.unpin();
   if (Value) {
      Self->Feedback = *Value;
      Self->Feedback.pin();
   }
   else Self->Feedback.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Flags: Optional flags.

-FIELD-
Header: Private.  The first 32 bytes of a compression object's file header.

This field is only of use to derived classes that need to examine the first 32 bytes of a compressed file's header.

*********************************************************************************************************************/

static ERR GET_Header(extCompression *Self, uint8_t **Header)
{
   *Header = Self->Header;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Path: Set if the compressed data originates from, or is to be saved to a file source.

To load or create a new file archive, set the Path field to the path of that file.

-FIELD-
MinOutputSize: Indicates the minimum output buffer size that will be needed during de/compression.

This field indicates the minimum allowable buffer size for output during decompression and compression processing.
This field must be checked before allocating your own buffers for holding compressed and decompressed output, as
failing to allocate enough buffer space is extremely likely to result in overflow errors.

-FIELD-
Output: Resulting messages will be sent to the object referred to in this field.

If this field is set to a valid object ID, text messages will be sent to that object when the compression object is
used.  This can be helpful for notifying the user of the results of compression, decompression and removal of files.

The target object must be capable of processing incoming text from data channels.

-FIELD-
Password: Required if an archive needs an encryption password for access.

Set the Password field if an archive will use a password for the encryption of its contents.  The string must be
null-terminated and not more than 128 bytes in length.

It is recommended that the Password is set before or immediately after initialisation.  To change the password
of an existing archive, create a new compression object with the desired password and transfer the existing data
across to it.

*********************************************************************************************************************/

static ERR SET_Password(extCompression *Self, std::string_view &Value)
{
   if (not Value.empty()) {
      Self->Password.assign(Value.substr(0, 127));
      Self->Flags |= CMF::PASSWORD;
   }
   else Self->Password.clear();

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Permissions: Default permissions for decompressed files are defined here.

By default the permissions of files added to an archive are derived from their source location.  This behaviour can be
over-ridden by setting the Permissions field.  Valid permission flags are outlined in the @File class.

-FIELD-
Size: Indicates the size of the source archive, in bytes.

*********************************************************************************************************************/

static ERR GET_Size(extCompression *Self, int64_t *Value)
{
   *Value = 0;
   if (Self->FileIO) return Self->FileIO->getSize(*Value);
   else return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SegmentSize: Private. Splits the compressed file if it surpasses a set byte limit.

-FIELD-
TotalOutput: The total number of bytes that have been output during the compression or decompression of streamed data.

-FIELD-
UncompressedSize: The total decompressed size of all files in an archive.

If you would like to know the total number of bytes that have been compressed into a compression object, read this
field.  This will tell you the maximum byte count used if every file were to be decompressed.  Header and tail
information that may identify the compressed data is not included in the total.

*********************************************************************************************************************/

static ERR GET_UncompressedSize(extCompression *Self, int64_t *Value)
{
   int64_t size = 0;
   for (auto &f : Self->Files) size += f.OriginalSize;
   *Value = size;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
WindowBits: Special option for certain compression formats.

The WindowBits field defines the size of the sliding window frame for the default compression format (DEFLATE), but may
also be of use to other compression formats that use this technique.

For DEFLATE compression, the window bits range must lie between 8 and 15.  Please note that if the value is negative,
the algorithm will not output the traditional zlib header information.

To support GZIP decompression, please set the WindowBits value to 47.
-END-

*********************************************************************************************************************/

static ERR SET_WindowBits(extCompression *Self, int Value)
{
   if (((Value >= 8) and (Value <= 15)) or ((Value >= -15) and (Value <= -8)) or
       (Value IS 15 + 32) or (Value IS 16 + 32)) {
      Self->WindowBits = Value;
      return ERR::Okay;
   }
   else return kt::Log().warning(ERR::OutOfRange);
}

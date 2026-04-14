/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
Compression: Compresses data into archives, supporting a variety of compression formats.

The Compression class provides an interface to compress and decompress data.  It provides support for file
based compression as well as memory based compression routines.  The base class uses zip algorithms to support pkzip
files, while other forms of compressed data can be supported by installing additional compression sub-classes.

The following examples demonstrate basic usage of compression objects in Tiri:

<pre>
// Create a new zip archive and compress two files.

cmp = obj.new('compression', { path='temp:result.zip', flags='!NEW' } )
err = cmp.mtCompressFile('config:defs/compression.def', '')
err = cmp.mtCompressFile('config:defs/core.def', '')

// Decompress all *.def files in the root of an archive.

cmp = obj.new('compression', { path='temp:result.zip' } )
err = cmp.mtDecompressFile('*.def', 'temp:')
</pre>

It is strongly advised that Compression objects are created for the purpose of either writing to, or reading from the
target archive.  The class is not designed for both purposes simultaneously, particularly due to considerations for
maximising operational speed.

If data is to be encrypted or decrypted, set the #Password field with a null-terminated encryption string. If the
password for an encrypted file, errors will be returned when trying to decompress the information (the source archive
may be reported as a corrupted file).

To list the contents of an archive, use the #Scan() method.

To adjust the level of compression used to pack each file, set the #CompressionLevel field to a value between 0 and
100%.

This code is based on the work of Jean-loup Gailly and Mark Adler.

-END-

*********************************************************************************************************************/

#define ZLIB_MEM_LEVEL 8

#include "zlib.h"

#define PRV_FILE
#include "../defs.h"
#include <kotuku/main.h>
#include <sstream>

//********************************************************************************************************************
// Central folder structure for each archived file.  This appears at the end of the zip file.

static const int LIST_SIGNATURE      = 0;
static const int LIST_VERSION        = 4;
static const int LIST_OS             = 5;
static const int LIST_REQUIRED_VER   = 6;
static const int LIST_REQUIRED_OS    = 7;
static const int LIST_FLAGS          = 8;
static const int LIST_METHOD         = 10;
static const int LIST_TIMESTAMP      = 12;
static const int LIST_CRC            = 16;  // Checksum
static const int LIST_COMPRESSEDSIZE = 20;
static const int LIST_FILESIZE       = 24;  // Original file size
static const int LIST_NAMELEN        = 28;  // File name
static const int LIST_EXTRALEN       = 30;  // System specific information
static const int LIST_COMMENTLEN     = 32;  // Optional comment
static const int LIST_DISKNO         = 34;  // Disk number start
static const int LIST_IFILE          = 36;  // Internal file attributes (pkzip specific)
static const int LIST_ATTRIB         = 38;  // System specific file attributes
static const int LIST_OFFSET         = 42;  // Relative offset of local header
static const int LIST_LENGTH         = 46;  // END

PACK(struct zipentry {
   uint8_t version;
   uint8_t ostype;
   uint8_t required_version;
   uint8_t required_os;
   uint16_t flags;
   uint16_t deflatemethod;
   uint32_t timestamp;
   uint32_t crc32;
   uint32_t compressedsize;
   uint32_t originalsize;
   uint16_t namelen;
   uint16_t extralen;
   uint16_t commentlen;
   uint16_t diskno;
   uint16_t ifile;
   uint32_t attrib;
   uint32_t offset;
});

//********************************************************************************************************************

static const int TAIL_FILECOUNT      = 8;
static const int TAIL_TOTALFILECOUNT = 10;
static const int TAIL_FILELISTSIZE   = 12;
static const int TAIL_FILELISTOFFSET = 16;
static const int TAIL_COMMENTLEN     = 20;
static const int TAIL_LENGTH         = 22;

PACK(struct ziptail {
   uint32_t header;
   uint32_t size;
   uint16_t filecount;
   uint16_t diskfilecount;
   uint32_t listsize;
   uint32_t listoffset;
   uint16_t commentlen;
});

#define ZIP_KOTUKU 0x7e // Use this identifier to declare Kotuku zipped files

// The following flags can be tagged to each file entry in the zip file and are Kotuku-specific (identifiable by the
// ZIP_KOTUKU OS tag).  NOTE: The low order bits aren't used because WinZip, WinRar and so forth assume that
// those bits have meaning.

static const int ZIP_LINK   = 0x00010000; // The entry is a symbolic link
static const int ZIP_UEXEC  = 0x00020000; // Executable-access allowed (user)
static const int ZIP_GEXEC  = 0x00040000; // Executable-access allowed (group)
static const int ZIP_OEXEC  = 0x00080000; // Executable-access allowed (others/everyone)
static const int ZIP_UREAD  = 0x00100000; // Read-access allowed (user)
static const int ZIP_GREAD  = 0x00200000; // Read-access allowed (group)
static const int ZIP_OREAD  = 0x00400000; // Read-access allowed (others/everyone)
static const int ZIP_UWRITE = 0x00800000; // Write-access allowed (user)
static const int ZIP_GWRITE = 0x01000000; // Write-access allowed (group)
static const int ZIP_OWRITE = 0x02000000; // Write-access allowed (others/everyone)

#define ZIP_SECURITY (ZIP_UEXEC | ZIP_GEXEC | ZIP_OEXEC | ZIP_UREAD | ZIP_GREAD | ZIP_OREAD | ZIP_UWRITE | ZIP_GWRITE | ZIP_OWRITE)

struct ZipFile {
   std::string Name;
   std::string Comment;
   uint32_t  CompressedSize = 0;
   uint32_t  OriginalSize = 0;
   int   Year = 0;
   int   Flags = 0;         // These match the zip 'attrib' value
   uint32_t  TimeStamp = 0;     // Time stamp information
   uint32_t  CRC = 0;           // CRC validation number
   uint32_t  Offset = 0;        // Byte offset of the file within the archive
   uint16_t  NameLen = 0;       // The zip record's name length, including padding.
   uint16_t  CommentLen = 0;    // The zip record's comment length, including padding.
   uint16_t  DeflateMethod = 0; // Set to 8 for normal deflation
   uint8_t  Month = 0;
   uint8_t  Day = 0;
   uint8_t  Hour = 0;
   uint8_t  Minute = 0;
   bool   IsFolder = false;

   ZipFile() { }

   ZipFile(CSTRING pName) {
      Name.assign(pName);
   }

   ZipFile(std::string pName) {
      Name = pName;
   }
};

static const int SIZE_COMPRESSION_BUFFER = 16384;

//********************************************************************************************************************
// File header.  Compressed data is prefixed with this information.

static const int HEAD_DEFLATEMETHOD  = 8;
static const int HEAD_TIMESTAMP      = 10;
static const int HEAD_CRC            = 14;
static const int HEAD_COMPRESSEDSIZE = 18;
static const int HEAD_FILESIZE       = 22;
static const int HEAD_NAMELEN        = 26;  // File name
static const int HEAD_EXTRALEN       = 28;  // System specific information
static const int HEAD_LENGTH         = 30;  // END

class extCompression : public objCompression {
   public:
   OBJECTPTR FileIO;             // File input/output
   STRING *  FileList;           // List of all files held in the compression object
   STRING    Path;               // Location of the compressed data
   uint8_t     Header[32];         // The first 32 bytes of data from the compressed file (for sub-classes only)
   char      Password[128];      // Password for the compressed object
   FUNCTION  Feedback;           // Set a function here to get de/compression feedack
   uint32_t     ArchiveHash;        // Archive reference, used for the 'archive:' volume

   // Zip only fields
   z_stream Zip;
   z_stream InflateStream;
   z_stream DeflateStream;
   std::list<ZipFile> Files;    // List of files in the archive (must be in order of the archive's entries)
   uint8_t  *Output;
   uint8_t  *Input;
   uint8_t  *OutputBuffer;        // Output buffer for compressed data
   int   OutputSize;           // Size of OutputBuffer
   int   TotalFiles;
   int   FileIndex;
   int16_t   CompressionCount;  // Counter of times that compression has occurred
   bool   Deflating;
   bool   Inflating;
};

static ERR compress_folder(extCompression *, std::string, std::string);
static ERR compress_file(extCompression *, std::string, std::string, bool);
static void print(extCompression *, CSTRING);
static void print(extCompression *, std::string);
static ERR remove_file(extCompression *, std::list<ZipFile>::iterator &);
static ERR scan_zip(extCompression *);
static ERR fast_scan_zip(extCompression *);
static ERR send_feedback(extCompression *, CompressionFeedback *);
static void write_eof(extCompression *);
void zipfile_to_item(ZipFile &, CompressedItem &);

static ERR GET_Size(extCompression *, int64_t *);

//********************************************************************************************************************
// Special definitions.

static const uint8_t glHeader[HEAD_LENGTH] = {
   'P', 'K', 0x03, 0x04,   // 0 Signature
   0x14, 0x00,             // 4 Version 2.0
   0x00, 0x00,             // 6 Flags
   0x08, 0x00,             // 8 Deflation method
   0x00, 0x00, 0x00, 0x00, // 10 Time stamp
   0x00, 0x00, 0x00, 0x00, // 14 CRC
   0x00, 0x00, 0x00, 0x00, // 18 Compressed Size
   0x00, 0x00, 0x00, 0x00, // 22 Original File Size
   0x00, 0x00,             // 26 Length of path/filename
   0x00, 0x00              // 28 Length of extra field.
};

static const uint8_t glList[LIST_LENGTH] = {
   'P', 'K', 0x01, 0x02,   // 00 Signature
   0x14, ZIP_KOTUKU,      // 04 Version 2.0, host OS
   0x14, 0x00,             // 06 Version need to extract, OS needed to extract
   0x00, 0x00,             // 08 Flags
   0x08, 0x00,             // 10 Deflation method
   0x00, 0x00, 0x00, 0x00, // 12 Time stamp
   0x00, 0x00, 0x00, 0x00, // 16 CRC
   0x00, 0x00, 0x00, 0x00, // 20 Compressed Size
   0x00, 0x00, 0x00, 0x00, // 24 Original File Size
   0x00, 0x00,             // 28 Length of path/filename
   0x00, 0x00,             // 30 Length of extra field
   0x00, 0x00,             // 32 Length of comment
   0x00, 0x00,             // 34 Disk number start
   0x00, 0x00,             // 36 File attribute: 0 = Binary, 1 = ASCII
   0x00, 0x00, 0x00, 0x00, // 38 File permissions?
   0x00, 0x00, 0x00, 0x00, // 42 Offset of compressed data within the file
   // File name follows
   // Extra field follows
   // Comment follows
};

static const uint8_t glTail[TAIL_LENGTH] = {
   'P', 'K', 0x05, 0x06,   // 0 Signature
   0x00, 0x00,             // 4 Number of this disk
   0x00, 0x00,             // 6 Number of the disk with starting central directory
   0x00, 0x00,             // 8 Number of files in the central directory for this zip file
   0x00, 0x00,             // 10 Number of files in the archive spanning all disks
   0x00, 0x00, 0x00, 0x00, // 12 Size of file list
   0x00, 0x00, 0x00, 0x00, // 16 Offset of the file list with respect to starting disk number
   0x00, 0x00              // 20 Length of zip file comment
   // End of file comment follows
};

//********************************************************************************************************************

ERR convert_zip_error(struct z_stream_s *Stream, int Result)
{
   pf::Log log;

   ERR error;
   switch(Result) {
      case Z_STREAM_ERROR:  error = ERR::CompressionStreamError;
      case Z_DATA_ERROR:    error = ERR::InvalidData;
      case Z_MEM_ERROR:     error = ERR::Memory;
      case Z_BUF_ERROR:     error = ERR::BufferOverflow;
      case Z_VERSION_ERROR: error = ERR::WrongVersion;
      default:              error = ERR::CompressionStreamError;
   }

   if (Stream->msg) log.warning("%s", Stream->msg);
   else log.warning("Zip error %d: %s", Result, GetErrorMsg(error));

   return error;
}

//********************************************************************************************************************

static void notify_free_feedback(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   auto Self = (extCompression *)CurrentContext();
   Self->Feedback.clear();
}

/*********************************************************************************************************************

-METHOD-
CompressBuffer: Compresses a plain memory area into an empty buffer.

This method provides a simple way of compressing a memory area into a buffer.  It requires a reference to the
source data and a buffer large enough to accept the compressed information.  Generally the destination buffer should
be no smaller than 75% of the size of the source data.  If the destination buffer is not large enough, an error of
`ERR::BufferOverflow` will be returned.  The size of the compressed data will be returned in the Result parameter.

To decompress the data that is output by this function, use the #DecompressBuffer() method.

The compression method used to compress the data will be identified in the first 32 bits of output, for example,
`ZLIB`.  The following 32 bits will indicate the length of the compressed data section, followed by the data itself.

-INPUT-
buf(ptr) Input: Pointer to the source data.
bufsize InputSize: Byte length of the source data.
buf(ptr) Output: Pointer to a destination buffer.
bufsize OutputSize: Available space in the destination buffer.
&int Result: The size of the compressed data will be returned in this parameter.

-ERRORS-
Okay: The data was compressed successfully.  The Result parameter indicates the size of the compressed data.
Args
NullArgs
Failed
BufferOverflow: The output buffer is not large enough.
-END-

*********************************************************************************************************************/

static ERR COMPRESSION_CompressBuffer(extCompression *Self, struct cmp::CompressBuffer *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Input) or (Args->InputSize <= 0) or (!Args->Output) or (Args->OutputSize <= 8)) {
      return log.warning(ERR::Args);
   }

   Self->Zip.next_in   = (Bytef *)Args->Input;
   Self->Zip.avail_in  = Args->InputSize;
   Self->Zip.next_out  = (Bytef *)Args->Output + 8;
   Self->Zip.avail_out = Args->OutputSize - 8;

   int level = Self->CompressionLevel / 10;
   if (level < 0) level = 0;
   else if (level > 9) level = 9;

   if (deflateInit2(&Self->Zip, level, Z_DEFLATED, Self->WindowBits, ZLIB_MEM_LEVEL, Z_DEFAULT_STRATEGY) IS Z_OK) {
      if (deflate(&Self->Zip, Z_FINISH) IS Z_STREAM_END) {
         Args->Result = Self->Zip.total_out + 8;
         deflateEnd(&Self->Zip);

         ((char *)Args->Output)[0] = 'Z';
         ((char *)Args->Output)[1] = 'L';
         ((char *)Args->Output)[2] = 'I';
         ((char *)Args->Output)[3] = 'B';
         ((int *)Args->Output)[1] = Self->Zip.total_out;
         return ERR::Okay;
      }
      else {
         deflateEnd(&Self->Zip);
         return log.warning(ERR::BufferOverflow);
      }
   }
   else return log.warning(ERR::InvalidCompression);
}

/*********************************************************************************************************************

-METHOD-
CompressFile: Add files to a compression object.

The CompressFile method is used to add new files and folders to a compression object. The client must supply the
`Location` of the file to compress, as well as the `Path` that is prefixed to the file name when it is in the
compression object.  The `Location` parameter accepts wildcards, allowing multiple files to be processed in a single
function call.

To compress all contents of a folder, specify its path in the `Location` parameter and ensure that it is fully
qualified by appending a forward slash or colon character.

The `Path` parameter must include a trailing slash when targeting a folder, otherwise the source file will be renamed
to suit the target path.  If the `Path` starts with a forward slash and the source is a folder, the name of that
folder will be used in the target path for the compressed files and folders.

-INPUT-
cstr Location: The location of the file(s) to add.
cstr Path:     The path that is prefixed to the file name when added to the compression object.  May be `NULL` for no path.

-ERRORS-
Okay: The file was added to the compression object.
Args:
File: An error was encountered when trying to open the source file.
NoPermission: The `READ_ONLY` flag has been set on the compression object.
NoSupport: The sub-class does not support this method.

*********************************************************************************************************************/

static ERR COMPRESSION_CompressFile(extCompression *Self, struct cmp::CompressFile *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Location) or (!*Args->Location)) return log.warning(ERR::NullArgs);
   if (!Self->FileIO) return log.warning(ERR::MissingPath);

   if ((Self->Flags & CMF::READ_ONLY) != CMF::NIL) return log.warning(ERR::NoPermission);

   if (Self->isSubClass()) return log.warning(ERR::NoSupport);

   if (Self->OutputID) {
      std::ostringstream out;
      out << "Compressing \"" << Args->Location << "\" to \"" << Self->Path << "\".\n";
      print(Self, out.str());
   }

   std::string src(Args->Location);
   std::string path;
   bool incdir = false;
   if (!Args->Path) path = "";
   else { // Accept the path by default but check it for illegal symbols just in case
      if (Args->Path[0] IS '/') { // Special mode: prefix src folder name to the root path
         incdir = true;
         path.assign(Args->Path + 1);
      }
      else path.assign(Args->Path);

      for (int i=0; path[i]; i++) {
         if (path.find_first_of("*?\":|<>") != std::string::npos) {
            log.warning("Illegal characters in path: %s", path.c_str());
            if (Self->OutputID) {
               std::ostringstream out;
               out << "Warning - path ignored due to illegal characters: " << path << "\n";
               print(Self, out.str());
            }
            path.clear();
            break;
         }
      }
   }

   log.branch("Location: %s, Path: %s", src.c_str(), path.c_str());

   Self->FileIndex = 0;

   if ((src.back() IS '/') or (src.back() IS '\\') or (src.back() IS ':')) { // The source is a folder
      if ((!path.empty()) or (incdir)) {
         // This subroutine creates a path custom string if the inclusive folder name option is on, or if the path is
         // missing a terminating slash character.

         int inclen = 0, i = 0;
         if (incdir) {
            i = src.size() - 1;
            while ((i > 0) and (src[i-1] != '/') and (src[i-1] != '\\') and (src[i-1] != ':')) {
               inclen++;
               i--;
            }
         }

         if ((inclen) or ((path.back() != '/') and (path.back() != '\\'))) {
            std::string newpath;
            if (inclen > 0) newpath.append(src, i);
            newpath += path;
            if ((newpath.back() != '/') and (newpath.back() != '\\')) newpath += '/';

            return compress_folder(Self, src, newpath);
         }
      }

      return compress_folder(Self, src, path);
   }

   ERR error = ERR::Okay;

   // Check the location string for wildcards, * and ?

   bool wildcard = false;
   int pathlen;
   for (pathlen=src.size(); pathlen > 0; pathlen--) {
      if ((src[pathlen-1] IS '*') or (src[pathlen-1] IS '?')) wildcard = true;
      else if ((src[pathlen-1] IS ':') or (src[pathlen-1] IS '/') or (src[pathlen-1] IS '\\')) break;
   }

   if (!wildcard) {
      return compress_file(Self, src, path, FALSE);
   }
   else {
      std::string filename;
      filename.assign(src, pathlen, src.size() - pathlen); // Extract the file name without the path

      std::string srcfolder(src, pathlen); // Extract the path without the file name

      DirInfo *dir;
      if (OpenDir(srcfolder.c_str(), RDF::FILE, &dir) IS ERR::Okay) {
         while (ScanDir(dir) IS ERR::Okay) {
            FileInfo *scan = dir->Info;
            if (wildcmp(filename, scan->Name)) {
               auto folder = src.substr(0, pathlen);
               folder.append(scan->Name);
               error = compress_file(Self, folder, path, FALSE);
            }
         }

         FreeResource(dir);
      }
   }

   if (Self->OutputID) {
      int64_t size;
      GET_Size(Self, &size);
      std::ostringstream out;
      out << "\nCompression complete.  Archive is " << size <<  " bytes in size.";
      print(Self, out.str());
   }

   return error;
}

/*********************************************************************************************************************

-METHOD-
CompressStreamStart: Initialises a new compression stream.

The level of compression is determined by the #CompressionLevel field value.

-ERRORS-
Okay
Failed: Failed to initialise the decompression process.

*********************************************************************************************************************/

static ERR COMPRESSION_CompressStreamStart(extCompression *Self)
{
   pf::Log log;

   if (Self->Deflating) {
      deflateEnd(&Self->DeflateStream);
      Self->Deflating = false;
   }

   int level = Self->CompressionLevel / 10;
   if (level < 0) level = 0;
   else if (level > 9) level = 9;

   clearmem(&Self->DeflateStream, sizeof(Self->DeflateStream));

   Self->TotalOutput = 0;
   if (auto err = deflateInit2(&Self->DeflateStream, level, Z_DEFLATED, Self->WindowBits, ZLIB_MEM_LEVEL, Z_DEFAULT_STRATEGY); err IS Z_OK) {
      log.trace("Compression stream initialised.");
      Self->Deflating = true;
      return ERR::Okay;
   }
   else return log.warning(ERR::InvalidCompression);
}

/*********************************************************************************************************************

-METHOD-
CompressStream: Compresses streamed data into a buffer.

Use the CompressStream method to compress incoming streams of data whilst using a minimal amount of memory.  The
compression process is handled in three phases of Start, Compress and End.  The methods provided for each phase are
#CompressStreamStart(), #CompressStream() and #CompressStreamEnd().

A compression object can manage only one compression stream at any given time.  If it is necessary to compress
multiple streams at once, create a compression object for each individual stream.

No meta-information is written to the stream, so the client will need a way to record the total number of bytes that
have been output during the compression process. This value must be stored somewhere in order to decompress the
stream correctly.  There is also no header information recorded to identify the type of algorithm used to compress
the stream.  We recommend that the compression object's sub-class ID is stored for future reference.

The following C code illustrates a simple means of compressing a file to another file using a stream:

<pre>
if (auto error = mtCompressStreamStart(compress); error IS ERR::Okay) {
   LONG len;
   LONG cmpsize = 0;
   UBYTE input[4096];
   while ((error = acRead(file, input, sizeof(input), &len)) IS ERR::Okay) {
      if (!len) break; // No more data to read.

      error = mtCompressStream(compress, input, len, &callback, NULL, 0);
      if (error != ERR::Okay) break;

      if (result > 0) {
         cmpsize += result;
         error = acWrite(outfile, output, result, &len);
         if (error != ERR::Okay) break;
      }
   }

   if (error IS ERR::Okay) {
      if ((error = mtCompressStreamEnd(compress, NULL, 0)) IS ERR::Okay) {
         cmpsize += result;
         error = acWrite(outfile, output, result, &len);
      }
   }
}
</pre>

Please note that, depending on the type of algorithm, this method will not always write data to the output buffer.  The
algorithm may store a copy of the input and wait for more data for efficiency reasons.  Any unwritten data will be
resolved when the stream is terminated with #CompressStreamEnd().  To check if data was output by this
function, either set a flag in the callback function or compare the #TotalOutput value to its original setting
before CompressStream was called.

-INPUT-
buf(ptr) Input: Pointer to the source data.
bufsize Length: Amount of data to compress, in bytes.
ptr(func) Callback: This callback function will be called with a pointer to the compressed data.
buf(ptr) Output: Optional.  Points to a buffer that will receive the compressed data.  Must be equal to or larger than the #MinOutputSize field.
bufsize OutputSize: Indicates the size of the `Output` buffer, otherwise set to zero.

-ERRORS-
Okay
NullArgs
Args
BufferOverflow: The output buffer is not large enough to contain the compressed data.
Retry: Please recall the method using a larger output buffer.
-END-

*********************************************************************************************************************/

static ERR COMPRESSION_CompressStream(extCompression *Self, struct cmp::CompressStream *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Input) or (!Args->Callback)) return log.warning(ERR::NullArgs);

   if (!Self->Deflating) return log.warning(ERR::InvalidState);

   Self->DeflateStream.next_in   = (Bytef *)Args->Input;
   Self->DeflateStream.avail_in  = Args->Length;

   APTR output;
   int err, outputsize;
   if ((output = Args->Output)) {
      outputsize = Args->OutputSize;
      if (outputsize < Self->MinOutputSize) {
         log.warning("OutputSize (%d) < MinOutputSize (%d)", outputsize, Self->MinOutputSize);
         return ERR::BufferOverflow;
      }
   }
   else if ((output = Self->OutputBuffer)) {
      outputsize = Self->OutputSize;
   }
   else {
      Self->OutputSize = 32 * 1024;
      if (AllocMemory(Self->OutputSize, MEM::DATA|MEM::NO_CLEAR, (APTR *)&Self->OutputBuffer, nullptr) != ERR::Okay) {
         return ERR::AllocMemory;
      }
      output = Self->OutputBuffer;
      outputsize = Self->OutputSize;
   }

   log.trace("Compressing Input: %p, Len: %d to buffer of size %d bytes.", Args->Input, Args->Length, outputsize);

   // If zlib succeeds but sets avail_out to zero, this means that data was written to the output buffer, but the
   // output buffer is not large enough (so keep calling until avail_out > 0).

   ERR error;
   Self->DeflateStream.avail_out = 0;
   while (Self->DeflateStream.avail_out IS 0) {
      Self->DeflateStream.next_out  = (Bytef *)output;
      Self->DeflateStream.avail_out = outputsize;
      if ((err = deflate(&Self->DeflateStream, Z_NO_FLUSH))) {
         deflateEnd(&Self->DeflateStream);
         error = ERR::BufferOverflow;
         break;
      }
      else error = ERR::Okay;

      auto len = outputsize - Self->DeflateStream.avail_out; // Get number of compressed bytes that were output

      if (len > 0) {
         Self->TotalOutput += len;

         log.trace("%d bytes (total %" PF64 ") were compressed.", len, Self->TotalOutput);

         if (Args->Callback->isC()) {
            pf::SwitchContext context(Args->Callback->Context);
            auto routine = (ERR (*)(extCompression *, APTR, int, APTR))Args->Callback->Routine;
            error = routine(Self, output, len, Args->Callback->Meta);
         }
         else if (Args->Callback->isScript()) {
            if (sc::Call(*Args->Callback, std::to_array<ScriptArg>({
                  { "Compression",  Self, FD_OBJECTPTR },
                  { "Output",       output, FD_BUFFER },
                  { "OutputLength", int64_t(len), FD_INT64|FD_BUFSIZE }
               }), error) != ERR::Okay) error = ERR::Function;
         }
         else {
            log.warning("Callback function structure does not specify a recognised Type.");
            break;
         }
      }
      else {
         // deflate() may not output anything if it needs more data to fill up a compression frame.  Return ERR::Okay
         // and wait for more data, or for the developer to call CompressStreamEnd().

         //log.trace("No data output on this cycle.");
         break;
      }
   }

   if (error != ERR::Okay) log.warning(error);
   return error;
}

/*********************************************************************************************************************

-METHOD-
CompressStreamEnd: Ends the compression of an open stream.

To end the compression process, this method must be called to write any final blocks of data and remove the resources
that were allocated.

The expected format of the `Callback` function is specified in the #CompressStream() method.

-INPUT-
ptr(func) Callback: Refers to a function that will be called for each compressed block of data.
buf(ptr) Output: Optional pointer to a buffer that will receive the compressed data.  If not set, the compression object will use its own buffer.
bufsize OutputSize: Size of the `Output` buffer (ignored if Output is `NULL`).

-ERRORS-
Okay
NullArgs
BufferOverflow: The supplied Output buffer is not large enough (check the #MinOutputSize field for the minimum allowable size).

*********************************************************************************************************************/

static ERR COMPRESSION_CompressStreamEnd(extCompression *Self, struct cmp::CompressStreamEnd *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Callback)) return log.warning(ERR::NullArgs);
   if (!Self->Deflating) return ERR::Okay;

   APTR output;
   int outputsize;

   if ((output = Args->Output)) {
      outputsize = Args->OutputSize;
      if (outputsize < Self->MinOutputSize) return log.warning(ERR::BufferOverflow);
   }
   else if ((output = Self->OutputBuffer)) {
      outputsize = Self->OutputSize;
   }
   else return log.warning(ERR::FieldNotSet);

   log.trace("Output Size: %d", outputsize);

   Self->DeflateStream.next_in   = 0;
   Self->DeflateStream.avail_in  = 0;
   Self->DeflateStream.avail_out = 0;

   ERR error;
   int err = Z_OK;
   while ((Self->DeflateStream.avail_out IS 0) and (err IS Z_OK)) {
      Self->DeflateStream.next_out  = (Bytef *)output;
      Self->DeflateStream.avail_out = outputsize;
      if ((err = deflate(&Self->DeflateStream, Z_FINISH)) and (err != Z_STREAM_END)) {
         error = log.warning(ERR::BufferOverflow);
         break;
      }

      Self->TotalOutput += outputsize - Self->DeflateStream.avail_out;

      if (Args->Callback->isC()) {
         pf::SwitchContext context(Args->Callback->Context);
         auto routine = (ERR (*)(extCompression *, APTR, int, APTR Meta))Args->Callback->Routine;
         error = routine(Self, output, outputsize - Self->DeflateStream.avail_out, Args->Callback->Meta);
      }
      else if (Args->Callback->isScript()) {
         if (sc::Call(*Args->Callback, std::to_array<ScriptArg>({
            { "Compression",  Self,   FD_OBJECTPTR },
            { "Output",       output, FD_BUFFER },
            { "OutputLength", int64_t(outputsize - Self->DeflateStream.avail_out), FD_INT64|FD_BUFSIZE }
         }), error) != ERR::Okay) error = ERR::Function;
      }
      else error = ERR::Okay;
   }

   // Free the output buffer if it is quite large

   if ((Self->OutputBuffer) and (Self->OutputSize > 64 * 1024)) {
      FreeResource(Self->OutputBuffer);
      Self->OutputBuffer = nullptr;
      Self->OutputSize = 0;
   }

   deflateEnd(&Self->DeflateStream);
   clearmem(&Self->DeflateStream, sizeof(Self->DeflateStream));
   Self->Deflating = false;
   return error;
}

/*********************************************************************************************************************

-METHOD-
DecompressStreamStart: Initialises a new decompression stream.

Use the DecompressStreamStart method to initialise a new decompression stream.  No parameters are required.

If a decompression stream is already active at the time that this method is called, all resources associated with that
stream will be deallocated so that the new stream can be initiated.

To decompress the data stream, follow this call with repeated calls to #DecompressStream() until all the data
has been processed, then call #DecompressStreamEnd().

-ERRORS-
Okay
Failed: Failed to initialise the decompression process.

*********************************************************************************************************************/

static ERR COMPRESSION_DecompressStreamStart(extCompression *Self)
{
   pf::Log log;

   if (Self->Inflating) { inflateEnd(&Self->InflateStream); Self->Inflating = false; }

   clearmem(&Self->InflateStream, sizeof(Self->InflateStream));

   Self->TotalOutput = 0;

   if (auto err = inflateInit2(&Self->InflateStream, Self->WindowBits); err IS Z_OK) {
      log.trace("Decompression stream initialised.");
      Self->Inflating = true;
      return ERR::Okay;
   }
   else return log.warning(ERR::InvalidCompression);
}

/*********************************************************************************************************************

-METHOD-
DecompressStream: Decompresses streamed data to an output buffer.

Call DecompressStream repeatedly to decompress a data stream and process the results in a callback routine.  The client
will need to provide a pointer to the data in the `Input` parameter and indicate its size in `Length`.  The decompression
routine will call the routine that was specified in `Callback` for each block that is decompressed.

The format of the `Callback` routine is `ERR Function(*Compression, APTR Buffer, LONG Length)`

The `Buffer` will refer to the start of the decompressed data and its size will be indicated in `Length`.  If the
`Callback` routine returns an error of any kind, the decompression process will be stopped and the error code will be
immediately returned by the method.

Optionally, the client can specify an output buffer in the `Output` parameter.  This can be a valuable
optimisation technique, as it will eliminate the need to copy data out of the compression object's internal buffer.

When there is no more data in the decompression stream or if an error has occurred, the client must call
#DecompressStreamEnd().

-INPUT-
buf(ptr) Input: Pointer to data to decompress.
bufsize Length: Amount of data to decompress from the Input parameter.
ptr(func) Callback: Refers to a function that will be called for each decompressed block of information.
buf(ptr) Output: Optional pointer to a buffer that will receive the decompressed data.  If not set, the compression object will use its own buffer.
bufsize OutputSize: Size of the buffer specified in Output (value ignored if `Output` is `NULL`).

-ERRORS-
Okay
NullArgs
AllocMemory
BufferOverflow: The output buffer is not large enough.

*********************************************************************************************************************/

static ERR COMPRESSION_DecompressStream(extCompression *Self, struct cmp::DecompressStream *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Input) or (!Args->Callback)) return log.warning(ERR::NullArgs);
   if (!Self->Inflating) return ERR::Okay; // Decompression is complete

   APTR output;
   int outputsize;

   if ((output = Args->Output)) {
      outputsize = Args->OutputSize;
      if (outputsize < Self->MinOutputSize) return log.warning(ERR::BufferOverflow);
   }
   else if ((output = Self->OutputBuffer)) {
      outputsize = Self->OutputSize;
   }
   else {
      Self->OutputSize = 32 * 1024;
      if (AllocMemory(Self->OutputSize, MEM::DATA|MEM::NO_CLEAR, (APTR *)&Self->OutputBuffer, nullptr) != ERR::Okay) {
         return ERR::AllocMemory;
      }
      output = Self->OutputBuffer;
      outputsize = Self->OutputSize;
   }

   Self->InflateStream.next_in  = (Bytef *)Args->Input;
   Self->InflateStream.avail_in = Args->Length;

   // Keep looping until Z_STREAM_END or an error is returned

   ERR error = ERR::Okay;
   int result = Z_OK;
   while ((result IS Z_OK) and (Self->InflateStream.avail_in > 0)) {
      Self->InflateStream.next_out  = (Bytef *)output;
      Self->InflateStream.avail_out = outputsize;
      result = inflate(&Self->InflateStream, Z_SYNC_FLUSH);

      if ((result) and (result != Z_STREAM_END)) {
         error = convert_zip_error(&Self->InflateStream, result);
         break;
      }

      if (error != ERR::Okay) break;

      // Write out the decompressed data

      int len = outputsize - Self->InflateStream.avail_out;
      if (len > 0) {
         if (Args->Callback->isC()) {
            pf::SwitchContext context(Args->Callback->Context);
            auto routine = (ERR (*)(extCompression *, APTR, int, APTR))Args->Callback->Routine;
            error = routine(Self, output, len, Args->Callback->Meta);
         }
         else if (Args->Callback->isScript()) {
            if (sc::Call(*Args->Callback, std::to_array<ScriptArg>({
               { "Compression",  Self,   FD_OBJECTPTR },
               { "Output",       output, FD_BUFFER },
               { "OutputLength", len,    FD_INT|FD_BUFSIZE }
            }), error) != ERR::Okay) error = ERR::Function;
         }
         else {
            log.warning("Callback function structure does not specify a recognised Type.");
            break;
         }
      }

      if (error != ERR::Okay) break;

      if (result IS Z_STREAM_END) { // Decompression is complete, auto-perform DecompressStreamEnd()
         inflateEnd(&Self->InflateStream);
         Self->Inflating = false;
         Self->TotalOutput = Self->InflateStream.total_out;
         break;
      }
   }

   if (error != ERR::Okay) log.warning(error);
   return error;
}

/*********************************************************************************************************************

-METHOD-
DecompressStreamEnd: Must be called at the end of the decompression process.

To end the decompression process, this method must be called to write any final blocks of data and remove the resources
that were allocated during decompression.

-INPUT-
ptr(func) Callback: Refers to a function that will be called for each decompressed block of information.

-ERRORS-
Okay
NullArgs

*********************************************************************************************************************/

static ERR COMPRESSION_DecompressStreamEnd(extCompression *Self, struct cmp::DecompressStreamEnd *Args)
{
   if (!Self->Inflating) return ERR::Okay; // If not inflating, not a problem

   if ((!Args) or (!Args->Callback)) return ERR::NullArgs;

   Self->TotalOutput = Self->InflateStream.total_out;
   inflateEnd(&Self->InflateStream);
   Self->Inflating = false;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
DecompressBuffer: Decompresses data originating from the #CompressBuffer() method.

This method is used to decompress data that has been packed using the #CompressBuffer() method.  A pointer to the
compressed data and an output buffer large enough to contain the decompressed data are required.  If the output buffer
is not large enough to contain the data, the method will write out as much information as it can and then return with
an error code of `ERR::BufferOverflow`.

-INPUT-
buf(ptr) Input: Pointer to the compressed data.
buf(ptr) Output: Pointer to the decompression buffer.
bufsize OutputSize: Size of the decompression buffer.
&int Result: The amount of bytes decompressed will be returned in this parameter.

-ERRORS-
Okay
Args
BufferOverflow: The output buffer is not large enough to hold the decompressed information.

*********************************************************************************************************************/

static ERR COMPRESSION_DecompressBuffer(extCompression *Self, struct cmp::DecompressBuffer *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Input) or (!Args->Output) or (Args->OutputSize <= 0)) {
      return log.warning(ERR::NullArgs);
   }

   Self->Zip.next_in   = (Bytef *)Args->Input + 8;
   Self->Zip.avail_in  = ((int *)Args->Input)[1];
   Self->Zip.next_out  = (Bytef *)Args->Output;
   Self->Zip.avail_out = Args->OutputSize;

   if (inflateInit2(&Self->Zip, Self->WindowBits) IS Z_OK) {
      int err;
      if ((err = inflate(&Self->Zip, Z_FINISH)) IS Z_STREAM_END) {
         Args->Result = Self->Zip.total_out;
         inflateEnd(&Self->Zip);
         return ERR::Okay;
      }
      else {
         inflateEnd(&Self->Zip);
         if (Self->Zip.msg) log.warning("%s", Self->Zip.msg);
         else log.warning(ERR::BufferOverflow);
         return ERR::BufferOverflow;
      }
   }
   else return log.warning(ERR::InvalidCompression);
}

/*********************************************************************************************************************

-METHOD-
DecompressFile: Extracts one or more files from a compression object.

Use the DecompressFile() method to decompress a file or files to a destination folder.  The exact path name of the
compressed file is required for extraction unless using wildcards.  A single asterisk in the Path parameter will
extract all files in a compression object.

When specifying the `Dest` parameter, it is recommended that you specify a folder location by using a forward slash at
the end of the string.  If this is omitted, the destination will be interpreted as a file name.  If the destination
file already exists, it will be overwritten by the decompressed data.

This method sends feedback at regular intervals during decompression.  For further information on receiving feedback,
please refer to the #Feedback field.

-INPUT-
cstr Path: The full path name of the file to extract from the archive.
cstr Dest: The destination to extract the file to.
int Flags: Optional flags.  Currently unused.

-ERRORS-
Okay: The file was successfully extracted.
Args
NullArgs
MissingPath
NoData
File: A destination file could not be created.
Seek
Write: Failed to write uncompressed information to a destination file.
Cancelled: The decompression process was cancelled by the feedback mechanism.
Failed

*********************************************************************************************************************/

static ERR COMPRESSION_DecompressFile(extCompression *Self, struct cmp::DecompressFile *Args)
{
   pf::Log log;

   if (Self->Files.empty()) return ERR::NoData;

   // Validate arguments

   if ((!Args) or (!Args->Path)) {
      if (Self->OutputID) print(Self, "Please supply a Path setting that refers to a compressed file archive.\n");

      return log.warning(ERR::NullArgs);
   }

   if (!Args->Dest) {
      if (Self->OutputID) print(Self, "Please supply a Destination that refers to a folder for decompression.\n");

      return log.warning(ERR::NullArgs);
   }

   if ((!*Args->Path) or (!*Args->Dest)) {
      if (Self->OutputID) print(Self, "Please supply valid Path and Destination settings.\n");

      return log.warning(ERR::Args);
   }

   if (!Self->FileIO) {
      if (Self->OutputID) print(Self, "Internal error - decompression aborted.\n");

      return log.warning(ERR::MissingPath);
   }

   // If the object belongs to a Compression sub-class, return ERR::NoSupport

   if (Self->isSubClass()) return ERR::NoSupport;

   // Tell the user what we are doing

   if (Self->OutputID) {
      std::ostringstream out;
      out << "Decompressing archive \"" << Self->Path << "\" with path \"" << Args->Path << "\" to \"" << Args->Dest << "\".\n";
      print(Self, out.str());
   }

   // Search for the file(s) in our archive that match the given name and extract them to the destination folder.

   log.branch("%s TO %s, Permissions: $%.8x", Args->Path, Args->Dest, int(Self->Permissions));

   std::string destpath(Args->Dest);
   auto dest_len = destpath.size();

   uint16_t pathend = 0;
   for (uint16_t i=0; Args->Path[i]; i++) if ((Args->Path[i] IS '/') or (Args->Path[i] IS '\\')) pathend = i + 1;

   ERR error      = ERR::Okay;
   bool inflate_end = false;
   Self->FileIndex = 0;

   CompressionFeedback feedback;
   clearmem(&feedback, sizeof(feedback));

   for (auto &zf : Self->Files) {
      log.trace("Found %s", zf.Name);
      if (wildcmp(Args->Path, zf.Name)) {
         log.trace("Extracting \"%s\"", zf.Name);

         if (Self->OutputID) {
            std::ostringstream out;
            out << "  " << zf.Name;
            print(Self, out.str());
         }

         // If the destination path specifies a folder, add the name of the file to the destination to generate the
         // correct file name.

         destpath.resize(dest_len);
         if (destpath.ends_with('/') or destpath.ends_with('\\') or destpath.ends_with(':')) {
            destpath.append(zf.Name, pathend);
         }

         // If the destination is a folder that already exists, skip this compression entry

         if (destpath.ends_with('/') or destpath.ends_with('\\')) {
            LOC result;
            if ((AnalysePath(destpath.c_str(), &result) IS ERR::Okay) and (result IS LOC::DIRECTORY)) {
               Self->FileIndex++;
               continue;
            }
         }

         // Send compression feedback

         feedback.Year   = 1980 + ((zf.TimeStamp>>25) & 0x3f);
         feedback.Month  = (zf.TimeStamp>>21) & 0x0f;
         feedback.Day    = (zf.TimeStamp>>16) & 0x1f;
         feedback.Hour   = (zf.TimeStamp>>11) & 0x1f;
         feedback.Minute = (zf.TimeStamp>>5)  & 0x3f;
         feedback.Second = (zf.TimeStamp>>1)  & 0x0f;
         feedback.FeedbackID     = FDB::DECOMPRESS_FILE;
         feedback.Index          = Self->FileIndex;
         feedback.Path           = zf.Name.c_str();
         feedback.Dest           = destpath.c_str();
         feedback.OriginalSize   = zf.OriginalSize;
         feedback.CompressedSize = zf.CompressedSize;
         feedback.Progress       = 0;

         error = send_feedback(Self, &feedback);
         if ((error IS ERR::Terminate) or (error IS ERR::Cancelled)) {
            error = ERR::Cancelled;
            goto exit;
         }
         else if (error IS ERR::Skip) {
            error = ERR::Okay;
            Self->FileIndex++; // Increase counter to show that the file was analysed
            continue;
         }
         else error = ERR::Okay;

         // Seek to the start of the compressed data

         if (acSeek(Self->FileIO, zf.Offset + HEAD_NAMELEN, SEEK::START) != ERR::Okay) {
            error = log.warning(ERR::Seek);
            goto exit;
         }

         uint16_t namelen, extralen;
         if (fl::ReadLE(Self->FileIO, &namelen) != ERR::Okay) { error = ERR::Read; goto exit; }
         if (fl::ReadLE(Self->FileIO, &extralen) != ERR::Okay) { error = ERR::Read; goto exit; }
         if (acSeek(Self->FileIO, namelen + extralen, SEEK::CURRENT) != ERR::Okay) {
            error = log.warning(ERR::Seek);
            goto exit;
         }

         if (zf.Flags & ZIP_LINK) {
            // For symbolic links, decompress the data to get the destination link string

            Self->Zip.next_in   = 0;
            Self->Zip.avail_in  = 0;
            Self->Zip.next_out  = 0;
            Self->Zip.avail_out = 0;

            if (zf.CompressedSize > 0) {
               if (zf.DeflateMethod IS 0) {
                  // This routine is used if the file is stored rather than compressed

                  struct acRead read = { .Buffer = Self->Input, .Length = SIZE_COMPRESSION_BUFFER-1 };
                  if ((error = Action(AC::Read, Self->FileIO, &read)) IS ERR::Okay) {
                     Self->Input[read.Result] = 0;
                     DeleteFile(destpath.c_str(), nullptr);
                     error = CreateLink(destpath.c_str(), (CSTRING)Self->Input);
                     if (error IS ERR::NoSupport) error = ERR::Okay;
                  }

                  if (error != ERR::Okay) goto exit;
               }
               else if ((zf.DeflateMethod IS 8) and (inflateInit2(&Self->Zip, -MAX_WBITS) IS Z_OK)) {
                  // Decompressing a link

                  inflate_end = true;

                  struct acRead read;
                  read.Buffer = Self->Input;
                  if (zf.CompressedSize < SIZE_COMPRESSION_BUFFER) read.Length = zf.CompressedSize;
                  else read.Length = SIZE_COMPRESSION_BUFFER;

                  auto err = Z_OK;
                  if ((error = Action(AC::Read, Self->FileIO, &read)) != ERR::Okay) goto exit;
                  if (read.Result <= 0) { error = ERR::Read; goto exit; }

                  Self->Zip.next_in   = Self->Input;
                  Self->Zip.avail_in  = read.Result;
                  Self->Zip.next_out  = Self->Output;
                  Self->Zip.avail_out = SIZE_COMPRESSION_BUFFER-1;

                  err = inflate(&Self->Zip, Z_SYNC_FLUSH);

                  if ((err != Z_OK) and (err != Z_STREAM_END)) {
                     if (Self->Zip.msg) log.warning("%s", Self->Zip.msg);
                     error = ERR::InvalidCompression;
                     goto exit;
                  }

                  Self->Output[zf.OriginalSize] = 0; // !!! We should terminate according to the amount of data decompressed
                  DeleteFile(destpath.c_str(), nullptr);
                  error = CreateLink(destpath.c_str(), (CSTRING)Self->Output);
                  if (error IS ERR::NoSupport) error = ERR::Okay;

                  inflateEnd(&Self->Zip);
                  inflate_end = false;
               }
            }
         }
         else {
            // Create the destination file or folder

            PERMIT permissions;

            if ((Self->Flags & CMF::APPLY_SECURITY) != CMF::NIL) {
               if (zf.Flags & ZIP_SECURITY) {
                  permissions = PERMIT::NIL;
                  if (zf.Flags & ZIP_UEXEC) permissions |= PERMIT::USER_EXEC;
                  if (zf.Flags & ZIP_GEXEC) permissions |= PERMIT::GROUP_EXEC;
                  if (zf.Flags & ZIP_OEXEC) permissions |= PERMIT::OTHERS_EXEC;

                  if (zf.Flags & ZIP_UREAD) permissions |= PERMIT::USER_READ;
                  if (zf.Flags & ZIP_GREAD) permissions |= PERMIT::GROUP_READ;
                  if (zf.Flags & ZIP_OREAD) permissions |= PERMIT::OTHERS_READ;

                  if (zf.Flags & ZIP_UWRITE) permissions |= PERMIT::USER_WRITE;
                  if (zf.Flags & ZIP_GWRITE) permissions |= PERMIT::GROUP_WRITE;
                  if (zf.Flags & ZIP_OWRITE) permissions |= PERMIT::OTHERS_WRITE;
               }
               else permissions = Self->Permissions;
            }
            else permissions = Self->Permissions;

            auto file = objFile::create {
               fl::Path(destpath), fl::Flags(FL::NEW|FL::WRITE), fl::Permissions(permissions)
            };

            if (!file.ok()) {
               log.warning("Error %d creating file \"%s\".", int(file.error), destpath.c_str());
               error = ERR::File;
               goto exit;
            }

            Self->Zip.next_in   = 0;
            Self->Zip.avail_in  = 0;
            Self->Zip.next_out  = 0;
            Self->Zip.avail_out = 0;

            if ((zf.CompressedSize > 0) and ((file->Flags & FL::FILE) != FL::NIL)) {
               if (zf.DeflateMethod IS 0) {
                  // This routine is used if the file is stored rather than compressed

                  log.trace("Extracting file without compression.");

                  int inputlen = zf.CompressedSize;

                  struct acRead read = {
                     .Buffer = Self->Input,
                     .Length = (inputlen < SIZE_COMPRESSION_BUFFER) ? inputlen : SIZE_COMPRESSION_BUFFER
                  };

                  while (((error = Action(AC::Read, Self->FileIO, &read)) IS ERR::Okay) and (read.Result > 0)) {
                     struct acWrite write = { .Buffer = Self->Input, .Length = read.Result };
                     if (Action(AC::Write, *file, &write) != ERR::Okay) { error = log.warning(ERR::Write); goto exit; }

                     inputlen -= read.Result;
                     if (inputlen <= 0) break;
                     if (inputlen < SIZE_COMPRESSION_BUFFER) read.Length = inputlen;
                     else read.Length = SIZE_COMPRESSION_BUFFER;
                  }

                  if (error != ERR::Okay) goto exit;
               }
               else if ((zf.DeflateMethod IS 8) and (inflateInit2(&Self->Zip, -MAX_WBITS) IS Z_OK)) {
                  // Decompressing a file

                  log.trace("Inflating file from %d -> %d bytes @ offset %d.", zf.CompressedSize, zf.OriginalSize, zf.Offset);

                  inflate_end = true;

                  struct acRead read = {
                     .Buffer = Self->Input,
                     .Length = (zf.CompressedSize < SIZE_COMPRESSION_BUFFER) ? (int)zf.CompressedSize : SIZE_COMPRESSION_BUFFER
                  };

                  if ((error = Action(AC::Read, Self->FileIO, &read)) != ERR::Okay) goto exit;
                  if (read.Result <= 0) { error = ERR::Read; goto exit; }
                  int inputlen = zf.CompressedSize - read.Result;

                  Self->Zip.next_in   = Self->Input;
                  Self->Zip.avail_in  = read.Result;
                  Self->Zip.next_out  = Self->Output;
                  Self->Zip.avail_out = SIZE_COMPRESSION_BUFFER;

                  // Keep loooping until Z_STREAM_END or an error is returned

                  auto err = Z_OK;
                  while (err IS Z_OK) {
                     err = inflate(&Self->Zip, Z_SYNC_FLUSH);

                     if ((err != Z_OK) and (err != Z_STREAM_END)) {
                        if (Self->Zip.msg) log.warning("%s", Self->Zip.msg);
                        error = ERR::InvalidCompression;
                        goto exit;
                     }

                     // Write out the decompressed data

                     struct acWrite write = {
                        .Buffer = Self->Output,
                        .Length = (int)(SIZE_COMPRESSION_BUFFER - Self->Zip.avail_out)
                     };
                     if (Action(AC::Write, *file, &write) != ERR::Okay) { error = log.warning(ERR::Write); goto exit; }

                     // Exit if all data has been written out

                     if (Self->Zip.total_out IS zf.OriginalSize) break;

                     feedback.Progress = Self->Zip.total_out;
                     send_feedback(Self, &feedback);

                     // Reset the output buffer

                     Self->Zip.next_out  = Self->Output;
                     Self->Zip.avail_out = SIZE_COMPRESSION_BUFFER;

                     // Read more data if necessary

                     if ((Self->Zip.avail_in <= 0) and (inputlen > 0)) {
                        if (inputlen < SIZE_COMPRESSION_BUFFER) read.Length = inputlen;
                        else read.Length = SIZE_COMPRESSION_BUFFER;

                        if ((error = Action(AC::Read, Self->FileIO, &read)) != ERR::Okay) goto exit;
                        if (read.Result <= 0) { error = ERR::Read; break; }
                        inputlen -= read.Result;

                        Self->Zip.next_in  = Self->Input;
                        Self->Zip.avail_in = read.Result;
                     }
                  }

                  // Terminate the inflation process

                  inflateEnd(&Self->Zip);
                  inflate_end = false;
               }
            }

            // Give the file a date that matches the original

            file->setDate(feedback.Year, feedback.Month, feedback.Day, feedback.Hour, feedback.Minute, feedback.Second, FDT::NIL);
         }

         if (feedback.Progress < feedback.OriginalSize) {
            feedback.Progress = feedback.OriginalSize;
            send_feedback(Self, &feedback);
         }

         Self->FileIndex++;
      }
   }

   if (Self->OutputID) print(Self, "\nDecompression complete.");

exit:
   if (inflate_end) inflateEnd(&Self->Zip);

   if ((error IS ERR::Okay) and (Self->FileIndex <= 0)) {
      log.msg("No files matched the path \"%s\".", Args->Path);
      error = ERR::Search;
   }

   return error;
}

/*********************************************************************************************************************

-METHOD-
DecompressObject: Decompresses one file to a target object.

The DecompressObject method will decompress a file to a target object, using a series of #Write() calls.

This method sends feedback at regular intervals during decompression.  For further information on receiving feedback,
please refer to the #Feedback field.

Note that if decompressing to a @File object, the seek position will point to the end of the file after this
method returns.  Reset the seek position to zero if the decompressed data needs to be read back.

-INPUT-
cstr Path: The location of the source file within the archive.  If a wildcard is used, the first matching file is extracted.
obj Object: The target object for the decompressed source data.

-ERRORS-
Okay
NullArgs
MissingPath
Seek
Write
Failed

*********************************************************************************************************************/

static ERR COMPRESSION_DecompressObject(extCompression *Self, struct cmp::DecompressObject *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Path) or (!Args->Path[0])) return log.warning(ERR::NullArgs);
   if (!Args->Object) return log.warning(ERR::NullArgs);
   if (!Self->FileIO) return log.warning(ERR::MissingPath);
   if (Self->isSubClass()) return ERR::NoSupport; // Object belongs to a Compression sub-class

   log.branch("%s TO %p, Permissions: $%.8x", Args->Path, Args->Object, int(Self->Permissions));

   bool inflate_end = false;
   Self->FileIndex = 0;

   CompressionFeedback fb;
   clearmem(&fb, sizeof(fb));

   ERR error = ERR::Okay;
   int total_scanned = 0;
   for (auto &list : Self->Files) {
      total_scanned++;
      if (!wildcmp(Args->Path, list.Name)) continue;

      log.trace("Decompressing \"%s\"", list.Name);

      // Send compression feedback

      fb.Year   = 1980 + ((list.TimeStamp>>25) & 0x3f);
      fb.Month  = (list.TimeStamp>>21) & 0x0f;
      fb.Day    = (list.TimeStamp>>16) & 0x1f;
      fb.Hour   = (list.TimeStamp>>11) & 0x1f;
      fb.Minute = (list.TimeStamp>>5)  & 0x3f;
      fb.Second = (list.TimeStamp>>1)  & 0x0f;
      fb.FeedbackID     = FDB::DECOMPRESS_OBJECT;
      fb.Index          = Self->FileIndex;
      fb.Path           = list.Name.c_str();
      fb.Dest           = nullptr;
      fb.OriginalSize   = list.OriginalSize;
      fb.CompressedSize = list.CompressedSize;
      fb.Progress       = 0;

      send_feedback(Self, &fb);

      // Seek to the start of the compressed data

      if (acSeek(Self->FileIO, list.Offset + HEAD_NAMELEN, SEEK::START) != ERR::Okay) {
         return log.warning(ERR::Seek);
      }

      uint16_t namelen, extralen;
      if (fl::ReadLE(Self->FileIO, &namelen) != ERR::Okay) return ERR::Read;
      if (fl::ReadLE(Self->FileIO, &extralen) != ERR::Okay) return ERR::Read;
      if (acSeek(Self->FileIO, namelen + extralen, SEEK::CURRENT) != ERR::Okay) {
         return log.warning(ERR::Seek);
      }

      if (list.Flags & ZIP_LINK) { // For symbolic links, decompress the data to get the destination link string
         log.warning("Unable to unzip symbolic link %s (flags $%.8x), size %d.", list.Name.c_str(), list.Flags, list.OriginalSize);
         return ERR::InvalidCompression;
      }
      else { // Create the destination file or folder
         Self->Zip.next_in   = 0;
         Self->Zip.avail_in  = 0;
         Self->Zip.next_out  = 0;
         Self->Zip.avail_out = 0;

         if (list.CompressedSize > 0) {
            if (list.DeflateMethod IS 0) {
               // This routine is used if the file is stored rather than compressed

               int inputlen = list.CompressedSize;

               struct acRead read = { .Buffer = Self->Input };
               if (inputlen < SIZE_COMPRESSION_BUFFER) read.Length = inputlen;
               else read.Length = SIZE_COMPRESSION_BUFFER;

               while (((error = Action(AC::Read, Self->FileIO, &read)) IS ERR::Okay) and (read.Result > 0)) {
                  struct acWrite write = { .Buffer = Self->Input, .Length = read.Result };
                  if (Action(AC::Write, Args->Object, &write) != ERR::Okay) { error = ERR::Write; goto exit; }

                  inputlen -= read.Result;
                  if (inputlen <= 0) break;
                  if (inputlen < SIZE_COMPRESSION_BUFFER) read.Length = inputlen;
                  else read.Length = SIZE_COMPRESSION_BUFFER;
               }

               if (error != ERR::Okay) goto exit;
            }
            else if ((list.DeflateMethod IS 8) and (inflateInit2(&Self->Zip, -MAX_WBITS) IS Z_OK)) {
               // Decompressing a file

               inflate_end = true;

               struct acRead read;
               read.Buffer = Self->Input;
               if (list.CompressedSize < SIZE_COMPRESSION_BUFFER) read.Length = list.CompressedSize;
               else read.Length = SIZE_COMPRESSION_BUFFER;

               auto err = Z_OK;
               if ((error = Action(AC::Read, Self->FileIO, &read)) != ERR::Okay) goto exit;
               if (read.Result <= 0) { error = ERR::Read; goto exit; }
               int inputlen = list.CompressedSize - read.Result;

               Self->Zip.next_in   = Self->Input;
               Self->Zip.avail_in  = read.Result;
               Self->Zip.next_out  = Self->Output;
               Self->Zip.avail_out = SIZE_COMPRESSION_BUFFER;

               // Keep loooping until Z_STREAM_END or an error is returned

               while (err IS Z_OK) {
                  err = inflate(&Self->Zip, Z_SYNC_FLUSH);

                  if ((err != Z_OK) and (err != Z_STREAM_END)) {
                     if (Self->Zip.msg) log.warning("%s", Self->Zip.msg);
                     error = ERR::Decompression;
                     goto exit;
                  }

                  // Write out the decompressed data

                  struct acWrite write = { Self->Output, (int)(SIZE_COMPRESSION_BUFFER - Self->Zip.avail_out) };
                  if (Action(AC::Write, Args->Object, &write) != ERR::Okay) { error = ERR::Write; goto exit; }

                  // Exit if all data has been written out

                  if (Self->Zip.total_out IS list.OriginalSize) break;

                  fb.Progress = Self->Zip.total_out;
                  send_feedback(Self, &fb);

                  // Reset the output buffer

                  Self->Zip.next_out  = Self->Output;
                  Self->Zip.avail_out = SIZE_COMPRESSION_BUFFER;

                  // Read more data if necessary

                  if ((Self->Zip.avail_in <= 0) and (inputlen > 0)) {
                     if (inputlen < SIZE_COMPRESSION_BUFFER) read.Length = inputlen;
                     else read.Length = SIZE_COMPRESSION_BUFFER;

                     if ((error = Action(AC::Read, Self->FileIO, &read)) != ERR::Okay) goto exit;
                     if (read.Result <= 0) { error = ERR::Read; break; }
                     inputlen -= read.Result;

                     Self->Zip.next_in  = Self->Input;
                     Self->Zip.avail_in = read.Result;
                  }
               }

               // Terminate the inflation process

               inflateEnd(&Self->Zip);
               inflate_end = false;
            }
         }
      }

      if (fb.Progress < fb.OriginalSize) {
         fb.Progress = fb.OriginalSize;
         send_feedback(Self, &fb);
      }

      Self->FileIndex++;
      break;
   }

   if (error != ERR::Okay) {
      log.msg("No files matched the path \"%s\" from %d files.", Args->Path, total_scanned);
      return ERR::Search;
   }

exit:
   if (inflate_end) inflateEnd(&Self->Zip);
   if (error != ERR::Okay) log.warning(error);
   return error;
}

/*********************************************************************************************************************

-METHOD-
Find: Find the first item that matches a given filter.

Use the Find() method to search for a specific item in an archive.  The algorithm will return the first item that
matches the `Path` string in conjunction with the `Case` and `Wildcard` options.

If successful, the discovered item is returned as a !CompressedItem.  The result is temporary and
values will be discarded on the next call to this method.  If persistent values are required, copy the resulting
structure immediately after the call.

-INPUT-
cstr Path: Search for a specific item or items, using wildcards.
int CaseSensitive: Set to `true` if `Path` comparisons are case-sensitive.
int Wildcard: Set to `true` if `Path` uses wildcards.
&struct(*CompressedItem) Item: The discovered item is returned in this parameter, or `NULL` if the search failed.

-ERRORS-
Okay
NoSupport
NullArgs
Search

*********************************************************************************************************************/

static thread_local CompressedItem glFindMeta;

static ERR COMPRESSION_Find(extCompression *Self, struct cmp::Find *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Path)) return log.warning(ERR::NullArgs);
   if (Self->isSubClass()) return ERR::NoSupport;

   log.traceBranch("Path: %s, Case: %d, Wildcard: %d", Args->Path, Args->CaseSensitive, Args->Wildcard);
   for (auto &item : Self->Files) {
      if (Args->Wildcard) {
         if (!wildcmp(Args->Path, item.Name, Args->CaseSensitive)) continue;
      }
      else if (Args->CaseSensitive) {
         if (item.Name != Args->Path) continue;
      }
      else if (!iequals(item.Name, Args->Path)) continue;

      zipfile_to_item(item, glFindMeta);
      Args->Item = &glFindMeta;
      return ERR::Okay;
   }

   return ERR::Search;
}

/*********************************************************************************************************************
-ACTION-
Flush: Flushes all pending actions.
-END-
*********************************************************************************************************************/

static ERR COMPRESSION_Flush(extCompression *Self)
{
   if (Self->isSubClass()) return ERR::Okay;

   Self->Zip.avail_in = 0;

   bool done = false;

   for (;;) {
      // Write out any bytes that are still left in the compression buffer

      int length, zerror;
      if ((length = SIZE_COMPRESSION_BUFFER - Self->Zip.avail_out) > 0) {
         struct acWrite write = { Self->Output, length };
         if (Action(AC::Write, Self->FileIO, &write) != ERR::Okay) return ERR::Write;
         Self->Zip.next_out  = Self->Output;
         Self->Zip.avail_out = SIZE_COMPRESSION_BUFFER;
      }

      if (done) break;

      zerror = deflate(&Self->Zip, Z_FINISH);

      // Ignore the second of two consecutive flushes:

      if ((!length) and (zerror IS Z_BUF_ERROR)) zerror = Z_OK;

      done = ((Self->Zip.avail_out != 0) or (zerror IS Z_STREAM_END));

      if ((zerror != Z_OK) and (zerror != Z_STREAM_END)) break;
   }

   acFlush(Self->FileIO);

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR COMPRESSION_Free(extCompression *Self)
{
   // Before terminating anything, write the EOF signature (if modifications have been made).

   write_eof(Self);

   if (Self->ArchiveHash)  {
      remove_archive(Self);
      Self->ArchiveHash = 0;
   }

   if (Self->Feedback.isScript()) {
      UnsubscribeAction(Self->Feedback.Context, AC::Free);
      Self->Feedback.clear();
   }

   if (Self->Inflating)    { inflateEnd(&Self->InflateStream); Self->Inflating = false; }
   if (Self->Deflating)    { deflateEnd(&Self->DeflateStream); Self->Deflating = false; }
   if (Self->OutputBuffer) { FreeResource(Self->OutputBuffer); Self->OutputBuffer = nullptr; }
   if (Self->Input)        { FreeResource(Self->Input); Self->Input = nullptr; }
   if (Self->Output)       { FreeResource(Self->Output); Self->Output = nullptr; }
   if (Self->FileIO)       { FreeResource(Self->FileIO); Self->FileIO = nullptr; }
   if (Self->Path)         { FreeResource(Self->Path); Self->Path = nullptr; }

   Self->~extCompression();

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR COMPRESSION_Init(extCompression *Self)
{
   pf::Log log;
   auto path = Self->get<STRING>(FID_Path);

   if (!path) {
      // If no location has been set, assume that the developer only wants to use the buffer or stream compression routines.

      return ERR::Okay;
   }
   else if ((Self->Flags & CMF::NEW) != CMF::NIL) {
      // If the NEW flag is set then create a new archive, destroying any file already at that location

      if ((Self->FileIO = objFile::create::local(fl::Path(path), fl::Flags(FL::READ|FL::WRITE|FL::NEW)))) {
         return ERR::Okay;
      }
      else {
         if (Self->OutputID) {
            std::ostringstream out;
            out << "Failed to create file \"" << path << "\".";
            print(Self, out.str());
         }

         return log.warning(ERR::CreateObject);
      }
   }
   else {
      ERR error = ERR::Okay;
      LOC type;
      bool exists = ((AnalysePath(path, &type) IS ERR::Okay) and (type IS LOC::FILE));

      if (exists) {
         pf::Create<objFile> file({
            fl::Path(path),
            fl::Flags(FL::READ|FL::APPROXIMATE|(((Self->Flags & CMF::READ_ONLY) != CMF::NIL) ? FL::NIL : FL::WRITE))
         }, NF::LOCAL);

         // Try switching to read-only access if we were denied permission.

         if (file.ok()) Self->FileIO = *file;
         else if ((file.error IS ERR::NoPermission) and ((Self->Flags & CMF::READ_ONLY) IS CMF::NIL)) {
            log.trace("Trying read-only access...");

            if ((Self->FileIO = objFile::create::local(fl::Path(path), fl::Flags(FL::READ|FL::APPROXIMATE)))) {
               Self->Flags |= CMF::READ_ONLY;
            }
            else error = ERR::File;
         }
         else error = ERR::File;
      }
      else error = ERR::DoesNotExist;

      if (error IS ERR::Okay) { // Test the given location to see if it matches our supported file format (pkzip).
         int result;
         if (acRead(Self->FileIO, Self->Header, sizeof(Self->Header), &result) != ERR::Okay) return log.warning(ERR::Read);

         // If the file is empty then we will accept it as a zip file

         if (!result) return ERR::Okay;

         // Check for a pkzip header

         if ((Self->Header[0] IS 0x50) and (Self->Header[1] IS 0x4b) and
             (Self->Header[2] IS 0x03) and (Self->Header[3] IS 0x04)) {
            error = fast_scan_zip(Self);
            if (error != ERR::Okay) return log.warning(error);
            else return error;
         }
         else return ERR::NoSupport;
      }
      else if ((!exists) and ((Self->Flags & CMF::CREATE_FILE) != CMF::NIL)) {
         // Create a new file if the requested location does not exist

         log.detail("Creating a new file because the location does not exist.");

         if ((Self->FileIO = objFile::create::local(fl::Path(path), fl::Flags(FL::READ|FL::WRITE|FL::NEW)))) {
            return ERR::Okay;
         }
         else {
            if (Self->OutputID) {
               std::ostringstream out;
               out << "Failed to create file \"" << path << "\".";
               print(Self, out.str());
            }

            return log.warning(ERR::CreateObject);
         }
      }
      else {
         std::ostringstream out;
         out << "Failed to open \"" << path << "\".";
         print(Self, out.str());
         return log.warning(ERR::File);
      }
   }
}

//********************************************************************************************************************

static ERR COMPRESSION_NewObject(extCompression *Self)
{
   pf::Log log;

   if (AllocMemory(SIZE_COMPRESSION_BUFFER, MEM::DATA, (APTR *)&Self->Output, nullptr) IS ERR::Okay) {
      if (AllocMemory(SIZE_COMPRESSION_BUFFER, MEM::DATA, (APTR *)&Self->Input, nullptr) IS ERR::Okay) {
         Self->CompressionLevel = 60; // 60% compression by default
         Self->Permissions      = PERMIT::NIL; // Inherit permissions by default. PERMIT::READ|PERMIT::WRITE|PERMIT::GROUP_READ|PERMIT::GROUP_WRITE;
         Self->MinOutputSize    = (32 * 1024) + 2048; // Has to at least match the minimum 'window size' of each compression block, plus extra in case of overflow.  Min window size is typically 16k
         Self->WindowBits = MAX_WBITS; // If negative then you get raw compression when dealing with buffers and stream data, i.e. no header information
         return ERR::Okay;
      }
      else return log.warning(ERR::AllocMemory);
   }
   else return log.warning(ERR::AllocMemory);
}

static ERR COMPRESSION_NewPlacement(extCompression *Self)
{
   new (Self) extCompression;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
RemoveFile: Deletes one or more files from a compression object.

This method deletes compressed files from a compression object.  If the file is in a folder then the client must
specify the complete path in conjunction with the file name.  Wild cards are accepted if you want to delete multiple
files.  A `Path` setting of `*` will delete an archive's entire contents, while a more conservative `Path` of
`documents/ *` would delete all files and directories under the documents path.  Directories can be declared using
either the back-slash or the forward-slash characters.

Depending on internal optimisation techniques, the compressed file may not shrink from deletions until the compression
object is closed or the #Flush() action is called.

-INPUT-
cstr Path: The full path name of the file to delete from the archive.

-ERRORS-
Okay: The file was successfully deleted.
NullArgs
NoSupport
-END-

*********************************************************************************************************************/

static ERR COMPRESSION_RemoveFile(extCompression *Self, struct cmp::RemoveFile *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Path)) return log.warning(ERR::NullArgs);

   if (Self->isSubClass()) return ERR::NoSupport;

   // Search for the file(s) in our archive that match the given name and delete them.

   log.msg("%s", Args->Path);

   for (auto it = Self->Files.begin(); it != Self->Files.end(); ) {
      if (wildcmp(Args->Path, it->Name)) {
         // Delete the file from the archive

         if (Self->OutputID) {
            std::ostringstream out;
            out << "Removing file \"" << it->Name << "\".";
            print(Self, out.str());
         }

         if (auto error = remove_file(Self, it); error != ERR::Okay) return error;
      }
      else it++;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Scan: Scan the archive's index of compressed data.

Use the Scan() method to search an archive's list of items.  Optional filtering can be applied using the `Folder` parameter
to limit results to those within a folder, and `Filter` parameter to apply wildcard matching to item names.  Each item
that is discovered during the scan will be passed to the function referenced in the `Callback` parameter.  If the
`Callback` function returns `ERR::Terminate`, the scan will stop immediately.  The prototype of the `Callback`
function is `ERR Function(*Compression, *CompressedItem)`.

The !CompressedItem structure consists of the following fields:

!CompressedItem

To search for a single item with a path and name already known, use the #Find() method instead.

-INPUT-
cstr Folder: If defined, only items within the specified folder are returned.  Use an empty string for files in the root folder.
cstr Filter: Search for a specific item or items by name, using wildcards.  If `NULL` or an empty string, all items will be scanned.
ptr(func) Callback: This callback function will be called with a pointer to a !CompressedItem structure.

-ERRORS-
Okay
NoSupport
NullArgs
-END-

*********************************************************************************************************************/

static ERR COMPRESSION_Scan(extCompression *Self, struct cmp::Scan *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Callback)) return log.warning(ERR::NullArgs);

   if (Self->isSubClass()) return ERR::NoSupport;

   log.traceBranch("Folder: \"%s\", Filter: \"%s\"", Args->Folder, Args->Filter);

   int folder_len = 0;
   if (Args->Folder) {
      folder_len = strlen(Args->Folder);
      if ((folder_len > 0) and (Args->Folder[folder_len-1] IS '/')) folder_len--;
   }

   ERR error = ERR::Okay;

   for (auto &item : Self->Files) {
      log.trace("Item: %s", item.Name);

      if (Args->Folder) {
         if (std::ssize(item.Name) > folder_len) {
            if (iequals(Args->Folder, item.Name)) {
               if ((folder_len > 0) and (item.Name[folder_len] != '/')) continue;
               if ((item.Name[folder_len] IS '/') and (!item.Name[folder_len+1])) continue;

               // Skip this item if it is within other sub-folders.

               int i;
               for (i=folder_len+1; item.Name[i]; i++) {
                  if (item.Name[i] IS '/') break;
               }
               if (item.Name[i] IS '/') continue;
            }
            else continue;
         }
         else continue;
      }

      if ((Args->Filter) and (Args->Filter[0])) {
         if (wildcmp(Args->Filter, item.Name)) break;
         else continue;
      }

      CompressedItem meta;
      zipfile_to_item(item, meta);

      {
         if (Args->Callback->isC()) {
            pf::SwitchContext context(Args->Callback->Context);
            auto routine = (ERR (*)(extCompression *, CompressedItem *, APTR))Args->Callback->Routine;
            error = routine(Self, &meta, Args->Callback->Meta);
         }
         else if (Args->Callback->isScript()) {
            if (sc::Call(*Args->Callback, std::to_array<ScriptArg>({
               { "Compression", Self, FD_OBJECTPTR },
               { "CompressedItem:Item", &meta, FD_STRUCT|FD_PTR }
            }), error) != ERR::Okay) error = ERR::Function;
         }
         else error = log.warning(ERR::WrongType);

         if (error != ERR::Okay) break; // Break the scanning loop.
      }
   }

   return error;
}

//********************************************************************************************************************

#include "compression_fields.cpp"
#include "compression_func.cpp"

static const FieldDef clPermissionFlags[] = {
   { "Read",         PERMIT::READ },
   { "Write",        PERMIT::WRITE },
   { "Exec",         PERMIT::EXEC },
   { "Executable",   PERMIT::EXEC },
   { "Delete",       PERMIT::DELETE },
   { "Hidden",       PERMIT::HIDDEN },
   { "Archive",      PERMIT::ARCHIVE },
   { "Password",     PERMIT::PASSWORD },
   { "UserID",       PERMIT::USERID },
   { "GroupID",      PERMIT::GROUPID },
   { "OthersRead",   PERMIT::OTHERS_READ },
   { "OthersWrite",  PERMIT::OTHERS_WRITE },
   { "OthersExec",   PERMIT::OTHERS_EXEC },
   { "OthersDelete", PERMIT::OTHERS_DELETE },
   { "GroupRead",    PERMIT::GROUP_READ },
   { "GroupWrite",   PERMIT::GROUP_WRITE },
   { "GroupExec",    PERMIT::GROUP_EXEC },
   { "GroupDelete",  PERMIT::GROUP_DELETE },
   { "AllRead",      PERMIT::ALL_READ },
   { "AllWrite",     PERMIT::ALL_WRITE },
   { "AllExec",      PERMIT::ALL_EXEC },
   { nullptr, 0 }
};

#include "class_compression_def.c"

static const FieldArray clFields[] = {
   { "TotalOutput",      FDF_INT64|FDF_R },
   { "Output",           FDF_OBJECTID|FDF_RI },
   { "CompressionLevel", FDF_INT|FDF_RW, nullptr, SET_CompressionLevel },
   { "Flags",            FDF_INTFLAGS|FDF_RW, nullptr, nullptr, &clCompressionFlags },
   { "SegmentSize",      FDF_INT|FDF_SYSTEM|FDF_RW },
   { "Permissions",      FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clPermissionFlags },
   { "MinOutputSize",    FDF_INT|FDF_R },
   { "WindowBits",       FDF_INT|FDF_RW, nullptr, SET_WindowBits },
   // Virtual fields
   { "ArchiveName",      FDF_STRING|FDF_W,       nullptr, SET_ArchiveName },
   { "Path",             FDF_STRING|FDF_RW,      GET_Path, SET_Path },
   { "Feedback",         FDF_FUNCTIONPTR|FDF_RW, GET_Feedback, SET_Feedback },
   { "Header",           FDF_POINTER|FDF_R,      GET_Header },
   { "Password",         FDF_STRING|FDF_RW,      GET_Password, SET_Password },
   { "Size",             FDF_INT64|FDF_R,        GET_Size },
   { "Src",              FDF_SYNONYM|FDF_STRING|FDF_RW, GET_Path, SET_Path },
   { "UncompressedSize", FDF_INT64|FDF_R,        GET_UncompressedSize },
   END_FIELD
};

//********************************************************************************************************************

extern ERR add_compression_class(void)
{
   glCompressionClass = extMetaClass::create::global(
      fl::ClassVersion(VER_COMPRESSION),
      fl::Name("Compression"),
      fl::FileExtension("*.zip"),
      fl::FileDescription("ZIP File"),
      fl::FileHeader("[0:$504b0304]"),
      fl::Icon("filetypes/archive"),
      fl::Category(CCF::DATA),
      fl::Actions(clCompressionActions),
      fl::Methods(clCompressionMethods),
      fl::Fields(clFields),
      fl::Size(sizeof(extCompression)),
      fl::Path("modules:core"));

   return glCompressionClass ? ERR::Okay : ERR::AddClass;
}

//********************************************************************************************************************

#include "class_archive.cpp"
#include "class_compressed_stream.cpp"

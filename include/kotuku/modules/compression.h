#pragma once

// Copyright: Paul Manias © 1996-2026
// Generator: idl-c

#include <kotuku/main.h>

class objCompression;
class objCompressedStream;

// Compression flags

enum class CMF : uint32_t {
   NIL = 0,
   PASSWORD = 0x00000001,
   NEW = 0x00000002,
   CREATE_FILE = 0x00000004,
   READ_ONLY = 0x00000008,
   NO_LINKS = 0x00000010,
   APPLY_SECURITY = 0x00000020,
};

DEFINE_ENUM_FLAG_OPERATORS(CMF)

// Compression stream formats

enum class CF : int {
   NIL = 0,
   GZIP = 1,
   ZLIB = 2,
   DEFLATE = 3,
};

// Compression feedback event indicators.

enum class FDB : int {
   NIL = 0,
   DECOMPRESS_FILE = 1,
   COMPRESS_FILE = 2,
   REMOVE_FILE = 3,
   DECOMPRESS_OBJECT = 4,
};

struct CompressionFeedback {
   FDB     FeedbackID;      // Set to one of the FDB event indicators
   int     Index;           // Index of the current file
   CSTRING Path;            // Name of the current file/path in the archive
   CSTRING Dest;            // Destination file/path during decompression
   int64_t Progress;        // Progress indicator (byte position for the file being de/compressed).
   int64_t OriginalSize;    // Original size of the file
   int64_t CompressedSize;  // Compressed size of the file
   int16_t Year;            // Year of the original file's datestamp.
   int16_t Month;           // Month of the original file's datestamp.
   int16_t Day;             // Day of the original file's datestamp.
   int16_t Hour;            // Hour of the original file's datestamp.
   int16_t Minute;          // Minute of the original file's datestamp.
   int16_t Second;          // Second of the original file's datestamp.
   CompressionFeedback() : FeedbackID(FDB::NIL), Index(0), Path(nullptr), Dest(nullptr),
      Progress(0), OriginalSize(0), CompressedSize(0),
      Year(0), Month(0), Day(0), Hour(0), Minute(0), Second(0) { }

   CompressionFeedback(FDB pFeedback, int pIndex, CSTRING pPath, CSTRING pDest) :
      FeedbackID(pFeedback), Index(pIndex), Path(pPath), Dest(pDest),
      Progress(0), OriginalSize(0), CompressedSize(0),
      Year(0), Month(0), Day(0), Hour(0), Minute(0), Second(0) { }
};

struct CompressedItem {
   int64_t OriginalSize;            // Original size of the file
   int64_t CompressedSize;          // Compressed size of the file
   struct CompressedItem * Next;    // Used only if this is a linked-list.
   CSTRING Path;                    // Path to the file (includes folder prefixes).  Archived folders will include the trailing slash.
   PERMIT  Permissions;             // Original permissions - see PERMIT flags.
   int     UserID;                  // Original user ID
   int     GroupID;                 // Original group ID
   int     OthersID;                // Original others ID
   FL      Flags;                   // FL flags
   struct DateTime Created;         // Date and time of the file's creation.
   struct DateTime Modified;        // Date and time last modified.
   ankerl::unordered_dense::map<std::string, std::string> *Tags;
};

// Compression class definition

#define VER_COMPRESSION (1.000000)

// Compression methods

namespace cmp {
struct CompressBuffer { APTR Input; int InputSize; APTR Output; int OutputSize; int Result; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct CompressFile { std::string_view Location; std::string_view Path; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DecompressBuffer { APTR Input; APTR Output; int OutputSize; int Result; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DecompressFile { std::string_view Path; std::string_view Dest; int Flags; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RemoveFile { std::string_view Path; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct CompressStream { APTR Input; int Length; FUNCTION *Callback; APTR Output; int OutputSize; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DecompressStream { APTR Input; int Length; FUNCTION *Callback; APTR Output; int OutputSize; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct CompressStreamStart { static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct CompressStreamEnd { FUNCTION *Callback; APTR Output; int OutputSize; static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DecompressStreamEnd { FUNCTION *Callback; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DecompressStreamStart { static const AC id = AC(-11); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DecompressObject { std::string_view Path; OBJECTPTR Object; static const AC id = AC(-12); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Scan { std::string_view Folder; std::string_view Filter; FUNCTION *Callback; static const AC id = AC(-13); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Find { std::string_view Path; int CaseSensitive; int Wildcard; struct CompressedItem *Item; static const AC id = AC(-14); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objCompression : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::COMPRESSION;
   static constexpr CSTRING CLASS_NAME = "Compression";

   using create = kt::Create<objCompression>;
   objCompression(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   std::string Path;        // Set if the compressed data originates from, or is to be saved to a file source.
   std::string Password;    // Required if an archive needs an encryption password for access.
   int64_t  TotalOutput;    // The total number of bytes that have been output during the compression or decompression of streamed data.
   OBJECTID OutputID;       // Resulting messages will be sent to the object referred to in this field.
   int      CompressionLevel; // The compression level to use when compressing data.
   CMF      Flags;          // Optional flags.
   int      SegmentSize;    // Private. Splits the compressed file if it surpasses a set byte limit.
   PERMIT   Permissions;    // Default permissions for decompressed files are defined here.
   int      MinOutputSize;  // Indicates the minimum output buffer size that will be needed during de/compression.
   int      WindowBits;     // Special option for certain compression formats.

   // Action stubs

   inline ERR flush() noexcept { return Action(AC::Flush, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR compressBuffer(APTR Input, int InputSize, APTR Output, int OutputSize, int * Result) noexcept {
      struct cmp::CompressBuffer args = { Input, InputSize, Output, OutputSize, (int)0 };
      ERR error = Action(AC(-1), this, &args);
      if (Result) *Result = args.Result;
      return error;
   }
   inline ERR compressFile(const std::string_view &Location, const std::string_view &Path) noexcept {
      struct cmp::CompressFile args = { Location, Path };
      return Action(AC(-2), this, &args);
   }
   inline ERR decompressBuffer(APTR Input, APTR Output, int OutputSize, int * Result) noexcept {
      struct cmp::DecompressBuffer args = { Input, Output, OutputSize, (int)0 };
      ERR error = Action(AC(-3), this, &args);
      if (Result) *Result = args.Result;
      return error;
   }
   inline ERR decompressFile(const std::string_view &Path, const std::string_view &Dest, int Flags) noexcept {
      struct cmp::DecompressFile args = { Path, Dest, Flags };
      return Action(AC(-4), this, &args);
   }
   inline ERR removeFile(const std::string_view &Path) noexcept {
      struct cmp::RemoveFile args = { Path };
      return Action(AC(-5), this, &args);
   }
   inline ERR compressStream(APTR Input, int Length, FUNCTION Callback, APTR Output, int OutputSize) noexcept {
      struct cmp::CompressStream args = { Input, Length, &Callback, Output, OutputSize };
      return Action(AC(-6), this, &args);
   }
   inline ERR decompressStream(APTR Input, int Length, FUNCTION Callback, APTR Output, int OutputSize) noexcept {
      struct cmp::DecompressStream args = { Input, Length, &Callback, Output, OutputSize };
      return Action(AC(-7), this, &args);
   }
   inline ERR compressStreamStart() noexcept {
      return Action(AC(-8), this, nullptr);
   }
   inline ERR compressStreamEnd(FUNCTION Callback, APTR Output, int OutputSize) noexcept {
      struct cmp::CompressStreamEnd args = { &Callback, Output, OutputSize };
      return Action(AC(-9), this, &args);
   }
   inline ERR decompressStreamEnd(FUNCTION Callback) noexcept {
      struct cmp::DecompressStreamEnd args = { &Callback };
      return Action(AC(-10), this, &args);
   }
   inline ERR decompressStreamStart() noexcept {
      return Action(AC(-11), this, nullptr);
   }
   inline ERR decompressObject(const std::string_view &Path, OBJECTPTR Object) noexcept {
      struct cmp::DecompressObject args = { Path, Object };
      return Action(AC(-12), this, &args);
   }
   inline ERR scan(const std::string_view &Folder, const std::string_view &Filter, FUNCTION Callback) noexcept {
      struct cmp::Scan args = { Folder, Filter, &Callback };
      return Action(AC(-13), this, &args);
   }
   inline ERR find(const std::string_view &Path, int CaseSensitive, int Wildcard, struct CompressedItem ** Item) noexcept {
      struct cmp::Find args = { Path, CaseSensitive, Wildcard, (struct CompressedItem *)0 };
      ERR error = Action(AC(-14), this, &args);
      if (Item) *Item = args.Item;
      return error;
   }

   // Customised field getting

   inline ERR getPath(std::string_view &Value) noexcept {
      Value = this->Path;
      return ERR::Okay;
   }

   inline ERR getPassword(std::string_view &Value) noexcept {
      Value = this->Password;
      return ERR::Okay;
   }

   inline ERR getTotalOutput(int64_t &Value) noexcept {
      Value = this->TotalOutput;
      return ERR::Okay;
   }

   inline ERR getOutput(OBJECTID &Value) noexcept {
      Value = this->OutputID;
      return ERR::Okay;
   }

   inline ERR getCompressionLevel(int &Value) noexcept {
      Value = this->CompressionLevel;
      return ERR::Okay;
   }

   inline ERR getFlags(CMF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getPermissions(PERMIT &Value) noexcept {
      Value = this->Permissions;
      return ERR::Okay;
   }

   inline ERR getMinOutputSize(int &Value) noexcept {
      Value = this->MinOutputSize;
      return ERR::Okay;
   }

   inline ERR getWindowBits(int &Value) noexcept {
      Value = this->WindowBits;
      return ERR::Okay;
   }

   inline ERR getFeedback(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getSize(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getUncompressedSize(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setPath(const std::string_view &Value) noexcept {
      this->Path = Value;
      return ERR::Okay;
   }

   inline ERR setPassword(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setOutput(OBJECTID Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->OutputID = Value;
      return ERR::Okay;
   }

   inline ERR setCompressionLevel(const int Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setFlags(const CMF Value) noexcept {
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setPermissions(const PERMIT Value) noexcept {
      this->Permissions = Value;
      return ERR::Okay;
   }

   inline ERR setWindowBits(const int Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setArchiveName(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, 0x00804200, &Value);
   }

   inline ERR setFeedback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

};

// CompressedStream class definition

#define VER_COMPRESSEDSTREAM (1.000000)

class objCompressedStream : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::COMPRESSEDSTREAM;
   static constexpr CSTRING CLASS_NAME = "CompressedStream";

   using create = kt::Create<objCompressedStream>;
   objCompressedStream(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   int64_t   TotalOutput;  // A live counter of total bytes that have been output by the stream.
   OBJECTPTR Input;        // An input object that will supply data for decompression.
   OBJECTPTR Output;       // A target object that will receive data compressed by the stream.
   CF        Format;       // The format of the compressed stream.  The default is GZIP.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }
   template <class T, class U> ERR read(APTR Buffer, T Size, U *Result) noexcept {
      static_assert(std::is_integral<U>::value, "Result value must be an integer type");
      static_assert(std::is_integral<T>::value, "Size value must be an integer type");
      const int bytes = (Size > 0x7fffffff) ? 0x7fffffff : Size;
      struct acRead read = { (int8_t *)Buffer, bytes };
      if (auto error = Action(AC::Read, this, &read); error IS ERR::Okay) {
         *Result = U(read.Result);
         return ERR::Okay;
      }
      else { *Result = 0; return error; }
   }
   template <class T> ERR read(APTR Buffer, T Size) noexcept {
      static_assert(std::is_integral<T>::value, "Size value must be an integer type");
      const int bytes = (Size > 0x7fffffff) ? 0x7fffffff : Size;
      struct acRead read = { (int8_t *)Buffer, bytes };
      return Action(AC::Read, this, &read);
   }
   inline ERR reset() noexcept { return Action(AC::Reset, this, nullptr); }
   inline ERR seek(double Offset, SEEK Position = SEEK::CURRENT) noexcept {
      struct acSeek args = { Offset, Position };
      return Action(AC::Seek, this, &args);
   }
   inline ERR seekStart(double Offset) noexcept { return seek(Offset, SEEK::START); }
   inline ERR seekEnd(double Offset) noexcept { return seek(Offset, SEEK::END); }
   inline ERR seekCurrent(double Offset) noexcept { return seek(Offset, SEEK::CURRENT); }
   inline ERR write(CPTR Buffer, int Size, int *Result = nullptr) noexcept {
      struct acWrite write = { (int8_t *)Buffer, Size };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }
   inline ERR write(std::string Buffer, int *Result = nullptr) noexcept {
      struct acWrite write = { (int8_t *)Buffer.c_str(), int(Buffer.size()) };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }

   // Customised field getting

   inline ERR getTotalOutput(int64_t &Value) noexcept {
      Value = this->TotalOutput;
      return ERR::Okay;
   }

   inline ERR getInput(OBJECTPTR &Value) noexcept {
      Value = this->Input;
      return ERR::Okay;
   }

   inline ERR getOutput(OBJECTPTR &Value) noexcept {
      Value = this->Output;
      return ERR::Okay;
   }

   inline ERR getFormat(CF &Value) noexcept {
      Value = this->Format;
      return ERR::Okay;
   }

   inline ERR getSize(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->GetValue(this, &Value);
   }


   // Customised field setting

   inline ERR setInput(OBJECTPTR Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Input = Value;
      return ERR::Okay;
   }

   inline ERR setOutput(OBJECTPTR Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Output = Value;
      return ERR::Okay;
   }

   inline ERR setFormat(const CF Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Format = Value;
      return ERR::Okay;
   }

};
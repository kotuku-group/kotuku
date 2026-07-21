#pragma once

// Copyright: Paul Manias © 1996-2026
// Generator: idl-c

#include <kotuku/main.h>

class objStorageDevice;
class objFile;

// Flags for the File Watch() method.

enum class MFF : uint32_t {
   NIL = 0,
   READ = 0x00000001,
   MODIFY = 0x00000002,
   WRITE = 0x00000002,
   CREATE = 0x00000004,
   DELETE = 0x00000008,
   MOVED = 0x00000010,
   RENAME = 0x00000010,
   ATTRIB = 0x00000020,
   OPENED = 0x00000040,
   CLOSED = 0x00000080,
   UNMOUNT = 0x00000100,
   FOLDER = 0x00000200,
   FILE = 0x00000400,
   SELF = 0x00000800,
   DEEP = 0x00001000,
};

DEFINE_ENUM_FLAG_OPERATORS(MFF)

// Flags for the SetDate() file method.

enum class FDT : int {
   NIL = 0,
   MODIFIED = 0,
   CREATED = 1,
   ACCESSED = 2,
   ARCHIVED = 3,
};

// StorageDevice class definition

#define VER_STORAGEDEVICE (1.000000)

class objStorageDevice : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::STORAGEDEVICE;
   static constexpr CSTRING CLASS_NAME = "StorageDevice";

   using create = kt::Create<objStorageDevice>;
   objStorageDevice(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   DEVICE  DeviceFlags;   // These read-only flags identify the type of device and its features.
   int64_t DeviceSize;    // Total storage capacity of the resolved volume, measured in bytes.
   int64_t BytesFree;     // Amount of storage space available to the current user, measured in bytes.
   int64_t BytesUsed;     // Amount of storage space in use, measured in bytes.
   std::string DeviceID;  // Unique device identifier for the mounted volume, when available.
   std::string Volume;    // The volume name of the device to query.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getDeviceFlags(DEVICE &Value) noexcept {
      Value = this->DeviceFlags;
      return ERR::Okay;
   }

   inline ERR getDeviceSize(int64_t &Value) noexcept {
      Value = this->DeviceSize;
      return ERR::Okay;
   }

   inline ERR getBytesFree(int64_t &Value) noexcept {
      Value = this->BytesFree;
      return ERR::Okay;
   }

   inline ERR getBytesUsed(int64_t &Value) noexcept {
      Value = this->BytesUsed;
      return ERR::Okay;
   }

   inline ERR getDevice(std::string_view &Value) noexcept {
      Value = this->DeviceID;
      return ERR::Okay;
   }

   inline ERR getVolume(std::string_view &Value) noexcept {
      Value = this->Volume;
      return ERR::Okay;
   }


   // Customised field setting

   inline ERR setVolume(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, 0x00904500, &Value);
   }

};

// File class definition

#define VER_FILE (1.200000)

// File methods

namespace fl {
struct StartStream { OBJECTID SubscriberID; FL Flags; int Length; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct StopStream { static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Delete { FUNCTION Callback; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Move { std::string_view Dest; FUNCTION Callback; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Copy { std::string_view Dest; FUNCTION Callback; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetDate { int Year; int Month; int Day; int Hour; int Minute; int Second; FDT Type; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ReadLine { std::string *Result; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct BufferContent { static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Next { objFile *File; static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Watch { FUNCTION Callback; MFF Flags; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objFile : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::FILE;
   static constexpr CSTRING CLASS_NAME = "File";

   using create = kt::Create<objFile>;
   objFile(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   int64_t Position;             // The current read/write byte position in a file.
   std::string Path;             // Specifies the location of a file or folder.
   FL      Flags;                // File flags and options.
   PERMIT  Permissions;          // Manages the permissions of a file.
   kt::vector<int8_t> Buffer;    // Points to the internal data buffer if the file content is held in memory.
   int64_t Size;                 // The byte size of a file.
   public:
   inline std::string readLine() {
      std::string str;
      struct fl::ReadLine args { &str };
      Action(fl::ReadLine::id, this, &args);
      return str;
   }

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR dataFeed(OBJECTPTR Object, DATA Datatype, std::span<const int8_t> Buffer) noexcept {
      struct acDataFeed args = { Object, Datatype, Buffer };
      return Action(AC::DataFeed, this, &args);
   }
   inline ERR flush() noexcept { return Action(AC::Flush, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR query() noexcept { return Action(AC::Query, this, nullptr); }
   template <class T> ERR read(std::span<int8_t> Buffer, T *Result) noexcept {
      static_assert(std::is_integral<T>::value, "Result value must be an integer type");
      struct acRead read = { Buffer };
      if (auto error = Action(AC::Read, this, &read); error IS ERR::Okay) {
         *Result = T(read.Result);
         return ERR::Okay;
      }
      else { *Result = 0; return error; }
   }
   inline ERR read(std::span<int8_t> Buffer) noexcept {
      struct acRead read = { Buffer };
      return Action(AC::Read, this, &read);
   }
   inline ERR rename(std::string_view Name) noexcept {
      struct acRename args = { Name };
      return Action(AC::Rename, this, &args);
   }
   inline ERR reset() noexcept { return Action(AC::Reset, this, nullptr); }
   inline ERR seek(double Offset, SEEK Position = SEEK::CURRENT) noexcept {
      struct acSeek args = { Offset, Position };
      return Action(AC::Seek, this, &args);
   }
   inline ERR seekStart(double Offset) noexcept { return seek(Offset, SEEK::START); }
   inline ERR seekEnd(double Offset) noexcept { return seek(Offset, SEEK::END); }
   inline ERR seekCurrent(double Offset) noexcept { return seek(Offset, SEEK::CURRENT); }
   inline ERR write(std::span<const int8_t> Buffer, int *Result = nullptr) noexcept {
      struct acWrite write = { Buffer };
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
      struct acWrite write = { std::span((const int8_t *)Buffer.data(), Buffer.size()) };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }
   inline ERR startStream(OBJECTID SubscriberID, FL Flags, int Length) noexcept {
      struct fl::StartStream args = { SubscriberID, Flags, Length };
      return Action(AC(-1), this, &args);
   }
   inline ERR stopStream() noexcept {
      return Action(AC(-2), this, nullptr);
   }
   inline ERR del(FUNCTION Callback) noexcept {
      struct fl::Delete args = { Callback };
      return Action(AC(-3), this, &args);
   }
   inline ERR move(const std::string_view &Dest, FUNCTION Callback) noexcept {
      struct fl::Move args = { Dest, Callback };
      return Action(AC(-4), this, &args);
   }
   inline ERR copy(const std::string_view &Dest, FUNCTION Callback) noexcept {
      struct fl::Copy args = { Dest, Callback };
      return Action(AC(-5), this, &args);
   }
   inline ERR setDate(int Year, int Month, int Day, int Hour, int Minute, int Second, FDT Type) noexcept {
      struct fl::SetDate args = { Year, Month, Day, Hour, Minute, Second, Type };
      return Action(AC(-6), this, &args);
   }
   inline ERR readLine(std::string &Result) noexcept {
      struct fl::ReadLine args = { &Result };
      ERR error = Action(AC(-7), this, &args);
      return error;
   }
   inline ERR bufferContent() noexcept {
      return Action(AC(-8), this, nullptr);
   }
   inline ERR next(objFile ** File) noexcept {
      struct fl::Next args = { (objFile *)0 };
      ERR error = Action(AC(-9), this, &args);
      if (File) *File = args.File;
      return error;
   }
   inline ERR watch(FUNCTION Callback, MFF Flags) noexcept {
      struct fl::Watch args = { Callback, Flags };
      return Action(AC(-10), this, &args);
   }

   // Customised field getting

   inline ERR getPosition(int64_t &Value) noexcept {
      Value = this->Position;
      return ERR::Okay;
   }

   inline ERR getPath(std::string_view &Value) noexcept {
      Value = this->Path;
      return ERR::Okay;
   }

   inline ERR getFlags(FL &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getPermissions(PERMIT &Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getBuffer(std::span<int8_t> &Value) noexcept {
      auto field = &this->Class->Dictionary[12];
      auto get_field = (ERR (*)(APTR, std::span<int8_t> &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getSize(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getDate(struct DateTime * &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getCreated(struct DateTime * &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getHandle(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->GetValue(this, &Value);
   }

   inline ERR getIcon(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getResolvedPath(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[18];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getTimestamp(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getLink(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getUser(int &Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getGroup(int &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setPosition(const int64_t Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, FD_INT64, &Value);
   }

   inline ERR setPath(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, 0x00804500, &Value);
   }

   inline ERR setFlags(const FL Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setPermissions(const PERMIT Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setSize(const int64_t Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_INT64, &Value);
   }

   inline ERR setDate(const struct DateTime & Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, FD_STRUCT, &Value);
   }

   inline ERR setLink(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(this, field, 0x00804308, &Value);
   }

   inline ERR setUser(const int Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setGroup(const int Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

};

namespace fl {

// Read endian values from files and objects.

template<class T> ERR ReadLE(OBJECTPTR Object, T *Result)
{
   uint8_t data[sizeof(T)];
   struct acRead read = { .Buffer = std::span<int8_t>((int8_t *)data, sizeof(T)) };
   if (!Action(AC::Read, Object, &read)) {
      if (read.Result IS sizeof(T)) {
         if constexpr (std::endian::native IS std::endian::little) {
            *Result = ((T *)data)[0];
         }
         else {
            switch(sizeof(T)) {
               case 2:  *Result = (data[1]<<8) | data[0]; break;
               case 4:  *Result = (data[0]<<24)|(data[1]<<16)|(data[2]<<8)|(data[3]); break;
               case 8:  *Result = ((int64_t)data[0]<<56)|((int64_t)data[1]<<48)|((int64_t)data[2]<<40)|((int64_t)data[3]<<32)|(data[4]<<24)|(data[5]<<16)|(data[6]<<8)|(data[7]); break;
               default: *Result = ((T *)data)[0];
            }
         }
         return ERR::Okay;
      }
      else return ERR::Read;
   }
   else return ERR::Read;
}

template<class T> ERR ReadBE(OBJECTPTR Object, T *Result)
{
   uint8_t data[sizeof(T)];
   struct acRead read = { .Buffer = std::span<int8_t>((int8_t *)data, sizeof(T)) };
   if (!Action(AC::Read, Object, &read)) {
      if (read.Result IS sizeof(T)) {
         if constexpr (std::endian::native IS std::endian::little) {
            switch(sizeof(T)) {
               case 2:  *Result = (data[1]<<8) | data[0]; break;
               case 4:  *Result = (data[0]<<24)|(data[1]<<16)|(data[2]<<8)|(data[3]); break;
               case 8:  *Result = ((int64_t)data[0]<<56)|((int64_t)data[1]<<48)|((int64_t)data[2]<<40)|((int64_t)data[3]<<32)|(data[4]<<24)|(data[5]<<16)|(data[6]<<8)|(data[7]); break;
               default: *Result = ((T *)data)[0];
            }
         }
         else {
            *Result = ((T *)data)[0];
         }
         return ERR::Okay;
      }
      else return ERR::Read;
   }
   else return ERR::Read;
}

} // fl namespace
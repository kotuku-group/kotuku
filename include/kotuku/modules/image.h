#pragma once

// Name:      image.h
// Copyright: Paul Manias © 2001-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_IMAGE (1)

#include <kotuku/modules/display.h>

class objImage;

// Flags for the Image class.

enum class PCF : uint32_t {
   NIL = 0,
   NO_PALETTE = 0x00000001,
   SCALABLE = 0x00000002,
   NEW = 0x00000004,
   MASK = 0x00000008,
   ALPHA = 0x00000010,
   LAZY = 0x00000020,
   FORCE_ALPHA_32 = 0x00000040,
};

DEFINE_ENUM_FLAG_OPERATORS(PCF)

// Image class definition

#define VER_IMAGE (1.000000)

class objImage : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::IMAGE;
   static constexpr CSTRING CLASS_NAME = "Image";

   using create = kt::Create<objImage>;

   std::string Path;    // The location of source image data.
   objBitmap * Bitmap;  // Represents image data.
   objBitmap * Mask;    // Refers to a Bitmap that imposes a mask on the image.
   PCF Flags;           // Optional initialisation flags.
   int DisplayHeight;   // The preferred height to use when displaying the image.
   int DisplayWidth;    // The preferred width to use when displaying the image.
   int Quality;         // Defines the quality level to use when saving the image.
   public:
   objImage(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID), Quality(80) {}

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR query() noexcept { return Action(AC::Query, this, nullptr); }
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
   inline ERR refresh() noexcept { return Action(AC::Refresh, this, nullptr); }
   inline ERR saveImage(OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) noexcept {
      struct acSaveImage args = { Dest, { ClassID } };
      return Action(AC::SaveImage, this, &args);
   }
   inline ERR saveToObject(OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) noexcept {
      struct acSaveToObject args = { Dest, { ClassID } };
      return Action(AC::SaveToObject, this, &args);
   }
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

   inline ERR getPath(std::string_view &Value) noexcept {
      Value = this->Path;
      return ERR::Okay;
   }

   inline ERR getBitmap(objBitmap * &Value) noexcept {
      Value = this->Bitmap;
      return ERR::Okay;
   }

   inline ERR getMask(objBitmap * &Value) noexcept {
      Value = this->Mask;
      return ERR::Okay;
   }

   inline ERR getFlags(PCF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getDisplayHeight(int &Value) noexcept {
      Value = this->DisplayHeight;
      return ERR::Okay;
   }

   inline ERR getDisplayWidth(int &Value) noexcept {
      Value = this->DisplayWidth;
      return ERR::Okay;
   }

   inline ERR getQuality(int &Value) noexcept {
      Value = this->Quality;
      return ERR::Okay;
   }

   inline ERR getAuthor(std::string_view &Value) noexcept {
      Value = *((std::string *)(((int8_t *)this) + CLASS_OFFSET + 64));
      return ERR::Okay;
   }

   inline ERR getCopyright(std::string_view &Value) noexcept {
      Value = *((std::string *)(((int8_t *)this) + CLASS_OFFSET + 96));
      return ERR::Okay;
   }

   inline ERR getTitle(std::string_view &Value) noexcept {
      Value = *((std::string *)(((int8_t *)this) + CLASS_OFFSET + 128));
      return ERR::Okay;
   }

   inline ERR getSoftware(std::string_view &Value) noexcept {
      Value = *((std::string *)(((int8_t *)this) + CLASS_OFFSET + 160));
      return ERR::Okay;
   }

   inline ERR getDescription(std::string_view &Value) noexcept {
      Value = *((std::string *)(((int8_t *)this) + CLASS_OFFSET + 192));
      return ERR::Okay;
   }

   inline ERR getDisclaimer(std::string_view &Value) noexcept {
      Value = *((std::string *)(((int8_t *)this) + CLASS_OFFSET + 224));
      return ERR::Okay;
   }

   inline ERR getHeader(std::span<int8_t> &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto get_field = (ERR (*)(APTR, std::span<int8_t> &))field->GetValue;
      return get_field(this, Value);
   }


   // Customised field setting

   inline ERR setPath(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Path = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const PCF Value) noexcept {
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setDisplayHeight(const int Value) noexcept {
      this->DisplayHeight = Value;
      return ERR::Okay;
   }

   inline ERR setDisplayWidth(const int Value) noexcept {
      this->DisplayWidth = Value;
      return ERR::Okay;
   }

   inline ERR setQuality(const int Value) noexcept {
      this->Quality = Value;
      return ERR::Okay;
   }

   inline ERR setAuthor(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setCopyright(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setTitle(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setSoftware(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setDescription(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setDisclaimer(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setHeader(std::span<const int8_t> Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, 0x01101508, &Value);
   }

};

namespace fl {
   using namespace kt;
constexpr FieldValue DisplayWidth(int Value) { return FieldValue(strhash("displayWidth"), Value); }
constexpr FieldValue DisplayHeight(int Value) { return FieldValue(strhash("displayHeight"), Value); }
}

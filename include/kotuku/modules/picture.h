#pragma once

// Name:      picture.h
// Copyright: Paul Manias © 2001-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_PICTURE (1)

#include <kotuku/modules/display.h>

class objPicture;

// Flags for the Picture class.

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

// Picture class definition

#define VER_PICTURE (1.000000)

class objPicture : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::PICTURE;
   static constexpr CSTRING CLASS_NAME = "Picture";

   using create = pf::Create<objPicture>;

   objBitmap * Bitmap;    // Represents a picture's image data.
   objBitmap * Mask;      // Refers to a Bitmap that imposes a mask on the image.
   PCF Flags;             // Optional initialisation flags.
   int DisplayHeight;     // The preferred height to use when displaying the image.
   int DisplayWidth;      // The preferred width to use when displaying the image.
   int Quality;           // Defines the quality level to use when saving the image.
   int FrameRate;         // Refresh & redraw the picture X times per second.  Used by pictures that have an animation refresh rate

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
         *Result = static_cast<U>(read.Result);
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
   inline int writeResult(CPTR Buffer, int Size) noexcept {
      struct acWrite write = { (int8_t *)Buffer, Size };
      if (Action(AC::Write, this, &write) IS ERR::Okay) return write.Result;
      else return 0;
   }

   // Customised field setting

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

   template <class T> inline ERR setAuthor(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setCopyright(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setDescription(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setDisclaimer(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   inline ERR setHeader(APTR Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      auto target = this;
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(target, field, 0x08000500, Value, 1);
   }

   template <class T> inline ERR setPath(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(target, field, 0x08800500, to_cstring(Value), 1);
   }

   template <class T> inline ERR setSoftware(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[21];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setTitle(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

};

namespace fl {
   using namespace pf;
constexpr FieldValue DisplayWidth(int Value) { return FieldValue(FID_DisplayWidth, Value); }
constexpr FieldValue DisplayHeight(int Value) { return FieldValue(FID_DisplayHeight, Value); }
}

#pragma once

// Name:      lzma.h
// Copyright: Paul Manias © 2024-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_LZMA (1)

#include <kotuku/modules/compression.h>

#include <kotuku/modules/core.h>

class objLZMAStream;

// LZMAStream class definition

#define VER_LZMASTREAM (1.000000)

class objLZMAStream : public objCompressedStream {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::LZMASTREAM;
   static constexpr CSTRING CLASS_NAME = "LZMAStream";

   using create = kt::Create<objLZMAStream>;
   objLZMAStream(objMetaClass *pClass, OBJECTID pUID) noexcept : objCompressedStream(pClass, pUID) {}

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }
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

   // Customised field getting

   inline ERR getSize(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setSize(const int64_t Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_INT64, &Value);
   }

};
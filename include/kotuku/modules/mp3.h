#pragma once

// Name:      mp3.h
// Copyright: Paul Manias © 2006-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_MP3 (1)

#include <kotuku/modules/audio.h>

class objMP3;

// Controls the cost and detail of an MP3 metadata query.

enum class MPF : uint32_t {
   NIL = 0,
   TAGS = 0x00000001,
   ESTIMATE = 0x00000002,
   SCAN = 0x00000004,
};

DEFINE_ENUM_FLAG_OPERATORS(MPF)

#ifdef KOTUKU_STATIC
#define JUMPTABLE_MP3 [[maybe_unused]] static struct MP3Base *MP3Base = nullptr;
#else
#define JUMPTABLE_MP3 struct MP3Base *MP3Base = nullptr;
#endif

struct MP3Base {
#ifndef KOTUKU_STATIC
   ERR (*_GetMetadata)(const std::string_view &Path, MPF Flags, std::string *XML);
#endif // KOTUKU_STATIC
};

#if !defined(KOTUKU_STATIC) and !defined(PRV_MP3_MODULE)
extern struct MP3Base *MP3Base;
namespace mp3 {
inline ERR GetMetadata(const std::string_view &Path, MPF Flags, std::string *XML) { return MP3Base->_GetMetadata(Path,Flags,XML); }
} // namespace
#else
namespace mp3 {
extern ERR GetMetadata(const std::string_view &Path, MPF Flags, std::string *XML);
} // namespace
#endif // KOTUKU_STATIC

#ifndef PRV_MP3_MODULE
namespace mp3 {
inline ERR GetMetadata(const std::string_view &Path, MPF Flags, std::string &XML) {
   return GetMetadata(Path, Flags, &XML);
}
}
#endif
// MP3 class definition

#define VER_MP3 (1.000000)

class objMP3 : public objSound {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::MP3;
   static constexpr CSTRING CLASS_NAME = "MP3";

   using create = kt::Create<objMP3>;
   objMP3(objMetaClass *pClass, OBJECTID pUID) noexcept : objSound(pClass, pUID) {}

#ifdef PRV_MP3
   ~objMP3();
#endif

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
   inline ERR seek(double Offset, SEEK Position = SEEK::CURRENT) noexcept {
      struct acSeek args = { Offset, Position };
      return Action(AC::Seek, this, &args);
   }
   inline ERR seekStart(double Offset) noexcept { return seek(Offset, SEEK::START); }
   inline ERR seekEnd(double Offset) noexcept { return seek(Offset, SEEK::END); }
   inline ERR seekCurrent(double Offset) noexcept { return seek(Offset, SEEK::CURRENT); }

   // Customised field getting


   // Customised field setting

};
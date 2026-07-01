#pragma once

// Copyright: Paul Manias © 1996-2026
// Generator: idl-c

#include <kotuku/main.h>

class objTime;

// Time class definition

#define VER_TIME (1.000000)

// Time methods

namespace pt {
struct SetTime { static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetTimeZoneInfo { std::string_view ZoneID; int StartYear; int EndYear; struct TimeZoneInfo *Info; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objTime : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::TIME;
   static constexpr CSTRING CLASS_NAME = "Time";

   using create = kt::Create<objTime>;
   objTime(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   int64_t SystemTime;    // Represents the system time when the time object was last queried.
   int     Year;          // Year (-ve for BC, +ve for AD).
   int     Month;         // Month (1 - 12)
   int     Day;           // Day (1 - 31)
   int     Hour;          // Hour (0 - 23)
   int     Minute;        // Minute (0 - 59)
   int     Second;        // Second (0 - 59)
   int     TimeZone;      // No information.
   int     DayOfWeek;     // Day of week (0 - 6) starting from Sunday.
   int     MilliSecond;   // A millisecond is one thousandth of a second (0 - 999)
   int     MicroSecond;   // A microsecond is one millionth of a second (0 - 999999)

   // Action stubs

   inline ERR query() noexcept { return Action(AC::Query, this, nullptr); }
   inline ERR refresh() noexcept { return Action(AC::Refresh, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR setTime() noexcept {
      return Action(AC(-1), this, nullptr);
   }
   inline ERR getTimeZoneInfo(const std::string_view &ZoneID, int StartYear, int EndYear, struct TimeZoneInfo ** Info) noexcept {
      struct pt::GetTimeZoneInfo args = { ZoneID, StartYear, EndYear, (struct TimeZoneInfo *)0 };
      ERR error = Action(AC(-2), this, &args);
      if (Info) *Info = args.Info;
      return error;
   }

   // Customised field getting

   inline ERR getSystemTime(int64_t &Value) noexcept {
      Value = this->SystemTime;
      return ERR::Okay;
   }

   inline ERR getYear(int &Value) noexcept {
      Value = this->Year;
      return ERR::Okay;
   }

   inline ERR getMonth(int &Value) noexcept {
      Value = this->Month;
      return ERR::Okay;
   }

   inline ERR getDay(int &Value) noexcept {
      Value = this->Day;
      return ERR::Okay;
   }

   inline ERR getHour(int &Value) noexcept {
      Value = this->Hour;
      return ERR::Okay;
   }

   inline ERR getMinute(int &Value) noexcept {
      Value = this->Minute;
      return ERR::Okay;
   }

   inline ERR getSecond(int &Value) noexcept {
      Value = this->Second;
      return ERR::Okay;
   }

   inline ERR getDayOfWeek(int &Value) noexcept {
      Value = this->DayOfWeek;
      return ERR::Okay;
   }

   inline ERR getMilliSecond(int &Value) noexcept {
      Value = this->MilliSecond;
      return ERR::Okay;
   }

   inline ERR getMicroSecond(int &Value) noexcept {
      Value = this->MicroSecond;
      return ERR::Okay;
   }

   inline ERR getTimestamp(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->GetValue(this, &Value);
   }


   // Customised field setting

   inline ERR setSystemTime(const int64_t Value) noexcept {
      this->SystemTime = Value;
      return ERR::Okay;
   }

   inline ERR setYear(const int Value) noexcept {
      this->Year = Value;
      return ERR::Okay;
   }

   inline ERR setMonth(const int Value) noexcept {
      this->Month = Value;
      return ERR::Okay;
   }

   inline ERR setDay(const int Value) noexcept {
      this->Day = Value;
      return ERR::Okay;
   }

   inline ERR setHour(const int Value) noexcept {
      this->Hour = Value;
      return ERR::Okay;
   }

   inline ERR setMinute(const int Value) noexcept {
      this->Minute = Value;
      return ERR::Okay;
   }

   inline ERR setSecond(const int Value) noexcept {
      this->Second = Value;
      return ERR::Okay;
   }

   inline ERR setDayOfWeek(const int Value) noexcept {
      this->DayOfWeek = Value;
      return ERR::Okay;
   }

   inline ERR setMilliSecond(const int Value) noexcept {
      this->MilliSecond = Value;
      return ERR::Okay;
   }

   inline ERR setMicroSecond(const int Value) noexcept {
      this->MicroSecond = Value;
      return ERR::Okay;
   }

};
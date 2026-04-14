/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
Time: Simplifies the management of date/time information.

The Time class is available for programs that require time and date management in a multi-platform manner.

To get the current system time, use the #Query() action.
-END-

*********************************************************************************************************************/

#include "../defs.h"
#include <kotuku/main.h>
#include <chrono>
#include <time.h>

#ifdef __unix__
#include <sys/ioctl.h>
#include <sys/time.h>
 #ifdef __linux__
  #include <linux/rtc.h>
 #endif
#include <fcntl.h>
#include <unistd.h>
#endif

static ERR GET_TimeStamp(objTime *, int64_t *);

static ERR TIME_Query(objTime *);
static ERR TIME_SetTime(objTime *);

/*********************************************************************************************************************
-ACTION-
Query: Updates the values in a time object with the current system date and time.
-END-
*********************************************************************************************************************/

static ERR TIME_Query(objTime *Self)
{
   auto now = std::chrono::system_clock::now();
   auto duration_since_epoch = now.time_since_epoch();
   auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration_since_epoch).count();

   // Get current timezone and convert to local time
   auto current_zone = std::chrono::current_zone();
   if (!current_zone) return ERR::SystemCall;

   auto local_time = current_zone->to_local(now);
   auto local_days = std::chrono::floor<std::chrono::days>(local_time);
   auto ymd = std::chrono::year_month_day{local_days};
   auto tod = std::chrono::hh_mm_ss{local_time - local_days};

   if (!ymd.ok()) return ERR::SystemCall;

   Self->Year  = int(ymd.year());
   Self->Month = unsigned(ymd.month());
   Self->Day   = unsigned(ymd.day());
   Self->Hour   = int(tod.hours().count());
   Self->Minute = int(tod.minutes().count());
   Self->Second = int(tod.seconds().count());

   auto subsec_us = std::chrono::duration_cast<std::chrono::microseconds>(tod.subseconds()).count();
   Self->MilliSecond = int(subsec_us / 1000);
   Self->MicroSecond = int(subsec_us % 1000000);
   Self->SystemTime  = int64_t(microseconds);

   // Calculate the day of the week (0 = Sunday) using Zeller's congruence
   int a = (14 - Self->Month) / 12;
   int y = Self->Year - a;
   int m = Self->Month + 12 * a - 2;
   Self->DayOfWeek = (Self->Day + y + (y / 4) - (y / 100) + (y / 400) + (31 * m) / 12) % 7;

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SetTime: Apply the time to the system clock.

This method will apply the time object's values to the BIOS.  Depending on the host platform, this method may only
work if the user is logged in as the administrator.
-END-

*********************************************************************************************************************/

static ERR TIME_SetTime(objTime *Self)
{
#ifdef __unix__
   pf::Log log;
   struct timeval tmday;
   struct tm time;
   int fd;

   log.branch();

   // Set the BIOS clock

   #ifdef __APPLE__
      log.warning("No support for modifying the BIOS clock in OS X build");
   #else
      if ((fd = open("/dev/rtc", O_RDONLY|O_NONBLOCK)) != -1) {
         time.tm_year  = Self->Year - 1900;
         time.tm_mon   = Self->Month - 1;
         time.tm_mday  = Self->Day;
         time.tm_hour  = Self->Hour;
         time.tm_min   = Self->Minute;
         time.tm_sec   = Self->Second;
         time.tm_isdst = -1;
         time.tm_wday  = 0;
         time.tm_yday  = 0;
         ioctl(fd, RTC_SET_TIME, &time);
         close(fd);
      }
      else log.warning("/dev/rtc not available.");
   #endif

   // Set the internal system clock

   time.tm_year  = Self->Year - 1900;
   time.tm_mon   = Self->Month - 1;
   time.tm_mday  = Self->Day;
   time.tm_hour  = Self->Hour;
   time.tm_min   = Self->Minute;
   time.tm_sec   = Self->Second;
   time.tm_isdst = -1;
   time.tm_wday  = 0;
   time.tm_yday  = 0;

   if ((tmday.tv_sec = mktime(&time)) != -1) {
      tmday.tv_usec = 0;
      if (settimeofday(&tmday, nullptr) IS -1) {
         log.warning("settimeofday() failed.");
      }
   }
   else log.warning("mktime() failed [%d/%d/%d, %d:%d:%d]", Self->Day, Self->Month, Self->Year, Self->Hour, Self->Minute, Self->Second);

   return ERR::Okay;

#elif _WIN32
   // Use Windows wrapper function to set system time
   if (winSetSystemTime(Self->Year, Self->Month, Self->Day, Self->Hour, Self->Minute, Self->Second)) {
      return ERR::Okay;
   }
   else {
      // Setting system time failed - likely due to insufficient privileges
      // Process needs SE_SYSTEMTIME_NAME privilege or to run as administrator
      return ERR::PermissionDenied;
   }

#else
   return ERR::NoSupport;
#endif
}

/*********************************************************************************************************************

-FIELD-
Day: Day (1 - 31)

-FIELD-
DayOfWeek: Day of week (0 - 6) starting from Sunday.

-FIELD-
Hour: Hour (0 - 23)

-FIELD-
MicroSecond: A microsecond is one millionth of a second (0 - 999999)

-FIELD-
MilliSecond: A millisecond is one thousandth of a second (0 - 999)

-FIELD-
Minute: Minute (0 - 59)

-FIELD-
Month: Month (1 - 12)

-FIELD-
Second: Second (0 - 59)

-FIELD-
SystemTime: Represents the system time when the time object was last queried.

The SystemTime field returns the system time if the Time object has been queried.  The time is represented in
microseconds.  This field serves no purpose beyond its initial query value.

-FIELD-
TimeZone: No information.
Status: private

-FIELD-
TimeStamp: Read this field to get representation of the time as a single integer.

The TimeStamp field is a 64-bit integer that represents the time object as an approximation of the number of
milliseconds represented in the time object (approximately the total amount of time passed since Zero-AD).  This is
convenient for summarising a time value for comparison with other time stamps, or for storing time in a 64-bit space.

The TimeStamp value is dynamically calculated when reading this field.

*********************************************************************************************************************/

static ERR GET_TimeStamp(objTime *Self, int64_t *Value)
{
   *Value = Self->Second +
            (int64_t(Self->Minute) * 60) +
            (int64_t(Self->Hour) * 60 * 60) +
            (int64_t(Self->Day) * 60 * 60 * 24) +
            (int64_t(Self->Month) * 60 * 60 * 24 * 31) +
            (int64_t(Self->Year) * 60 * 60 * 24 * 31 * 12);

   *Value = *Value * 1000000LL;

   *Value += Self->MilliSecond;

   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Year: Year (-ve for BC, +ve for AD).
-END-
*********************************************************************************************************************/

static const FieldArray clFields[] = {
   { "SystemTime",   FDF_INT64|FDF_RW },
   { "Year",         FDF_INT|FDF_RW },
   { "Month",        FDF_INT|FDF_RW },
   { "Day",          FDF_INT|FDF_RW },
   { "Hour",         FDF_INT|FDF_RW },
   { "Minute",       FDF_INT|FDF_RW },
   { "Second",       FDF_INT|FDF_RW },
   { "TimeZone",     FDF_INT|FDF_RW },
   { "DayOfWeek",    FDF_INT|FDF_RW },
   { "MilliSecond",  FDF_INT|FDF_RW },
   { "MicroSecond",  FDF_INT|FDF_RW },
   // Virtual fields
   { "TimeStamp",    FDF_INT64|FDF_R, GET_TimeStamp },
   END_FIELD
};

static const ActionArray clActions[] = {
   { AC::Query,   TIME_Query },
   { AC::Refresh, TIME_Query },
   { AC::NIL, nullptr }
};

static const MethodEntry clMethods[] = {
   { pt::SetTime::id, (APTR)TIME_SetTime, "SetTime", 0, 0 },
   { AC::NIL, nullptr, nullptr, nullptr, 0 }
};

//********************************************************************************************************************

extern ERR add_time_class(void)
{
   glTimeClass = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::TIME),
      fl::ClassVersion(VER_TIME),
      fl::Name("Time"),
      fl::Category(CCF::SYSTEM),
      fl::Actions(clActions),
      fl::Methods(clMethods),
      fl::Fields(clFields),
      fl::Size(sizeof(objTime)),
      fl::Path("modules:core"));

   return glTimeClass ? ERR::Okay : ERR::AddClass;
}

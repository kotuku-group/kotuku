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
#include <cstring>
#include <fstream>
#include <chrono>
#include <time.h>
#include <vector>

#ifdef __unix__
#include <filesystem>
#include <sys/ioctl.h>
#include <sys/time.h>
 #ifdef __linux__
  #include <linux/rtc.h>
 #endif
#include <fcntl.h>
#include <unistd.h>
#endif

static ERR GET_Timestamp(objTime *, int64_t *);

static ERR TIME_Query(objTime *);
static ERR TIME_SetTime(objTime *);

static ERR tzi_free(APTR Address)
{
   ((struct TimeZoneInfo *)Address)->~TimeZoneInfo();
   return ERR::Okay;
}

static ResourceManager glTimeZoneHandler = { "TimeZoneInfo", &tzi_free };

//********************************************************************************************************************

static int64_t utc_year_start_us(const int Year)
{
   auto day = std::chrono::sys_days(std::chrono::year(Year) / std::chrono::January / 1);
   return int64_t(std::chrono::duration_cast<std::chrono::microseconds>(day.time_since_epoch()).count());
}

//********************************************************************************************************************

static bool is_utc_zone(std::string_view ZoneID)
{
   return iequals(ZoneID, "UTC") or iequals(ZoneID, "Etc/UTC") or iequals(ZoneID, "Etc/GMT") or
      iequals(ZoneID, "GMT") or iequals(ZoneID, "Zulu");
}

//********************************************************************************************************************

static void fill_utc_info(TimeZoneInfo &Info, const int StartYear, const int EndYear, const int IsLocal,
   const int IsFallback)
{
   Info = TimeZoneInfo();
   Info.ZoneID             = "UTC";
   Info.NativeID           = "UTC";
   Info.Source             = "utc";
   Info.BaseOffset         = 0;
   Info.TransitionCount    = 0;
   Info.TransitionsWritten = 0;
   Info.StartYear          = StartYear;
   Info.EndYear            = EndYear;
   Info.IsLocal            = IsLocal;
   Info.IsFallback         = IsFallback;
}

#ifdef _WIN32

static ERR get_windows_timezone_info(std::string_view ZoneID, const int StartYear, const int EndYear,
   TimeZoneInfo &Info)
{
   rkTimeZoneInfo host_info;
   ERR error = winGetTimeZoneInfo(ZoneID, StartYear, EndYear, host_info);
   if (error IS ERR::Okay) { }
   else return error;

   Info = TimeZoneInfo();
   Info.ZoneID             = std::move(host_info.ZoneID);
   Info.NativeID           = std::move(host_info.NativeID);
   Info.Source             = std::move(host_info.Source);
   Info.DataPath           = std::move(host_info.DataPath);
   Info.Version            = std::move(host_info.Version);
   Info.BaseOffset         = host_info.BaseOffset;
   Info.TransitionCount    = host_info.TransitionCount;
   Info.TransitionsWritten = host_info.TransitionsWritten;
   Info.StartYear          = host_info.StartYear;
   Info.EndYear            = host_info.EndYear;
   Info.IsLocal            = host_info.IsLocal;
   Info.IsFallback         = host_info.IsFallback;

   Info.Transitions.reserve(host_info.Transitions.size());
   for (auto &host_transition : host_info.Transitions) {
      auto &transition = Info.Transitions.emplace_back();
      transition.Instant         = host_transition.Instant;
      transition.Abbreviation    = std::move(host_transition.Abbreviation);
      transition.OffsetBefore    = host_transition.OffsetBefore;
      transition.OffsetAfter     = host_transition.OffsetAfter;
      transition.DaylightSaving  = host_transition.DaylightSaving;
   }

   return ERR::Okay;
}

#endif

#if defined(__linux__)

struct TZifType {
   int Offset;
   int IsDst;
   int Abbreviation;
};

//********************************************************************************************************************

static uint32_t tzif_u32(const unsigned char *Data)
{
   return (uint32_t(Data[0]) << 24) | (uint32_t(Data[1]) << 16) | (uint32_t(Data[2]) << 8) | uint32_t(Data[3]);
}

//********************************************************************************************************************

static int32_t tzif_i32(const unsigned char *Data)
{
   return int32_t(tzif_u32(Data));
}

//********************************************************************************************************************

static int64_t tzif_i64(const unsigned char *Data)
{
   const uint64_t value = (uint64_t(Data[0]) << 56) | (uint64_t(Data[1]) << 48) | (uint64_t(Data[2]) << 40) |
      (uint64_t(Data[3]) << 32) | (uint64_t(Data[4]) << 24) | (uint64_t(Data[5]) << 16) |
      (uint64_t(Data[6]) << 8) | uint64_t(Data[7]);
   return int64_t(value);
}

//********************************************************************************************************************

static bool read_binary_file(const std::filesystem::path &Path, std::vector<unsigned char> &Data)
{
   std::ifstream file(Path, std::ios::binary);
   if (not file) return false;

   Data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
   return not Data.empty();
}

//********************************************************************************************************************

static std::string read_tzdb_version()
{
   std::ifstream file("/usr/share/zoneinfo/tzdata.zi");
   std::string line;
   if (std::getline(file, line) and line.starts_with("# version ")) return line.substr(10);
   return {};
}

//********************************************************************************************************************

static bool safe_iana_zone(std::string_view ZoneID)
{
   return (not ZoneID.empty()) and (ZoneID.front() != '/') and (ZoneID.front() != '\\') and
      (ZoneID.find("..") IS std::string_view::npos) and (ZoneID.find('\\') IS std::string_view::npos);
}

//********************************************************************************************************************

static bool tzif_block_size(const std::vector<unsigned char> &Data, const size_t Offset, const bool Wide,
   size_t &BlockSize)
{
   if (Data.size() < Offset + 44) return false;
   if (memcmp(Data.data() + Offset, "TZif", 4) != 0) return false;

   const uint32_t ttisgmtcnt = tzif_u32(Data.data() + Offset + 20);
   const uint32_t ttisstdcnt = tzif_u32(Data.data() + Offset + 24);
   const uint32_t leapcnt    = tzif_u32(Data.data() + Offset + 28);
   const uint32_t timecnt    = tzif_u32(Data.data() + Offset + 32);
   const uint32_t typecnt    = tzif_u32(Data.data() + Offset + 36);
   const uint32_t charcnt    = tzif_u32(Data.data() + Offset + 40);

   const size_t time_size = Wide ? 8 : 4;
   const size_t leap_size = Wide ? 12 : 8;
   BlockSize = 44 + (size_t(timecnt) * time_size) + timecnt + (size_t(typecnt) * 6) + charcnt +
      (size_t(leapcnt) * leap_size) + ttisstdcnt + ttisgmtcnt;

   return Data.size() >= Offset + BlockSize;
}

//********************************************************************************************************************

static bool parse_tzif_block(const std::vector<unsigned char> &Data, const size_t Offset, const bool Wide,
   std::string_view ZoneID, const std::filesystem::path &Path, const int StartYear, const int EndYear,
   const int IsLocal, TimeZoneInfo &Info)
{
   size_t block_size = 0;
   if (not tzif_block_size(Data, Offset, Wide, block_size)) return false;

   const uint32_t timecnt = tzif_u32(Data.data() + Offset + 32);
   const uint32_t typecnt = tzif_u32(Data.data() + Offset + 36);
   const uint32_t charcnt = tzif_u32(Data.data() + Offset + 40);
   if (typecnt IS 0) return false;

   size_t pos = Offset + 44;
   std::vector<int64_t> times(timecnt);
   for (uint32_t i = 0; i < timecnt; i++) {
      times[i] = Wide ? tzif_i64(Data.data() + pos) : int64_t(tzif_i32(Data.data() + pos));
      pos += Wide ? 8 : 4;
   }

   std::vector<unsigned char> indices(timecnt);
   for (uint32_t i = 0; i < timecnt; i++) indices[i] = Data[pos++];

   std::vector<TZifType> types(typecnt);
   for (uint32_t i = 0; i < typecnt; i++) {
      types[i].Offset       = tzif_i32(Data.data() + pos);
      types[i].IsDst        = int(Data[pos + 4]);
      types[i].Abbreviation = int(Data[pos + 5]);
      pos += 6;
   }

   const char *abbreviations = (const char *)(Data.data() + pos);

   int base_type = 0;
   for (uint32_t i = 0; i < typecnt; i++) {
      if (types[i].IsDst IS 0) {
         base_type = int(i);
         break;
      }
   }

   Info = TimeZoneInfo();
   Info.ZoneID      = std::string(ZoneID);
   Info.NativeID    = std::string(ZoneID);
   Info.Source      = "tzif";
   Info.DataPath    = Path.generic_string();
   Info.Version     = read_tzdb_version();
   Info.BaseOffset  = types[base_type].Offset;
   Info.StartYear   = StartYear;
   Info.EndYear     = EndYear;
   Info.IsLocal     = IsLocal;
   Info.IsFallback  = 0;

   int previous_type = base_type;
   const int64_t start_us = utc_year_start_us(StartYear);
   const int64_t end_us = utc_year_start_us(EndYear + 1);

   for (uint32_t i = 0; i < timecnt; i++) {
      const int after_type = int(indices[i]);
      if ((after_type < 0) or (after_type >= int(types.size()))) return false;

      const int64_t instant_us = times[i] * 1000000LL;
      if ((instant_us >= start_us) and (instant_us < end_us)) {
         TimeZoneTransition transition;
         transition.Instant         = instant_us;
         transition.OffsetBefore    = types[previous_type].Offset;
         transition.OffsetAfter     = types[after_type].Offset;
         transition.DaylightSaving  = types[after_type].IsDst ? 1 : 0;

         const int abbr_index = types[after_type].Abbreviation;
         if ((abbr_index >= 0) and (abbr_index < int(charcnt))) transition.Abbreviation = abbreviations + abbr_index;

         Info.Transitions.push_back(std::move(transition));
      }

      previous_type = after_type;
   }

   Info.TransitionCount    = int(Info.Transitions.size());
   Info.TransitionsWritten = Info.TransitionCount;
   return true;
}

//********************************************************************************************************************

static bool parse_tzif_file(const std::filesystem::path &Path, std::string_view ZoneID, const int StartYear,
   const int EndYear, const int IsLocal, TimeZoneInfo &Info)
{
   std::vector<unsigned char> data;
   if (not read_binary_file(Path, data)) return false;
   if (data.size() < 44) return false;
   if (memcmp(data.data(), "TZif", 4) != 0) return false;

   const char version = char(data[4]);
   if ((version IS '2') or (version IS '3') or (version IS '4')) {
      size_t first_block = 0;
      if (not tzif_block_size(data, 0, false, first_block)) return false;
      return parse_tzif_block(data, first_block, true, ZoneID, Path, StartYear, EndYear, IsLocal, Info);
   }

   return parse_tzif_block(data, 0, false, ZoneID, Path, StartYear, EndYear, IsLocal, Info);
}

//********************************************************************************************************************

static std::string local_linux_zone_id(const std::filesystem::path &Path)
{
   std::error_code error;
   if (not std::filesystem::is_symlink(Path, error)) return "local";

   auto target = std::filesystem::read_symlink(Path, error);
   if (error) return "local";

   if (target.is_relative()) target = std::filesystem::weakly_canonical(Path.parent_path() / target, error);
   if (error) return "local";

   const auto target_string = target.generic_string();
   constexpr std::string_view prefix = "/usr/share/zoneinfo/";
   if (target_string.starts_with(prefix)) return target_string.substr(prefix.size());
   return "local";
}

//********************************************************************************************************************

static ERR get_linux_timezone_info(std::string_view ZoneID, const int StartYear, const int EndYear, TimeZoneInfo &Info)
{
   const bool is_local = ZoneID.empty();
   std::filesystem::path path;
   std::string public_id;

   if (is_local) {
      path = "/etc/localtime";
      public_id = local_linux_zone_id(path);
   }
   else {
      if (not safe_iana_zone(ZoneID)) return ERR::Search;
      path = std::filesystem::path("/usr/share/zoneinfo") / std::string(ZoneID);
      public_id = std::string(ZoneID);
   }

   if (parse_tzif_file(path, public_id, StartYear, EndYear, is_local ? 1 : 0, Info)) return ERR::Okay;

   if (is_local) {
      fill_utc_info(Info, StartYear, EndYear, 1, 1);
      return ERR::Okay;
   }

   return ERR::Search;
}

#endif

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

//********************************************************************************************************************

static ERR TIME_Refresh(objTime *Self)
{
   return TIME_Query(Self);
}

/*********************************************************************************************************************

-METHOD-
GetTimeZoneInfo: .

...

-INPUT-
cstr ZoneID: Empty or NULL requests the local system zone.
int StartYear: Inclusive first year.  Must be non-zero.
int EndYear: Inclusive final year.  Must be >= StartYear.
!struct(*TimeZoneInfo) Info: Receives metadata and transitions.

-END-

*********************************************************************************************************************/

static ERR TIME_GetTimeZoneInfo(objTime *Self, struct pt::GetTimeZoneInfo *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);
   if ((Args->StartYear IS 0) or (Args->EndYear < Args->StartYear)) return log.warning(ERR::OutOfRange);

   struct TimeZoneInfo *tz;
   if (AllocMemory(sizeof(struct TimeZoneInfo), MEM::DATA|MEM::MANAGED, (APTR *)&tz, nullptr) IS ERR::Okay) {
      new (tz) struct TimeZoneInfo;
      SetResourceMgr(tz, &glTimeZoneHandler);
      Args->Info = tz;

      const std::string_view zone_id = Args->ZoneID ? std::string_view(Args->ZoneID) : std::string_view();
      if (is_utc_zone(zone_id)) {
         fill_utc_info(*tz, Args->StartYear, Args->EndYear, zone_id.empty() ? 1 : 0, 0);
         return ERR::Okay;
      }

   #ifdef _WIN32
      return get_windows_timezone_info(zone_id, Args->StartYear, Args->EndYear, *tz);
   #elif defined(__linux__)
      return get_linux_timezone_info(zone_id, Args->StartYear, Args->EndYear, *tz);
   #else
      if (zone_id.empty()) {
         fill_utc_info(*tz, Args->StartYear, Args->EndYear, 1, 1);
         return ERR::Okay;
      }
      else return ERR::Search;
   #endif
   }
   else return log.warning(ERR::AllocMemory);
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
   kt::Log log;
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
Timestamp: Read this field to get representation of the time as a single integer.

The Timestamp field is a 64-bit integer that represents the time object as an approximation of the number of
milliseconds represented in the time object (approximately the total amount of time passed since Zero-AD).  This is
convenient for summarising a time value for comparison with other time stamps, or for storing time in a 64-bit space.

The Timestamp value is dynamically calculated when reading this field.

*********************************************************************************************************************/

static ERR GET_Timestamp(objTime *Self, int64_t *Value)
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
   { "Timestamp",    FDF_INT64|FDF_R, GET_Timestamp },
   END_FIELD
};

#include "class_time_def.c"

//********************************************************************************************************************

extern ERR add_time_class(void)
{
   glTimeClass = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::TIME),
      fl::ClassVersion(VER_TIME),
      fl::Name("Time"),
      fl::Category(CCF::SYSTEM),
      fl::Actions(clTimeActions),
      fl::Methods(clTimeMethods),
      fl::Fields(clFields),
      fl::Size(sizeof(objTime)),
      fl::Path("modules:core"));

   return glTimeClass ? ERR::Okay : ERR::AddClass;
}

//********************************************************************************************************************
// XPath Date and Time Functions

namespace {

std::chrono::system_clock::time_point current_utc_time_point()
{
   auto now = std::chrono::system_clock::now();
   return std::chrono::time_point_cast<std::chrono::seconds>(now);
}

std::tm make_utc_tm(std::chrono::system_clock::time_point TimePoint)
{
   std::time_t raw_time = std::chrono::system_clock::to_time_t(TimePoint);
   std::tm utc{};
#if defined(_WIN32)
   gmtime_s(&utc, &raw_time);
#else
   gmtime_r(&raw_time, &utc);
#endif
   return utc;
}

std::string format_utc_date(const std::tm &Tm)
{
   char buffer[32];
   std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", Tm.tm_year + 1900, Tm.tm_mon + 1, Tm.tm_mday);
   return std::string(buffer);
}

std::string format_utc_time(const std::tm &Tm)
{
   char buffer[32];
   std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", Tm.tm_hour, Tm.tm_min, Tm.tm_sec);
   return std::string(buffer);
}

} // namespace

XPathVal XPathFunctionLibrary::function_current_date(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   auto now = current_utc_time_point();
   auto tm = make_utc_tm(now);
   return XPathVal(XPVT::Date, format_utc_date(tm));
}

XPathVal XPathFunctionLibrary::function_current_time(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   auto now = current_utc_time_point();
   auto tm = make_utc_tm(now);
   std::string time = format_utc_time(tm);
   time.push_back('Z');
   return XPathVal(XPVT::Time, std::move(time));
}

XPathVal XPathFunctionLibrary::function_current_date_time(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   auto now = current_utc_time_point();
   auto tm = make_utc_tm(now);
   std::string date = format_utc_date(tm);
   std::string time = format_utc_time(tm);
   std::string combined;
   combined.reserve(date.length() + time.length() + 2);
   combined.append(date);
   combined.push_back('T');
   combined.append(time);
   combined.push_back('Z');
   return XPathVal(XPVT::DateTime, std::move(combined));
}

XPathVal XPathFunctionLibrary::function_date_time(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() < 2u) return XPathVal();
   if (Args[0].is_empty() or Args[1].is_empty()) return XPathVal();

   std::string date_value = Args[0].to_string();
   std::string time_value = Args[1].to_string();

   DateTimeComponents combined;
   if (!combine_date_and_time(date_value, time_value, combined)) {
      if (Context.expression_unsupported) *Context.expression_unsupported = true;
      return XPathVal();
   }

   return XPathVal(XPVT::DateTime, serialise_date_time_components(combined));
}

XPathVal XPathFunctionLibrary::function_year_from_date_time(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args[0].is_empty()) return XPathVal();

   DateTimeComponents components;
   if (!parse_date_time_components(Args[0].to_string(), components) or !components.has_date) {
      return XPathVal(std::numeric_limits<double>::quiet_NaN());
   }

   return XPathVal((double)components.year);
}

XPathVal XPathFunctionLibrary::function_month_from_date_time(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args[0].is_empty()) return XPathVal();

   DateTimeComponents components;
   if (!parse_date_time_components(Args[0].to_string(), components) or !components.has_date) {
      return XPathVal(std::numeric_limits<double>::quiet_NaN());
   }

   return XPathVal((double)components.month);
}

XPathVal XPathFunctionLibrary::function_day_from_date_time(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args[0].is_empty()) return XPathVal();

   DateTimeComponents components;
   if (!parse_date_time_components(Args[0].to_string(), components) or !components.has_date) {
      return XPathVal(std::numeric_limits<double>::quiet_NaN());
   }

   return XPathVal((double)components.day);
}

XPathVal XPathFunctionLibrary::function_hours_from_date_time(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args[0].is_empty()) return XPathVal();

   DateTimeComponents components;
   if (!parse_date_time_components(Args[0].to_string(), components) or !components.has_time) {
      return XPathVal(std::numeric_limits<double>::quiet_NaN());
   }

   return XPathVal((double)components.hour);
}

XPathVal XPathFunctionLibrary::function_minutes_from_date_time(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args[0].is_empty()) return XPathVal();

   DateTimeComponents components;
   if (!parse_date_time_components(Args[0].to_string(), components) or !components.has_time) {
      return XPathVal(std::numeric_limits<double>::quiet_NaN());
   }

   return XPathVal((double)components.minute);
}

XPathVal XPathFunctionLibrary::function_seconds_from_date_time(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args[0].is_empty()) return XPathVal();

   DateTimeComponents components;
   if (!parse_date_time_components(Args[0].to_string(), components) or !components.has_time) {
      return XPathVal(std::numeric_limits<double>::quiet_NaN());
   }

   return XPathVal(components.second);
}

XPathVal XPathFunctionLibrary::function_timezone_from_date_time(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args[0].is_empty()) return XPathVal();

   DateTimeComponents components;
   if (!parse_date_time_components(Args[0].to_string(), components)) {
      if (Context.expression_unsupported) *Context.expression_unsupported = true;
      return XPathVal();
   }

   if (!components.has_timezone) return XPathVal();

   return XPathVal(format_timezone_duration(components.timezone_offset_minutes));
}

XPathVal XPathFunctionLibrary::function_year_from_date(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args[0].is_empty()) return XPathVal();

   DateTimeComponents components;
   if (!parse_date_value(Args[0].to_string(), components) or !components.has_date) {
      return XPathVal(std::numeric_limits<double>::quiet_NaN());
   }

   return XPathVal((double)components.year);
}

XPathVal XPathFunctionLibrary::function_month_from_date(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args[0].is_empty()) return XPathVal();

   DateTimeComponents components;
   if (!parse_date_value(Args[0].to_string(), components) or !components.has_date) {
      return XPathVal(std::numeric_limits<double>::quiet_NaN());
   }

   return XPathVal((double)components.month);
}

XPathVal XPathFunctionLibrary::function_day_from_date(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args[0].is_empty()) return XPathVal();

   DateTimeComponents components;
   if (!parse_date_value(Args[0].to_string(), components) or !components.has_date) {
      return XPathVal(std::numeric_limits<double>::quiet_NaN());
   }

   return XPathVal((double)components.day);
}

XPathVal XPathFunctionLibrary::function_timezone_from_date(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args[0].is_empty()) return XPathVal();

   DateTimeComponents components;
   if (!parse_date_value(Args[0].to_string(), components)) {
      if (Context.expression_unsupported) *Context.expression_unsupported = true;
      return XPathVal();
   }

   if (!components.has_timezone) return XPathVal();

   return XPathVal(format_timezone_duration(components.timezone_offset_minutes));
}

XPathVal XPathFunctionLibrary::function_hours_from_time(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args[0].is_empty()) return XPathVal();

   DateTimeComponents components;
   if (!parse_time_value(Args[0].to_string(), components) or !components.has_time) {
      return XPathVal(std::numeric_limits<double>::quiet_NaN());
   }

   return XPathVal((double)components.hour);
}

XPathVal XPathFunctionLibrary::function_minutes_from_time(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args[0].is_empty()) return XPathVal();

   DateTimeComponents components;
   if (!parse_time_value(Args[0].to_string(), components) or !components.has_time) {
      return XPathVal(std::numeric_limits<double>::quiet_NaN());
   }

   return XPathVal((double)components.minute);
}

XPathVal XPathFunctionLibrary::function_seconds_from_time(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args[0].is_empty()) return XPathVal();

   DateTimeComponents components;
   if (!parse_time_value(Args[0].to_string(), components) or !components.has_time) {
      return XPathVal(std::numeric_limits<double>::quiet_NaN());
   }

   return XPathVal(components.second);
}

XPathVal XPathFunctionLibrary::function_timezone_from_time(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args[0].is_empty()) return XPathVal();

   DateTimeComponents components;
   if (!parse_time_value(Args[0].to_string(), components)) {
      if (Context.expression_unsupported) *Context.expression_unsupported = true;
      return XPathVal();
   }

   if (!components.has_timezone) return XPathVal();

   return XPathVal(format_timezone_duration(components.timezone_offset_minutes));
}

XPathVal XPathFunctionLibrary::function_adjust_date_time_to_timezone(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args[0].is_empty()) return XPathVal();

   std::string value = Args[0].to_string();
   DateTimeComponents components;
   if (!parse_date_time_components(value, components)) {
      if (Context.expression_unsupported) *Context.expression_unsupported = true;
      return XPathVal(XPVT::DateTime, value);
   }

   bool remove_timezone = false;
   bool have_target = false;
   int target_offset = 0;

   if (Args.size() > 1u) {
      if (Args[1].is_empty()) {
         remove_timezone = true;
      }
      else {
         int parsed_offset = 0;
         if (!parse_timezone_duration(Args[1].to_string(), parsed_offset)) {
            if (Context.expression_unsupported) *Context.expression_unsupported = true;
            return XPathVal(XPVT::DateTime, value);
         }
         target_offset = parsed_offset;
         have_target = true;
      }
   }

   if (remove_timezone) {
      components.has_timezone = false;
      components.timezone_offset_minutes = 0;
      components.timezone_is_utc = false;
      return XPathVal(XPVT::DateTime, serialise_date_time_components(components));
   }

   if (!have_target) {
      target_offset = 0;
      have_target = true;
   }

   std::chrono::sys_time<std::chrono::microseconds> utc_time;
   if (!components_to_utc_time(components, 0, utc_time)) {
      if (Context.expression_unsupported) *Context.expression_unsupported = true;
      return XPathVal(XPVT::DateTime, value);
   }

   DateTimeComponents adjusted = components_from_utc_time(utc_time, target_offset, true, true, true);
   return XPathVal(XPVT::DateTime, serialise_date_time_components(adjusted));
}

XPathVal XPathFunctionLibrary::function_adjust_date_to_timezone(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args[0].is_empty()) return XPathVal();

   std::string value = Args[0].to_string();
   DateTimeComponents components;
   if (!parse_date_value(value, components)) {
      if (Context.expression_unsupported) *Context.expression_unsupported = true;
      return XPathVal(XPVT::Date, value);
   }

   bool remove_timezone = false;
   bool have_target = false;
   int target_offset = 0;

   if (Args.size() > 1u) {
      if (Args[1].is_empty()) {
         remove_timezone = true;
      }
      else {
         int parsed_offset = 0;
         if (!parse_timezone_duration(Args[1].to_string(), parsed_offset)) {
            if (Context.expression_unsupported) *Context.expression_unsupported = true;
            return XPathVal(XPVT::Date, value);
         }
         target_offset = parsed_offset;
         have_target = true;
      }
   }

   if (remove_timezone) {
      components.has_timezone = false;
      components.timezone_offset_minutes = 0;
      components.timezone_is_utc = false;
      return XPathVal(XPVT::Date, serialise_date_only(components, false));
   }

   if (!have_target) {
      target_offset = 0;
      have_target = true;
   }

   std::chrono::sys_time<std::chrono::microseconds> utc_time;
   if (!components_to_utc_time(components, 0, utc_time)) {
      if (Context.expression_unsupported) *Context.expression_unsupported = true;
      return XPathVal(XPVT::Date, value);
   }

   DateTimeComponents adjusted = components_from_utc_time(utc_time, target_offset, true, true, false);
   return XPathVal(XPVT::Date, serialise_date_only(adjusted, true));
}

XPathVal XPathFunctionLibrary::function_adjust_time_to_timezone(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty() or Args[0].is_empty()) return XPathVal();

   std::string value = Args[0].to_string();
   DateTimeComponents components;
   if (!parse_time_value(value, components)) {
      if (Context.expression_unsupported) *Context.expression_unsupported = true;
      return XPathVal(XPVT::Time, value);
   }

   bool remove_timezone = false;
   bool have_target = false;
   int target_offset = 0;

   if (Args.size() > 1u) {
      if (Args[1].is_empty()) {
         remove_timezone = true;
      }
      else {
         int parsed_offset = 0;
         if (!parse_timezone_duration(Args[1].to_string(), parsed_offset)) {
            if (Context.expression_unsupported) *Context.expression_unsupported = true;
            return XPathVal(XPVT::Time, value);
         }
         target_offset = parsed_offset;
         have_target = true;
      }
   }

   if (remove_timezone) {
      components.has_timezone = false;
      components.timezone_offset_minutes = 0;
      components.timezone_is_utc = false;
      return XPathVal(XPVT::Time, serialise_time_only(components, false));
   }

   if (!have_target) {
      target_offset = 0;
      have_target = true;
   }

   std::chrono::sys_time<std::chrono::microseconds> utc_time;
   if (!components_to_utc_time(components, 0, utc_time)) {
      if (Context.expression_unsupported) *Context.expression_unsupported = true;
      return XPathVal(XPVT::Time, value);
   }

   DateTimeComponents adjusted = components_from_utc_time(utc_time, target_offset, true, false, true);
   return XPathVal(XPVT::Time, serialise_time_only(adjusted, true));
}

XPathVal XPathFunctionLibrary::function_implicit_timezone(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   return XPathVal(std::string("PT0S"));
}

XPathVal XPathFunctionLibrary::function_years_from_duration(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   DurationComponents components;
   DurationParseStatus status = prepare_duration_components(Args, components, false, false);

   if (status IS DurationParseStatus::Empty) return XPathVal();
   if (status IS DurationParseStatus::Error) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   double value = (double)components.years.count();
   if (components.negative) value = -value;
   return XPathVal(value);
}

XPathVal XPathFunctionLibrary::function_months_from_duration(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   DurationComponents components;
   DurationParseStatus status = prepare_duration_components(Args, components, false, false);

   if (status IS DurationParseStatus::Empty) return XPathVal();
   if (status IS DurationParseStatus::Error) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   double value = (double)components.months.count();
   if (components.negative) value = -value;
   return XPathVal(value);
}

XPathVal XPathFunctionLibrary::function_days_from_duration(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   DurationComponents components;
   DurationParseStatus status = prepare_duration_components(Args, components, false, false);

   if (status IS DurationParseStatus::Empty) return XPathVal();
   if (status IS DurationParseStatus::Error) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   double value = (double)components.days.count();
   if (components.negative) value = -value;
   return XPathVal(value);
}

XPathVal XPathFunctionLibrary::function_hours_from_duration(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   DurationComponents components;
   DurationParseStatus status = prepare_duration_components(Args, components, false, false);

   if (status IS DurationParseStatus::Empty) return XPathVal();
   if (status IS DurationParseStatus::Error) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   double value = (double)components.hours.count();
   if (components.negative) value = -value;
   return XPathVal(value);
}

XPathVal XPathFunctionLibrary::function_minutes_from_duration(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   DurationComponents components;
   DurationParseStatus status = prepare_duration_components(Args, components, false, false);

   if (status IS DurationParseStatus::Empty) return XPathVal();
   if (status IS DurationParseStatus::Error) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   double value = (double)components.minutes.count();
   if (components.negative) value = -value;
   return XPathVal(value);
}

XPathVal XPathFunctionLibrary::function_seconds_from_duration(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   DurationComponents components;
   DurationParseStatus status = prepare_duration_components(Args, components, false, false);

   if (status IS DurationParseStatus::Empty) return XPathVal();
   if (status IS DurationParseStatus::Error) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   double value = components.seconds.count();
   if (components.negative) value = -value;
   return XPathVal(value);
}

XPathVal XPathFunctionLibrary::function_years_from_year_month_duration(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   DurationComponents components;
   DurationParseStatus status = prepare_duration_components(Args, components, true, false);

   if (status IS DurationParseStatus::Empty) return XPathVal();
   if (status IS DurationParseStatus::Error) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   double value = (double)components.years.count();
   if (components.negative) value = -value;
   return XPathVal(value);
}

XPathVal XPathFunctionLibrary::function_months_from_year_month_duration(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   DurationComponents components;
   DurationParseStatus status = prepare_duration_components(Args, components, true, false);

   if (status IS DurationParseStatus::Empty) return XPathVal();
   if (status IS DurationParseStatus::Error) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   double value = (double)components.months.count();
   if (components.negative) value = -value;
   return XPathVal(value);
}

XPathVal XPathFunctionLibrary::function_days_from_day_time_duration(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   DurationComponents components;
   DurationParseStatus status = prepare_duration_components(Args, components, false, true);

   if (status IS DurationParseStatus::Empty) return XPathVal();
   if (status IS DurationParseStatus::Error) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   double value = (double)components.days.count();
   if (components.negative) value = -value;
   return XPathVal(value);
}

XPathVal XPathFunctionLibrary::function_hours_from_day_time_duration(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   DurationComponents components;
   DurationParseStatus status = prepare_duration_components(Args, components, false, true);

   if (status IS DurationParseStatus::Empty) return XPathVal();
   if (status IS DurationParseStatus::Error) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   double value = (double)components.hours.count();
   if (components.negative) value = -value;
   return XPathVal(value);
}

XPathVal XPathFunctionLibrary::function_minutes_from_day_time_duration(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   DurationComponents components;
   DurationParseStatus status = prepare_duration_components(Args, components, false, true);

   if (status IS DurationParseStatus::Empty) return XPathVal();
   if (status IS DurationParseStatus::Error) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   double value = (double)components.minutes.count();
   if (components.negative) value = -value;
   return XPathVal(value);
}

XPathVal XPathFunctionLibrary::function_seconds_from_day_time_duration(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   DurationComponents components;
   DurationParseStatus status = prepare_duration_components(Args, components, false, true);

   if (status IS DurationParseStatus::Empty) return XPathVal();
   if (status IS DurationParseStatus::Error) return XPathVal(std::numeric_limits<double>::quiet_NaN());

   double value = components.seconds.count();
   if (components.negative) value = -value;
   return XPathVal(value);
}

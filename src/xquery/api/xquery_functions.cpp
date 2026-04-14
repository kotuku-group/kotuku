// XPath Function Library and Value System Implementation
//
// XPath expressions depend on a rich set of standard functions and a loosely typed value model.  This
// file provides both: XPathVal encapsulates conversions between node-sets, numbers, booleans, and
// strings, while the function registry offers implementations of the core function library required by
// the evaluator.  The code emphasises fidelity to the XPath 2.0 specification—string coercions mirror
// the spec's edge cases, numeric conversions preserve NaN semantics, and node-set operations respect
// document order guarantees enforced elsewhere in the module.
//
// The implementation is intentionally self-contained.  The evaluator interacts with XPathVal to
// manipulate intermediate results and delegates built-in function invocations to the routines defined
// below.  Keeping the behaviour consolidated here simplifies future extensions (for example, adding
// namespace-aware functions or performance-focused helpers) without polluting the evaluator with
// coercion details.

#include <kotuku/modules/regex.h>

#include "xquery_functions.h"
#include "../../xml/schema/type_checker.h"
#include "../../xml/uri_utils.h"
#include "../eval/date_time_utils.h"

#include <chrono>
#include <ctime>

#include "../../link/unicode.h"

namespace {

//********************************************************************************************************************
// Determines whether a character qualifies as an unreserved URI character according to RFC 3986.  Unreserved
// characters include alpha-numerics and a restricted set of punctuation that do not require percent-encoding in URIs.

static bool is_unreserved_uri_character(unsigned char Code)
{
   if ((Code >= 'A' and Code <= 'Z') or (Code >= 'a' and Code <= 'z') or (Code >= '0' and Code <= '9')) return true;

   switch (Code) {
      case '-':
      case '_':
      case '.':
      case '!':
      case '~':
      case '*':
      case '\'':
      case '(':
      case ')':
         return true;
   }

   return false;
}

//********************************************************************************************************************
// Encodes a string for use in URIs by percent-encoding characters that are not unreserved according to RFC 3986.
// This implementation preserves alphanumerics and RFC 3986 unreserved punctuation while encoding all other bytes
// as percent-encoded octets.

static std::string encode_for_uri_impl(const std::string &Value)
{
   std::string result;
   result.reserve(Value.length() * 3);

   for (char ch : Value) {
      unsigned char code = (unsigned char)ch;
      if (is_unreserved_uri_character(code)) {
         result.push_back((char)code);
      }
      else {
         static const char hex_digits[] = "0123456789ABCDEF";
         result.push_back('%');
         result.push_back(hex_digits[(code >> 4) & 0x0F]);
         result.push_back(hex_digits[code & 0x0F]);
      }
   }

   return result;
}

//********************************************************************************************************************
// Performs a global search-and-replace operation, replacing all occurrences of From with To in the Text string.
// This function modifies the text in-place and handles overlapping replacements by advancing past the inserted text.

static void replace_all(std::string &Text, std::string_view From, std::string_view To)
{
   if (From.empty()) return;

   size_t position = 0;
   while (true) {
      position = Text.find(From, position);
      if (position IS std::string::npos) break;
      Text.replace(position, From.length(), To);
      position += To.length();
   }
}

//********************************************************************************************************************
// Encodes a string for HTML-escaped URI representation by first percent-encoding the string and then escaping
// the percent-encoded sequences as HTML entities. This produces a URI that is safe for embedding in HTML attributes.

static std::string escape_html_uri_impl(const std::string &Value)
{
   std::string encoded = encode_for_uri_impl(Value);

   // Convert percent-encoded sequences to HTML entity equivalents for safe HTML embedding
   replace_all(encoded, "%26", "&amp;");
   replace_all(encoded, "%3C", "&lt;");
   replace_all(encoded, "%3E", "&gt;");
   replace_all(encoded, "%22", "&quot;");
   replace_all(encoded, "%27", "&apos;");

   return encoded;
}

//********************************************************************************************************************
// Converts a string to upper or lower case by transforming each character. This operates on bytes rather than
// Unicode codepoints, suitable for ASCII and Latin-1 text processing.

static std::string apply_string_case(const std::string &Value, bool Upper)
{
   std::string result = Value;
   std::transform(result.begin(), result.end(), result.begin(), [Upper](char Ch) {
      unsigned char code = (unsigned char)Ch;
      return Upper ? (char)std::toupper(code) : (char)std::tolower(code);
   });
   return result;
}

//********************************************************************************************************************
// Appends a Unicode codepoint to the output string as UTF-8 encoded bytes. Invalid codepoints (surrogates or
// values exceeding U+10FFFF) are replaced with the replacement character U+FFFD.

static void append_codepoint_utf8(std::string &Output, uint32_t Codepoint)
{
   // Validate codepoint: reject surrogates and values beyond Unicode range
   if ((Codepoint > 0x10FFFFu) or ((Codepoint >= 0xD800u) and (Codepoint <= 0xDFFFu))) {
      Codepoint = 0xFFFDu;
   }

   char buffer[8] = { 0 };
   int written = UTF8WriteValue((int)Codepoint, buffer, (int)sizeof(buffer));
   if (written <= 0) return;

   Output.append(buffer, (size_t)written);
}

//********************************************************************************************************************
// Decodes a UTF-8 encoded string into a vector of Unicode codepoints. Invalid UTF-8 sequences are treated as
// individual bytes and preserved as their numeric values.

static std::vector<uint32_t> decode_codepoints(const std::string &Input)
{
   std::vector<uint32_t> codepoints;
   codepoints.reserve(Input.length());

   size_t offset = 0u;
   while (offset < Input.length()) {
      int length = 0;
      uint32_t value = UTF8ReadValue(Input.c_str() + offset, &length);
      if (length <= 0) {
         // Invalid UTF-8 sequence; treat byte as individual codepoint
         value = (unsigned char)Input[offset];
         length = 1;
      }

      codepoints.push_back(value);
      offset += (size_t)length;
   }

   return codepoints;
}

//********************************************************************************************************************
// Encodes a sequence of Unicode codepoints as a UTF-8 string.

static std::string encode_codepoints(const std::vector<uint32_t> &Codepoints)
{
   std::string output;
   for (uint32_t code : Codepoints) append_codepoint_utf8(output, code);
   return output;
}

//********************************************************************************************************************
// Performs Unicode normalisation according to the specified form (NFC, NFD, NFKC, or NFKD). This is a simplified
// implementation that handles only the most common European character combinations. Unsupported normalisation forms
// set the Unsupported flag and return the original string unmodified.

static std::string simple_normalise_unicode(const std::string &Value, std::string_view Form, bool *Unsupported)
{
   if (Form.empty()) return Value;

   std::string normalised_form(Form);
   std::transform(normalised_form.begin(), normalised_form.end(), normalised_form.begin(), [](char Ch) {
      unsigned char code = (unsigned char)Ch;
      return (char)std::toupper(code);
   });

   // Composed forms: combine compatible sequences into precomposed characters
   if ((normalised_form IS "NFC") or (normalised_form IS "NFKC")) {
      auto codepoints = decode_codepoints(Value);
      std::vector<uint32_t> result;
      result.reserve(codepoints.size());

      for (size_t index = 0u; index < codepoints.size(); ++index) {
         uint32_t code = codepoints[index];
         if ((index + 1u) < codepoints.size()) {
            uint32_t next = codepoints[index + 1u];
            // Compose e + combining acute accent into precomposed é
            if ((code IS 0x0065u) and (next IS 0x0301u)) {
               result.push_back(0x00E9u);
               index++;
               continue;
            }
            // Compose E + combining acute accent into precomposed É
            if ((code IS 0x0045u) and (next IS 0x0301u)) {
               result.push_back(0x00C9u);
               index++;
               continue;
            }
         }
         result.push_back(code);
      }

      return encode_codepoints(result);
   }

   // Decomposed forms: split precomposed characters into base + combining marks
   if ((normalised_form IS "NFD") or (normalised_form IS "NFKD")) {
      auto codepoints = decode_codepoints(Value);
      std::vector<uint32_t> result;
      result.reserve(codepoints.size() * 2u);

      for (uint32_t code : codepoints) {
         // Decompose precomposed é into e + combining acute accent
         if (code IS 0x00E9u) {
            result.push_back(0x0065u);
            result.push_back(0x0301u);
            continue;
         }
         // Decompose precomposed É into E + combining acute accent
         if (code IS 0x00C9u) {
            result.push_back(0x0045u);
            result.push_back(0x0301u);
            continue;
         }
         result.push_back(code);
      }

      return encode_codepoints(result);
   }

   if (Unsupported) *Unsupported = true;
   return Value;
}

using xml::uri::is_absolute_uri;
using xml::uri::strip_query_fragment;
using xml::uri::normalise_path_segments;
using xml::uri::resolve_relative_uri;

//********************************************************************************************************************
// Represents the parsed components of an XML dateTime value, including optional date, time, and timezone
// information. Default values follow W3C XML Schema conventions.

struct DateTimeComponents
{
   int year = 0;
   int month = 1;
   int day = 1;
   int hour = 0;
   int minute = 0;
   double second = 0.0;
   bool has_date = false;
   bool has_time = false;
   bool has_timezone = false;
   bool timezone_is_utc = false;
   int timezone_offset_minutes = 0;
};

//********************************************************************************************************************
// Parses a fixed-width decimal integer from a string view using std::from_chars. Returns true if parsing succeeds;
// false if the text cannot be completely converted to an integer.

static bool parse_fixed_number(std::string_view Text, int &Output)
{
   int value = 0;
   auto result = std::from_chars(Text.data(), Text.data() + Text.length(), value);
   if (result.ec != std::errc()) return false;
   Output = value;
   return true;
}

enum class DurationParseStatus { Empty, Error, Value };

//********************************************************************************************************************
// Validates and prepares duration components from XPath function arguments. Enforces constraints for year-month
// or day-time only durations and normalises the components. Returns Empty if no arguments, Error if invalid, or
// Value if successfully parsed.

static DurationParseStatus prepare_duration_components(const std::vector<XPathVal> &Args,
   DurationComponents &Components, bool RequireYearMonthOnly, bool RequireDayTimeOnly)
{
   if (Args.empty()) return DurationParseStatus::Empty;
   if (Args.size() != 1u) return DurationParseStatus::Error;
   if (Args[0].is_empty()) return DurationParseStatus::Empty;

   std::string value = Args[0].to_string();
   if (not parse_xs_duration(value, Components)) return DurationParseStatus::Error;

   if (RequireYearMonthOnly) {
      if (Components.has_day or Components.has_hour or Components.has_minute or Components.has_second) {
         return DurationParseStatus::Error;
      }
   }

   if (RequireDayTimeOnly) {
      if (Components.has_year or Components.has_month) return DurationParseStatus::Error;
   }

   normalise_duration_components(Components);

   return DurationParseStatus::Value;
}

//********************************************************************************************************************
// Parses timezone information from a string, supporting UTC designation ('Z'), and ±HH:MM or ±HHMM formats.
// Returns true if parsing succeeds; false if the timezone specification is malformed.

static bool parse_timezone(std::string_view Text, DateTimeComponents &Components)
{
   if (Text.empty()) return true;

   Components.has_timezone = true;

   if ((Text.length() IS 1u) and ((Text[0] IS 'Z') or (Text[0] IS 'z'))) {
      Components.timezone_is_utc = true;
      Components.timezone_offset_minutes = 0;
      return true;
   }

   if (Text.length() < 3u) return false;
   char sign = Text[0];
   if ((sign != '+') and (sign != '-')) return false;

   std::string_view hours_view = Text.substr(1u, 2u);
   int hours = 0;
   if (not parse_fixed_number(hours_view, hours)) return false;

   size_t pos = 3u;
   int minutes = 0;
   if ((Text.length() >= 6u) and (Text[3] IS ':')) {
      std::string_view minutes_view = Text.substr(4u, 2u);
      if (not parse_fixed_number(minutes_view, minutes)) return false;
      pos = 6u;
   }
   else if (Text.length() >= 5u) {
      std::string_view minutes_view = Text.substr(3u, 2u);
      if (not parse_fixed_number(minutes_view, minutes)) return false;
      pos = 5u;
   }

   if (Text.length() != pos) return false;

   int total = hours * 60 + minutes;
   if (sign IS '-') total = -total;
   Components.timezone_offset_minutes = total;
   Components.timezone_is_utc = (total IS 0);
   return true;
}

//********************************************************************************************************************
// Parses time values from string in HH:MM:SS format with optional fractional seconds and timezone offset.
// Returns true if time format is valid; false if malformed or missing required components.

static bool parse_time_value(std::string_view Text, DateTimeComponents &Components)
{
   if (Text.length() < 8u) return false;

   size_t tz_pos = std::string::npos;
   for (size_t index = 0u; index < Text.length(); ++index) {
      char ch = Text[index];
      if ((ch IS '+') or (ch IS '-') or (ch IS 'Z') or (ch IS 'z')) {
         if (index >= 5u) {
            tz_pos = index;
            break;
         }
      }
   }

   std::string_view time_section = Text;
   std::string_view tz_section;

   if (tz_pos != std::string::npos) {
      time_section = Text.substr(0u, tz_pos);
      tz_section = Text.substr(tz_pos);
   }

   if ((time_section.length() < 8u) or (time_section[2] != ':') or (time_section[5] != ':')) return false;

   int hour = 0;
   int minute = 0;
   int second = 0;

   if (not parse_fixed_number(time_section.substr(0u, 2u), hour)) return false;
   if (not parse_fixed_number(time_section.substr(3u, 2u), minute)) return false;
   if (not parse_fixed_number(time_section.substr(6u, 2u), second)) return false;

   Components.hour = hour;
   Components.minute = minute;
   Components.second = (double)second;
   Components.has_time = true;

   size_t fractional_pos = time_section.find('.');
   if (fractional_pos != std::string::npos) {
      std::string_view fraction = time_section.substr(fractional_pos + 1u);
      int fraction_value = 0;
      if (not fraction.empty() and parse_fixed_number(fraction, fraction_value)) {
         double scale = std::pow(10.0, (double)fraction.length());
         Components.second += (double)fraction_value / scale;
      }
   }

   if (not tz_section.empty()) return parse_timezone(tz_section, Components);
   return true;
}

//********************************************************************************************************************
// Parses date values from string in YYYY-MM-DD format with optional timezone offset.
// Returns true if date format is valid; false if malformed or missing required components.

static bool parse_date_value(std::string_view Text, DateTimeComponents &Components)
{
   if (Text.length() < 10u) return false;
   if ((Text[4] != '-') or (Text[7] != '-')) return false;

   int year = 0;
   int month = 0;
   int day = 0;

   if (not parse_fixed_number(Text.substr(0u, 4u), year)) return false;
   if (not parse_fixed_number(Text.substr(5u, 2u), month)) return false;
   if (not parse_fixed_number(Text.substr(8u, 2u), day)) return false;

   Components.year = year;
   Components.month = month;
   Components.day = day;
   Components.has_date = true;

   if (Text.length() IS 10u) return true;

   std::string_view tz_section = Text.substr(10u);
   return parse_timezone(tz_section, Components);
}

//********************************************************************************************************************
// Routes datetime parsing to appropriate function based on content: dispatches to parse_date_value if hyphens
// are found, parse_time_value otherwise, or combined parsing if 'T' separator is present.

static bool parse_date_time_components(std::string_view Text, DateTimeComponents &Components)
{
   size_t t_pos = Text.find('T');
   if (t_pos != std::string::npos) {
      std::string_view date_part = Text.substr(0u, t_pos);
      std::string_view time_part = Text.substr(t_pos + 1u);
      return parse_date_value(date_part, Components) and parse_time_value(time_part, Components);
   }

   if (Text.find('-') != std::string::npos) return parse_date_value(Text, Components);
   return parse_time_value(Text, Components);
}

//********************************************************************************************************************
// Formats an integer value with optional zero-padding or space-padding to reach the specified width. Preserves
// the sign of negative values by prepending '-' after padding is applied.

static std::string format_integer_component(int64_t Value, int Width, bool ZeroPad)
{
   bool negative = Value < 0;
   uint64_t absolute = negative ? (uint64_t)(-Value) : (uint64_t)Value;
   std::string digits = std::to_string(absolute);

   if ((Width > 0) and ((int)digits.length() < Width)) {
      std::string padding((size_t)(Width - (int)digits.length()), ZeroPad ? '0' : ' ');
      digits.insert(0u, padding);
   }

   if (negative) digits.insert(digits.begin(), '-');
   return digits;
}

//********************************************************************************************************************
// Formats timezone offset as a string: returns 'Z' for UTC, or ±HH:MM for other offsets.
// Returns empty string if no timezone is present in the components.

static std::string format_timezone(const DateTimeComponents &Components)
{
   if (not Components.has_timezone) return std::string();
   if (Components.timezone_is_utc or (Components.timezone_offset_minutes IS 0)) return std::string("Z");

   int offset = Components.timezone_offset_minutes;
   char sign = '+';
   if (offset < 0) {
      sign = '-';
      offset = -offset;
   }

   int hours = offset / 60;
   int minutes = offset % 60;

   return std::format("{}{:02d}:{:02d}", sign, hours, minutes);
}

//********************************************************************************************************************
// Formats a single datetime component based on token specification (e.g., 'Y004' for year with 4-digit padding).
// Supports: Y (year), M (month), D (day), H (hour), m (minute), s (second), Z/z (timezone). Returns the original
// token if no matching component is found.

static std::string format_component(const DateTimeComponents &Components, const std::string &Token)
{
   if (Token.empty()) return std::string();

   char symbol = Token[0];
   std::string spec = Token.substr(1u);

   int width = 0;
   bool zero_pad = false;
   for (char ch : spec) {
      if (std::isdigit((unsigned char)ch)) {
         width++;
         if (ch IS '0') zero_pad = true;
      }
   }

   switch (symbol) {
      case 'Y': return format_integer_component(Components.year, (width > 0) ? width : 4, true);
      case 'M': return format_integer_component(Components.month, (width > 0) ? width : 2, true);
      case 'D': return format_integer_component(Components.day, (width > 0) ? width : 2, true);
      case 'H': return format_integer_component(Components.hour, (width > 0) ? width : 2, true);
      case 'm': return format_integer_component(Components.minute, (width > 0) ? width : 2, true);
      case 's': {
         int64_t rounded = (int64_t)std::llround(Components.second);
         return format_integer_component(rounded, (width > 0) ? width : 2, true);
      }
      case 'Z':
      case 'z':
         return format_timezone(Components);
      default:
         break;
   }

   return Token;
}

//********************************************************************************************************************
// Formats datetime components using a picture string with [token] placeholders and 'quoted literal' sections.
// Processes the picture character-by-character, replacing tokens with formatted components and preserving
// quoted literal text.

static std::string format_with_picture(const DateTimeComponents &Components, const std::string &Picture)
{
   std::string output;
   for (size_t index = 0u; index < Picture.length();) {
      char ch = Picture[index];
      if (ch IS '[') {
         size_t end = Picture.find(']', index + 1u);
         if (end IS std::string::npos) break;
         std::string token = Picture.substr(index + 1u, end - index - 1u);
         output.append(format_component(Components, token));
         index = end + 1u;
      }
      else if (ch IS '\'') {
         size_t end = Picture.find('\'', index + 1u);
         if (end IS std::string::npos) break;
         output.append(Picture.substr(index + 1u, end - index - 1u));
         index = end + 1u;
      }
      else {
         output.push_back(ch);
         ++index;
      }
   }
   return output;
}

//********************************************************************************************************************
// Formats seconds as SS.ffffff format with fractional microseconds. Removes trailing zeros from fractional part
// and clamps negative values to zero. Handles rounding when fractional overflow occurs.

static std::string format_seconds_field(double Value)
{
   if (Value < 0.0) Value = 0.0;

   double integral_part = 0.0;
   double fractional_part = std::modf(Value, &integral_part);

   int64_t integral_seconds = (int64_t)integral_part;
   int64_t fractional_microseconds = (int64_t)std::llround(fractional_part * 1000000.0);

   if (fractional_microseconds >= 1000000ll) {
      fractional_microseconds -= 1000000ll;
      ++integral_seconds;
   }

   std::string seconds = format_integer_component(integral_seconds, 2, true);

   if (fractional_microseconds > 0ll) {
      std::string fractional_digits = std::to_string(fractional_microseconds);
      while (fractional_digits.length() < 6u) fractional_digits.insert(0u, 1u, '0');
      while ((not fractional_digits.empty()) and (fractional_digits.back() IS '0')) fractional_digits.pop_back();
      if (not fractional_digits.empty()) {
         seconds.push_back('.');
         seconds.append(fractional_digits);
      }
   }

   return seconds;
}

//********************************************************************************************************************
// Serialises date component to YYYY-MM-DD string format with optional timezone suffix.

static std::string serialise_date_only(const DateTimeComponents &Components, bool IncludeTimezone)
{
   auto year = format_integer_component(Components.year, 4, true);
   auto month = format_integer_component(Components.month, 2, true);
   auto day = format_integer_component(Components.day, 2, true);

   std::string result = std::format("{}-{}-{}", year, month, day);

   if (IncludeTimezone and Components.has_timezone) result.append(format_timezone(Components));

   return result;
}

//********************************************************************************************************************
// Serialises time component to HH:MM:SS.ffffff string format with optional timezone suffix.

static std::string serialise_time_only(const DateTimeComponents &Components, bool IncludeTimezone)
{
   auto hour = format_integer_component(Components.hour, 2, true);
   auto minute = format_integer_component(Components.minute, 2, true);
   auto second = format_seconds_field(Components.second);

   std::string result = std::format("{}:{}:{}", hour, minute, second);

   if (IncludeTimezone and Components.has_timezone) result.append(format_timezone(Components));

   return result;
}

//********************************************************************************************************************
// Serialises both date and time components combined with 'T' separator, returning the full ISO 8601 string.

static std::string serialise_date_time_components(const DateTimeComponents &Components)
{
   std::string result = serialise_date_only(Components, false);
   result.push_back('T');
   result.append(serialise_time_only(Components, true));
   return result;
}

//********************************************************************************************************************
// Merges separate date and time strings into a single DateTimeComponents structure. Validates that timezone
// information is consistent between both parts if both are specified. Returns false if parsing fails or
// timezone conflict is detected.

static bool combine_date_and_time(const std::string &DateValue, const std::string &TimeValue,
   DateTimeComponents &Combined)
{
   DateTimeComponents date_components;
   if (not parse_date_value(DateValue, date_components)) return false;

   DateTimeComponents time_components;
   if (not parse_time_value(TimeValue, time_components)) return false;

   Combined = date_components;
   Combined.hour = time_components.hour;
   Combined.minute = time_components.minute;
   Combined.second = time_components.second;
   Combined.has_time = time_components.has_time;

   bool date_has_timezone = date_components.has_timezone;
   bool time_has_timezone = time_components.has_timezone;

   if (date_has_timezone and time_has_timezone) {
      if (date_components.timezone_offset_minutes != time_components.timezone_offset_minutes) return false;
      Combined.has_timezone = true;
      Combined.timezone_offset_minutes = time_components.timezone_offset_minutes;
      Combined.timezone_is_utc = time_components.timezone_is_utc;
   }
   else if (time_has_timezone) {
      Combined.has_timezone = true;
      Combined.timezone_offset_minutes = time_components.timezone_offset_minutes;
      Combined.timezone_is_utc = time_components.timezone_is_utc;
   }
   else if (date_has_timezone) {
      Combined.has_timezone = true;
      Combined.timezone_offset_minutes = date_components.timezone_offset_minutes;
      Combined.timezone_is_utc = date_components.timezone_is_utc;
   }
   else {
      Combined.has_timezone = false;
      Combined.timezone_offset_minutes = 0;
      Combined.timezone_is_utc = false;
   }

   return true;
}

//********************************************************************************************************************
// Parses timezone offset expressed as ISO 8601 duration format (e.g., PT5H30M for +5:30). Validates that
// only hour and minute components are present, enforces valid range (-14:00 to +14:00), and rejects second
// components. Returns true if successfully parsed; false if format is invalid or out of range.

static bool parse_timezone_duration(const std::string &Text, int &OffsetMinutes)
{
   DurationComponents components;
   if (not parse_xs_duration(Text, components)) return false;

   normalise_duration_components(components);

   if (components.has_year or components.has_month or components.has_day) return false;
   if (components.has_second) return false;

   int64_t total_minutes = components.hours.count() * 60ll + components.minutes.count();
   if (components.negative) total_minutes = -total_minutes;

   if (total_minutes < -14ll * 60ll or total_minutes > 14ll * 60ll) return false;

   OffsetMinutes = (int)total_minutes;
   return true;
}

//********************************************************************************************************************
// Formats timezone offset in minutes as ISO 8601 duration string (e.g., PT5H30M for 330 minutes). Handles
// negative offsets by prepending '-' to the duration. Returns "PT0S" for zero offset.

static std::string format_timezone_duration(int OffsetMinutes)
{
   if (OffsetMinutes IS 0) return std::string("PT0S");

   std::string result;
   if (OffsetMinutes < 0) {
      result.append("-PT");
      OffsetMinutes = -OffsetMinutes;
   }
   else {
      result.append("PT");
   }

   int hours = OffsetMinutes / 60;
   int minutes = OffsetMinutes % 60;

   if (hours != 0) {
      result.append(std::format("{}H", hours));
   }

   if (minutes != 0) {
      result.append(std::format("{}M", minutes));
   }

   if ((hours IS 0) and (minutes IS 0)) result.append("0S");

   return result;
}

//********************************************************************************************************************
// Converts DateTimeComponents to UTC time point by validating date/time values, constructing a local time point,
// and applying timezone offset. Returns false if date/time values are invalid. ImplicitTimezoneMinutes is used
// if no explicit timezone is present in the components.

static bool components_to_utc_time(const DateTimeComponents &Components, int ImplicitTimezoneMinutes,
   std::chrono::sys_time<std::chrono::microseconds> &UtcTime)
{
   using namespace std::chrono;

   int year_value = Components.has_date ? Components.year : 1970;
   int month_value = Components.has_date ? Components.month : 1;
   int day_value = Components.has_date ? Components.day : 1;

   year y(year_value);
   month m(month_value);
   day d(day_value);

   if (!y.ok() or !m.ok() or !d.ok()) return false;

   year_month_day ymd(y, m, d);
   if (not ymd.ok()) return false;

   sys_days day_point(ymd);

   int hour = Components.has_time ? Components.hour : 0;
   int minute = Components.has_time ? Components.minute : 0;
   double seconds_value = Components.has_time ? Components.second : 0.0;

   double integral_part = 0.0;
   double fractional_part = std::modf(seconds_value, &integral_part);

   auto integral_seconds = (int64_t)integral_part;
   auto microseconds_value = (int64_t)std::llround(fractional_part * 1000000.0);

   if (microseconds_value >= 1000000ll) {
      microseconds_value -= 1000000ll;
      ++integral_seconds;
   }

   auto time_duration = hours(hour) + minutes(minute) + seconds(integral_seconds)
      + microseconds(microseconds_value);

   sys_time<microseconds> local_time(day_point.time_since_epoch() + duration_cast<microseconds>(time_duration));

   int timezone_offset = Components.has_timezone ? Components.timezone_offset_minutes : ImplicitTimezoneMinutes;

   UtcTime = local_time - minutes(timezone_offset);
   return true;
}

//********************************************************************************************************************
// Extracts date and time components from a UTC time point by converting to local time using TargetOffsetMinutes.
// Selectively includes date/time/timezone components based on flags. Defaults to 1970-01-01 00:00:00 if date or
// time components are not requested.

static DateTimeComponents components_from_utc_time(const std::chrono::sys_time<std::chrono::microseconds> &UtcTime,
   int TargetOffsetMinutes, bool IncludeTimezone, bool IncludeDate, bool IncludeTime)
{
   using namespace std::chrono;

   sys_time<microseconds> local_time = UtcTime + minutes(TargetOffsetMinutes);
   auto day_point = floor<days>(local_time);

   DateTimeComponents result;

   if (IncludeDate) {
      year_month_day ymd(day_point);
      result.year = int(ymd.year());
      result.month = (int)unsigned(ymd.month());
      result.day = (int)unsigned(ymd.day());
      result.has_date = true;
   }

   if (IncludeTime) {
      auto day_time = local_time - day_point;
      auto hour_duration = duration_cast<hours>(day_time);
      auto minute_duration = duration_cast<minutes>(day_time - hour_duration);
      auto second_duration = duration_cast<seconds>(day_time - hour_duration - minute_duration);
      auto micro_duration = duration_cast<microseconds>(day_time - hour_duration - minute_duration - second_duration);

      result.hour = (int)hour_duration.count();
      result.minute = (int)minute_duration.count();
      result.second = (double)second_duration.count() + (double)micro_duration.count() / 1000000.0;
      result.has_time = true;
   }

   if (IncludeTimezone) {
      result.has_timezone = true;
      result.timezone_offset_minutes = TargetOffsetMinutes;
      result.timezone_is_utc = (TargetOffsetMinutes IS 0);
   }

   return result;
}

//********************************************************************************************************************
// Formats integer according to picture specification with '#' (optional digit), '0' (required zero-padded digit),
// and ',' (thousands grouping). Applies the padding and grouping rules from the picture to produce the final output.

static std::string format_integer_picture(int64_t Value, const std::string &Picture)
{
   bool negative = Value < 0;
   uint64_t absolute = negative ? (uint64_t)(-Value) : (uint64_t)Value;
   std::string digits = std::to_string(absolute);

   size_t digit_slots = 0u;
   bool zero_pad = false;
   bool grouping = false;

   for (char ch : Picture) {
      if ((ch IS '#') or (ch IS '0')) {
         digit_slots++;
         if (ch IS '0') zero_pad = true;
      }
      if (ch IS ',') grouping = true;
   }

   if ((digit_slots > digits.length()) and (digit_slots > 0u)) {
      std::string padding(digit_slots - digits.length(), zero_pad ? '0' : ' ');
      digits.insert(0u, padding);
   }

   if (grouping) {
      std::string grouped;
      int count = 0;
      for (auto iter = digits.rbegin(); iter != digits.rend(); ++iter) {
         if ((count > 0) and ((count % 3) IS 0)) grouped.push_back(',');
         grouped.push_back(*iter);
         ++count;
      }
      std::reverse(grouped.begin(), grouped.end());
      digits = grouped;
   }

   if (negative) digits.insert(digits.begin(), '-');
   return digits;
}

//********************************************************************************************************************
// Produces a concise string representation of an XPathVal for diagnostic output in trace() and error() functions.
// Node sets show summary statistics and first few entries; other types show their string representation.

static std::string describe_xpath_value(const XPathVal &Value)
{
   switch (Value.Type) {
      case XPVT::Boolean:
         return (Value.NumberValue != 0.0 and !std::isnan(Value.NumberValue)) ? std::string("true") : std::string("false");

      case XPVT::Number:
         return Value.to_string();

      case XPVT::Map:
         return std::format("map[{}]", Value.map_storage ? Value.map_storage->size() : 0);

      case XPVT::Array:
         return std::format("array[{}]", Value.array_storage ? Value.array_storage->size() : 0);

      case XPVT::String:
      case XPVT::Date:
      case XPVT::Time:
      case XPVT::DateTime:
         return Value.StringValue;

      case XPVT::NodeSet: {
         std::vector<std::string> entries;

         if (Value.node_set_string_override.has_value()) {
            entries.push_back(*Value.node_set_string_override);
         }
         else if (not Value.node_set_attributes.empty()) {
            for (const XMLAttrib *attribute : Value.node_set_attributes) {
               if (not attribute) continue;
               entries.push_back(attribute->Value);
            }
         }
         else if (not Value.node_set_string_values.empty()) {
            entries.insert(entries.end(), Value.node_set_string_values.begin(), Value.node_set_string_values.end());
         }
         else {
            for (XTag *node : Value.node_set) {
               if (not node) continue;
               entries.push_back(XPathVal::node_string_value(node));
            }
         }

         size_t total_count = entries.size();
         if ((not total_count) and not Value.node_set.empty()) total_count = Value.node_set.size();
         if ((not total_count) and not Value.node_set_attributes.empty()) total_count = Value.node_set_attributes.size();
         if ((not total_count) and not Value.node_set_string_values.empty()) {
            total_count = Value.node_set_string_values.size();
         }

         if (entries.empty()) {
            if (not total_count) return std::string("()");
         }

         size_t summary_limit = entries.size();
         if (summary_limit > 3) summary_limit = 3;

         std::string summary;
         for (size_t index = 0; index < summary_limit; ++index) {
            if (index > 0) summary.append(", ");
            summary.append(entries[index]);
         }

         if (entries.size() > summary_limit) summary.append(", ...");

         if (total_count > 1) {
            if (not summary.empty()) return std::format("node-set[{}]: {}", total_count, summary);
            else return std::format("node-set[{}]", total_count);
         }

         if (not summary.empty()) return summary;

         return std::string("()");
      }
   }

   return std::string();
}

//********************************************************************************************************************
// Constructs a regex option flags set from a string of XPath regex flags. Supported flags: 'i' (case-insensitive),
// 'm' (multi-line mode), 's' (dot matches all). Unrecognised flags set the UnsupportedFlag if provided.

static REGEX build_regex_options(const std::string &Flags, bool *UnsupportedFlag)
{
   REGEX options = REGEX::NIL;

   for (char flag : Flags) {
      unsigned char code = (unsigned char)flag;
      char normalised = (char)std::tolower(code);

      if (normalised IS 'i') options |= REGEX::ICASE;
      else if (normalised IS 'm') options |= REGEX::MULTILINE;
      else if (normalised IS 's') options |= REGEX::DOT_ALL;
      else if (UnsupportedFlag) *UnsupportedFlag = true;
   }

   return options;
}

//********************************************************************************************************************
// Extracts numeric values from the node set representation of an XPathVal. The function attempts to convert each
// node's string value to a number, filtering out NaN results. Multiple storage representations are checked:
// override string, attributes, explicit string values, and finally node content.

static void append_numbers_from_nodeset(const XPathVal &Value, std::vector<double> &Numbers)
{
   if (Value.node_set_string_override.has_value()) {
      double number = XPathVal::string_to_number(*Value.node_set_string_override);
      if (not std::isnan(number)) Numbers.push_back(number);
      return;
   }

   if (not Value.node_set_attributes.empty()) {
      for (const XMLAttrib *attribute : Value.node_set_attributes) {
         if (not attribute) continue;
         double number = XPathVal::string_to_number(attribute->Value);
         if (not std::isnan(number)) Numbers.push_back(number);
      }
      return;
   }

   if (not Value.node_set_string_values.empty()) {
      for (const std::string &entry : Value.node_set_string_values) {
         double number = XPathVal::string_to_number(entry);
         if (not std::isnan(number)) Numbers.push_back(number);
      }
      return;
   }

   for (XTag *node : Value.node_set) {
      if (not node) continue;
      std::string content = XPathVal::node_string_value(node);
      double number = XPathVal::string_to_number(content);
      if (not std::isnan(number)) Numbers.push_back(number);
   }
}

//********************************************************************************************************************
// Extracts numeric values from an XPathVal according to its type. Node sets are processed through
// append_numbers_from_nodeset; strings, dates, and times are converted to numbers; and booleans are directly
// appended as their numeric representation. NaN values are filtered out.

static void append_numbers_from_value(const XPathVal &Value, std::vector<double> &Numbers)
{
   switch (Value.Type) {
      case XPVT::Number:
         if (not std::isnan(Value.NumberValue)) Numbers.push_back(Value.NumberValue);
         break;
      case XPVT::String:
      case XPVT::Date:
      case XPVT::Time:
      case XPVT::DateTime: {
         double number = XPathVal::string_to_number(Value.StringValue);
         if (not std::isnan(number)) Numbers.push_back(number);
         break;
      }
      case XPVT::Boolean:
         Numbers.push_back(Value.NumberValue);
         break;
      case XPVT::NodeSet:
         append_numbers_from_nodeset(Value, Numbers);
         break;
      case XPVT::Map:
      case XPVT::Array:
         // Function items cannot be coerced into numeric values. They are intentionally ignored
         // so that mixed sequences can still contribute their numeric members.
         break;
   }
}

//********************************************************************************************************************
// Accumulates nodes, attributes, and string values during sequence construction. Used to build the final result
// of sequence-producing operations where heterogeneous content (nodes, attributes, strings) must be combined.

struct SequenceBuilder
{
   NODES nodes;
   std::vector<const XMLAttrib *> attributes;
   std::vector<std::string> strings;
};

//********************************************************************************************************************
// Maintains state for the analyze-string function during regex matching. Tracks the builder accumulating results,
// the input string data, and the last processed offset to identify gaps between matches.

struct AnalyzeStringState
{
   SequenceBuilder *builder;
   const char *input_data;
   size_t input_length;
   size_t last_offset;
};

//********************************************************************************************************************
// Returns the logical length of a sequence value. For node sets, returns the size of the largest populated
// collection (nodes, attributes, or strings). For scalar values, returns 1 if non-empty, 0 otherwise.

static size_t sequence_length(const XPathVal &Value)
{
   if (Value.Type IS XPVT::NodeSet) {
      size_t length = Value.node_set.size();
      if (length < Value.node_set_attributes.size()) length = Value.node_set_attributes.size();
      if (length < Value.node_set_string_values.size()) length = Value.node_set_string_values.size();
      if ((not length) and Value.node_set_string_override.has_value()) length = 1;
      return length;
   }

   return Value.is_empty() ? 0 : 1;
}

//********************************************************************************************************************
// Retrieves the string representation of a sequence item at the specified index. For node sets, checks string
// values, override, attributes, and nodes in order. For scalars, returns string representation only if index is 0.

static std::string sequence_item_string(const XPathVal &Value, size_t Index)
{
   if (Value.Type IS XPVT::NodeSet) {
      if (Index < Value.node_set_string_values.size()) return Value.node_set_string_values[Index];

      bool use_override = Value.node_set_string_override.has_value() and (Index IS 0) and
         Value.node_set_string_values.empty();
      if (use_override) return *Value.node_set_string_override;

      if (Index < Value.node_set_attributes.size()) {
         const XMLAttrib *attribute = Value.node_set_attributes[Index];
         if (attribute) return attribute->Value;
      }

      if (Index < Value.node_set.size()) {
         XTag *node = Value.node_set[Index];
         if (node) return XPathVal::node_string_value(node);
      }

      return std::string();
   }

   return Value.to_string();
}

//********************************************************************************************************************
// Appends a single sequence item (at the specified index) to the SequenceBuilder, accumulating nodes, attributes,
// and string values simultaneously to maintain parallel structures.

static void append_sequence_item(const XPathVal &Value, size_t Index, SequenceBuilder &Builder)
{
   XTag *node = nullptr;
   if (Index < Value.node_set.size()) node = Value.node_set[Index];
   Builder.nodes.push_back(node);

   const XMLAttrib *attribute = nullptr;
   if (Index < Value.node_set_attributes.size()) attribute = Value.node_set_attributes[Index];
   Builder.attributes.push_back(attribute);

   std::string entry = sequence_item_string(Value, Index);
   Builder.strings.push_back(entry);
}

//********************************************************************************************************************
// Adds an entire XPathVal to the sequence builder. For node sets, appends each item; for empty scalars, does
// nothing; for non-empty scalars, appends as a string value with null node and attribute.

static void append_value_to_sequence(const XPathVal &Value, SequenceBuilder &Builder)
{
   if (Value.Type IS XPVT::NodeSet) {
      size_t length = sequence_length(Value);
      for (size_t index = 0; index < length; ++index) append_sequence_item(Value, index, Builder);
      return;
   }

   if (Value.is_empty()) return;

   Builder.nodes.push_back(nullptr);
   Builder.attributes.push_back(nullptr);
   Builder.strings.push_back(Value.to_string());
}

//********************************************************************************************************************
// Constructs an XPathVal node set from accumulated SequenceBuilder contents. Optimises single string values by
// setting the string override flag. Returns the finalised sequence value.

static XPathVal make_sequence_value(SequenceBuilder &&Builder)
{
   XPathVal result;
   result.Type = XPVT::NodeSet;
   result.node_set = std::move(Builder.nodes);
   result.node_set_attributes = std::move(Builder.attributes);
   result.node_set_string_values = std::move(Builder.strings);

   if ((result.node_set_string_values.size() IS 1) and result.node_set.empty() and
       result.node_set_attributes.empty()) {
      result.node_set_string_override = result.node_set_string_values[0];
   }

   return result;
}

//********************************************************************************************************************
// Extracts a single item from an XPathVal sequence at the specified index. Returns empty if index is out of bounds
// (for node sets) or out of range (for scalars). Optimises single-item node set with string override flag.

static XPathVal extract_sequence_item(const XPathVal &Value, size_t Index)
{
   if (Value.Type IS XPVT::NodeSet) {
      size_t length = sequence_length(Value);
      if (Index >= length) return XPathVal();

      XPathVal result;
      result.Type = XPVT::NodeSet;

      XTag *node = nullptr;
      if (Index < Value.node_set.size()) node = Value.node_set[Index];
      result.node_set.push_back(node);

      const XMLAttrib *attribute = nullptr;
      if (Index < Value.node_set_attributes.size()) attribute = Value.node_set_attributes[Index];
      result.node_set_attributes.push_back(attribute);

      std::string entry = sequence_item_string(Value, Index);
      result.node_set_string_values.push_back(entry);

      bool use_override = Value.node_set_string_override.has_value() and (Index IS 0) and
         Value.node_set_string_values.empty();
      if (use_override or (result.node_set_string_values.size() IS 1 and result.node_set.empty() and
         result.node_set_attributes.empty())) {
         result.node_set_string_override = entry;
      }

      return result;
   }

   if (not Index) return Value;
   return XPathVal();
}

//********************************************************************************************************************
// Compares two floating-point numbers for approximate equality according to XPath semantics. NaN is not equal to
// anything including itself. Infinities are equal only if both have the same sign. Finite values use scaled
// tolerance (relative for large magnitudes, absolute for small values) to account for floating-point precision.

static bool numeric_equal(double Left, double Right)
{
   if (std::isnan(Left) or std::isnan(Right)) return false;
   if (std::isinf(Left) or std::isinf(Right)) return Left IS Right;

   double abs_left = std::fabs(Left);
   double abs_right = std::fabs(Right);
   double larger = (abs_left > abs_right) ? abs_left : abs_right;

   // Use absolute tolerance for values near zero
   if (larger <= 1.0) {
      return std::fabs(Left - Right) <= std::numeric_limits<double>::epsilon() * 16;
   }

   // Use relative tolerance for larger magnitudes
   return std::fabs(Left - Right) <= larger * std::numeric_limits<double>::epsilon() * 16;
}

//********************************************************************************************************************
// Compares two XPathVal values for equality according to XPath type coercion rules. Boolean values trigger
// comparison in boolean context; numbers in numeric context; and strings/node-sets in string context. This
// implements the complex comparison semantics defined by XPath 2.0 specifications.

static bool xpath_values_equal(const XPathVal &Left, const XPathVal &Right)
{
   auto left_type = Left.Type;
   auto right_type = Right.Type;

   if ((left_type IS XPVT::Boolean) or (right_type IS XPVT::Boolean)) {
      bool left_boolean = Left.to_boolean();
      bool right_boolean = Right.to_boolean();
      return left_boolean IS right_boolean;
   }

   if ((left_type IS XPVT::Number) or (right_type IS XPVT::Number)) {
      double left_number = Left.to_number();
      double right_number = Right.to_number();
      if (std::isnan(left_number) or std::isnan(right_number)) return false;
      return numeric_equal(left_number, right_number);
   }

   if ((left_type IS XPVT::NodeSet) or (right_type IS XPVT::NodeSet)) {
      if ((left_type IS XPVT::NodeSet) and (right_type IS XPVT::NodeSet)) {
         XTag *left_node = Left.node_set.empty() ? nullptr : Left.node_set[0];
         XTag *right_node = Right.node_set.empty() ? nullptr : Right.node_set[0];
         if (left_node or right_node) {
            if (left_node IS right_node) return true;
            if ((left_node IS nullptr) or (right_node IS nullptr)) return false;
         }

         const XMLAttrib *left_attribute = Left.node_set_attributes.empty() ? nullptr : Left.node_set_attributes[0];
         const XMLAttrib *right_attribute = Right.node_set_attributes.empty() ? nullptr : Right.node_set_attributes[0];
         if (left_attribute or right_attribute) {
            if (left_attribute IS right_attribute) return true;
            if ((left_attribute IS nullptr) or (right_attribute IS nullptr)) return false;
         }
      }

      std::string left_string = Left.to_string();
      std::string right_string = Right.to_string();
      return left_string.compare(right_string) IS 0;
   }

   std::string left_string = Left.to_string();
   std::string right_string = Right.to_string();
   return left_string.compare(right_string) IS 0;
}

//********************************************************************************************************************
// Records a function argument cardinality error by setting the unsupported flag and appending a diagnostic message
// to the document's error log. Used for reporting violations of function parameter constraints.

static void flag_cardinality_error(const XPathContext &Context, std::string_view FunctionName,
   std::string_view Message)
{
   if (Context.expression_unsupported) *Context.expression_unsupported = true;

   if (Context.xml) {
      // Determine the appropriate XQuery error code based on the function name
      std::string error_code;
      if (FunctionName IS "zero-or-one") error_code = "FORG0003";
      else if (FunctionName IS "one-or-more") error_code = "FORG0004";
      else if (FunctionName IS "exactly-one") error_code = "FORG0005";
      else error_code = "FORG0006"; // General invalid argument type

      if (not Context.xml->ErrorMsg.empty()) Context.xml->ErrorMsg.append("\n");
      Context.xml->ErrorMsg.append(std::format("{}: XPath function {}: {}", error_code, FunctionName, Message));
   }
}

//********************************************************************************************************************
// Walks up the element tree to locate a namespace URI declaration corresponding to the requested prefix. For an
// empty prefix, finds the default namespace. Returns an empty string if no matching declaration is found in any
// ancestor element.

static std::string find_in_scope_namespace(XTag *Node, extXML *Document, std::string_view Prefix)
{
   XTag *current = Node;

   while (current) {
      for (size_t index = 1; index < current->Attribs.size(); ++index) {
         const auto &attrib = current->Attribs[index];

         if (Prefix.empty()) {
            if ((attrib.Name.length() IS 5) and (attrib.Name.compare(0, 5, "xmlns") IS 0)) return attrib.Value;
         }
         else {
            if (attrib.Name.compare(0, 6, "xmlns:") IS 0) {
               std::string declared = attrib.Name.substr(6);
               if ((declared.length() IS Prefix.size()) and (declared.compare(Prefix) IS 0)) return attrib.Value;
            }
         }
      }

      if (not Document) break;
      if (current->ParentID IS 0) break;
      current = Document->getTag(current->ParentID);
   }

   return std::string();
}

//********************************************************************************************************************
// Searches up the element tree for an xml:lang attribute. Returns the first discovered language tag or an empty
// string if no language declaration is found in the element or any ancestor.

static std::string find_language_for_node(XTag *Node, extXML *Document)
{
   XTag *current = Node;

   while (current) {
      for (size_t index = 1; index < current->Attribs.size(); ++index) {
         const auto &attrib = current->Attribs[index];
         if (pf::iequals(attrib.Name, "xml:lang")) return attrib.Value;
      }

      if (not Document) break;
      if (current->ParentID IS 0) break;
      current = Document->getTag(current->ParentID);
   }

   return std::string();
}

//********************************************************************************************************************
// Determines whether a candidate language tag matches a requested language according to RFC 4647 basic filtering.
// The candidate matches if it exactly equals the requested language or begins with it followed by a hyphen.
// Empty requested strings never match.

static bool language_matches(const std::string &Candidate, const std::string &Requested)
{
   if (Requested.empty()) return false;

   std::string candidate_lower = lowercase_copy(Candidate);
   std::string requested_lower = lowercase_copy(Requested);

   if (candidate_lower.compare(0, requested_lower.length(), requested_lower) IS 0) {
      if (candidate_lower.length() IS requested_lower.length()) return true;
      if ((candidate_lower.length() > requested_lower.length()) and (candidate_lower[requested_lower.length()] IS '-')) return true;
   }

   return false;
}

} // namespace

//********************************************************************************************************************

#include "../functions/function_library.cpp"
#include "../functions/accessor_support.cpp"
#include "../functions/func_accessors.cpp"
#include "../functions/func_nodeset.cpp"
#include "../functions/func_documents.cpp"
#include "../functions/func_qnames.cpp"
#include "../functions/func_strings.cpp"
#include "../functions/func_diagnostics.cpp"
#include "../functions/func_booleans.cpp"
#include "../functions/func_sequences.cpp"
#include "../functions/func_maps.cpp"
#include "../functions/func_arrays.cpp"
#include "../functions/func_numbers.cpp"
#include "../functions/func_datetimes.cpp"

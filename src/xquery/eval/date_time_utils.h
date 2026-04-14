#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <chrono>

struct DurationComponents
{
   bool negative = false;
   bool has_year = false;
   bool has_month = false;
   bool has_day = false;
   bool has_hour = false;
   bool has_minute = false;
   bool has_second = false;
   std::chrono::years years{0};
   std::chrono::months months{0};
   std::chrono::days days{0};
   std::chrono::hours hours{0};
   std::chrono::minutes minutes{0};
   std::chrono::duration<double> seconds{0.0};
};

bool is_valid_timezone(std::string_view Value);
bool parse_xs_date_components(std::string_view Value, long long &Year, int &Month, int &Day, size_t &NextIndex);
bool is_valid_xs_date(std::string_view Value);
bool is_valid_xs_date_no_timezone(std::string_view Value);
bool is_valid_xs_time(std::string_view Value);
bool is_valid_xs_datetime(std::string_view Value);
std::optional<std::string> extract_date_from_datetime(std::string_view Value);
std::optional<std::string> extract_time_from_datetime(std::string_view Value);
std::optional<std::string> canonicalise_xs_date(std::string_view Value);
std::optional<std::string> canonicalise_xs_time(std::string_view Value);
std::optional<std::string> canonicalise_xs_datetime(std::string_view Value);
bool parse_xs_duration(std::string_view Value, DurationComponents &Components);
void normalise_duration_components(DurationComponents &Components);

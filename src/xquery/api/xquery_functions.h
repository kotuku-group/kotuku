// XPath Function Library and Value System
//
// This file contains:
// - XPath value types and conversions
// - Function library for XPath functions
// - Context management for function evaluation

#pragma once

#include <kotuku/main.h>
#include <kotuku/modules/xml.h>
#include <ankerl/unordered_dense.h>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

#include "../../xml/xpath_value.h"
#include "../../xml/xml.h"
#include "../xquery.h"

//********************************************************************************************************************
// XPath Function Library

using XPathFunction = std::function<XPathVal(const std::vector<XPathVal> &, const XPathContext &)>;

class XPathFunctionLibrary {
   private:
   ankerl::unordered_dense::map<std::string, XPathFunction, TransparentStringHash, TransparentStringEqual> functions;
   void register_function(std::string_view Name, XPathFunction Func);
   const XPathFunction * find_function(std::string_view Name) const;

   // Size estimation helpers for string operations
   static size_t estimate_concat_size(const std::vector<XPathVal> &Args);
   static size_t estimate_normalize_space_size(const std::string &Input);
   static size_t estimate_translate_size(const std::string &Source, const std::string &From);

   XPathFunctionLibrary();

   public:
   static const XPathFunctionLibrary & instance();

   ~XPathFunctionLibrary() = default;

   bool has_function(std::string_view Name) const;
   XPathVal call_function(std::string_view Name, const std::vector<XPathVal> &Args, const XPathContext &Context) const;

   // Core XPath 1.0 functions
   static XPathVal function_last(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_position(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_count(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_id(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_idref(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_root(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_doc(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_doc_available(const std::vector<XPathVal> &Args, const XPathContext &Context);

   // Accessor Functions
   static XPathVal function_base_uri(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_data(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_document_uri(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_node_name(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_nilled(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_static_base_uri(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_default_collation(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_collection(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_uri_collection(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_unparsed_text(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_unparsed_text_available(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_unparsed_text_lines(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_local_name(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_namespace_uri(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_name(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_QName(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_resolve_QName(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_prefix_from_QName(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_local_name_from_QName(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_namespace_uri_from_QName(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_namespace_uri_for_prefix(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_in_scope_prefixes(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_string(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_concat(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_codepoints_to_string(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_string_to_codepoints(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_compare(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_codepoint_equal(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_starts_with(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_ends_with(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_contains(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_substring_before(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_substring_after(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_substring(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_string_length(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_normalize_space(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_normalize_unicode(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_string_join(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_iri_to_uri(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_analyze_string(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_resolve_uri(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_format_date(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_format_time(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_format_date_time(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_format_integer(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_translate(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_upper_case(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_lower_case(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_encode_for_uri(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_escape_html_uri(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_error(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_trace(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_boolean(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_not(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_true(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_false(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_lang(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_exists(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_index_of(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_empty(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_distinct_values(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_insert_before(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_remove(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_reverse(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_subsequence(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_unordered(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_deep_equal(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_zero_or_one(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_one_or_more(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_exactly_one(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_number(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_sum(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_floor(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_ceiling(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_round(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_round_half_to_even(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_abs(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_min(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_max(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_avg(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_math_pi(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_math_sin(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_math_cos(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_math_tan(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_math_asin(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_math_acos(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_math_atan(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_math_atan2(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_math_exp(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_math_exp10(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_math_log(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_math_log10(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_math_pow(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_math_sqrt(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_current_date(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_current_time(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_current_date_time(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_date_time(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_year_from_date_time(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_month_from_date_time(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_day_from_date_time(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_hours_from_date_time(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_minutes_from_date_time(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_seconds_from_date_time(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_timezone_from_date_time(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_year_from_date(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_month_from_date(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_day_from_date(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_timezone_from_date(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_hours_from_time(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_minutes_from_time(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_seconds_from_time(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_timezone_from_time(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_adjust_date_time_to_timezone(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_adjust_date_to_timezone(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_adjust_time_to_timezone(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_implicit_timezone(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_years_from_duration(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_months_from_duration(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_days_from_duration(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_hours_from_duration(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_minutes_from_duration(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_seconds_from_duration(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_years_from_year_month_duration(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_months_from_year_month_duration(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_days_from_day_time_duration(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_hours_from_day_time_duration(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_minutes_from_day_time_duration(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_seconds_from_day_time_duration(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_matches(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_replace(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_tokenize(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_map_entry(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_map_put(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_map_get(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_map_contains(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_map_size(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_map_keys(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_map_merge(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_array_size(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_array_get(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_array_append(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_array_insert_before(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_array_remove(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_array_join(const std::vector<XPathVal> &Args, const XPathContext &Context);
   static XPathVal function_array_flatten(const std::vector<XPathVal> &Args, const XPathContext &Context);
};

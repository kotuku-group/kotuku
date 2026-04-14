// XPathFunctionLibrary Implementation

//********************************************************************************************************************
// Constructs the function library and registers all core XPath functions.

XPathFunctionLibrary::XPathFunctionLibrary()
{
   // Node Set Functions
   register_function("last", function_last); // XP1.0
   register_function("position", function_position); // XP1.0
   register_function("count", function_count); // XP1.0
   register_function("id", function_id); // XP1.0
   register_function("idref", function_idref); // XP2.0
   register_function("root", function_root); // XP2.0

   // Document Functions
   register_function("local-name", function_local_name); // XP1.0
   register_function("namespace-uri", function_namespace_uri); // XP1.0
   register_function("name", function_name); // XP1.0
   register_function("doc", function_doc); // XQ1.0
   register_function("doc-available", function_doc_available); // XQ1.0
   register_function("collection", function_collection); // XQ1.0
   register_function("unparsed-text", function_unparsed_text); // XQ1.0
   register_function("unparsed-text-available", function_unparsed_text_available); // XQ1.0
   register_function("unparsed-text-lines", function_unparsed_text_lines); // XQ3.0
   register_function("uri-collection", function_uri_collection); // XQ3.0

   // Accessor Functions
   register_function("base-uri", function_base_uri); // XP2.0
   register_function("data", function_data); // XP2.0
   register_function("document-uri", function_document_uri); // XP2.0
   register_function("node-name", function_node_name); // XP2.0
   register_function("nilled", function_nilled); // XP2.0
   register_function("static-base-uri", function_static_base_uri); // XP2.0
   register_function("default-collation", function_default_collation); // XP2.0

   // QName Functions
   register_function("QName", function_QName); // XP2.0
   register_function("resolve-QName", function_resolve_QName); // XP2.0
   register_function("prefix-from-QName", function_prefix_from_QName); // XP2.0
   register_function("local-name-from-QName", function_local_name_from_QName); // XP2.0
   register_function("namespace-uri-from-QName", function_namespace_uri_from_QName); // XP2.0
   register_function("namespace-uri-for-prefix", function_namespace_uri_for_prefix); // XP2.0
   register_function("in-scope-prefixes", function_in_scope_prefixes); // XP2.0

   // String Functions
   register_function("string", function_string); // XP1.0
   register_function("concat", function_concat); // XP1.0
   register_function("starts-with", function_starts_with); // XP1.0
   register_function("contains", function_contains); // XP1.0
   register_function("substring-before", function_substring_before); // XP1.0
   register_function("substring-after", function_substring_after); // XP1.0
   register_function("substring", function_substring); // XP1.0
   register_function("string-length", function_string_length); // XP1.0
   register_function("normalize-space", function_normalize_space); // XP1.0
   register_function("translate", function_translate); // XP1.0
   register_function("codepoints-to-string", function_codepoints_to_string); // XP2.0
   register_function("string-to-codepoints", function_string_to_codepoints); // XP2.0
   register_function("compare", function_compare); // XP2.0
   register_function("codepoint-equal", function_codepoint_equal); // XP2.0
   register_function("ends-with", function_ends_with); // XP2.0
   register_function("normalize-unicode", function_normalize_unicode); // XP2.0
   register_function("string-join", function_string_join); // XP2.0
   register_function("iri-to-uri", function_iri_to_uri); // XP2.0
   register_function("upper-case", function_upper_case); // XP2.0
   register_function("lower-case", function_lower_case); // XP2.0
   register_function("encode-for-uri", function_encode_for_uri); // XP2.0
   register_function("escape-html-uri", function_escape_html_uri); // XP2.0
   register_function("matches", function_matches); // XP2.0
   register_function("replace", function_replace); // XP2.0
   register_function("tokenize", function_tokenize); // XP2.0
   register_function("analyze-string", function_analyze_string); // XP2.0
   register_function("resolve-uri", function_resolve_uri); // XP2.0
   register_function("format-date", function_format_date); // XP2.0
   register_function("format-time", function_format_time); // XP2.0
   register_function("format-dateTime", function_format_date_time); // XP2.0
   register_function("format-integer", function_format_integer); // XP3.0

   // Diagnostics Functions
   register_function("error", function_error); // XP2.0
   register_function("trace", function_trace); // XP3.1

   // Boolean Functions
   register_function("boolean", function_boolean); // XP1.0
   register_function("not", function_not); // XP1.0
   register_function("true", function_true); // XP1.0
   register_function("false", function_false); // XP1.0
   register_function("lang", function_lang); // XP1.0
   register_function("exists", function_exists); // XP2.0

   // Sequence Functions
   register_function("index-of", function_index_of); // XP2.0
   register_function("empty", function_empty); // XP2.0
   register_function("distinct-values", function_distinct_values); // XP2.0
   register_function("insert-before", function_insert_before); // XP2.0
   register_function("remove", function_remove); // XP2.0
   register_function("reverse", function_reverse); // XP2.0
   register_function("subsequence", function_subsequence); // XP2.0
   register_function("unordered", function_unordered); // XP2.0
   register_function("deep-equal", function_deep_equal); // XP2.0
   register_function("zero-or-one", function_zero_or_one); // XP2.0
   register_function("one-or-more", function_one_or_more); // XP2.0
   register_function("exactly-one", function_exactly_one); // XP2.0

   // Map Functions
   register_function("map:entry", function_map_entry); // XQ3.1
   register_function("map:put", function_map_put); // XQ3.1
   register_function("map:get", function_map_get); // XQ3.1
   register_function("map:contains", function_map_contains); // XQ3.1
   register_function("map:size", function_map_size); // XQ3.1
   register_function("map:keys", function_map_keys); // XQ3.1
   register_function("map:merge", function_map_merge); // XQ3.1

   // Array Functions
   register_function("array:size", function_array_size); // XQ3.1
   register_function("array:get", function_array_get); // XQ3.1
   register_function("array:append", function_array_append); // XQ3.1
   register_function("array:insert-before", function_array_insert_before); // XQ3.1
   register_function("array:remove", function_array_remove); // XQ3.1
   register_function("array:join", function_array_join); // XQ3.1
   register_function("array:flatten", function_array_flatten); // XQ3.1

   // Number Functions
   register_function("number", function_number); // XP1.0
   register_function("sum", function_sum); // XP1.0
   register_function("floor", function_floor); // XP1.0
   register_function("ceiling", function_ceiling); // XP1.0
   register_function("round", function_round); // XP1.0
   register_function("round-half-to-even", function_round_half_to_even); // XP2.0
   register_function("abs", function_abs); // XP2.0
   register_function("min", function_min); // XP2.0
   register_function("max", function_max); // XP2.0
   register_function("avg", function_avg); // XP2.0

   // Math Namespace Functions
   register_function("math:pi", function_math_pi); // XQ3.0
   register_function("math:sin", function_math_sin); // XQ3.0
   register_function("math:cos", function_math_cos); // XQ3.0
   register_function("math:tan", function_math_tan); // XQ3.0
   register_function("math:asin", function_math_asin); // XQ3.0
   register_function("math:acos", function_math_acos); // XQ3.0
   register_function("math:atan", function_math_atan); // XQ3.0
   register_function("math:atan2", function_math_atan2); // XQ3.0
   register_function("math:sqrt", function_math_sqrt); // XQ3.0
   register_function("math:exp", function_math_exp); // XQ3.0
   register_function("math:exp10", function_math_exp10); // XQ3.0
   register_function("math:log", function_math_log); // XQ3.0
   register_function("math:log10", function_math_log10); // XQ3.0
   register_function("math:pow", function_math_pow); // XQ3.0

   // Date and Time Functions
   register_function("current-date", function_current_date); // XP2.0
   register_function("current-time", function_current_time); // XP2.0
   register_function("current-dateTime", function_current_date_time); // XP2.0
   register_function("dateTime", function_date_time); // XP2.0
   register_function("year-from-dateTime", function_year_from_date_time); // XP2.0
   register_function("month-from-dateTime", function_month_from_date_time); // XP2.0
   register_function("day-from-dateTime", function_day_from_date_time); // XP2.0
   register_function("hours-from-dateTime", function_hours_from_date_time); // XP2.0
   register_function("minutes-from-dateTime", function_minutes_from_date_time); // XP2.0
   register_function("seconds-from-dateTime", function_seconds_from_date_time); // XP2.0
   register_function("timezone-from-dateTime", function_timezone_from_date_time); // XP2.0
   register_function("year-from-date", function_year_from_date); // XP2.0
   register_function("month-from-date", function_month_from_date); // XP2.0
   register_function("day-from-date", function_day_from_date); // XP2.0
   register_function("timezone-from-date", function_timezone_from_date); // XP2.0
   register_function("hours-from-time", function_hours_from_time); // XP2.0
   register_function("minutes-from-time", function_minutes_from_time); // XP2.0
   register_function("seconds-from-time", function_seconds_from_time); // XP2.0
   register_function("timezone-from-time", function_timezone_from_time); // XP2.0
   register_function("adjust-dateTime-to-timezone", function_adjust_date_time_to_timezone); // XP2.0
   register_function("adjust-date-to-timezone", function_adjust_date_to_timezone); // XP2.0
   register_function("adjust-time-to-timezone", function_adjust_time_to_timezone); // XP2.0
   register_function("implicit-timezone", function_implicit_timezone); // XP2.0
   register_function("years-from-duration", function_years_from_duration); // XP2.0
   register_function("months-from-duration", function_months_from_duration); // XP2.0
   register_function("days-from-duration", function_days_from_duration); // XP2.0
   register_function("hours-from-duration", function_hours_from_duration); // XP2.0
   register_function("minutes-from-duration", function_minutes_from_duration); // XP2.0
   register_function("seconds-from-duration", function_seconds_from_duration); // XP2.0
   register_function("years-from-yearMonthDuration", function_years_from_year_month_duration); // XP2.0
   register_function("months-from-yearMonthDuration", function_months_from_year_month_duration); // XP2.0
   register_function("days-from-dayTimeDuration", function_days_from_day_time_duration); // XP2.0
   register_function("hours-from-dayTimeDuration", function_hours_from_day_time_duration); // XP2.0
   register_function("minutes-from-dayTimeDuration", function_minutes_from_day_time_duration); // XP2.0
   register_function("seconds-from-dayTimeDuration", function_seconds_from_day_time_duration); // XP2.0
}

//********************************************************************************************************************
// Returns the singleton instance of the XPathFunctionLibrary, creating it on first call using call_once for
// thread-safe initialisation.

const XPathFunctionLibrary & XPathFunctionLibrary::instance()
{
   static std::once_flag initialise_flag;
   static std::unique_ptr<XPathFunctionLibrary> shared_library;

   std::call_once(initialise_flag, [&]() {
      shared_library = std::unique_ptr<XPathFunctionLibrary>(new XPathFunctionLibrary());
   });

   return *shared_library;
}

//********************************************************************************************************************
// Checks whether a function with the given name is registered in the library. Returns true if found; false
// otherwise.

bool XPathFunctionLibrary::has_function(std::string_view Name) const
{
   return find_function(Name) != nullptr;
}

//********************************************************************************************************************
// Invokes a registered function by name with the provided arguments and context. Sets expression_unsupported and
// appends an error message if the function is not found.

XPathVal XPathFunctionLibrary::call_function(std::string_view Name, const std::vector<XPathVal> &Args,
   const XPathContext &Context) const
{
   if (const auto *function_ptr = find_function(Name)) {
      return (*function_ptr)(Args, Context);
   }

   *Context.expression_unsupported = true;

   if (Context.xml) {
      if (not Context.xml->ErrorMsg.empty()) Context.xml->ErrorMsg.append("\n");
      Context.xml->ErrorMsg.append("Unsupported XPath function: ").append(Name);
   }
   else if (Context.eval) {
      Context.eval->record_error(std::string("Unsupported XPath function: ").append(Name), true);
   }

   return XPathVal();
}

//********************************************************************************************************************
// Registers a function implementation in the library map under the given name. Replaces any existing function
// with the same name.

void XPathFunctionLibrary::register_function(std::string_view Name, XPathFunction Func)
{
   functions.insert_or_assign(std::string(Name), std::move(Func));
}

//********************************************************************************************************************
// Looks up a function by name and returns a pointer to its implementation if found; nullptr otherwise.

const XPathFunction * XPathFunctionLibrary::find_function(std::string_view Name) const
{
   auto iter = functions.find(Name);
   if (iter != functions.end()) return &iter->second;
   return nullptr;
}

//********************************************************************************************************************
// Estimates the buffer size needed for concatenating the string representations of all arguments. Uses type-based
// heuristics (e.g., 32 bytes for numbers, 5 for "false") for conservative overestimation.

size_t XPathFunctionLibrary::estimate_concat_size(const std::vector<XPathVal> &Args)
{
   size_t total = 0;
   for (const auto &arg : Args) {
      // Conservative estimate based on type
      switch (arg.Type) {
         case XPVT::String:
         case XPVT::Date:
         case XPVT::Time:
         case XPVT::DateTime:
            total += arg.StringValue.length();
            break;
         case XPVT::Number:
            total += 32; // Conservative estimate for number formatting
            break;
         case XPVT::Boolean:
            total += 5; // "false" is longest
            break;
         case XPVT::NodeSet:
            if (arg.node_set_string_override.has_value()) total += arg.node_set_string_override->length();
            else if (not arg.node_set_string_values.empty()) total += arg.node_set_string_values[0].length();
            else total += 64; // Conservative estimate for node content
            break;
         case XPVT::Map:
         case XPVT::Array: {
            std::string summary = arg.to_string();
            total += summary.length();
            break;
         }
      }
   }
   return total;
}

//********************************************************************************************************************
// Estimates the output size for normalize-space operation, returning input length as worst case (no collapsing).

size_t XPathFunctionLibrary::estimate_normalize_space_size(const std::string &Input)
{
   // Worst case: no whitespace collapsing needed
   return Input.length();
}

//********************************************************************************************************************
// Estimates the output size for translate operation, returning source length as worst case (no characters removed).

size_t XPathFunctionLibrary::estimate_translate_size(const std::string &Source, const std::string &From)
{
   // Best case: no characters removed, worst case: same size as source
   return Source.length();
}

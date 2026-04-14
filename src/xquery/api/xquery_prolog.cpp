// XQuery Prolog and Module Management
//
// Implements the XQuery prolog data structures used by the XPath/XQuery engine. The prolog records
// construction mode, default namespaces, collations, decimal formats, and user declarations of functions
// and variables. It also normalises QNames, resolves prefixes, and validates that library modules export
// symbols in the declared target namespace.
//
// This translation unit additionally provides a lightweight module cache that consults the owning XML
// document, resolves import location hints, loads library modules, compiles them, and enforces circular-
// dependency and namespace checks. Prolog lookups (functions, variables, prefixes) are optimised via
// canonical keys such as the qname/arity signature.

#include <ranges>

#include "xquery_errors.h"
#include "../functions/accessor_support.h"
#include "../../xml/uri_utils.h"

//********************************************************************************************************************
// File-scope helpers

static std::string xp_normalise_cache_key(const std::string &Value)
{
   return xml::uri::normalise_uri_separators(Value);
}

static std::string xp_strip_file_scheme(const std::string &Value)
{
   if (not Value.rfind("file:", 0)) {
      std::string stripped = Value.substr(5);
      if (not stripped.rfind("//", 0)) stripped = stripped.substr(2);
      return stripped;
   }
   return Value;
}

static bool xp_is_windows_drive_path(std::string_view Value)
{
   if (Value.size() < 3U) return false;
   char letter = Value[0];
   bool letter_is_alpha = ((letter >= 'A') and (letter <= 'Z')) or ((letter >= 'a') and (letter <= 'z'));
   if (not letter_is_alpha) return false;
   if (Value[1] IS ':') {
      char slash = Value[2];
      constexpr char backslash = '\\';
      return (slash IS '/') or (slash IS backslash);
   }
   return false;
}

static std::string xp_resolve_hint_to_path(const std::string &Hint, const XQueryProlog &Prolog,
   const std::optional<std::string> &BaseDir)
{
   std::string normalised = xp_normalise_cache_key(Hint);
   if (normalised.empty()) return std::string();

   if (xml::uri::is_absolute_uri(normalised)) {
      if (not normalised.rfind("file:", 0)) {
         return xp_normalise_cache_key(xp_strip_file_scheme(normalised));
      }
      // Treat Windows-style drive paths (e.g. "E:/...") as filesystem paths
      if (xp_is_windows_drive_path(normalised)) {
         return xp_normalise_cache_key(normalised);
      }
      return std::string();
   }

   if (not Prolog.static_base_uri.empty()) {
      std::string resolved = xml::uri::resolve_relative_uri(normalised, Prolog.static_base_uri);
      if (not resolved.rfind("file:", 0)) {
         return xp_normalise_cache_key(xp_strip_file_scheme(resolved));
      }
      // Accept absolute Windows drive paths resolved from a non-URI base
      if (xp_is_windows_drive_path(resolved)) {
         return xp_normalise_cache_key(resolved);
      }
      if (not xml::uri::is_absolute_uri(resolved)) return xp_normalise_cache_key(resolved);
   }

   if (BaseDir) {
      std::string combined = std::format("{}{}", *BaseDir, normalised);
      return xp_normalise_cache_key(combined);
   }

   return normalised;
}

//********************************************************************************************************************
// Structured function key construction helper (interns QName)
[[nodiscard]] inline FunctionKey make_function_key(std::string_view QName, size_t Arity)
{
   FunctionKey key;
   key.qname = global_string_pool().intern(QName);
   key.arity = Arity;
   return key;
}

//********************************************************************************************************************
// Initialises the prolog defaults so that standard collations and decimal format entries are always present.

XQueryProlog::XQueryProlog()
{
   default_collation = "http://www.w3.org/2005/xpath-functions/collation/codepoint";
   decimal_formats[std::string()] = DecimalFormat();
}

//********************************************************************************************************************
// Returns the canonical signature text used to register the function in the prolog lookup table.
// The signature is cached on first access to avoid repeated string allocations.

const std::string & XQueryFunction::signature() const
{
   if (cached_signature.empty()) {
      cached_signature = std::format("{}/{}", qname, parameter_names.size());
   }
   return cached_signature;
}

//********************************************************************************************************************
// Attempts to locate a compiled module for the supplied URI, optionally consulting the owning document cache.

CompiledXQuery * XQueryModuleCache::fetch_or_load(std::string_view URI, const XQueryProlog &Prolog,
   XPathEvaluator &Eval) const
{
   pf::Log log(__FUNCTION__);

   log.branch("URI: %.*s", int(URI.size()), URI.data());

   if (URI.empty()) return nullptr;

   auto base_dir = xpath::accessor::resolve_document_base_directory(Eval.query->Path);

   auto uri_key = xp_normalise_cache_key(std::string(URI));
   std::string original_uri(URI);

   // Check if already loaded

   auto existing = modules.find(uri_key);
   if (existing != modules.end()) return existing->second.get();

   // Detect circular dependencies

   if (loading_in_progress.contains(uri_key)) {
      Eval.record_error("XQDY0054: Circular module dependency detected: " + uri_key);
      return nullptr;
   }

   // Find matching import declaration

   const XQueryModuleImport *import_decl = nullptr;
   for (const auto &imp : Prolog.module_imports) {
      std::string normalised_namespace = xp_normalise_cache_key(imp.target_namespace);
      if ((normalised_namespace IS uri_key) or (imp.target_namespace IS original_uri)) {
         import_decl = &imp;
         break;
      }
   }

   if (not import_decl) {
      Eval.record_error("XQST0059: No import declaration found for: " + uri_key);
      return nullptr;
   }

   std::vector<std::string> location_candidates;
   if (import_decl) {
      for (const auto &hint : import_decl->location_hints) {
         std::string candidate = xp_resolve_hint_to_path(hint, Prolog, base_dir);
         if (candidate.empty()) continue;

         if (std::ranges::any_of(location_candidates, [&](const std::string &existing_path) {
            return existing_path IS candidate;
         })) {
            continue;
         }

         location_candidates.push_back(std::move(candidate));
      }
   }

   // Check document cache for pre-loaded modules

   if (auto it = modules.find(uri_key); it != modules.end()) {
      // Mirror document-cached module into this cache for consistent lookups
      modules[uri_key] = it->second;
      return it->second.get();
   }

   if (original_uri != uri_key) {
      if (auto it = modules.find(original_uri); it != modules.end()) {
         modules[uri_key] = it->second;
         return it->second.get();
      }
   }

   for (const auto &candidate : location_candidates) {
      if (auto it = modules.find(candidate); it != modules.end()) {
         modules[uri_key] = it->second;
         return it->second.get();
      }
   }

   // Mark as loading to detect recursion

   loading_in_progress.insert(uri_key);

   auto cleanup = pf::Defer([&]() {
      loading_in_progress.erase(uri_key);
   });

   // Load file content

   if (location_candidates.empty()) location_candidates.push_back(uri_key);

   std::string *content = nullptr;
   std::string loaded_location;
   const std::optional<std::string> encoding("utf-8");

   for (const auto &candidate : location_candidates) {
      if (read_text_resource(Eval, candidate, encoding, content)) {
         loaded_location = candidate;
         break;
      }
   }

   if (not content) {
      std::string attempted;
      for (size_t index = 0; index < location_candidates.size(); ++index) {
         if (index > 0) attempted += std::format(", {}", location_candidates[index]);
         else attempted += std::string(location_candidates[index]);
      }

      Eval.record_error(std::format("XQST0059: Cannot load module for namespace {} (attempted: {})", uri_key, attempted));
      return nullptr;
   }

   // Compile the module query

   CompiledXQuery compiled;
   {
      XPathTokeniser tokeniser;
      XPathParser parser;

      auto token_block = tokeniser.tokenize(content->c_str());
      compiled = parser.parse(std::move(token_block));

      if ((compiled.prolog) and (compiled.prolog->is_library_module)) {
         if (not compiled.expression) compiled.expression = std::make_unique<XPathNode>(XQueryNodeType::EMPTY_SEQUENCE);
      }
      else if (not compiled.expression) {
         Eval.record_error(std::format("Cannot compile module: {}", URI));
         return nullptr;
      }

      // Bind/propagate module cache for subsequent imports and evaluation
      std::shared_ptr<XQueryModuleCache> module_cache_ptr = Prolog.get_module_cache();
      if (not module_cache_ptr) {
         module_cache_ptr = std::make_shared<XQueryModuleCache>();
         module_cache_ptr->query = query;
      }

      compiled.module_cache = module_cache_ptr;
      if (compiled.prolog) compiled.prolog->bind_module_cache(module_cache_ptr);
   }

   // Verify that it's a library module

   auto module_prolog = compiled.prolog;
   if ((not module_prolog) or (not module_prolog->is_library_module)) {
      Eval.record_error(std::format("Module is not a library module: {}", uri_key));
      return nullptr;
   }

   // Validate namespace matches

   if (module_prolog->module_namespace_uri != uri_key) {
      Eval.record_error(std::format("Module namespace mismatch: expected {}", uri_key));
      return nullptr;
   }

   // Validate exports

   if (not module_prolog->validate_library_exports()) {
      Eval.record_error(std::format("Module exports not in target namespace: {}", uri_key));
      return nullptr;
   }

   // The static_base_uri will initially be set to the XML object's path, change it to the actual folder that the
   // file was loaded from.

   if (not module_prolog->static_base_uri_declared) {
      module_prolog->static_base_uri = xml::uri::extract_directory_path(loaded_location.empty() ? uri_key : loaded_location);
      log.msg("static-base-uri updated to %s", module_prolog->static_base_uri.c_str());
   }

   // Eagerly resolve transitive imports to detect cycles and propagate base URIs
   for (const auto &imp : module_prolog->module_imports) {
      std::string_view dep_uri = imp.normalised_target_namespace.empty()
         ? std::string_view(imp.target_namespace)
         : std::string_view(imp.normalised_target_namespace);
      if (not fetch_or_load(dep_uri, *module_prolog, Eval)) {
         // Do not cache partially loaded module on failure
         return nullptr;
      }
   }

   // Cache the module (only after resolving imports to allow circular detection via loading_in_progress)
   // Store in local variable first to avoid dangling references if map reallocates during subsequent insertions
   auto cached = std::make_shared<CompiledXQuery>(std::move(compiled));
   modules[uri_key] = cached;
   if (original_uri != uri_key) modules[original_uri] = cached;
   if (not loaded_location.empty()) modules[loaded_location] = cached;

   return cached.get();
}

//********************************************************************************************************************

const CompiledXQuery * XQueryModuleCache::find_module(std::string_view uri) const
{
   std::string original(uri);
   std::string uri_key = xml::uri::normalise_uri_separators(original);
   auto existing = modules.find(uri_key);
   if (existing != modules.end()) return existing->second.get();
   if (uri_key != original) {
      auto fallback = modules.find(original);
      if (fallback != modules.end()) return fallback->second.get();
   }
   return nullptr;
}

//********************************************************************************************************************
// Performs a lookup for a user-defined function using the generated signature key.

const XQueryFunction * XQueryProlog::find_function(std::string_view qname, size_t arity) const
{
   auto entry = functions.find(make_function_key(qname, arity));
   if (entry != functions.end()) return &entry->second;
   return nullptr;
}

//********************************************************************************************************************
// Retrieves a declared variable definition by its canonical QName string.

const XQueryVariable * XQueryProlog::find_variable(std::string_view qname) const
{
   auto entry = variables.find(qname);
   if (entry != variables.end()) return &entry->second;
   return nullptr;
}

//********************************************************************************************************************
// Resolves a namespace prefix against the prolog declarations, falling back to the document bindings when required.

uint32_t XQueryProlog::resolve_prefix(std::string_view prefix, const extXML *document) const
{
   auto mapping = declared_namespaces.find(prefix);
   if (mapping != declared_namespaces.end()) return mapping->second;

   if (prefix.empty()) {
      if (default_element_namespace.has_value()) return *default_element_namespace;
      return 0;
   }

   if (document) {
      auto doc_entry = document->Prefixes.find(std::string(prefix));
      if (doc_entry != document->Prefixes.end()) return doc_entry->second;
   }

   return 0;
}

//********************************************************************************************************************
// Records a namespace binding inside the prolog and optionally mirrors it into the backing document.  Rejects duplicates.

bool XQueryProlog::declare_namespace(std::string_view prefix, std::string_view uri, extXML *document)
{
   std::string cleaned = xml::uri::normalise_uri_separators(std::string(uri));
   auto hash = pf::strhash(cleaned);
   std::string prefix_key(prefix);
   auto inserted = declared_namespaces.emplace(prefix_key, hash);
   if (not inserted.second) return false;
   declared_namespace_uris[prefix_key] = cleaned;

   if (document) {
      document->NSRegistry[hash] = cleaned;
      document->Prefixes[prefix_key] = hash;
   }
   return true;
}

//********************************************************************************************************************
// Stores a variable declaration, ensuring the original QName is preserved as the map key.  Rejects duplicates.

bool XQueryProlog::declare_variable(std::string_view qname, XQueryVariable variable)
{
   std::string key(qname);
   auto inserted = variables.emplace(key, std::move(variable));
   if (not inserted.second) return false;
   inserted.first->second.qname = key;
   return true;
}

//********************************************************************************************************************
// Inserts a function declaration using the computed signature as the lookup handle.  Rejects duplicates.

bool XQueryProlog::declare_function(XQueryFunction function)
{
   FunctionKey key;
   key.qname = global_string_pool().intern(function.qname);
   key.arity = function.parameter_names.size();
   auto inserted = functions.emplace(std::move(key), std::move(function));
   if (not inserted.second) return false;
   return true;
}

//********************************************************************************************************************
// Records a module import declaration, ensuring no duplicate imports for the same namespace (XQST0047).

bool XQueryProlog::declare_module_import(XQueryModuleImport import_decl, std::string *error_message)
{
   std::string namespace_key = xml::uri::normalise_uri_separators(import_decl.target_namespace);

   // Check for duplicate module imports (XQST0047)
   for (const auto &existing : module_imports) {
      std::string existing_key = existing.normalised_target_namespace.empty()
         ? xml::uri::normalise_uri_separators(existing.target_namespace)
         : existing.normalised_target_namespace;
      if (existing_key IS namespace_key) {
         if (error_message) {
            *error_message = xquery::errors::duplicate_module_import(namespace_key);
         }
         return false;
      }
   }

   import_decl.normalised_target_namespace = namespace_key;
   module_imports.push_back(std::move(import_decl));
   return true;
}

//********************************************************************************************************************

bool XQueryProlog::validate_library_exports() const
{
   return validate_library_exports_detailed().valid;
}

//********************************************************************************************************************

XQueryProlog::ExportValidationResult XQueryProlog::validate_library_exports_detailed() const
{
   ExportValidationResult result;

   if (not is_library_module) {
      result.valid = true;
      return result;
   }

   if (not module_namespace_uri) {
      result.valid = false;
      result.error_message = "Library module is missing namespace URI declaration";
      return result;
   }

   uint32_t module_hash = pf::strhash(*module_namespace_uri);

   auto matches_namespace = [&](std::string_view qname) -> bool {
      if (qname.empty()) return false;

      // Q{uri}local format
      if ((qname.size() > 2U) and (qname[0] IS 'Q') and (qname[1] IS '{')) {
         size_t closing = qname.find('}');
         if (closing IS std::string::npos) return false;
         std::string_view uri_view = qname.substr(2U, closing - 2U);
         return uri_view IS std::string_view(*module_namespace_uri);
      }

      // prefix:local format
      size_t colon = qname.find(':');
      if (colon IS std::string::npos) return false;
      std::string_view prefix = qname.substr(0, colon);
      uint32_t prefix_hash = resolve_prefix(prefix, nullptr);
      if (prefix_hash IS 0) return false;
      return prefix_hash IS module_hash;
   };

   // Validate all functions are in the module namespace (XQST0048)
   for (const auto &entry : functions) {
      if (not matches_namespace(entry.second.qname)) {
         result.valid = false;
         result.problematic_qname = entry.second.qname;
         result.is_function = true;
         result.error_message = xquery::errors::export_not_in_namespace(
            "Function", entry.second.qname, *module_namespace_uri);
         return result;
      }
   }

   // Validate all variables are in the module namespace (XQST0048)
   for (const auto &entry : variables) {
      if (not matches_namespace(entry.second.qname)) {
         result.valid = false;
         result.problematic_qname = entry.second.qname;
         result.is_function = false;
         result.error_message = xquery::errors::export_not_in_namespace(
            "Variable", entry.second.qname, *module_namespace_uri);
         return result;
      }
   }

   result.valid = true;
   return result;
}

//********************************************************************************************************************
// Associates a module cache with the prolog so evaluators can reuse loaded modules.

void XQueryProlog::bind_module_cache(std::shared_ptr<XQueryModuleCache> cache)
{
   module_cache = cache;
}

//********************************************************************************************************************
// Returns the active module cache if one has been attached to the prolog.

std::shared_ptr<XQueryModuleCache> XQueryProlog::get_module_cache() const
{
   return module_cache.lock();
}

//********************************************************************************************************************
// Normalises a function QName using the prolog and document namespace tables to produce the canonical expanded form.

std::string XQueryProlog::normalise_function_qname(std::string_view qname, const XPathNode *Node) const
{
   auto build_expanded = [](const std::string &NamespaceURI, std::string_view Local) {
      return std::format("Q{{{}}}{}", NamespaceURI, Local);
   };

   if (size_t colon = qname.find(':'); colon != std::string_view::npos) {
      std::string prefix(qname.substr(0, colon));
      std::string_view local_view = qname.substr(colon + 1);

      auto uri_entry = declared_namespace_uris.find(prefix);
      if (uri_entry != declared_namespace_uris.end()) {
         return build_expanded(uri_entry->second, local_view);
      }

      // Built-in fallback for the standard function namespace prefix "fn"
      if (prefix IS std::string("fn")) {
         static const std::string functions_ns("http://www.w3.org/2005/xpath-functions");
         return build_expanded(functions_ns, local_view);
      }
      return std::string(qname);
   }
   else if (default_function_namespace_uri.has_value()) {
      return build_expanded(*default_function_namespace_uri, qname);
   }
   else if (default_function_namespace.has_value()) {
      return std::format("Q{{{}}}{}", *default_function_namespace, qname);
   }
   else return std::string(qname);
}

//********************************************************************************************************************
// XPath Document and Text Retrieval Functions

#include "accessor_support.h"
#include <filesystem>

namespace fs = std::filesystem;

//*********************************************************************************************************************
// Load (or retrieve from cache) a text resource.  Returns true if successful.
// TODO: Support Encoding parameters other than UTF-8 (converts the source to UTF-8).

[[maybe_unused]] static bool read_text_resource(XPathEvaluator &Eval, const std::string &URI, const std::optional<std::string> &Encoding,
   std::string * &Result)
{
   pf::Log log(__FUNCTION__);

   log.branch("Loading: %s", URI.c_str());

   if (Encoding.has_value()) {
      std::string lowered = lowercase_copy(*Encoding);
      if ((lowered != "utf-8") and (lowered != "utf8")) return false;
   }

   // TODO: File class needs to support URI locations (http://, file:// etc).

   objFile::create file { fl::Path(URI), fl::Flags(FL::READ) };
   if (file.ok()) {
      std::string buffer;
      buffer.resize(file->get<size_t>(FID_Size));
      file->read(buffer.data(), buffer.size());
      Eval.text_cache[URI] = std::move(normalise_newlines(buffer));
      Result = &Eval.text_cache[URI];
      return true;
   }
   else return false;
}

//*********************************************************************************************************************
// Get the directory of the current document, if available.

static std::optional<std::string> get_context_directory(const XPathContext &Context)
{
   if ((Context.xml) and (Context.xml->Path) and (*Context.xml->Path)) {
      std::string resolved;
      if (ResolvePath(Context.xml->Path, RSF::NO_FILE_CHECK, &resolved) IS ERR::Okay) {
         fs::path base_path(resolved);
         base_path = base_path.parent_path();
         return base_path.string();
      }

      std::string raw = Context.xml->Path;
      size_t slash = raw.find_last_of("/\\");
      if (slash != std::string::npos) return raw.substr(0, slash + 1u);
   }

   return std::nullopt;
}

//*********************************************************************************************************************
// Resolve a resource URI to a usable path or string.
// URI's can absolute paths, e.g. temp:thing.xml or relative, e.g. thing.xml.  Relative paths will ultimately use the
// current working path and it is the responsibility of the caller to manage the path prior to making queries.

static bool resolve_resource_location(const XPathContext &Context, const std::string &URI, std::string &Resolved)
{
   if (URI.empty()) return false;
   if (is_string_uri(URI)) { Resolved = URI; return true; }

   if (ResolvePath(URI, RSF::NO_FILE_CHECK, &Resolved) IS ERR::Okay) {
      return true;
   }
   else Resolved = URI;

   return true;
}

//*********************************************************************************************************************
// Load (or retrieve from cache) an XML document.
// Note: For the time being, cached XML documents are considered read-only (modifying the tags would upset cached
// tag references).

static extXML * load_document(CompiledXQuery *State, const std::string &URI)
{
   auto existing = State->XMLCache.find(URI);
   if (existing != State->XMLCache.end()) return existing->second;

   // TODO: Loading from URI's needs to be supported by the File class.

   auto document = pf::Create<extXML>::global({ fl::Path(URI), fl::Flags(XMF::WELL_FORMED | XMF::NAMESPACE_AWARE) });

   if (!document) return nullptr;
   if (document->Tags.empty()) return nullptr;

   (void)document->getMap(); // Build ID map now
   State->XMLCache[URI] = document;
   return document;
}

//*********************************************************************************************************************
// Locate the root node of the document containing a given node.

static XTag * locate_root_node(extXML *Document, XTag *Node)
{
   if ((!Document) or (!Node)) return nullptr;

   XTag *current = Node;
   while (current->ParentID != 0) {
      XTag *parent = Document->getTag(current->ParentID);
      if (!parent) break;
      current = parent;
   }

   return current;
}

//*********************************************************************************************************************
// Split a string into tokens based on whitespace.

static std::vector<std::string> split_whitespace_tokens(std::string_view Value)
{
   std::vector<std::string> tokens;
   size_t offset = 0;

   while (offset < Value.length()) {
      while ((offset < Value.length()) and std::isspace((unsigned char)Value[offset])) offset++;
      size_t start = offset;
      while ((offset < Value.length()) and (not std::isspace((unsigned char)Value[offset]))) offset++;
      if (offset > start) tokens.emplace_back(Value.substr(start, offset - start));
   }

   return tokens;
}

//*********************************************************************************************************************
// Collect all nodes in the document that have an IDREF or IDREFS attribute matching one of the target IDs.

static void collect_idref_matches(extXML *Document, const std::unordered_set<std::string> &Targets,
   std::unordered_set<const XTag *> &Seen, pf::vector<XTag *> &Matches)
{
   if ((!Document) or Targets.empty()) return;

   pf::vector<XTag *> stack;
   stack.reserve(Document->Tags.size());
   for (auto &root : Document->Tags) stack.push_back(&root);

   while (!stack.empty()) {
      XTag *current = stack.back();
      stack.pop_back();

      if ((!current->Attribs.empty()) and current->isTag()) {
         for (size_t index = 1; index < current->Attribs.size(); ++index) {
            const auto &attrib = current->Attribs[index];
            if (attrib.Name.empty()) continue;

            if (pf::iequals(attrib.Name, "idref") or pf::iequals(attrib.Name, "xml:idref") or
                pf::iequals(attrib.Name, "idrefs") or pf::iequals(attrib.Name, "xml:idrefs")) {
               auto tokens = split_whitespace_tokens(attrib.Value);
               bool matched = false;
               for (const auto &token : tokens) {
                  if (Targets.find(token) != Targets.end()) { matched = true; break; }
               }

               if (matched) {
                  if (Seen.insert(current).second) Matches.push_back(current);
                  break;
               }
            }
         }
      }

      for (auto &child : current->Children) stack.push_back(&child);
   }
}

//*********************************************************************************************************************
// Enumerate all XML files in a directory.

static std::vector<std::string> enumerate_collection(const std::string &Directory)
{
   std::vector<std::string> entries;
   std::error_code ec;
   fs::directory_iterator iter(fs::path(Directory), fs::directory_options::skip_permission_denied, ec);
   if (ec) return entries;

   fs::directory_iterator end;
   for (; iter != end; iter.increment(ec)) {
      if (ec) break;
      const fs::directory_entry &entry = *iter;

      std::error_code status_ec;
      if (not entry.is_regular_file(status_ec)) continue;

      std::string extension = entry.path().extension().string();
      if (pf::iequals(extension, ".xml")) entries.push_back(entry.path().string());
   }

   std::sort(entries.begin(), entries.end());
   return entries;
}

//*********************************************************************************************************************
// XPath Document and Text Retrieval Functions
// See https://www.w3.org/TR/xpath-functions-31/#docfunc for details

XPathVal XPathFunctionLibrary::function_root(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   XTag *node = nullptr;

   if (!Args.empty()) {
      const XPathVal &value = Args[0];
      if ((value.Type IS XPVT::NodeSet) and (not value.node_set.empty())) node = value.node_set[0];
      else return XPathVal(pf::vector<XTag *>());
   }
   else node = Context.context_node;

   if (!node) return XPathVal(pf::vector<XTag *>());

   extXML *document = xpath::accessor::locate_node_document(Context, node);
   if (!document) return XPathVal(pf::vector<XTag *>());

   XTag *root = locate_root_node(document, node);
   if (!root) return XPathVal(pf::vector<XTag *>());

   pf::vector<XTag *> result = { root };
   return XPathVal(result);
}

//*********************************************************************************************************************
// The doc() function loads an XML document from a given URI and returns its top-level element nodes.

XPathVal XPathFunctionLibrary::function_doc(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.empty()) return XPathVal(pf::vector<XTag *>());

   std::string uri = Args[0].to_string();
   if (uri.empty()) return XPathVal(pf::vector<XTag *>());

   std::string resolved;
   if (!resolve_resource_location(Context, uri, resolved)) return XPathVal(pf::vector<XTag *>());

   auto document = load_document(Context.eval->parse_context, resolved);
   if (!document) return XPathVal(pf::vector<XTag *>());

   pf::vector<XTag *> nodes;
   for (auto &tag : document->Tags) {
      if ((tag.Flags & XTF::INSTRUCTION) != XTF::NIL) continue;
      nodes.push_back(&tag);
   }

   return XPathVal(nodes);
}

//*********************************************************************************************************************
// The doc-available() function checks if a document at a given URI can be loaded.

XPathVal XPathFunctionLibrary::function_doc_available(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.empty()) return XPathVal(false);
   if (!Context.xml) return XPathVal(false);

   std::string uri = Args[0].to_string();
   if (uri.empty()) return XPathVal(false);

   std::string resolved;
   if (!resolve_resource_location(Context, uri, resolved)) return XPathVal(false);

   if (is_string_uri(resolved)) return XPathVal(true);

   if (Context.eval->parse_context->XMLCache.find(resolved) != Context.eval->parse_context->XMLCache.end()) return XPathVal(true);

   // TODO: Testing validity of URI's needs to be supported by the File class.

   LOC file_type;
   if (AnalysePath(resolved.c_str(), &file_type) IS ERR::Okay) {
      return XPathVal(true);
   }

   return XPathVal(false);
}

//*********************************************************************************************************************
// The collection() function loads all XML documents in a given directory and returns their top-level element nodes.

XPathVal XPathFunctionLibrary::function_collection(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (!Context.xml) return XPathVal(pf::vector<XTag *>());

   std::string resolved;
   if (Args.empty()) {
      auto base = get_context_directory(Context);
      if (not base.has_value()) return XPathVal(pf::vector<XTag *>());
      resolved = *base;
   }
   else {
      std::string uri = Args[0].to_string();
      if (uri.empty()) return XPathVal(pf::vector<XTag *>());
      if (!resolve_resource_location(Context, uri, resolved)) return XPathVal(pf::vector<XTag *>());
   }

   if (is_string_uri(resolved)) return XPathVal(pf::vector<XTag *>());

   auto entries = enumerate_collection(resolved);
   pf::vector<XTag *> nodes;

   for (const auto &entry : entries) {
      auto document = load_document(Context.eval->parse_context, entry);
      if (!document) continue;

      for (auto &tag : document->Tags) {
         if ((tag.Flags & XTF::INSTRUCTION) != XTF::NIL) continue;
         nodes.push_back(&tag);
      }
   }

   return XPathVal(nodes);
}

//*********************************************************************************************************************
// The uri-collection() function enumerates all XML files in a given directory and returns their URIs.

XPathVal XPathFunctionLibrary::function_uri_collection(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (!Context.xml) return XPathVal(pf::vector<XTag *>());

   std::string resolved;
   if (Args.empty()) {
      auto base = get_context_directory(Context);
      if (not base.has_value()) return XPathVal(pf::vector<XTag *>());
      resolved = *base;
   }
   else {
      std::string uri = Args[0].to_string();
      if (uri.empty()) return XPathVal(pf::vector<XTag *>());
      if (!resolve_resource_location(Context, uri, resolved)) return XPathVal(pf::vector<XTag *>());
   }

   if (is_string_uri(resolved)) return XPathVal(pf::vector<XTag *>());

   auto entries = enumerate_collection(resolved);
   pf::vector<XTag *> nodes;
   std::vector<std::string> values;

   for (const auto &entry : entries) {
      nodes.push_back(nullptr);
      values.push_back(entry);
   }

   return XPathVal(nodes, std::nullopt, values);
}

//*********************************************************************************************************************
// The unparsed-text() function loads a text resource from a given URI and returns its content as a string.

XPathVal XPathFunctionLibrary::function_unparsed_text(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.empty()) return XPathVal(std::string());
   if (!Context.xml) return XPathVal(std::string());

   std::string uri = Args[0].to_string();
   if (uri.empty()) return XPathVal(std::string());

   std::optional<std::string> encoding;
   if (Args.size() > 1) {
      std::string value = Args[1].to_string();
      if (not value.empty()) encoding = value;
   }

   std::string resolved;
   if (!resolve_resource_location(Context, uri, resolved)) return XPathVal(std::string());

   std::string *text;
   if (!read_text_resource(*Context.eval, resolved, encoding, text)) return XPathVal(std::string());

   return XPathVal(*text);
}

//*********************************************************************************************************************
// The unparsed-text-available() function checks if a text resource at a given URI can be loaded.

XPathVal XPathFunctionLibrary::function_unparsed_text_available(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty()) return XPathVal(false);

   std::string uri = Args[0].to_string();
   if (uri.empty()) return XPathVal(false);

   std::optional<std::string> encoding;
   if (Args.size() > 1) {
      std::string value = Args[1].to_string();
      if (not value.empty()) encoding = value;
   }

   std::string resolved;
   if (!resolve_resource_location(Context, uri, resolved)) return XPathVal(false);

   std::string *text;
   if (read_text_resource(*Context.eval, resolved, encoding, text)) return XPathVal(true);
   return XPathVal(false);
}

//*********************************************************************************************************************
// The unparsed-text-lines() function loads a text resource from a given URI and returns its content as a sequence of lines.

XPathVal XPathFunctionLibrary::function_unparsed_text_lines(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty()) return XPathVal(pf::vector<XTag *>());

   std::string uri = Args[0].to_string();
   if (uri.empty()) return XPathVal(pf::vector<XTag *>());

   std::optional<std::string> encoding;
   if (Args.size() > 1) {
      std::string value = Args[1].to_string();
      if (not value.empty()) encoding = value;
   }

   std::string resolved;
   if (!resolve_resource_location(Context, uri, resolved)) return XPathVal(pf::vector<XTag *>());

   std::string *text;
   if (!read_text_resource(*Context.eval, resolved, encoding, text)) return XPathVal(pf::vector<XTag *>());

   std::vector<std::string> lines;

   size_t start = 0;
   while (start <= text->length()) {
      size_t end = text->find('\n', start);
      if (end IS std::string::npos) {
         lines.emplace_back(text->substr(start));
         break;
      }

      lines.emplace_back(text->substr(start, end - start));
      start = end + 1;
      if (start > text->length()) lines.emplace_back(std::string());
   }

   pf::vector<XTag *> nodes;
   for (size_t i = 0; i < lines.size(); ++i) nodes.push_back(nullptr);
   return XPathVal(nodes, std::nullopt, lines);
}

//*********************************************************************************************************************
// The idref() function returns all elements that have an IDREF or IDREFS attribute matching one of the given IDs.

XPathVal XPathFunctionLibrary::function_idref(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   pf::vector<XTag *> results;
   if (Args.empty()) return XPathVal(results);
   if (!Context.xml) return XPathVal(results);

   std::unordered_set<std::string> requested_ids;

   auto add_tokens = [&](const std::string &Value) {
      auto tokens = split_whitespace_tokens(Value);
      for (const auto &token : tokens) requested_ids.insert(token);
   };

   for (const auto &arg : Args) {
      switch (arg.Type) {
         case XPVT::NodeSet: {
            if (not arg.node_set_string_values.empty()) {
               for (const auto &entry : arg.node_set_string_values) add_tokens(entry);
            }
            else if (arg.node_set_string_override.has_value()) add_tokens(*arg.node_set_string_override);
            else {
               for (auto *node : arg.node_set) {
                  if (not node) continue;
                  add_tokens(node->getContent());
               }
            }
            break;
         }

         case XPVT::String:
         case XPVT::Date:
         case XPVT::Time:
         case XPVT::DateTime:
            add_tokens(arg.StringValue);
            break;

         case XPVT::Boolean:
            add_tokens(arg.to_string());
            break;

         case XPVT::Number:
            if (not std::isnan(arg.NumberValue)) add_tokens(arg.to_string());
            break;

         case XPVT::Map:
         case XPVT::Array:
            // Maps and arrays cannot contribute IDREF tokens.
            break;
      }
   }

   if (requested_ids.empty()) return XPathVal(results);

   std::unordered_set<const XTag *> seen;
   collect_idref_matches(Context.xml, requested_ids, seen, results);

   for (const auto &entry : Context.eval->parse_context->XMLCache) {
      collect_idref_matches(entry.second, requested_ids, seen, results);
   }

   return XPathVal(results);
}

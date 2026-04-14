#pragma once

#include <kotuku/system/errors.h>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <utility>
#include <string_view>
#include <cstring>
#include <kotuku/modules/xquery.h>
#include <array>
#include <unordered_set>
#include <unordered_map>

//*********************************************************************************************************************

enum class BinaryOperationKind {
   AND,
   OR,
   UNION,
   INTERSECT,
   EXCEPT,
   COMMA,
   GENERAL_EQ,
   GENERAL_NE,
   GENERAL_LT,
   GENERAL_LE,
   GENERAL_GT,
   GENERAL_GE,
   VALUE_EQ,
   VALUE_NE,
   VALUE_LT,
   VALUE_LE,
   VALUE_GT,
   VALUE_GE,
   ADD,
   SUB,
   MUL,
   DIV,
   MOD,
   RANGE,
   UNKNOWN
};

enum class UnaryOperationKind {
   NEGATE,
   LOGICAL_NOT,
   UNKNOWN
};

enum class XPathTokenType {
   // Path operators
   SLASH,             // /
   DOUBLE_SLASH,      // //
   DOT,               // .
   DOUBLE_DOT,        // ..

   // Identifiers and literals
   IDENTIFIER,        // element names, function names
   STRING,            // quoted strings
   NUMBER,            // numeric literals
   WILDCARD,          // *

   // Brackets and delimiters
   LBRACKET,          // [
   RBRACKET,          // ]
   LPAREN,            // (
   RPAREN,            // )
   AT,                // @
   COMMA,             // ,
   SEMICOLON,         // ;
   PIPE,              // |
   UNION,             // union keyword
   INTERSECT,         // intersect keyword
   EXCEPT,            // except keyword

   // Operators
   EQUALS,            // =
   NOT_EQUALS,        // !=
   LESS_THAN,         // <
   LESS_EQUAL,        // <=
   GREATER_THAN,      // >
   GREATER_EQUAL,     // >=
   EQ,                // eq
   NE,                // ne
   LT,                // lt
   LE,                // le
   GT,                // gt
   GE,                // ge

   // Boolean operators
   AND,               // and
   OR,                // or
   NOT,               // not

   // Flow keywords
   IF,                // if
   THEN,              // then
   ELSE,              // else
   FOR,               // for
   LET,               // let
   IN,                // in
   RETURN,            // return
   WHERE,             // where
   GROUP,             // group
   BY,                // by
   ORDER,             // order
   STABLE,            // stable
   ASCENDING,         // ascending
   DESCENDING,        // descending
   EMPTY,             // empty
   DEFAULT,           // default
   TYPESWITCH,        // typeswitch keyword
   CASE,              // case keyword
   DECLARE,           // declare keyword
   FUNCTION,          // function keyword
   VARIABLE,          // variable keyword
   NAMESPACE,         // namespace keyword
   EXTERNAL,          // external keyword
   BOUNDARY_SPACE,    // boundary-space keyword
   BASE_URI,          // base-uri keyword
   GREATEST,          // greatest
   LEAST,             // least
   COLLATION,         // collation
   CONSTRUCTION,      // construction
   ORDERING,          // ordering keyword used in prolog
   COPY_NAMESPACES,   // copy-namespaces keyword
   DECIMAL_FORMAT,    // decimal-format keyword
   OPTION,            // option keyword
   IMPORT,            // import keyword
   MODULE,            // module keyword
   SCHEMA,            // schema keyword
   COUNT,             // count
   SOME,              // some
   EVERY,             // every
   SATISFIES,         // satisfies
   CAST,              // cast keyword
   CASTABLE,          // castable keyword
   TREAT,             // treat keyword
   AS,                // as keyword
   INSTANCE,          // instance keyword
   OF,                // of keyword
   TO,                // to keyword
   MAP,               // map keyword (constructor)
   ARRAY,             // array keyword (constructor)

   // Arithmetic operators
   PLUS,              // +
   MINUS,             // -
   MULTIPLY,          // * (when not wildcard)
   DIVIDE,            // div
   MODULO,            // mod

   // Axis specifiers
   AXIS_SEPARATOR,    // ::
   COLON,             // :

   // Variables and functions
   DOLLAR,            // $
   ASSIGN,            // :=

   // Constructor delimiters
   LBRACE,            // {
   RBRACE,            // }
   TAG_OPEN,          // < (direct constructors)
   CLOSE_TAG_OPEN,    // </
   TAG_CLOSE,         // >
   EMPTY_TAG_CLOSE,   // />
   PI_START,          // <?
   PI_END,            // ?>

   // Special tokens
   TEXT_CONTENT,      // literal content inside direct constructors
   QUESTION_MARK,     // ? occurrence indicator
   LOOKUP,            // ? lookup operator
   END_OF_INPUT,
   UNKNOWN
};

// XPath Axis Types

enum class AxisType {
   CHILD,
   DESCENDANT,
   PARENT,
   ANCESTOR,
   FOLLOWING_SIBLING,
   PRECEDING_SIBLING,
   FOLLOWING,
   PRECEDING,
   ATTRIBUTE,
   NAMESPACE,
   SELF,
   DESCENDANT_OR_SELF,
   ANCESTOR_OR_SELF
};

enum class SequenceCardinality {
   ExactlyOne,
   ZeroOrOne,
   OneOrMore,
   ZeroOrMore
};

enum class SequenceItemKind {
   Atomic,
   Element,
   Attribute,
   Text,
   Node,
   Item,
   Map,
   Array,
   Function,
   EmptySequence
};

//********************************************************************************************************************

struct XPathNode;
struct SequenceEntry;

class XPathErrorReporter {
   public:
   virtual ~XPathErrorReporter() = default;
   virtual void record_error(std::string_view Message, bool Force = false) = 0;
   virtual void record_error(std::string_view Message, const XPathNode *Node, bool Force = false) = 0;
};

// Represents a user-defined decimal format declared in the prolog.

struct DecimalFormat {
   std::string name;
   std::string decimal_separator = ".";
   std::string grouping_separator = ",";
   std::string infinity = "INF";
   std::string minus_sign = "-";
   std::string nan = "NaN";
   std::string percent = "%";
   std::string per_mille = "\xE2\x80\xB0"; // UTF-8 encoding of ‰ (per-mille)
   std::string zero_digit = "0";
   std::string digit = "#";
   std::string pattern_separator = ";";
};

// Represents a user-defined XQuery function declared in the prolog.

struct XQueryFunction {
   std::string qname;
   std::vector<std::string> parameter_names;
   std::vector<std::string> parameter_types;
   std::optional<std::string> return_type;
   std::unique_ptr<XPathNode> body;
   bool is_external = false;
   mutable std::string cached_signature;

   [[nodiscard]] const std::string & signature() const;
};

//********************************************************************************************************************
// String interning pool for common identifiers (QNames, namespace URIs, etc.).

class StringInterner {
   private:
   std::unordered_set<std::string> pool;
   mutable std::mutex mutex;

   public:
   std::string_view intern(std::string_view str) {
      std::lock_guard<std::mutex> lock(mutex);
      auto it = pool.find(std::string(str));
      if (it != pool.end()) return *it;
      auto [it2, inserted] = pool.emplace(std::string(str));
      return *it2;
   }
};

inline StringInterner & global_string_pool() {
   static StringInterner pool;
   return pool;
}

// Structured key for function lookup: avoids string concatenation of "qname/arity"
struct FunctionKey {
   std::string_view qname;  // canonical QName (expanded or lexical as used by prolog), interned
   size_t arity = 0;

   [[nodiscard]] bool operator==(const FunctionKey &Other) const noexcept {
      return (qname IS Other.qname) and (arity IS Other.arity);
   }
};

struct FunctionKeyHash {
   using is_transparent = void;
   [[nodiscard]] size_t operator()(const FunctionKey &Key) const noexcept {
      size_t h1 = std::hash<std::string_view>{}(Key.qname);
      size_t h2 = std::hash<size_t>{}(Key.arity);
      return h1 ^ (h2 << 1);
   }
};

// Represents a user-defined XQuery variable declared in the prolog.

struct XQueryVariable {
   std::string qname;
   std::unique_ptr<XPathNode> initializer;
   bool is_external = false;
};

// Represents an XQuery module import declaration.

struct XQueryModuleImport {
   std::string target_namespace;
   // Pre-normalised key for fast lookups and duplicate detection
   std::string normalised_target_namespace;
   std::vector<std::string> location_hints;
};

struct XPathNode;

enum class TokenTextKind {
   BorrowedInput,
   ArenaOwned
};

enum class XPathLookupSpecifierKind {
   NCName,
   Wildcard,
   IntegerLiteral,
   Expression
};

struct TokenBuffer;

struct XPathAttributeValuePart {
   bool is_expression = false;
   std::string_view text;
   TokenTextKind text_kind = TokenTextKind::BorrowedInput;
};

struct XPathConstructorAttribute {
   std::string prefix;
   std::string name;
   std::string namespace_uri;
   bool is_namespace_declaration = false;
   std::vector<XPathAttributeValuePart> value_parts;
   std::vector<std::unique_ptr<XPathNode>> expression_parts;
   std::shared_ptr<TokenBuffer> value_storage;

   void set_expression_for_part(size_t index, std::unique_ptr<XPathNode> expr) {
      if (expression_parts.size() <= index) expression_parts.resize(index + 1);
      expression_parts[index] = std::move(expr);
   }

   [[nodiscard]] XPathNode * get_expression_for_part(size_t index) const {
      return index < expression_parts.size() ? expression_parts[index].get() : nullptr;
   }

   [[nodiscard]] std::vector<std::string> duplicate_value_parts() const {
      std::vector<std::string> copies;
      copies.reserve(value_parts.size());
      for (const auto &part : value_parts) copies.emplace_back(part.text);
      return copies;
   }
};

struct XPathConstructorInfo {
   std::string prefix, name, namespace_uri;
   bool is_empty_element = false;
   bool is_direct = false;
   std::vector<XPathConstructorAttribute> attributes;
};

struct XPathMapConstructorEntry {
   std::unique_ptr<XPathNode> key_expression;
   std::unique_ptr<XPathNode> value_expression;
};

struct XPathLookupSpecifier {
   XPathLookupSpecifierKind kind = XPathLookupSpecifierKind::NCName;
   std::string literal_value;
   std::unique_ptr<XPathNode> expression;
};

struct XPathOrderSpecOptions {
   bool is_descending = false;
   bool has_empty_mode = false;
   bool empty_is_greatest = false;
   std::string collation_uri;
   [[nodiscard]] bool has_collation() const { return !collation_uri.empty(); }
};

struct XPathGroupKeyInfo {
   std::string variable_name;
   [[nodiscard]] bool has_variable() const { return !variable_name.empty(); }
};

struct XPathTypeswitchCaseInfo {
   std::string variable_name;
   std::string sequence_type;
   bool is_default = false;

   [[nodiscard]] bool has_variable() const { return !variable_name.empty(); }
   [[nodiscard]] bool has_sequence_type() const { return !sequence_type.empty(); }
   [[nodiscard]] bool is_default_case() const { return is_default; }
};

struct XPathNode {
   XQueryNodeType type;
   std::string value;
   std::vector<std::unique_ptr<XPathNode>> children;
   std::optional<XPathConstructorInfo> constructor_info;
   std::vector<XPathAttributeValuePart> attribute_value_parts;
   bool attribute_value_has_expressions = false;
   std::unique_ptr<XPathNode> name_expression;
   bool order_clause_is_stable = false;
   std::optional<XPathOrderSpecOptions> order_spec_options;
   std::optional<XPathGroupKeyInfo> group_key_info;
   std::optional<XPathTypeswitchCaseInfo> typeswitch_case_info;
   // Cached operator metadata populated by the parser to avoid repeated string comparisons.
   std::optional<BinaryOperationKind> cached_binary_kind;
   std::optional<UnaryOperationKind> cached_unary_kind;
   std::vector<XPathMapConstructorEntry> map_constructor_entries;
   std::vector<std::unique_ptr<XPathNode>> array_constructor_members;
   std::vector<XPathLookupSpecifier> lookup_specifiers;

   XPathNode(XQueryNodeType t, std::string v = "") : type(t), value(std::move(v)) {}

   inline void add_child(std::unique_ptr<XPathNode> child) { children.push_back(std::move(child)); }
   [[nodiscard]] inline XPathNode * get_child(size_t index) const { return get_child_safe(index); }
   [[nodiscard]] inline XPathNode * get_child_safe(size_t index) const { return index < children.size() ? children[index].get() : nullptr; }
   [[nodiscard]] inline size_t child_count() const { return children.size(); }
   inline void set_constructor_info(XPathConstructorInfo info) { constructor_info = std::move(info); }
   [[nodiscard]] inline bool has_constructor_info() const { return constructor_info.has_value(); }
   [[nodiscard]] inline const XPathConstructorInfo * get_constructor_info() const { return constructor_info ? &(*constructor_info) : nullptr; }
   inline void set_name_expression(std::unique_ptr<XPathNode> expr) { name_expression = std::move(expr); }
   [[nodiscard]] inline XPathNode * get_name_expression() const { return name_expression.get(); }
   [[nodiscard]] inline bool has_name_expression() const { return name_expression != nullptr; }
   inline void set_group_key_info(XPathGroupKeyInfo Info) { group_key_info = std::move(Info); }
   [[nodiscard]] inline bool has_group_key_info() const { return group_key_info.has_value(); }
   [[nodiscard]] inline const XPathGroupKeyInfo * get_group_key_info() const { return group_key_info ? &(*group_key_info) : nullptr; }

   inline void set_typeswitch_case_info(XPathTypeswitchCaseInfo Info) { typeswitch_case_info = std::move(Info); }

   [[nodiscard]] inline bool has_typeswitch_case_info() const { return typeswitch_case_info.has_value(); }

   [[nodiscard]] inline const XPathTypeswitchCaseInfo * get_typeswitch_case_info() const { return typeswitch_case_info ? &(*typeswitch_case_info) : nullptr; }

   inline void set_cached_binary_kind(BinaryOperationKind Kind) { cached_binary_kind = Kind; }
   inline void clear_cached_binary_kind() { cached_binary_kind.reset(); }
   [[nodiscard]] inline bool has_cached_binary_kind() const { return cached_binary_kind.has_value(); }
   [[nodiscard]] inline std::optional<BinaryOperationKind> get_cached_binary_kind() const { return cached_binary_kind; }

   inline void set_cached_unary_kind(UnaryOperationKind Kind) { cached_unary_kind = Kind; }
   inline void clear_cached_unary_kind() { cached_unary_kind.reset(); }
   [[nodiscard]] inline bool has_cached_unary_kind() const { return cached_unary_kind.has_value(); }
   [[nodiscard]] inline std::optional<UnaryOperationKind> get_cached_unary_kind() const { return cached_unary_kind; }

   inline void add_map_entry(XPathMapConstructorEntry Entry) { map_constructor_entries.push_back(std::move(Entry)); }
   [[nodiscard]] inline size_t map_entry_count() const { return map_constructor_entries.size(); }
   [[nodiscard]] inline const XPathMapConstructorEntry * get_map_entry(size_t Index) const {
      return Index < map_constructor_entries.size() ? &map_constructor_entries[Index] : nullptr;
   }

   inline void add_array_member(std::unique_ptr<XPathNode> Member) {
      array_constructor_members.push_back(std::move(Member));
   }

   [[nodiscard]] inline size_t array_member_count() const { return array_constructor_members.size(); }

   [[nodiscard]] inline XPathNode * get_array_member(size_t Index) const {
      return Index < array_constructor_members.size() ? array_constructor_members[Index].get() : nullptr;
   }

   inline void add_lookup_specifier(XPathLookupSpecifier Specifier) {
      lookup_specifiers.push_back(std::move(Specifier));
   }

   [[nodiscard]] inline size_t lookup_specifier_count() const { return lookup_specifiers.size(); }

   [[nodiscard]] inline const XPathLookupSpecifier * get_lookup_specifier(size_t Index) const {
      return Index < lookup_specifiers.size() ? &lookup_specifiers[Index] : nullptr;
   }

   void set_attribute_value_parts(std::vector<XPathAttributeValuePart> parts) {
      attribute_value_has_expressions = false;
      for (const auto &part : parts) {
         if (part.is_expression) {
            attribute_value_has_expressions = true;
            break;
         }
      }
      attribute_value_parts = std::move(parts);
   }

   [[nodiscard]] inline bool has_attribute_value_parts() const { return not attribute_value_parts.empty(); }
   [[nodiscard]] inline std::string_view get_value_view() const { return value; }


   inline void set_order_spec_options(XPathOrderSpecOptions Options) {
      order_spec_options = std::move(Options);
   }

   [[nodiscard]] inline bool has_order_spec_options() const {
      return order_spec_options.has_value();
   }

   [[nodiscard]] inline const XPathOrderSpecOptions * get_order_spec_options() const {
      return order_spec_options ? &(*order_spec_options) : nullptr;
   }

   [[nodiscard]] inline bool child_is_type(size_t index, XQueryNodeType Type) const {
      const XPathNode *child = get_child_safe(index);
      return child and (child->type IS Type);
   }
};

//********************************************************************************************************************
// XPath Tokenization Infrastructure

struct TokenBuffer {
   TokenBuffer()
      : chunks(), total_size(0), base_chunk_capacity(initial_reserve_bytes), next_chunk_capacity(initial_reserve_bytes) {}

   explicit TokenBuffer(size_t ReserveBytes)
      : chunks(), total_size(0),
        base_chunk_capacity(ReserveBytes > initial_reserve_bytes ? ReserveBytes : initial_reserve_bytes),
        next_chunk_capacity(base_chunk_capacity) {}

   std::string_view write_copy(std::string_view Text) {
      if (Text.empty()) return std::string_view();
      ensure_capacity(Text.size());
      Chunk &chunk = chunks.back();
      char *start = chunk.data.get() + chunk.size;
      std::memcpy(start, Text.data(), Text.size());
      chunk.size += Text.size();
      total_size += Text.size();
      return std::string_view(start, Text.size());
   }

   void reset() {
      chunks.clear();
      total_size = 0;
      next_chunk_capacity = base_chunk_capacity;
   }

   void shrink_to_fit() {}

   [[nodiscard]] size_t size() const {
      return total_size;
   }

   [[nodiscard]] size_t capacity() const {
      size_t result = 0;
      for (const Chunk &chunk : chunks) {
         result += chunk.capacity;
      }
      return result;
   }

   [[nodiscard]] bool empty() const {
      return total_size IS 0;
   }

   private:
   struct Chunk {
      std::unique_ptr<char[]> data;
      size_t capacity;
      size_t size;
   };

   static constexpr size_t initial_reserve_bytes = 4096;

   void ensure_capacity(size_t AdditionalBytes) {
      if (chunks.empty()) {
         allocate_chunk(AdditionalBytes > base_chunk_capacity ? AdditionalBytes : base_chunk_capacity);
         return;
      }

      Chunk &chunk = chunks.back();
      if ((chunk.capacity - chunk.size) >= AdditionalBytes) return;

      size_t required = AdditionalBytes;
      size_t new_capacity = next_chunk_capacity;
      if (new_capacity < required) {
         while (new_capacity < required) new_capacity *= 2;
      }
      allocate_chunk(new_capacity);
   }

   void allocate_chunk(size_t Capacity) {
      Chunk chunk;
      chunk.data = std::make_unique<char[]>(Capacity);
      chunk.capacity = Capacity;
      chunk.size = 0;
      chunks.push_back(std::move(chunk));
      next_chunk_capacity = Capacity * 2;
   }

   std::vector<Chunk> chunks;
   size_t total_size;
   size_t base_chunk_capacity;
   size_t next_chunk_capacity;
};

struct XPathToken {
   XPathTokenType type = XPathTokenType::UNKNOWN;
   std::string_view text;
   size_t position = 0;
   size_t length = 0;
   TokenTextKind text_kind = TokenTextKind::BorrowedInput;
   bool is_attribute_value = false;
   std::vector<XPathAttributeValuePart> attribute_value_parts;

   XPathToken() = default;

   XPathToken(XPathTokenType Type, std::string_view Text, size_t Position = 0, size_t Length = 0, TokenTextKind Kind = TokenTextKind::BorrowedInput)
      : type(Type), text(Text), position(Position), length(Length), text_kind(Kind) {}

   [[nodiscard]] bool has_attribute_template() const {
      return is_attribute_value and !attribute_value_parts.empty();
   }
};

struct TokenBlock {
   std::shared_ptr<TokenBuffer> storage;
   std::vector<XPathToken> tokens;

   TokenBlock() : storage(std::make_shared<TokenBuffer>()), tokens() {}

   TokenBlock(std::shared_ptr<TokenBuffer> Storage, std::vector<XPathToken> Tokens)
      : storage(Storage ? std::move(Storage) : std::make_shared<TokenBuffer>()), tokens(std::move(Tokens)) {}

   TokenBlock(TokenBlock &&) = default;
   TokenBlock &operator=(TokenBlock &&) = default;
   TokenBlock(const TokenBlock &) = default;
   TokenBlock &operator=(const TokenBlock &) = default;

   [[nodiscard]] bool empty() const {
      return tokens.empty();
   }

   [[nodiscard]] size_t size() const {
      return tokens.size();
   }

   void ensure_storage() {
      if (!storage) storage = std::make_shared<TokenBuffer>();
   }

   std::string_view write_copy(std::string_view Text) {
      ensure_storage();
      return storage->write_copy(Text);
   }

   void reset() {
      tokens.clear();
      if (storage) storage->reset();
   }
};

extern std::string_view keyword_from_token_type(XPathTokenType Type);

//********************************************************************************************************************

class XPathTokeniser {
   private:
   std::string_view input;
   size_t position;
   size_t length;
   XPathTokenType previous_token_type;
   XPathTokenType prior_token_type;

   [[nodiscard]] bool is_alpha(char c) const;
   [[nodiscard]] bool is_digit(char c) const;
   [[nodiscard]] bool is_alnum(char c) const;
   [[nodiscard]] bool is_whitespace(char c) const;
   [[nodiscard]] bool is_name_start_char(char c) const;
   [[nodiscard]] bool is_name_char(char c) const;

   XPathToken scan_identifier();
   XPathToken scan_number();
   XPathToken scan_string(char QuoteChar, TokenBlock &Block);
   XPathToken scan_attribute_value(char QuoteChar, bool ProcessTemplates, TokenBlock &Block);
   XPathToken scan_operator();

   [[nodiscard]] char peek(size_t offset = 0) const;
   void skip_whitespace();
   [[nodiscard]] bool match(std::string_view Str);

   public:
   XPathTokeniser() : position(0), length(0), previous_token_type(XPathTokenType::UNKNOWN), prior_token_type(XPathTokenType::UNKNOWN) {}

   TokenBlock tokenize(std::string_view XPath);
   TokenBlock tokenize(std::string_view XPath, TokenBlock Block);
   bool has_more() const;
   [[nodiscard]] char current() const;
   void advance();
};

//********************************************************************************************************************

struct XQueryProlog;
struct XQueryModuleCache;

struct CompiledXQuery {
   std::unique_ptr<XPathNode> expression;
   std::shared_ptr<XQueryProlog> prolog;
   std::weak_ptr<XQueryModuleCache> module_cache;  // weak_ptr to break circular reference with XQueryModuleCache::modules
   std::string error_msg;

   // Cache for loaded XML documents, e.g. via the doc() function in XQuery.
   ankerl::unordered_dense::map<URI_STR, extXML *> XMLCache;

   CompiledXQuery() = default;
   CompiledXQuery(CompiledXQuery &&) = default;
   CompiledXQuery &operator=(CompiledXQuery &&) = default;
   CompiledXQuery(const CompiledXQuery &) = delete;
   CompiledXQuery &operator=(const CompiledXQuery &) = delete;

   ~CompiledXQuery() {
      for (auto &entry : XMLCache) {
         if (entry.second) FreeResource(entry.second);
      }
   }

   XQF feature_flags() const;
};

//********************************************************************************************************************
// Utilised to cache imported XQuery modules (compiled query result).

class extXQuery;
class XPathEvaluator;

struct XQueryModuleCache {
   // Referenced as a UID from xp::Compile() because it's a weak reference.
   // Used by fetch_or_load() primarily to determine the origin path of the XML data.
   extXQuery *query = nullptr;
   mutable ankerl::unordered_dense::map<std::string, std::shared_ptr<CompiledXQuery>> modules;
   mutable std::unordered_set<std::string> loading_in_progress;
   std::string base_path;

   CompiledXQuery * fetch_or_load(std::string_view, const struct XQueryProlog &, XPathEvaluator &) const;
   const CompiledXQuery * find_module(std::string_view uri) const;
};

//********************************************************************************************************************
// XPath Parser

class XPathParser {
   private:
   std::vector<XPathToken> tokens;
   size_t current_token;

   // Grammar rule methods
   std::unique_ptr<XPathNode> parse_expr();
   std::unique_ptr<XPathNode> parse_expr_single();
   std::unique_ptr<XPathNode> parse_flwor_expr();
   std::unique_ptr<XPathNode> parse_where_clause();
   std::unique_ptr<XPathNode> parse_group_clause();
   std::unique_ptr<XPathNode> parse_order_clause(bool StartsWithStable);
   std::unique_ptr<XPathNode> parse_order_spec();
   std::unique_ptr<XPathNode> parse_count_clause();
   std::unique_ptr<XPathNode> parse_or_expr();
   std::unique_ptr<XPathNode> parse_and_expr();
   std::unique_ptr<XPathNode> parse_equality_expr();
   std::unique_ptr<XPathNode> parse_relational_expr();
   std::unique_ptr<XPathNode> parse_instance_of_expr();
   std::unique_ptr<XPathNode> parse_range_expr();
   std::unique_ptr<XPathNode> parse_additive_expr();
   std::unique_ptr<XPathNode> parse_multiplicative_expr();
   std::unique_ptr<XPathNode> parse_cast_expr();
   std::unique_ptr<XPathNode> parse_unary_expr();
   std::unique_ptr<XPathNode> parse_intersect_expr();
   std::unique_ptr<XPathNode> parse_union_expr();
   std::unique_ptr<XPathNode> parse_path_expr();
   std::unique_ptr<XPathNode> parse_filter_expr();
   std::unique_ptr<XPathNode> parse_if_expr();
   std::unique_ptr<XPathNode> parse_typeswitch_expr();
   std::unique_ptr<XPathNode> parse_quantified_expr();
   std::unique_ptr<XPathNode> parse_location_path();
   std::unique_ptr<XPathNode> parse_absolute_location_path();
   std::unique_ptr<XPathNode> parse_relative_location_path();
   std::unique_ptr<XPathNode> parse_step();
   std::unique_ptr<XPathNode> parse_axis_specifier();
   std::unique_ptr<XPathNode> parse_node_test();
   std::unique_ptr<XPathNode> parse_predicate();
   std::unique_ptr<XPathNode> parse_predicate_value();
   std::unique_ptr<XPathNode> parse_abbreviated_step();
   std::unique_ptr<XPathNode> parse_primary_expr();
   std::unique_ptr<XPathNode> parse_function_call();
   std::unique_ptr<XPathNode> parse_argument();
   std::unique_ptr<XPathNode> parse_number();
   std::unique_ptr<XPathNode> parse_literal();
   std::unique_ptr<XPathNode> parse_variable_reference();
   std::unique_ptr<XPathNode> parse_direct_constructor();
   std::unique_ptr<XPathNode> parse_computed_constructor();
   std::unique_ptr<XPathNode> parse_computed_element_constructor();
   std::unique_ptr<XPathNode> parse_computed_attribute_constructor();
   std::unique_ptr<XPathNode> parse_computed_text_constructor();
   std::unique_ptr<XPathNode> parse_computed_comment_constructor();
   std::unique_ptr<XPathNode> parse_computed_pi_constructor();
   std::unique_ptr<XPathNode> parse_computed_document_constructor();
   std::unique_ptr<XPathNode> parse_map_constructor();
   std::unique_ptr<XPathNode> parse_array_constructor();
   std::unique_ptr<XPathNode> parse_lookup_expression(std::unique_ptr<XPathNode> Base);
   std::unique_ptr<XPathNode> parse_enclosed_expr();
   std::unique_ptr<XPathNode> parse_embedded_expr(std::string_view Source);

   // Prolog parsing helpers
   bool parse_prolog(XQueryProlog &prolog);
   bool parse_module_decl(XQueryProlog &prolog);
   bool parse_declare_statement(XQueryProlog &prolog);
   bool parse_namespace_decl(XQueryProlog &prolog);
   bool parse_default_namespace_decl(XQueryProlog &prolog, bool IsFunctionNamespace);
   bool parse_default_collation_decl(XQueryProlog &prolog);
   bool parse_variable_decl(XQueryProlog &prolog);
   bool parse_function_decl(XQueryProlog &prolog);
   bool parse_boundary_space_decl(XQueryProlog &prolog);
   bool parse_base_uri_decl(XQueryProlog &prolog);
   bool parse_construction_decl(XQueryProlog &prolog);
   bool parse_ordering_decl(XQueryProlog &prolog);
   bool parse_empty_order_decl(XQueryProlog &prolog);
   bool parse_copy_namespaces_decl(XQueryProlog &prolog);
   bool parse_decimal_format_decl(XQueryProlog &prolog);
   bool parse_option_decl(XQueryProlog &prolog);
   bool parse_import_statement(XQueryProlog &prolog);
   bool parse_import_module_decl(XQueryProlog &prolog);
   bool parse_import_schema_decl();
   std::optional<std::string> parse_qname_string();
   std::optional<std::string> parse_ncname();
   std::optional<std::string> parse_string_literal_value();

   XQF feature_flags();

   inline std::optional<std::string> parse_uri_literal() {
      return parse_string_literal_value();
   }

   // Consumes any declaration separators (semicolons) and returns true if any were found.

   inline bool consume_declaration_separator() {
      bool consumed = false;
      while (match(XPathTokenType::SEMICOLON)) consumed = true;
      return consumed;
   }

   std::optional<std::string> collect_sequence_type(bool StopAtReturnKeyword = false);

   // Utility methods
   [[nodiscard]] inline bool check(XPathTokenType type) const {
      return peek().type IS type;
   }

   [[nodiscard]] bool is_function_call_ahead(size_t Index) const;

   [[nodiscard]] inline bool match(XPathTokenType type) {
      if (check(type)) {
         advance();
         return true;
      }
      return false;
   }

   bool check_identifier_keyword(std::string_view Keyword) const;
   bool match_identifier_keyword(std::string_view Keyword, XPathTokenType KeywordType, XPathToken &OutToken);
   bool match_literal_keyword(std::string_view Keyword);
   bool check_literal_keyword(std::string_view Keyword) const;

   // Returns true if the given keyword token type can function as an identifier
   // in name contexts (element names, attribute names, function names, etc.).
   // All XPath/XQuery keywords are valid XML names and should be permitted.

   [[nodiscard]] inline bool is_keyword_acceptable_as_identifier(XPathTokenType Type) const {
      return not keyword_from_token_type(Type).empty();
   }

   // Helper that treats keyword tokens as identifiers in name contexts.
   // Use this for steps, function names, predicates, and variable bindings, where keywords are valid identifiers.
   // All XPath/XQuery keywords can function as element/attribute names since they are valid XML NCNames.

   bool is_identifier_token(const XPathToken &Token) const {
      if (Token.type IS XPathTokenType::IDENTIFIER) return true;
      return is_keyword_acceptable_as_identifier(Token.type);
   }

   [[nodiscard]] bool is_constructor_keyword(const XPathToken &Token) const;

   // Lightweight representation of a QName recognised within constructor syntax.

   struct ConstructorName {
      std::string Prefix;
      std::string LocalName;
   };

   std::optional<ConstructorName> parse_constructor_qname();
   bool consume_token(XPathTokenType, std::string_view);
   std::optional<XPathLookupSpecifier> parse_lookup_specifier();

   [[nodiscard]] inline const XPathToken & peek() const {
      return current_token < tokens.size() ? tokens[current_token] : tokens.back(); // END_OF_INPUT
   }

   [[nodiscard]] inline const XPathToken & previous() const {
      return tokens[current_token - 1];
   }

   [[nodiscard]] inline bool is_at_end() const {
      return peek().type IS XPathTokenType::END_OF_INPUT;
   }

   inline void advance() {
      if (!is_at_end()) current_token++;
   }

   [[nodiscard]] inline bool is_step_start_token(XPathTokenType type) const {
      // Structural tokens that start steps
      if (type IS XPathTokenType::DOT or
          type IS XPathTokenType::DOUBLE_DOT or
          type IS XPathTokenType::AT or
          type IS XPathTokenType::WILDCARD or
          type IS XPathTokenType::IDENTIFIER) {
         return true;
      }

      // All keyword tokens can start steps (as element names)
      return is_keyword_acceptable_as_identifier(type);
   }

   // Node creation helpers
   std::unique_ptr<XPathNode> create_binary_op(std::unique_ptr<XPathNode> Left, const XPathToken &Op, std::unique_ptr<XPathNode> Right);
   std::unique_ptr<XPathNode> create_unary_op(const XPathToken &Op, std::unique_ptr<XPathNode> Operand);

   public:
   XPathParser() : current_token(0) {}

   CompiledXQuery parse(TokenBlock TokenList);

   // Error handling

   inline void report_error(std::string_view Message) { errors.emplace_back(Message); }
   [[nodiscard]] inline bool has_errors() const { return !errors.empty(); }
   inline const std::vector<std::string> & get_errors() const { return errors; }

   private:
   std::vector<std::string> errors;
   XQueryProlog *active_prolog = nullptr;
   std::shared_ptr<TokenBuffer> token_storage;
};

//*********************************************************************************************************************

class extXQuery : public objXQuery {
public:
   ankerl::unordered_dense::map<std::string, std::string> Variables; // XPath variable references
   FUNCTION Callback;
   std::string Statement;
   std::string ErrorMsg;
   CompiledXQuery ParseResult; // Result of parsing the query.
   std::shared_ptr<XQueryModuleCache> ModuleCache; // Strong reference; ParseResult.module_cache is weak to break cycles
   XPathVal Result; // Result of the last execution.
   pf::vector<std::string> ListVariables; // List of variable names.
   pf::vector<std::string> ListFunctions; // List of function names.
   std::string ResultString; // Cached string representation of the result.
   std::string Path; // Base path for resolving relative URIs.
   size_t MemUsage; // Total bytes allocated during the most recent evaluation or compilation.
   extXML *XML; // During query execution, the context XML document.
   bool StaleBuild = true; // If true, the compiled query needs to be rebuilt.

   ~extXQuery() {
   }
};

// Transparent string hash/equality functors for heterogeneous lookup on ankerl maps
struct TransparentStringHash {
   using is_transparent = void;

   [[nodiscard]] size_t operator()(std::string_view Value) const noexcept { return std::hash<std::string_view>{}(Value); }
   [[nodiscard]] size_t operator()(const std::string &Value) const noexcept { return operator()(std::string_view(Value)); }
   [[nodiscard]] size_t operator()(const char *Value) const noexcept { return operator()(std::string_view(Value)); }
};

struct TransparentStringEqual {
   using is_transparent = void;

   [[nodiscard]] bool operator()(std::string_view Lhs, std::string_view Rhs) const noexcept { return Lhs IS Rhs; }
   [[nodiscard]] bool operator()(const std::string &Lhs, const std::string &Rhs) const noexcept { return std::string_view(Lhs) IS std::string_view(Rhs); }
   [[nodiscard]] bool operator()(const char *Lhs, const char *Rhs) const noexcept { return std::string_view(Lhs) IS std::string_view(Rhs); }
   [[nodiscard]] bool operator()(const std::string &Lhs, std::string_view Rhs) const noexcept { return std::string_view(Lhs) IS Rhs; }
   [[nodiscard]] bool operator()(std::string_view Lhs, const std::string &Rhs) const noexcept { return Lhs IS std::string_view(Rhs); }
   [[nodiscard]] bool operator()(const char *Lhs, std::string_view Rhs) const noexcept { return std::string_view(Lhs) IS Rhs; }
   [[nodiscard]] bool operator()(std::string_view Lhs, const char *Rhs) const noexcept { return Lhs IS std::string_view(Rhs); }
};

//********************************************************************************************************************
// If an XQuery expression contains a prolog, it will be parsed into this structure and maintained in the XPathNode
// prolog field.

struct XQueryProlog {
   XQueryProlog();

   struct CopyNamespaces {
      bool preserve = true;
      bool inherit = true;
   } copy_namespaces;

   struct ExportValidationResult {
      bool valid = true;
      std::string error_message;
      std::string problematic_qname;
      bool is_function = false;  // true if problematic item is a function, false if variable
   };

   enum class BoundarySpace { Preserve, Strip } boundary_space = BoundarySpace::Strip;
   enum class ConstructionMode { Preserve, Strip } construction_mode = ConstructionMode::Strip;
   enum class OrderingMode { Ordered, Unordered } ordering_mode = OrderingMode::Ordered;
   enum class EmptyOrder { Greatest, Least } empty_order = EmptyOrder::Greatest;

   ankerl::unordered_dense::map<std::string, uint32_t, TransparentStringHash, TransparentStringEqual> declared_namespaces;
   ankerl::unordered_dense::map<std::string, std::string, TransparentStringHash, TransparentStringEqual> declared_namespace_uris;
   ankerl::unordered_dense::map<std::string, XQueryVariable, TransparentStringHash, TransparentStringEqual> variables;
   ankerl::unordered_dense::map<FunctionKey, XQueryFunction, FunctionKeyHash> functions;
   ankerl::unordered_dense::map<std::string, DecimalFormat> decimal_formats;
   ankerl::unordered_dense::map<std::string, std::string> options;

   std::vector<XQueryModuleImport> module_imports;

   std::optional<uint32_t> default_element_namespace;
   std::optional<uint32_t> default_function_namespace;
   std::optional<std::string> default_element_namespace_uri;
   std::optional<std::string> default_function_namespace_uri;
   std::optional<std::string> module_namespace_uri;
   std::optional<std::string> module_namespace_prefix;

   std::string static_base_uri;
   std::string default_collation;

   bool is_library_module = false;
   bool static_base_uri_declared = false;
   bool default_collation_declared = false;
   bool boundary_space_declared = false;
   bool construction_declared = false;
   bool ordering_declared = false;
   bool empty_order_declared = false;
   bool copy_namespaces_declared = false;
   bool default_decimal_format_declared = false;

   bool declare_namespace(std::string_view prefix, std::string_view uri, extXML *document);
   bool declare_variable(std::string_view qname, XQueryVariable variable);
   bool declare_function(XQueryFunction function);
   bool declare_module_import(XQueryModuleImport import_decl, std::string *error_message = nullptr);
   [[nodiscard]] bool validate_library_exports() const;
   [[nodiscard]] ExportValidationResult validate_library_exports_detailed() const;
   [[nodiscard]] const XQueryFunction * find_function(std::string_view qname, size_t arity) const;
   [[nodiscard]] const XQueryVariable * find_variable(std::string_view qname) const;
   [[nodiscard]] uint32_t resolve_prefix(std::string_view prefix, const extXML *document) const;
   [[nodiscard]] std::string normalise_function_qname(std::string_view qname, const XPathNode *document = nullptr) const;
   void bind_module_cache(std::shared_ptr<XQueryModuleCache> cache);
   [[nodiscard]] std::shared_ptr<XQueryModuleCache> get_module_cache() const;

   private:
   std::weak_ptr<XQueryModuleCache> module_cache;
};



namespace xml::schema {
   class SchemaTypeRegistry;
   class SchemaTypeDescriptor;
   SchemaTypeRegistry & registry();
}

class XPathArena {
   private:
   template<typename T>
   struct TieredVectorPool {
      static constexpr size_t size_classes = 5;
      static constexpr size_t tier_limits[size_classes] = { 16, 64, 256, 1024, 4096 };

      std::array<std::vector<std::unique_ptr<std::vector<T>>>, size_classes> storage{};
      std::array<std::vector<std::vector<T>*>, size_classes> free_lists{};

      [[nodiscard]] static size_t select_tier(size_t size) {
         for (size_t i = 0; i < size_classes; ++i) if (size <= tier_limits[i]) return i;
         return size_classes - 1;
      }

      std::vector<T> & acquire() { return acquire(0); }

      std::vector<T> & acquire(size_t reserve_hint) {
         size_t tier = select_tier(reserve_hint > 0 ? reserve_hint : 1);
         for (size_t t = tier; t < size_classes; ++t) {
            auto &list = free_lists[t];
            if (!list.empty()) {
               auto *vec = list.back();
               list.pop_back();
               vec->clear();
               if (reserve_hint > 0 and vec->capacity() < reserve_hint) vec->reserve(reserve_hint);
               return *vec;
            }
         }
         auto &bucket = storage[tier];
         bucket.push_back(std::make_unique<std::vector<T>>());
         auto &vec = *bucket.back();
         vec.clear();
         if (reserve_hint > 0) vec.reserve(reserve_hint);
         return vec;
      }

      void release(std::vector<T> &vec) {
         vec.clear();
         size_t cap = vec.capacity();
         size_t tier = select_tier(cap > 0 ? cap : 1);
         free_lists[tier].push_back(&vec);
      }

      void reset() {
         for (size_t t = 0; t < size_classes; ++t) {
            free_lists[t].clear();
            for (auto &entry : storage[t]) {
               entry->clear();
               free_lists[t].push_back(entry.get());
            }
         }
      }
   };

   struct TieredNodeVectorPool {
      static constexpr size_t size_classes = 5;
      static constexpr size_t tier_limits[size_classes] = { 16, 64, 256, 1024, 4096 };

      std::array<std::vector<std::unique_ptr<NODES>>, size_classes> storage{};
      std::array<std::vector<NODES *>, size_classes> free_lists{};
      ankerl::unordered_dense::map<NODES *, size_t> allocated_tier;

      [[nodiscard]] static size_t select_tier(size_t size) {
         for (size_t i = 0; i < size_classes; ++i) if (size <= tier_limits[i]) return i;
         return size_classes - 1;
      }

      NODES & acquire() { return acquire(0); }

      NODES & acquire(size_t reserve_hint) {
         size_t tier = select_tier(reserve_hint > 0 ? reserve_hint : 1);

         // Prefer exact tier, then larger tiers
         for (size_t t = tier; t < size_classes; ++t) {
            auto &list = free_lists[t];
            if (!list.empty()) {
               auto *vec = list.back();
               list.pop_back();
               vec->clear();
               // Track location for proper release
               allocated_tier[vec] = t;
               return *vec;
            }
         }

         // Allocate new in preferred tier
         auto &bucket = storage[tier];
         bucket.push_back(std::make_unique<NODES>());
         auto &vec = *bucket.back();
         vec.clear();
         allocated_tier[&vec] = tier;
         return vec;
      }

      void release(NODES &vec) {
         vec.clear();
         size_t tier = 0;
         auto it = allocated_tier.find(&vec);
         if (it != allocated_tier.end()) tier = it->second;
         free_lists[tier].push_back(&vec);
      }

      void reset() {
         for (size_t t = 0; t < size_classes; ++t) {
            free_lists[t].clear();
            for (auto &entry : storage[t]) {
               entry->clear();
               free_lists[t].push_back(entry.get());
               allocated_tier[entry.get()] = t;
            }
         }
      }
   };

   TieredNodeVectorPool node_vectors;
   TieredVectorPool<const XMLAttrib *> attribute_vectors;
   TieredVectorPool<std::string> string_vectors;

   public:
   XPathArena() = default;
   XPathArena(const XPathArena &) = delete;
   XPathArena & operator=(const XPathArena &) = delete;

   NODES & acquire_node_vector() { return node_vectors.acquire(); }
   NODES & acquire_node_vector(size_t reserve_hint) { return node_vectors.acquire(reserve_hint); }
   void release_node_vector(NODES &Vector) { node_vectors.release(Vector); }

   std::vector<const XMLAttrib *> & acquire_attribute_vector() { return attribute_vectors.acquire(); }
   std::vector<const XMLAttrib *> & acquire_attribute_vector(size_t reserve_hint) { return attribute_vectors.acquire(reserve_hint); }
   void release_attribute_vector(std::vector<const XMLAttrib *> &Vector) { attribute_vectors.release(Vector); }

   std::vector<std::string> & acquire_string_vector() { return string_vectors.acquire(); }
   std::vector<std::string> & acquire_string_vector(size_t reserve_hint) { return string_vectors.acquire(reserve_hint); }
   void release_string_vector(std::vector<std::string> &Vector) { string_vectors.release(Vector); }

   void reset() {
      node_vectors.reset();
      attribute_vectors.reset();
      string_vectors.reset();
   }
};

//********************************************************************************************************************
// Axis Evaluation Engine

class AxisEvaluator {
   private:
   CompiledXQuery *state;
   extXML *xml;
   XPathArena & arena;
   std::vector<std::unique_ptr<XTag>> namespace_node_storage;
   bool id_cache_built = false;

   struct AncestorPathView {
      std::span<XTag *const> Path;
      NODES * Storage = nullptr;
      bool Cached = false;
   };

   ankerl::unordered_dense::map<XTag *, NODES *> ancestor_path_cache;
   std::vector<std::unique_ptr<NODES>> ancestor_path_storage;
   ankerl::unordered_dense::map<uint64_t, bool> document_order_cache;

   // Optimized namespace handling data structures
   struct NamespaceDeclaration {
      std::string prefix;
      std::string uri;

      bool operator<(const NamespaceDeclaration &other) const {
         return prefix < other.prefix;
      }
   };

   // Reusable storage for namespace processing to avoid allocations
   std::vector<NamespaceDeclaration> namespace_declarations;
   std::vector<int> visited_node_ids;

   // Namespace node pool for reuse to reduce allocations
   std::vector<std::unique_ptr<XTag>> namespace_node_pool;

   // Helper methods for specific axes
   void evaluate_child_axis(XTag *ContextNode, NODES &Output);
   void evaluate_descendant_axis(XTag *ContextNode, NODES &Output);
   void evaluate_parent_axis(XTag *ContextNode, NODES &Output);
   void evaluate_ancestor_axis(XTag *ContextNode, NODES &Output);
   void evaluate_following_sibling_axis(XTag *ContextNode, NODES &Output);
   void evaluate_preceding_sibling_axis(XTag *ContextNode, NODES &Output);
   void evaluate_following_axis(XTag *ContextNode, NODES &Output);
   void evaluate_preceding_axis(XTag *ContextNode, NODES &Output);
   void evaluate_namespace_axis(XTag *ContextNode, NODES &Output);
   void evaluate_self_axis(XTag *ContextNode, NODES &Output);
   void evaluate_descendant_or_self_axis(XTag *ContextNode, NODES &Output);
   void evaluate_ancestor_or_self_axis(XTag *ContextNode, NODES &Output);

   void collect_subtree_reverse(XTag *Node, NODES &Output);

   // Optimized namespace collection
   void collect_namespace_declarations(XTag *Node, std::vector<NamespaceDeclaration> &declarations);

   // Namespace node pooling helpers
   XTag * acquire_namespace_node();
   void recycle_namespace_nodes();

   // Document order utilities
   void sort_document_order(NODES &Nodes);
   AncestorPathView build_ancestor_path(XTag *Node);
   void release_ancestor_path(AncestorPathView &View);
   uint64_t make_document_order_key(XTag *Left, XTag *Right);

   // Helper methods for tag lookup
   void build_id_cache();
   XTag * find_parent(XTag *ReferenceNode);

   public:
   explicit AxisEvaluator(CompiledXQuery *State, extXML *XML, XPathArena &Arena)
      : state(State), xml(XML), arena(Arena) {}

   // Main evaluation method
   void evaluate_axis(AxisType Axis, XTag *ContextNode, NODES &Output);

   // Evaluation lifecycle helpers
   void reset_namespace_nodes();

   // Node-set utilities
   void normalise_node_set(NODES &Nodes);
   bool is_before_in_document_order(XTag *Node1, XTag *Node2);

   // Utility methods
   static AxisType parse_axis_name(std::string_view AxisName);
   static std::string_view axis_name_to_string(AxisType Axis);
   static bool is_reverse_axis(AxisType Axis);

   // Capacity estimation helper
   size_t estimate_result_size(AxisType Axis, XTag *ContextNode);
};

struct XMLAttrib;
class CompiledXQuery;
class XPathContext;
class XPathEvaluator;

//********************************************************************************************************************
// XPath Evaluation Context.  Stored in XPathEvaluator and initialised in its constructor.
// The context is pushed and popped as a stack frame during the evaluation process.

struct XPathContext {
   mutable XPathEvaluator *eval = nullptr;
   XTag *context_node = nullptr;
   const XMLAttrib * attribute_node = nullptr;
   size_t position = 1;
   size_t size = 1;
   ankerl::unordered_dense::map<std::string, XPathVal> * variables = nullptr;
   extXML *xml = nullptr;
   bool *expression_unsupported = nullptr;
   xml::schema::SchemaTypeRegistry * schema_registry = nullptr;
   std::shared_ptr<XQueryProlog> prolog;
   std::shared_ptr<XQueryModuleCache> module_cache;

   [[nodiscard]] inline std::shared_ptr<XQueryModuleCache> modules();

   XPathContext() = default;
};

struct SequenceTypeInfo;

class XPathEvaluator : public XPathErrorReporter {
   public:

   enum class PredicateResult {
      MATCH,
      NO_MATCH,
      UNSUPPORTED
   };

   extXQuery *query;
   extXML *xml;
   const XPathNode * query_root = nullptr;
   CompiledXQuery * parse_context = nullptr;
   XPathContext context;
   XPathArena arena;
   AxisEvaluator axis_evaluator;
   bool expression_unsupported = false;
   bool trace_xpath_enabled = false;
   bool construction_preserve_mode = false;

   // Variable storage owned by the evaluator
   ankerl::unordered_dense::map<std::string, XPathVal> variable_storage;
   ankerl::unordered_dense::map<std::string, XPathVal> prolog_variable_cache;
   std::unordered_set<std::string> variables_in_evaluation;

   // Tracks in-scope namespace declarations while building constructed nodes so nested
   // constructors inherit and override prefixes correctly.
   struct ConstructorNamespaceScope {
      const ConstructorNamespaceScope * parent = nullptr;
      ankerl::unordered_dense::map<std::string, uint32_t> prefix_bindings;
      std::optional<uint32_t> default_namespace;
   };

   std::vector<std::unique_ptr<XTag>> constructed_nodes;
   int next_constructed_node_id = -1;

   struct AxisMatch {
      XTag * node = nullptr;
      const XMLAttrib * attribute = nullptr;
   };

   using PredicateHandler = PredicateResult (XPathEvaluator::*)(const XPathNode *, uint32_t);

   // Handler for a specific XQuery node type evaluation
   using NodeEvaluationHandler = XPathVal (XPathEvaluator::*)(const XPathNode *, uint32_t);

   std::vector<XPathContext> context_stack;

   // Cache for any form of unparsed text resource, e.g. loaded via the unparsed-text() function in XQuery.

   ankerl::unordered_dense::map<std::string, std::string> text_cache;
   std::array<uint64_t, 64> node_dispatch_counters{};
   uint64_t binary_operator_cache_fallbacks = 0;
   uint64_t unary_operator_cache_fallbacks = 0;

   void reset_dispatch_metrics();
   [[nodiscard]] const std::array<uint64_t, 64> &dispatch_metrics() const;
   void record_dispatch_node(XQueryNodeType Type);
   [[nodiscard]] inline uint64_t binary_operator_cache_misses() const { return binary_operator_cache_fallbacks; }
   [[nodiscard]] inline uint64_t unary_operator_cache_misses() const { return unary_operator_cache_fallbacks; }

   std::vector<AxisMatch> dispatch_axis(AxisType Axis, XTag *ContextNode, const XMLAttrib *ContextAttribute = nullptr);
   extXML * resolve_document_for_node(XTag *Node) const;
   bool is_foreign_document_node(XTag *Node) const;
   NODES collect_step_results(const std::vector<AxisMatch> &,
      const std::vector<const XPathNode *> &, size_t, uint32_t, bool &);
   XPathVal evaluate_path_expression_value(const XPathNode *PathNode, uint32_t CurrentPrefix);
   XPathVal evaluate_path_from_nodes(const NODES &,
      const std::vector<const XMLAttrib *> &, const std::vector<const XPathNode *> &, const XPathNode *,
      const XPathNode *, uint32_t);
   ERR evaluate_top_level_expression(const XPathNode *Node, uint32_t CurrentPrefix);
   ERR process_expression_node_set(const XPathVal &Value);
   XPathVal evaluate_union_value(const std::vector<const XPathNode *> &Branches, uint32_t CurrentPrefix);
   XPathVal evaluate_intersect_value(const XPathNode *Left, const XPathNode *Right, uint32_t CurrentPrefix);
   XPathVal evaluate_except_value(const XPathNode *Left, const XPathNode *Right, uint32_t CurrentPrefix);
   ERR evaluate_union(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal evaluate_flwor_pipeline(const XPathNode *Node, uint32_t CurrentPrefix);
   void initialise_query_context(const XPathNode *Root);

   std::optional<uint32_t> resolve_constructor_prefix(const ConstructorNamespaceScope &Scope,
      std::string_view Prefix) const;
   uint32_t register_constructor_namespace(const std::string &URI) const;
   XTag clone_node_subtree(const XTag &Source, int ParentID);
   bool append_constructor_sequence(XTag &Parent, const XPathVal &Value,
      uint32_t CurrentPrefix, const ConstructorNamespaceScope &Scope, bool PreserveConstruction);
   std::optional<std::string> evaluate_attribute_value_template(const XPathConstructorAttribute &Attribute,
      uint32_t CurrentPrefix);
   std::optional<std::string> evaluate_constructor_content_string(const XPathNode *Node, uint32_t CurrentPrefix,
      bool ApplyWhitespaceRules, bool PreserveConstruction);
   std::optional<std::string> evaluate_constructor_name_string(const XPathNode *Node, uint32_t CurrentPrefix);
   std::optional<std::string> prepare_constructor_text(std::string_view Text, bool IsLiteral) const;
   bool prolog_has_boundary_space_preserve() const;
   bool prolog_construction_preserve() const;
   bool prolog_ordering_is_ordered() const;
   bool prolog_empty_is_greatest() const;
   std::optional<XTag> build_direct_element_node(const XPathNode *Node, uint32_t CurrentPrefix,
      ConstructorNamespaceScope *ParentScope, int ParentID);
   XPathVal evaluate_direct_element_constructor(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal evaluate_computed_element_constructor(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal evaluate_computed_attribute_constructor(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal evaluate_text_constructor(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal evaluate_comment_constructor(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal evaluate_pi_constructor(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal evaluate_document_constructor(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal nodeset_from_sequence_entries(const std::vector<SequenceEntry> &Entries);
   XPathVal materialise_sequence_value(const XPathValueSequence &Sequence);
   XPathVal concatenate_sequence_values(const std::vector<XPathVal> &Values);
   XPathVal apply_lookup_to_value(const XPathVal &BaseValue, const XPathLookupSpecifier &Specifier,
      uint32_t CurrentPrefix, const XPathNode *ContextNode);
   XPathVal lookup_map_value(const XPathVal &BaseValue, const XPathLookupSpecifier &Specifier,
      uint32_t CurrentPrefix, const XPathNode *ContextNode);
   XPathVal lookup_array_value(const XPathVal &BaseValue, const XPathLookupSpecifier &Specifier,
      uint32_t CurrentPrefix, const XPathNode *ContextNode);
   XPathVal lookup_nodeset_value(const XPathVal &BaseValue, const XPathLookupSpecifier &Specifier,
      uint32_t CurrentPrefix, const XPathNode *ContextNode);

   // Expression node type handlers
   XPathVal handle_empty_sequence(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_number(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_literal(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_map_constructor(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_array_constructor(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_lookup_expression(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_cast_expression(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_treat_as_expression(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_instance_of_expression(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_castable_expression(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_typeswitch_expression(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_union_node(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_conditional(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_let_expression(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_for_expression(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_quantified_expression(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_filter(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_path(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_unary_op(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_binary_op(const XPathNode *Node, uint32_t CurrentPrefix);
   bool is_arithmetic_chain_candidate(BinaryOperationKind OpKind) const;
   std::vector<const XPathNode *> collect_operation_chain(const XPathNode *Node, BinaryOperationKind OpKind) const;
   XPathVal evaluate_arithmetic_chain(const std::vector<const XPathNode *> &Operands,
      BinaryOperationKind OpKind, uint32_t CurrentPrefix);
   XPathVal handle_binary_logical(const XPathNode *Node, const XPathNode *Left, const XPathNode *Right,
      uint32_t CurrentPrefix, BinaryOperationKind OpKind);
   XPathVal handle_binary_comparison(const XPathNode *Node, const XPathNode *Left, const XPathNode *Right,
      uint32_t CurrentPrefix, BinaryOperationKind OpKind);
   XPathVal handle_binary_arithmetic(const XPathNode *Node, const XPathNode *Left, const XPathNode *Right,
      uint32_t CurrentPrefix, BinaryOperationKind OpKind);
   XPathVal handle_binary_sequence(const XPathNode *Node, const XPathNode *Left, const XPathNode *Right,
      uint32_t CurrentPrefix, BinaryOperationKind OpKind);
   XPathVal handle_binary_set_ops(const XPathNode *Node, const XPathNode *Left, const XPathNode *Right,
      uint32_t CurrentPrefix, BinaryOperationKind OpKind);
   XPathVal handle_expression_wrapper(const XPathNode *Node, uint32_t CurrentPrefix);
   XPathVal handle_variable_reference(const XPathNode *Node, uint32_t CurrentPrefix);

   std::optional<bool> matches_sequence_type(const XPathVal &Value, const SequenceTypeInfo &SequenceInfo,
      const XPathNode *ContextNode);

   void expand_axis_candidates(const AxisMatch &ContextEntry, AxisType Axis,
      const XPathNode *NodeTest, uint32_t CurrentPrefix, std::vector<AxisMatch> &FilteredMatches);
   ERR apply_predicates_to_candidates(const std::vector<const XPathNode *> &PredicateNodes,
      uint32_t CurrentPrefix, std::vector<AxisMatch> &Candidates, std::vector<AxisMatch> &ScratchBuffer);
   ERR process_step_matches(const std::vector<AxisMatch> &Matches, AxisType Axis, bool IsLastStep,
      bool &Matched, std::vector<AxisMatch> &NextContext, bool &ShouldTerminate);
   ERR invoke_callback(XTag *Node, const XMLAttrib *Attribute, bool &Matched, bool &ShouldTerminate);

   PredicateResult dispatch_predicate_operation(std::string_view OperationName, const XPathNode *Expression,
      uint32_t CurrentPrefix);
   const std::unordered_map<std::string_view, PredicateHandler> &predicate_handler_map() const;
   PredicateResult handle_attribute_exists_predicate(const XPathNode *Expression, uint32_t CurrentPrefix);
   PredicateResult handle_attribute_equals_predicate(const XPathNode *Expression, uint32_t CurrentPrefix);
   PredicateResult handle_content_equals_predicate(const XPathNode *Expression, uint32_t CurrentPrefix);

   std::string build_ast_signature(const XPathNode *Node) const;

   void record_error(std::string_view Message, bool Force = false) override;
   void record_error(std::string_view Message, const XPathNode *Node, bool Force = false) override;
   std::optional<XPathVal> resolve_user_defined_function(std::string_view FunctionName,
      const std::vector<XPathVal> &Args, uint32_t CurrentPrefix, const XPathNode *FuncNode);
   XPathVal evaluate_user_defined_function(const XQueryFunction &Function,
      const std::vector<XPathVal> &Args, uint32_t CurrentPrefix, const XPathNode *FuncNode);
   bool resolve_variable_value(std::string_view QName, uint32_t CurrentPrefix,
      XPathVal &OutValue, const XPathNode *ReferenceNode);

   public:
   // Constructor
   XPathEvaluator(extXQuery *, extXML *, const XPathNode *, CompiledXQuery *);

   ERR evaluate_ast(const XPathNode *Node, uint32_t CurrentPrefix);
   ERR evaluate_location_path(const XPathNode *PathNode, uint32_t CurrentPrefix);
   ERR evaluate_step_ast(const XPathNode *StepNode, uint32_t CurrentPrefix);
   ERR evaluate_step_sequence(const NODES &ContextNodes, const std::vector<const XPathNode *> &Steps, size_t StepIndex, uint32_t CurrentPrefix, bool &Matched);
   bool match_node_test(const XPathNode *NodeTest, AxisType Axis, XTag *Candidate, const XMLAttrib *Attribute, uint32_t CurrentPrefix);
   PredicateResult evaluate_predicate(const XPathNode *PredicateNode, uint32_t CurrentPrefix);

   XPathVal evaluate_expression(const XPathNode *ExprNode, uint32_t CurrentPrefix);
   XPathVal evaluate_function_call(const XPathNode *FuncNode, uint32_t CurrentPrefix);
   XPathVal evaluate_type_constructor(const std::shared_ptr<xml::schema::SchemaTypeDescriptor> &TargetDescriptor,
      const std::vector<XPathVal> &Args, const XPathNode *CallSite);

   // Entry point for compiled XPath evaluation
   ERR find_tag(const XPathNode &, uint32_t);

   inline bool is_trace_enabled() const { return trace_xpath_enabled; }

   // Full XPath expression evaluation returning computed values.  Will update the provided XPathValue
   ERR evaluate_xpath_expression(const XPathNode &, XPathVal *, uint32_t CurrentPrefix = 0);

   // Context management for AST evaluation
   void push_context(XTag *Node, size_t Position = 1, size_t Size = 1, const XMLAttrib *Attribute = nullptr);
   void pop_context();

   [[nodiscard]] inline XTag * get_context_node() const;
};

//********************************************************************************************************************

struct SequenceEntry {
   XTag * node = nullptr;
   const XMLAttrib * attribute = nullptr;
   std::string string_value;
   std::shared_ptr<XPathValue> composite_value;
};

struct ForBindingDefinition {
   std::string name;
   const XPathNode * sequence = nullptr;
};

struct QuantifiedBindingDefinition {
   std::string name;
   const XPathNode * sequence = nullptr;
};

struct CastTargetInfo {
   std::string type_name;
   bool allows_empty = false;
};

struct SequenceTypeInfo {
   SequenceCardinality occurrence = SequenceCardinality::ExactlyOne;
   SequenceItemKind kind = SequenceItemKind::Atomic;
   std::string type_name;
   bool map_allows_any = false;
   std::string map_key_type;
   std::shared_ptr<SequenceTypeInfo> map_value_type;
   bool array_allows_any = false;
   std::shared_ptr<SequenceTypeInfo> array_member_type;

   [[nodiscard]] inline bool allows_empty() const {
      return (occurrence IS SequenceCardinality::ZeroOrOne) or (occurrence IS SequenceCardinality::ZeroOrMore);
   }

   [[nodiscard]] inline bool allows_multiple() const {
      return (occurrence IS SequenceCardinality::OneOrMore) or (occurrence IS SequenceCardinality::ZeroOrMore);
   }
};

//********************************************************************************************************************

class VariableBindingGuard
{
   private:
   XPathContext & context;
   std::string variable_name;
   std::optional<XPathVal> previous_value;
   bool had_previous_value = false;

   public:
   VariableBindingGuard(XPathContext &Context, std::string Name, XPathVal Value)
      : context(Context), variable_name(std::move(Name))
   {
      auto existing = context.variables->find(variable_name);
      had_previous_value = (existing != context.variables->end());
      if (had_previous_value) previous_value = existing->second;

      (*context.variables)[variable_name] = std::move(Value);
   }

   ~VariableBindingGuard() {
      if (had_previous_value) (*context.variables)[variable_name] = *previous_value;
      else context.variables->erase(variable_name);
   }

   VariableBindingGuard(const VariableBindingGuard &) = delete;
   VariableBindingGuard & operator=(const VariableBindingGuard &) = delete;

   VariableBindingGuard(VariableBindingGuard &&) = default;
   VariableBindingGuard & operator=(VariableBindingGuard &&) = default;
};

[[nodiscard]] inline XTag * XPathEvaluator::get_context_node() const { return context.context_node; };
[[nodiscard]] inline std::shared_ptr<XQueryModuleCache> XPathContext::modules() { return eval->parse_context->module_cache.lock(); };

extern "C" ERR load_regex(void);

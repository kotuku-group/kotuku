// Parse-time descriptor helpers.

#include "static_type_descriptor.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <string>

#include <kotuku/main.h>
#include <kotuku/objects.h>

#include "ast/nodes.h"
#include "lexer.h"
#include "parse_types.h"
#include "../runtime/lj_array.h"
#include "../runtime/lj_struct.h"

bool StaticValueDescriptor::constrained() const noexcept
{
   if (this->primary IS TiriType::Object) return this->object_class_id != CLASSID::NIL;
   if (this->primary IS TiriType::Struct) return this->struct_def != nullptr;
   if (this->primary IS TiriType::Array) return this->array_element.known;
   return this->primary != TiriType::Unknown and this->primary != TiriType::Any;
}

bool StaticValueDescriptor::proved() const noexcept
{
   return this->proof IS StaticProof::Closed or this->proof IS StaticProof::Checked or
      this->proof IS StaticProof::Trusted;
}

[[nodiscard]] static AET public_array_storage(AET Storage) noexcept
{
   if (Storage IS AET::CSTR or Storage IS AET::STR_CPP) return AET::STR_GC;
   return Storage;
}

[[nodiscard]] static bool recursive_array_identity_matches(std::string_view Expected, std::string_view Actual)
{
   if (Expected IS Actual) return true;
   if (Expected IS "array" or Expected IS "array<any>") {
      return Actual IS "array" or (Actual.starts_with("array<") and Actual.ends_with('>'));
   }
   if (not Expected.starts_with("array<") or not Expected.ends_with('>') or
       not Actual.starts_with("array<") or not Actual.ends_with('>')) return false;
   std::string_view expected_member = Expected.substr(6, Expected.size() - 7);
   std::string_view actual_member = Actual.substr(6, Actual.size() - 7);
   if (expected_member IS "any") return true;
   if ((expected_member IS "array" or expected_member.starts_with("array<")) and
       (actual_member IS "array" or actual_member.starts_with("array<"))) {
      return recursive_array_identity_matches(expected_member, actual_member);
   }
   return false;
}

bool array_element_matches(
   const ArrayElementDescriptor &Expected, const ArrayElementDescriptor &Actual)
{
   if (not Expected.known or Expected.storage IS AET::ANY) return Actual.known;
   if (not Actual.known or public_array_storage(Expected.storage) != public_array_storage(Actual.storage)) return false;
   if (Expected.storage IS AET::STRUCT and Expected.struct_def) return Expected.struct_def IS Actual.struct_def;
   if (Expected.storage IS AET::ARRAY and Expected.nested_array_identity) {
      std::string_view expected_name(strdata(Expected.nested_array_identity), Expected.nested_array_identity->len);
      std::string_view actual_name = "array";
      if (Actual.nested_array_identity) {
         actual_name = std::string_view(
            strdata(Actual.nested_array_identity), Actual.nested_array_identity->len);
      }
      return recursive_array_identity_matches(expected_name, actual_name);
   }
   return true;
}

ArrayElementDescriptor describe_array_element(const GCarray *Array)
{
   if (not Array) return {};
   ArrayElementDescriptor result;
   result.storage = public_array_storage(Array->elemtype);
   result.logical_type = TiriType::Any;
   result.struct_def = Array->elemtype IS AET::STRUCT ? Array->structdef : nullptr;
   result.known = true;
   switch (result.storage) {
      case AET::BYTE:
      case AET::INT8:
      case AET::INT16:
      case AET::INT32:
      case AET::INT64:
      case AET::UINT8:
      case AET::UINT16:
      case AET::UINT32:
      case AET::UINT64:
      case AET::FLOAT:
      case AET::DOUBLE: result.logical_type = TiriType::Num; break;
      case AET::STR_GC: result.logical_type = TiriType::Str; break;
      case AET::TABLE:  result.logical_type = TiriType::Table; break;
      case AET::ARRAY:  result.logical_type = TiriType::Array; break;
      case AET::OBJECT: result.logical_type = TiriType::Object; break;
      case AET::STRUCT: result.logical_type = TiriType::Struct; break;
      case AET::PTR:
      case AET::ANY:    break;
      case AET::CSTR:
      case AET::STR_CPP:
      case AET::MAX: break;
   }
   return result;
}

bool array_element_matches(const ArrayElementDescriptor &Expected, const GCarray *Actual)
{
   if (Expected.storage IS AET::ARRAY and Expected.nested_array_identity) {
      if (not Actual or Actual->elemtype != AET::ARRAY or not gcref(Actual->type_identity)) return false;
      GCstr *actual_identity = strref(Actual->type_identity);
      std::string_view expected_name(strdata(Expected.nested_array_identity), Expected.nested_array_identity->len);
      std::string_view actual_name(strdata(actual_identity), actual_identity->len);
      return recursive_array_identity_matches(expected_name, actual_name);
   }
   return array_element_matches(Expected, describe_array_element(Actual));
}

std::string array_element_name(const ArrayElementDescriptor &Element)
{
   if (not Element.known) return "array";
   AET storage = public_array_storage(Element.storage);
   if (storage IS AET::STRUCT and Element.struct_def) return std::format("struct<{}>", Element.struct_def->Name);
   if (storage IS AET::ARRAY and Element.nested_array_identity) {
      return std::string(strdata(Element.nested_array_identity), Element.nested_array_identity->len);
   }
   return std::string(lj_array_elemtype_name(storage));
}

namespace {

class RecursiveArrayTypeParser {
public:
   RecursiveArrayTypeParser(std::string_view Text, lua_State *State, LexState *Lexer)
      : text_(Text), state_(State), lexer_(Lexer) { }

   std::optional<ArrayElementDescriptor> parse_element()
   {
      auto result = this->parse_member(0, nullptr);
      this->skip_space();
      if (not result or this->position_ != this->text_.size()) return std::nullopt;
      return result;
   }

   std::optional<std::string> canonical_name()
   {
      std::string canonical;
      auto result = this->parse_member(0, &canonical);
      this->skip_space();
      if (not result or this->position_ != this->text_.size()) return std::nullopt;
      if (result->storage IS AET::ARRAY) return canonical;
      return std::format("array<{}>", canonical);
   }

private:
   static constexpr size_t MAX_DEPTH = 32;

   void skip_space()
   {
      while (this->position_ < this->text_.size() and
             std::isspace(uint8_t(this->text_[this->position_]))) this->position_++;
   }

   bool consume(char Character)
   {
      this->skip_space();
      if (this->position_ >= this->text_.size() or this->text_[this->position_] != Character) return false;
      this->position_++;
      return true;
   }

   std::string_view identifier()
   {
      this->skip_space();
      size_t start = this->position_;
      while (this->position_ < this->text_.size()) {
         char character = this->text_[this->position_];
         if (not std::isalnum(uint8_t(character)) and character != '_') break;
         this->position_++;
      }
      return this->text_.substr(start, this->position_ - start);
   }

   std::optional<ArrayElementDescriptor> parse_member(size_t Depth, std::string *Canonical)
   {
      if (Depth >= MAX_DEPTH) return std::nullopt;
      std::string_view name = this->identifier();
      if (name.empty()) return std::nullopt;

      if (name IS "array") {
         ArrayElementDescriptor result { AET::ARRAY, TiriType::Array, CLASSID::NIL, nullptr, true };
         this->skip_space();
         if (this->position_ >= this->text_.size() or this->text_[this->position_] != '<') {
            if (Canonical) *Canonical = "array";
            return result;
         }
         this->position_++;
         std::string member_name;
         auto member = this->parse_member(Depth + 1, &member_name);
         if (not member or member->storage IS AET::PTR or
             (this->state_ and member->storage IS AET::STRUCT and not member->struct_def) or
             not this->consume('>')) {
            return std::nullopt;
         }
         std::string identity = std::format("array<{}>", member_name);
         if (this->lexer_) result.nested_array_identity = this->lexer_->keepstr(identity);
         else if (this->state_) {
            result.nested_array_identity = lj_str_new(this->state_, identity.data(), identity.size());
         }
         if (Canonical) *Canonical = identity;
         return result;
      }

      if (name IS "struct") {
         if (not this->consume('<')) {
            if (Canonical) *Canonical = "struct";
            return describe_array_element("struct", this->state_);
         }
         std::string_view structure_name = this->identifier();
         if (structure_name.empty() or not this->consume('>')) return std::nullopt;
         if (Canonical) *Canonical = std::format("struct<{}>", structure_name);
         auto result = describe_array_element(std::format("struct<{}>", structure_name), this->state_);
         if (this->state_ and result and not result->struct_def) return std::nullopt;
         return result;
      }

      auto result = describe_array_element(name, this->state_);
      if (result and result->storage IS AET::PTR) return std::nullopt;
      if (result and Canonical) *Canonical = array_element_name(*result);
      return result;
   }

   std::string_view text_;
   lua_State *state_ = nullptr;
   LexState *lexer_ = nullptr;
   size_t position_ = 0;
};

} // namespace

std::optional<ArrayElementDescriptor> parse_array_element_type(
   std::string_view Name, lua_State *State, LexState *Lexer)
{
   return RecursiveArrayTypeParser(Name, State, Lexer).parse_element();
}

std::optional<std::string> canonical_array_type_name(std::string_view Name, lua_State *State)
{
   return RecursiveArrayTypeParser(Name, State, nullptr).canonical_name();
}

StaticValueDescriptor StaticResultSet::value_at(size_t Position) const
{
   if (Position < this->stored_count) return this->values[Position];
   if (this->variadic and this->stored_count > 0) return this->values[this->stored_count - 1];

   StaticValueDescriptor result;
   if (not this->dynamic and Position >= this->declared_count) {
      result.primary = TiriType::Nil;
      result.nullable = true;
      result.proof = StaticProof::Closed;
   }
   else {
      result.primary = TiriType::Any;
      result.nullable = true;
      result.proof = StaticProof::Advisory;
   }
   return result;
}

StaticDescriptorCatalogue::StaticDescriptorCatalogue()
{
   this->values_.push_back(StaticValueDescriptor{});
   this->result_sets_.push_back(StaticResultSet{});
   this->callables_.push_back(StaticCallableDescriptor{});
   this->bindings_.push_back(StaticBindingDescriptor{});
}

StaticValueHandle StaticDescriptorCatalogue::add_value(const StaticValueDescriptor &Value)
{
   for (StaticValueHandle i(1); i < this->values_.size(); ++i) {
      if (this->values_[i] IS Value) return i;
   }
   this->values_.push_back(Value);
   return StaticValueHandle(this->values_.size() - 1);
}

StaticResultSetHandle StaticDescriptorCatalogue::add_results(const StaticResultSet &Results)
{
   this->result_sets_.push_back(Results);
   return StaticResultSetHandle(this->result_sets_.size() - 1);
}

StaticCallableHandle StaticDescriptorCatalogue::add_callable(const StaticCallableDescriptor &Callable)
{
   this->callables_.push_back(Callable);
   return StaticCallableHandle(this->callables_.size() - 1);
}

StaticBindingID StaticDescriptorCatalogue::add_binding(const StaticBindingDescriptor &Binding)
{
   this->bindings_.push_back(Binding);
   return StaticBindingID(this->bindings_.size() - 1);
}

const StaticValueDescriptor & StaticDescriptorCatalogue::value(StaticValueHandle Handle) const
{
   if (Handle.raw() >= this->values_.size()) std::abort();
   return this->values_[Handle.raw()];
}

const StaticResultSet & StaticDescriptorCatalogue::results(StaticResultSetHandle Handle) const
{
   if (Handle.raw() >= this->result_sets_.size()) std::abort();
   return this->result_sets_[Handle.raw()];
}

const StaticCallableDescriptor & StaticDescriptorCatalogue::callable(StaticCallableHandle Handle) const
{
   if (Handle.raw() >= this->callables_.size()) std::abort();
   return this->callables_[Handle.raw()];
}

StaticCallableDescriptor & StaticDescriptorCatalogue::callable(StaticCallableHandle Handle)
{
   if (Handle.raw() >= this->callables_.size()) std::abort();
   return this->callables_[Handle.raw()];
}

const StaticBindingDescriptor & StaticDescriptorCatalogue::binding(StaticBindingID ID) const
{
   if (ID.raw() >= this->bindings_.size()) std::abort();
   return this->bindings_[ID.raw()];
}

StaticBindingDescriptor & StaticDescriptorCatalogue::binding(StaticBindingID ID)
{
   if (ID.raw() >= this->bindings_.size()) std::abort();
   return this->bindings_[ID.raw()];
}

void StaticDescriptorCatalogue::clear_analysis()
{
   this->values_.resize(1);
   this->result_sets_.resize(1);
   this->callables_.resize(1);
   this->bindings_.resize(1);
}

static StaticProof weaker_proof(StaticProof Left, StaticProof Right)
{
   auto rank = [](StaticProof Proof) {
      switch (Proof) {
         case StaticProof::Advisory: return 0;
         case StaticProof::Closed:   return 3;
         case StaticProof::Checked:  return 2;
         case StaticProof::Trusted:  return 2;
      }
      return 0;
   };

   return rank(Left) <= rank(Right) ? Left : Right;
}

StaticValueDescriptor join_static_descriptors(
   const StaticValueDescriptor &Left, const StaticValueDescriptor &Right)
{
   StaticValueDescriptor result;
   result.nullable = Left.nullable or Right.nullable;

   if (Left.primary != Right.primary) {
      if (Left.primary IS TiriType::Nil) {
         result = Right;
         result.nullable = true;
         result.proof = weaker_proof(Left.proof, Right.proof);
         return result;
      }
      if (Right.primary IS TiriType::Nil) {
         result = Left;
         result.nullable = true;
         result.proof = weaker_proof(Left.proof, Right.proof);
         return result;
      }
      result.primary = TiriType::Any;
      result.proof = StaticProof::Advisory;
      return result;
   }

   result = Left;
   result.nullable = Left.nullable or Right.nullable;
   result.proof = weaker_proof(Left.proof, Right.proof);
   if (Left.object_class_id != Right.object_class_id) result.object_class_id = CLASSID::NIL;
   if (Left.struct_def != Right.struct_def) result.struct_def = nullptr;
   if (Left.module != Right.module) result.module = nullptr;
   if (Left.contextuality != Right.contextuality) result.contextuality = StaticContextuality::Unknown;
   if (Left.primary IS TiriType::Array and not (Left.array_element IS Right.array_element)) {
      if (Left.array_element.known and Right.array_element.known and
          (Left.array_element.storage IS AET::ANY or Right.array_element.storage IS AET::ANY)) {
         result.array_element = { AET::ANY, TiriType::Any, CLASSID::NIL, nullptr, true };
      }
      else {
         result.primary = TiriType::Any;
         result.array_element = {};
         result.proof = StaticProof::Advisory;
      }
   }
   return result;
}

StaticValueDescriptor describe_arithmetic_result(
   const StaticValueDescriptor &Left, const StaticValueDescriptor &Right)
{
   StaticValueDescriptor result;
   if (Left.primary IS TiriType::Num and Right.primary IS TiriType::Num and Left.proved() and Right.proved() and
       not Left.nullable and not Right.nullable) {
      result.primary = TiriType::Num;
      result.proof = StaticProof::Closed;
   }
   else {
      result.primary = TiriType::Num;
      result.proof = StaticProof::Advisory;
      result.nullable = Left.nullable or Right.nullable;
   }
   return result;
}

StaticValueDescriptor describe_unary_numeric_result(const StaticValueDescriptor &Operand)
{
   StaticValueDescriptor result;
   result.primary = TiriType::Num;
   if (Operand.primary IS TiriType::Num and Operand.proved() and not Operand.nullable) {
      result.proof = StaticProof::Closed;
   }
   else {
      result.proof = StaticProof::Advisory;
      result.nullable = Operand.nullable;
   }
   return result;
}

StaticValueDescriptor describe_length_result(const StaticValueDescriptor &Operand)
{
   StaticValueDescriptor result;
   result.primary = TiriType::Num;
   result.proof = StaticProof::Advisory;
   result.nullable = Operand.primary IS TiriType::Table or Operand.primary IS TiriType::Any or
      Operand.primary IS TiriType::Unknown;
   return result;
}

bool static_value_satisfies_contract(
   const StaticValueDescriptor &Value, const RuntimeContract &Contract)
{
   if (Contract.type IS TiriType::Unknown or Contract.type IS TiriType::Any) {
      if (not Contract.required) return true;
      return Value.proved() and Value.primary != TiriType::Nil and not Value.nullable;
   }
   if (not Value.proved()) return false;

   if (Value.primary IS TiriType::Nil) return Contract.nullable and not Contract.required;
   if (Value.nullable and (Contract.required or not Contract.nullable)) return false;
   if (Value.primary != Contract.type) return false;

   switch (Contract.type) {
      case TiriType::Object:
         return Contract.object_class_id IS CLASSID::NIL or
            Value.object_class_id IS Contract.object_class_id;
      case TiriType::Struct:
         return not Contract.struct_def or Value.struct_def IS Contract.struct_def;
      case TiriType::Array:
         return array_element_matches(Contract.array_element, Value.array_element);
      default:
         return true;
   }
}

StaticResultSet map_static_result_filter(
   const StaticResultSet &Source, uint64_t KeepMask, uint8_t ExplicitCount, bool TrailingKeep)
{
   StaticResultSet result;
   size_t output = 0;
   size_t source_limit = Source.dynamic or Source.variadic ?
      MAX_RETURN_TYPES : std::min<size_t>(Source.declared_count, MAX_RETURN_TYPES);

   for (size_t source = 0; source < source_limit and output < MAX_RETURN_TYPES; ++source) {
      bool keep = source < ExplicitCount ? ((KeepMask & (uint64_t(1) << source)) != 0) : TrailingKeep;
      if (not keep) continue;
      result.values[output++] = Source.value_at(source);
   }

   result.stored_count = uint8_t(output);
   result.declared_count = uint16_t(output);
   result.variadic = TrailingKeep and Source.variadic;
   result.dynamic = TrailingKeep and Source.dynamic;
   if (result.variadic or result.dynamic) result.declared_count = Source.declared_count;
   return result;
}

ObjectCallMemberKind classify_object_call_member(std::string_view Name)
{
   if (Name.size() < 3 or Name[2] < 'A' or Name[2] > 'Z') return ObjectCallMemberKind::None;
   if (Name.starts_with("ac")) return ObjectCallMemberKind::Action;
   if (Name.starts_with("mt")) return ObjectCallMemberKind::Method;
   return ObjectCallMemberKind::None;
}

StaticResultSet describe_native_prototype_results(const fprototype *Prototype)
{
   StaticResultSet result;
   if (not Prototype) return result;

   result.declared_count = Prototype->result_count;
   result.stored_count = uint8_t(std::min<size_t>(Prototype->result_count, MAX_RETURN_TYPES));
   for (size_t i = 0; i < result.stored_count; ++i) {
      StaticValueDescriptor &value = result.values[i];
      value.primary = Prototype->result_types[i];
      value.nullable = true;
      if (value.primary IS TiriType::Unknown) value.primary = TiriType::Any;
      value.proof = value.primary IS TiriType::Any ? StaticProof::Advisory : StaticProof::Trusted;
   }
   return result;
}

StaticResultSet describe_object_call_results(const FunctionField *Fields)
{
   StaticResultSet result;

   auto append = [&result](StaticValueDescriptor Value) {
      if (result.declared_count < MAX_RETURN_TYPES) {
         result.values[result.declared_count] = Value;
         result.stored_count++;
      }
      result.declared_count++;
   };

   append(StaticValueDescriptor{
      .primary = TiriType::Num,
      .proof = StaticProof::Trusted
   });

   if (not Fields) return result;

   for (size_t i = 0; Fields[i].Name; ++i) {
      uint32_t type = Fields[i].Type;
      StaticValueDescriptor value;
      value.proof = StaticProof::Trusted;

      if ((type & FDF_SPAN) IS FDF_SPAN) continue;
      else if ((type & FDF_VECTOR) IS FDF_VECTOR) {
         value.primary = TiriType::Array;
         value.nullable = true;
      }
      else if (type & FD_STR) {
         value.primary = TiriType::Str;
         value.nullable = not (type & FD_CPP) or (type & FD_MUTABLE);
      }
      else if (type & FD_STRUCT) {
         value.primary = (type & FD_RESOURCE) ? TiriType::Struct : TiriType::Table;
         value.nullable = true;
      }
      else if (type & FD_FUNCTION) {
         continue;
      }
      else if (type & FD_PTR) {
         value.primary = (type & FD_OBJECT) ? TiriType::Object : TiriType::Userdata;
         value.nullable = true;
      }
      else if (type & (FD_INT|FD_DOUBLE|FD_INT64)) {
         value.primary = TiriType::Num;
      }
      else if (type & FD_TAGS) {
         break;
      }
      else {
         break;
      }

      if (type & FD_RESULT) append(value);
   }

   return result;
}

StaticResultSet describe_module_call_results(const FunctionField *Fields, lua_State *State)
{
   StaticResultSet result;

   auto append = [&result](StaticValueDescriptor Value) {
      if (result.declared_count < MAX_RETURN_TYPES) {
         result.values[result.declared_count] = Value;
         result.stored_count++;
      }
      result.declared_count++;
   };

   auto resolve_struct = [State](const FunctionField &Field) -> struct_record * {
      if (not State or not Field.Name or not valid_struct_name(Field.Name)) return nullptr;
      return find_struct(State, struct_name_prefix(Field.Name));
   };

   auto describe_pointer = [&resolve_struct](const FunctionField &Field) {
      StaticValueDescriptor value;
      value.proof = StaticProof::Trusted;
      value.nullable = true;
      if (Field.Type & FD_OBJECT) value.primary = TiriType::Object;
      else if (Field.Type & FD_STRUCT) {
         value.primary = (Field.Type & FD_RESOURCE) ? TiriType::Struct : TiriType::Table;
         value.struct_def = resolve_struct(Field);
      }
      else value.primary = TiriType::Userdata;
      return value;
   };

   auto describe_vector_element = [&resolve_struct](const FunctionField &Field) {
      ArrayElementDescriptor element;
      element.known = true;
      if (Field.Type & FD_STRUCT) {
         element.storage = AET::STRUCT;
         element.logical_type = TiriType::Struct;
         element.struct_def = resolve_struct(Field);
      }
      else if (Field.Type & FD_OBJECT) {
         element.storage = AET::OBJECT;
         element.logical_type = TiriType::Object;
      }
      else if (Field.Type & FD_STR) {
         element.storage = AET::STR_GC;
         element.logical_type = TiriType::Str;
      }
      else if (Field.Type & FD_DOUBLE) {
         element.storage = AET::DOUBLE;
         element.logical_type = TiriType::Num;
      }
      else if (Field.Type & FD_INT64) {
         element.storage = (Field.Type & FD_UNSIGNED) ? AET::UINT64 : AET::INT64;
         element.logical_type = TiriType::Num;
      }
      else if (Field.Type & FD_INT) {
         element.storage = (Field.Type & FD_UNSIGNED) ? AET::UINT32 : AET::INT32;
         element.logical_type = TiriType::Num;
      }
      else if (Field.Type & FD_WORD) {
         element.storage = (Field.Type & FD_UNSIGNED) ? AET::UINT16 : AET::INT16;
         element.logical_type = TiriType::Num;
      }
      else if (Field.Type & FD_BYTE) {
         element.storage = AET::BYTE;
         element.logical_type = TiriType::Num;
      }
      else if (Field.Type & FD_PTR) {
         element.storage = AET::PTR;
         element.logical_type = TiriType::Userdata;
      }
      else element = {};
      return element;
   };

   if (not Fields or not Fields[0].Name) return result;

   const FunctionField &direct = Fields[0];
   if (direct.Type & FD_STR) {
      append(StaticValueDescriptor{
         .primary = TiriType::Str,
         .proof = StaticProof::Trusted,
         .nullable = true
      });
   }
   else if (direct.Type & FD_OBJECT) {
      append(StaticValueDescriptor{
         .primary = TiriType::Object,
         .proof = StaticProof::Trusted,
         .nullable = true
      });
   }
   else if (direct.Type & FD_PTR) {
      StaticValueDescriptor value = describe_pointer(direct);
      if ((direct.Type & FD_STRUCT) and not (direct.Type & FD_RESOURCE) and not value.struct_def) {
         value.primary = TiriType::Userdata;
      }
      append(value);
   }
   else if (direct.Type & (FD_INT|FD_ERROR|FD_DOUBLE|FD_INT64)) {
      append(StaticValueDescriptor{
         .primary = TiriType::Num,
         .proof = StaticProof::Trusted
      });
   }

   for (size_t i = 1; Fields[i].Name; ++i) {
      const FunctionField &field = Fields[i];
      uint32_t type = field.Type;
      if (not (type & FD_RESULT)) continue;

      StaticValueDescriptor value;
      value.proof = StaticProof::Trusted;
      value.nullable = true;

      if ((type & FDF_SPAN) IS FDF_SPAN) continue;
      if ((type & FDF_VECTOR) IS FDF_VECTOR) {
         value.primary = TiriType::Array;
         value.array_element = describe_vector_element(field);
         append(value);
      }
      else if (type & FD_STR) {
         value.primary = TiriType::Str;
         append(value);
      }
      else if (type & (FD_PTR|FD_STRUCT)) {
         if ((type & FD_PTR) and not (type & (FD_OBJECT|FD_STRUCT)) and (type & FD_ALLOC)) {
            value.primary = TiriType::Nil;
         }
         else value = describe_pointer(field);
         append(value);
      }
      else if (type & (FD_INT|FD_DOUBLE|FD_INT64)) {
         value.primary = TiriType::Num;
         append(value);
      }
      else break;
   }

   return result;
}

StaticValueDescriptor describe_struct_field(const struct_record *StructDef, GCstr *FieldName)
{
   StaticValueDescriptor result;
   if (not StructDef or not FieldName) return result;

   for (const auto &field : StructDef->Fields) {
      if (field.nameHash() != kt::strihash(strdata(FieldName))) continue;
      result.proof = StaticProof::Trusted;
      if (field.Type & (FD_ARRAY|FD_VECTOR)) {
         result.primary = TiriType::Array;
         if (auto element = describe_array_element(field)) result.array_element = *element;
      }
      else if ((field.Type & FD_STRUCT) and not (field.Type & FD_POINTER)) {
         result.primary = TiriType::Struct;
         result.struct_def = field.StructDefinition;
      }
      else if (field.Type & FD_OBJECT) {
         result.primary = TiriType::Object;
         result.object_class_id = field.ObjectClassID;
      }
      else if (field.Type & FD_STRING) result.primary = TiriType::Str;
      else if (field.NativeType IS NativeStructType::Bool) result.primary = TiriType::Bool;
      else if (field.Type & (FD_FLOAT|FD_DOUBLE|FD_INT64|FD_INT|FD_WORD|FD_BYTE)) {
         result.primary = TiriType::Num;
      }
      else if (field.Type & FD_FUNCTION) result.primary = TiriType::Func;
      else result.primary = TiriType::Any;
      return result;
   }
   return result;
}

std::optional<ArrayElementDescriptor> describe_array_element(std::string_view Name, lua_State *State)
{
   ArrayElementDescriptor result;
   result.known = true;

   if (Name IS "byte" or Name IS "char") {
      result = { AET::BYTE, TiriType::Num, CLASSID::NIL, nullptr, true };
   }
   else if (Name IS "int8") result = { AET::INT8, TiriType::Num, CLASSID::NIL, nullptr, true };
   else if (Name IS "int16") result = { AET::INT16, TiriType::Num, CLASSID::NIL, nullptr, true };
   else if (Name IS "int") result = { AET::INT32, TiriType::Num, CLASSID::NIL, nullptr, true };
   else if (Name IS "int64") result = { AET::INT64, TiriType::Num, CLASSID::NIL, nullptr, true };
   else if (Name IS "uint8") result = { AET::UINT8, TiriType::Num, CLASSID::NIL, nullptr, true };
   else if (Name IS "uint16") result = { AET::UINT16, TiriType::Num, CLASSID::NIL, nullptr, true };
   else if (Name IS "uint") result = { AET::UINT32, TiriType::Num, CLASSID::NIL, nullptr, true };
   else if (Name IS "uint64") result = { AET::UINT64, TiriType::Num, CLASSID::NIL, nullptr, true };
   else if (Name IS "float") result = { AET::FLOAT, TiriType::Num, CLASSID::NIL, nullptr, true };
   else if (Name IS "double") result = { AET::DOUBLE, TiriType::Num, CLASSID::NIL, nullptr, true };
   else if (Name IS "str") result = { AET::STR_GC, TiriType::Str, CLASSID::NIL, nullptr, true };
   else if (Name IS "table") result = { AET::TABLE, TiriType::Table, CLASSID::NIL, nullptr, true };
   else if (Name IS "array") result = { AET::ARRAY, TiriType::Array, CLASSID::NIL, nullptr, true };
   else if (Name IS "obj") result = { AET::OBJECT, TiriType::Object, CLASSID::NIL, nullptr, true };
   else if (Name IS "any") result = { AET::ANY, TiriType::Any, CLASSID::NIL, nullptr, true };
   else if (Name IS "pointer") result = { AET::PTR, TiriType::Any, CLASSID::NIL, nullptr, true };
   else if (Name IS "struct") result = { AET::STRUCT, TiriType::Struct, CLASSID::NIL, nullptr, true };
   else if (Name.starts_with("struct<") and Name.ends_with('>') and Name.size() > 8) {
      result = { AET::STRUCT, TiriType::Struct, CLASSID::NIL, nullptr, true };
      if (State) result.struct_def = find_struct(State, Name.substr(7, Name.size() - 8));
   }
   else return std::nullopt;

   return result;
}

std::optional<ArrayElementDescriptor> describe_array_element(const struct_field &Field)
{
   if (not (Field.Type & (FD_ARRAY|FD_VECTOR))) return std::nullopt;

   ArrayElementDescriptor result;
   result.known = true;
   result.logical_type = TiriType::Num;

   if ((Field.Type & FD_STRUCT) and not (Field.Type & FD_POINTER)) {
      result.storage = AET::STRUCT;
      result.logical_type = TiriType::Struct;
      result.struct_def = Field.StructDefinition;
   }
   else if (Field.Type & FD_FLOAT) result.storage = AET::FLOAT;
   else if (Field.Type & FD_DOUBLE) result.storage = AET::DOUBLE;
   else if (Field.Type & FD_INT64) result.storage = (Field.Type & FD_UNSIGNED) ? AET::UINT64 : AET::INT64;
   else if (Field.Type & FD_INT) result.storage = (Field.Type & FD_UNSIGNED) ? AET::UINT32 : AET::INT32;
   else if (Field.Type & FD_WORD) result.storage = (Field.Type & FD_UNSIGNED) ? AET::UINT16 : AET::INT16;
   else if (Field.Type & FD_BYTE) {
      result.storage = Field.NativeType IS NativeStructType::Int8 ? AET::INT8 : AET::BYTE;
   }
   else return std::nullopt;

   return result;
}

bool can_use_static_receiver(
   const StaticDescriptorCatalogue &Catalogue, StaticValueHandle Handle, TiriType Type, bool AllowNullable)
{
   if (not Handle) return false;
   const auto &descriptor = Catalogue.value(Handle);
   if (descriptor.primary != Type or not descriptor.proved() or (not AllowNullable and descriptor.nullable)) {
      return false;
   }
   if (Type IS TiriType::Object) return descriptor.object_class_id != CLASSID::NIL;
   return true;
}

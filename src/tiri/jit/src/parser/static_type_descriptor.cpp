// Parse-time descriptor helpers.

#include "static_type_descriptor.h"

#include <algorithm>

#include "ast/nodes.h"
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
   for (StaticValueHandle i = 1; i < this->values_.size(); ++i) {
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
   return this->values_[Handle < this->values_.size() ? Handle : 0];
}

const StaticResultSet & StaticDescriptorCatalogue::results(StaticResultSetHandle Handle) const
{
   return this->result_sets_[Handle < this->result_sets_.size() ? Handle : 0];
}

const StaticCallableDescriptor & StaticDescriptorCatalogue::callable(StaticCallableHandle Handle) const
{
   return this->callables_[Handle < this->callables_.size() ? Handle : 0];
}

StaticCallableDescriptor & StaticDescriptorCatalogue::callable(StaticCallableHandle Handle)
{
   return this->callables_[Handle < this->callables_.size() ? Handle : 0];
}

const StaticBindingDescriptor & StaticDescriptorCatalogue::binding(StaticBindingID ID) const
{
   return this->bindings_[ID < this->bindings_.size() ? ID : 0];
}

StaticBindingDescriptor & StaticDescriptorCatalogue::binding(StaticBindingID ID)
{
   return this->bindings_[ID < this->bindings_.size() ? ID : 0];
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
   if (not (Left.array_element IS Right.array_element)) result.array_element = {};
   return result;
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

std::optional<ArrayElementDescriptor> describe_array_element(std::string_view Name, lua_State *State)
{
   ArrayElementDescriptor result;
   result.known = true;

   if (Name IS "byte" or Name IS "char") result = { AET::BYTE, TiriType::Num, CLASSID::NIL, nullptr, true };
   else if (Name IS "int16") result = { AET::INT16, TiriType::Num, CLASSID::NIL, nullptr, true };
   else if (Name IS "int") result = { AET::INT32, TiriType::Num, CLASSID::NIL, nullptr, true };
   else if (Name IS "int64") result = { AET::INT64, TiriType::Num, CLASSID::NIL, nullptr, true };
   else if (Name IS "float") result = { AET::FLOAT, TiriType::Num, CLASSID::NIL, nullptr, true };
   else if (Name IS "double") result = { AET::DOUBLE, TiriType::Num, CLASSID::NIL, nullptr, true };
   else if (Name IS "string") result = { AET::STR_GC, TiriType::Str, CLASSID::NIL, nullptr, true };
   else if (Name IS "table") result = { AET::TABLE, TiriType::Table, CLASSID::NIL, nullptr, true };
   else if (Name IS "array") result = { AET::ARRAY, TiriType::Array, CLASSID::NIL, nullptr, true };
   else if (Name IS "object") result = { AET::OBJECT, TiriType::Object, CLASSID::NIL, nullptr, true };
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
   else if (Field.Type & FD_INT64) result.storage = AET::INT64;
   else if (Field.Type & FD_INT) result.storage = AET::INT32;
   else if (Field.Type & FD_WORD) result.storage = AET::INT16;
   else if (Field.Type & FD_BYTE) result.storage = AET::BYTE;
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

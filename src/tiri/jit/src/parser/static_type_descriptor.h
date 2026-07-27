// Parse-time value, result and callable descriptors for type-guided bytecode emission.
//
// These records never survive parsing.  Persistent prototype signatures continue to use ProtoTypeEntry.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "../runtime/lj_obj.h"

struct ExprNode;
struct FunctionField;
struct FunctionExprPayload;
struct struct_field;
struct struct_record;

using StaticValueHandle = uint32_t;
using StaticResultSetHandle = uint32_t;
using StaticCallableHandle = uint32_t;
using StaticBindingID = uint32_t;
struct static_module_signature;
using StaticModuleHandle = const static_module_signature *;

enum class StaticProof : uint8_t {
   Advisory,
   Closed,
   Checked,
   Trusted
};

struct ArrayElementDescriptor {
   AET storage = AET::ANY;
   TiriType logical_type = TiriType::Any;
   CLASSID object_class_id = CLASSID::NIL;
   struct_record *struct_def = nullptr;
   bool known = false;

   [[nodiscard]] bool operator==(const ArrayElementDescriptor &) const = default;
};

struct StaticValueDescriptor {
   TiriType primary = TiriType::Unknown;
   CLASSID object_class_id = CLASSID::NIL;
   struct_record *struct_def = nullptr;
   StaticModuleHandle module = nullptr;
   ArrayElementDescriptor array_element{};
   StaticProof proof = StaticProof::Advisory;
   bool nullable = false;

   [[nodiscard]] bool operator==(const StaticValueDescriptor &) const = default;
   [[nodiscard]] bool constrained() const noexcept;
   [[nodiscard]] bool proved() const noexcept;
};

struct StaticResultSet {
   std::array<StaticValueDescriptor, MAX_RETURN_TYPES> values{};
   uint16_t declared_count = 0;
   uint8_t stored_count = 0;
   bool variadic = false;
   bool dynamic = false;

   [[nodiscard]] StaticValueDescriptor value_at(size_t Position) const;
};

enum class StaticCallableSource : uint8_t {
   TiriFunction,
   NativePrototype,
   Intrinsic
};

struct StaticCallableDescriptor {
   const FunctionExprPayload *function = nullptr;
   StaticResultSetHandle results = 0;
   StaticBindingID binding_id = 0;
   StaticCallableSource source = StaticCallableSource::TiriFunction;
   bool immutable = false;
};

struct StaticBindingDescriptor {
   GCstr *name = nullptr;
   const ExprNode *initialiser = nullptr;
   const FunctionExprPayload *function = nullptr;
   StaticValueHandle value = 0;
   StaticCallableHandle callable = 0;
   StaticBindingID alias_of = 0;
   uint8_t result_position = 0;
   uint16_t function_depth = 0;
   bool immutable = true;
   bool is_const = false;
   bool is_parameter = false;
   bool captured = false;
   bool resolving = false;
};

class StaticDescriptorCatalogue {
public:
   StaticDescriptorCatalogue();

   [[nodiscard]] StaticValueHandle add_value(const StaticValueDescriptor &);
   [[nodiscard]] StaticResultSetHandle add_results(const StaticResultSet &);
   [[nodiscard]] StaticCallableHandle add_callable(const StaticCallableDescriptor &);
   [[nodiscard]] StaticBindingID add_binding(const StaticBindingDescriptor &);

   [[nodiscard]] const StaticValueDescriptor & value(StaticValueHandle) const;
   [[nodiscard]] const StaticResultSet & results(StaticResultSetHandle) const;
   [[nodiscard]] const StaticCallableDescriptor & callable(StaticCallableHandle) const;
   [[nodiscard]] StaticCallableDescriptor & callable(StaticCallableHandle);
   [[nodiscard]] const StaticBindingDescriptor & binding(StaticBindingID) const;
   [[nodiscard]] StaticBindingDescriptor & binding(StaticBindingID);
   [[nodiscard]] size_t binding_count() const noexcept { return this->bindings_.size(); }

   void clear_analysis();

private:
   std::vector<StaticValueDescriptor> values_;
   std::vector<StaticResultSet> result_sets_;
   std::vector<StaticCallableDescriptor> callables_;
   std::vector<StaticBindingDescriptor> bindings_;
};

[[nodiscard]] StaticValueDescriptor join_static_descriptors(
   const StaticValueDescriptor &, const StaticValueDescriptor &);
[[nodiscard]] StaticResultSet map_static_result_filter(
   const StaticResultSet &, uint64_t KeepMask, uint8_t ExplicitCount, bool TrailingKeep);
enum class ObjectCallMemberKind : uint8_t {
   None,
   Action,
   Method
};
[[nodiscard]] ObjectCallMemberKind classify_object_call_member(std::string_view);
[[nodiscard]] StaticResultSet describe_native_prototype_results(const fprototype *);
[[nodiscard]] StaticResultSet describe_object_call_results(const FunctionField *);
[[nodiscard]] StaticResultSet describe_module_call_results(
   const FunctionField *, lua_State *State = nullptr);
[[nodiscard]] StaticValueDescriptor describe_struct_field(const struct_record *, GCstr *);
[[nodiscard]] std::optional<ArrayElementDescriptor> describe_array_element(
   std::string_view Name, lua_State *State = nullptr);
[[nodiscard]] std::optional<ArrayElementDescriptor> describe_array_element(const struct_field &);
[[nodiscard]] bool can_use_static_receiver(
   const StaticDescriptorCatalogue &, StaticValueHandle, TiriType, bool AllowNullable = false);

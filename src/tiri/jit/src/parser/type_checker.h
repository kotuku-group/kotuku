#pragma once

#include <optional>
#include <string>
#include <vector>

#include "parser_diagnostics.h"
#include "ast/nodes.h"

struct InferredType {
   TiriType primary = TiriType::Any;
   bool is_constant = false;
   bool is_nullable = false;
   bool is_fixed = false;  // Type is locked, cannot change
   bool requires_destination_type = false; // Dynamic ingress cannot be replaced by a concrete type without annotation
   CLASSID object_class_id = CLASSID::NIL;  // CLASSID for Object types
   struct_record *struct_def = nullptr; // Resolved layout for Struct types and definition callables

   InferredType() = default;
   explicit InferredType(TiriType Primary, bool IsConstant = false, bool IsNullable = false, bool IsFixed = false,
      CLASSID ObjectClassId = CLASSID::NIL)
      : primary(Primary), is_constant(IsConstant), is_nullable(IsNullable), is_fixed(IsFixed),
        object_class_id(ObjectClassId) {}

   [[nodiscard]] bool matches(TiriType Expected) const
   {
      if (Expected IS TiriType::Any) return true;
      if (this->primary IS TiriType::Any) return true;
      if (this->primary IS TiriType::Nil) return true;  // nil matches any type (represents "no value")
      if (Expected IS TiriType::Func and this->primary IS TiriType::Table) {
         return true;  // Runtime func contracts accept tables whose metatable provides __call.
      }
      return this->primary IS Expected;
   }
};

struct TypeDiagnostic {
   SourceSpan location{};
   std::string message;
   TiriType expected = TiriType::Any;
   TiriType actual = TiriType::Any;
   ParserErrorCode code = ParserErrorCode::TypeMismatchArgument;
   uint8_t file_index = 0;  // FileSource index of the file the diagnostic belongs to

   TypeDiagnostic() = default;
   TypeDiagnostic(SourceSpan Location, std::string Message, TiriType Expected, TiriType Actual,
      ParserErrorCode Code = ParserErrorCode::TypeMismatchArgument)
      : location(Location), message(std::move(Message)), expected(Expected), actual(Actual), code(Code) {}
};

// Context for tracking function return type validation during type analysis
enum class ReturnInferenceState : uint8_t {
   Unobserved,
   NilOnly,
   Concrete,
   Dynamic
};

struct InferredReturnPosition {
   ReturnInferenceState state = ReturnInferenceState::Unobserved;
   InferredType concrete{};
   SourceSpan location{};
};

struct FunctionContext {
   const FunctionExprPayload* function = nullptr;  // The function being analysed
   FunctionReturnTypes expected_returns{};         // Declared or inferred return types
   std::array<InferredReturnPosition, MAX_RETURN_TYPES> inferred_returns{};
   uint8_t observed_return_count = 0;
   bool inference_failed = false;
   GCstr *function_name = nullptr;                 // Function name (for recursive detection)

   FunctionContext() = default;
   explicit FunctionContext(const FunctionExprPayload* Function, GCstr *Name = nullptr)
      : function(Function), function_name(Name) {}
};

// Information about an unused variable for reporting
struct UnusedVariableInfo {
   GCstr* name = nullptr;
   SourceSpan location{};
   bool is_parameter = false;
   bool is_function = false;

   UnusedVariableInfo() = default;
   UnusedVariableInfo(GCstr* Name, SourceSpan Location, bool IsParameter, bool IsFunction)
      : name(Name), location(Location), is_parameter(IsParameter), is_function(IsFunction) {}
};

class TypeCheckScope {
public:
   void declare_parameter(GCstr *, TiriType Type, struct_record *StructDef, SourceSpan Location = {});
   void declare_local(GCstr *, const InferredType &, SourceSpan Location = {}, bool IsConst = false);
   void declare_function(GCstr *, const FunctionExprPayload *, SourceSpan Location = {});
   void fix_local_type(GCstr *, TiriType Type, CLASSID ObjectClassId = CLASSID::NIL,
      struct_record *StructDef = nullptr);
   void mark_dynamic_ingress(GCstr *);

   // Mark a variable as used (called when variable is referenced)
   void mark_used(GCstr *);

   [[nodiscard]] std::optional<InferredType> lookup_parameter_type(GCstr *) const;
   [[nodiscard]] std::optional<InferredType> lookup_local_type(GCstr *) const;
   [[nodiscard]] const FunctionExprPayload * lookup_function(GCstr *) const;

   // Get all unused variables in this scope (for reporting on scope exit)
   [[nodiscard]] std::vector<UnusedVariableInfo> get_unused_variables() const;

   // Check if a local variable has the <const> attribute
   [[nodiscard]] bool is_local_const(GCstr *) const;

private:
   struct VariableInfo {
      GCstr *name = nullptr;
      InferredType type{};
      SourceSpan location{};
      bool is_parameter = false;
      bool is_used = false;
      bool is_const = false;  // True if declared with <const> attribute
      const FunctionExprPayload * function = nullptr;
   };

   std::vector<VariableInfo> variables_{};
};

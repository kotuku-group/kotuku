#include "parser/type_checker.h"

void TypeCheckScope::declare_parameter(GCstr *Name, TiriType Type, SourceSpan Location)
{
   VariableInfo info;
   info.name = Name;
   info.type.primary = Type;
   info.type.is_constant = false;
   info.type.is_nullable = false;
   info.location = Location;
   info.is_parameter = true;
   info.is_used = false;
   this->variables_.push_back(info);
}

void TypeCheckScope::declare_local(GCstr *Name, const InferredType &Type, SourceSpan Location, bool IsConst)
{
   VariableInfo info;
   info.name = Name;
   info.type = Type;
   info.location = Location;
   info.is_parameter = false;
   info.is_used = false;
   info.is_const = IsConst;
   this->variables_.push_back(info);
}

void TypeCheckScope::declare_function(GCstr *Name, const FunctionExprPayload *Function, SourceSpan Location)
{
   if (not Function) return;

   VariableInfo info;
   info.name = Name;
   info.type.primary = TiriType::Func;
   info.location = Location;
   info.is_parameter = false;
   info.is_used = false;
   info.function = Function;
   this->variables_.push_back(info);
}

std::optional<TiriType> TypeCheckScope::lookup_parameter_type(GCstr *Name) const
{
   for (auto it = this->variables_.rbegin(); it != this->variables_.rend(); ++it) {
      if (it->name IS Name and it->is_parameter) return it->type.primary;
   }
   return std::nullopt;
}

std::optional<InferredType> TypeCheckScope::lookup_local_type(GCstr *Name) const
{
   for (auto it = this->variables_.rbegin(); it != this->variables_.rend(); ++it) {
      if (it->name IS Name) return it->type;
   }
   return std::nullopt;
}

const FunctionExprPayload* TypeCheckScope::lookup_function(GCstr *Name) const
{
   for (auto it = this->variables_.rbegin(); it != this->variables_.rend(); ++it) {
      if (it->name IS Name and it->function) return it->function;
   }
   return nullptr;
}

void TypeCheckScope::fix_local_type(GCstr *Name, TiriType Type, CLASSID ObjectClassId)
{
   for (auto it = this->variables_.rbegin(); it != this->variables_.rend(); ++it) {
      if (it->name IS Name and not it->is_parameter) {
         it->type.primary = Type;
         it->type.is_fixed = true;
         it->type.object_class_id = ObjectClassId;
         return;
      }
   }
}

void TypeCheckScope::mark_used(GCstr *Name)
{
   if (not Name) return;
   for (auto it = this->variables_.rbegin(); it != this->variables_.rend(); ++it) {
      if (it->name IS Name) {
         it->is_used = true;
         return;  // Mark only the innermost variable with this name
      }
   }
}

std::vector<UnusedVariableInfo> TypeCheckScope::get_unused_variables() const
{
   std::vector<UnusedVariableInfo> unused;

   for (const auto& var : this->variables_) {
      if (var.is_used) continue;
      if (not var.name) continue;

      // Skip blank identifiers (single underscore)
      if (var.name->len IS 1 and strdata(var.name)[0] IS '_') continue;

      unused.emplace_back(var.name, var.location, var.is_parameter, var.function != nullptr);
   }

   return unused;
}

bool TypeCheckScope::is_local_const(GCstr *Name) const
{
   for (auto it = this->variables_.rbegin(); it != this->variables_.rend(); ++it) {
      if (it->name IS Name) return it->is_const;
   }
   return false;
}

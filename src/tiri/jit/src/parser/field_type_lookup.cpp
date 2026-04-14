// Compile-time field type lookup for object field accesses.
//
// Copyright (C) 2026 Paul Manias

#include "field_type_lookup.h"
#include <kotuku/main.h>
#include <kotuku/strings.hpp>
#include <span>
#include <cctype>
#include <algorithm>

// Map Kōtuku FD_* field flags to TiriType.
// Reference: lib_object.cpp shows the FD_* flag checking pattern.

static TiriType map_field_flags_to_tiri_type(uint32_t Flags)
{
   // NB: Order is important

   if (Flags & FD_ARRAY) return TiriType::Table; // TODO: Using TiriType::Array crashes - requires further investigation
   else if (Flags & FD_STRUCT) return TiriType::Table;
   else if (Flags & FD_STRING) return TiriType::Str;
   else if (Flags & (FD_OBJECT|FD_LOCAL)) return TiriType::Object;
   else if (Flags & FD_POINTER) return TiriType::Any; // Prefer runtime resolution for pointer types
   else if (Flags & (FD_DOUBLE|FD_FLOAT|FD_INT64|FD_INT)) return TiriType::Num;
   else if (Flags & FD_FUNCTION) return TiriType::Func;

   return TiriType::Any; // Prefer runtime resolution for unknown types
}

// If class not found, returns std::nullopt.
// If field not found, returns type TiriType::Unknown.

static std::optional<FieldTypeInfo> lookup_field_type(CLASSID ClassID, uint32_t FieldID)
{
   if (ClassID IS CLASSID::NIL) return std::nullopt;

   auto *meta_class = FindClass(ClassID);
   if (not meta_class) { // This should never happen - caller probably used an uninitialised variable
      pf::Log(__FUNCTION__).warning("Class ID $%.8x is invalid.", uint32_t(ClassID));
      return std::nullopt;
   }

   objMetaClass *src_class;
   Field *field;
   if (meta_class->findField(FieldID, &field, &src_class) IS ERR::Okay) {
      TiriType type = map_field_flags_to_tiri_type(field->Flags);
      // For object fields, try to extract the class ID from the Arg field

      CLASSID object_class_id = CLASSID::NIL;
      if ((field->Flags & (FD_OBJECT|FD_LOCAL)) and (field->Arg)) {
         object_class_id = CLASSID(field->Arg);
      }

      return FieldTypeInfo { type, object_class_id };
   }
   else { // Field not in dictionary - return Unknown type to signal error
      return FieldTypeInfo { TiriType::Unknown, CLASSID::NIL };
   }
}

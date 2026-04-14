// Compile-time field type lookup for object field accesses.
//
// This module provides field type resolution for Kōtuku object fields during parsing,
// enabling type mismatch detection at compile time rather than runtime.
//
// Copyright (C) 2025-2026 Paul Manias

#pragma once

#include "../runtime/lj_obj.h"
#include <kotuku/main.h>
#include <optional>
#include <string_view>

// Information about a field's type, returned from lookup_field_type().

struct FieldTypeInfo {
   TiriType type = TiriType::Str;            // The field's Tiri type (defaults to Str for unknown fields)
   CLASSID object_class_id = CLASSID::NIL;     // If type is Object, the CLASSID of the object class
};

// Look up the Tiri type of an object field at parse time.

[[nodiscard]] [[maybe_unused]] static std::optional<FieldTypeInfo> lookup_field_type(CLASSID, uint32_t);

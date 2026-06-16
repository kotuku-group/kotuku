/*********************************************************************************************************************

The source code for Kōtuku is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

-CATEGORY-
Name: Fields
-END-

*********************************************************************************************************************/

#include "defs.h"
#include <kotuku/main.h>
#include <kotuku/strings.hpp>

#include <stdarg.h>
#include <stdlib.h>
#include <cmath>

constexpr int OP_OR        = 0;
constexpr int OP_AND       = 1;
constexpr int OP_OVERWRITE = 2;

static ERR writeval_flags(OBJECTPTR, const Field *, int, CPTR , int);
static ERR writeval_long(OBJECTPTR, const Field *, int, CPTR , int);
static ERR writeval_large(OBJECTPTR, const Field *, int, CPTR , int);
static ERR writeval_double(OBJECTPTR, const Field *, int, CPTR , int);
static ERR writeval_function(OBJECTPTR, const Field *, int, CPTR , int);
static ERR writeval_ptr(OBJECTPTR, const Field *, int, CPTR , int);
static ERR writeval_cppstr(OBJECTPTR, const Field *, int, CPTR , int);
static ERR writeval_unit(OBJECTPTR, const Field *, int, CPTR , int);

static ERR setval_large(OBJECTPTR, const Field *, int Flags, CPTR , int);
static ERR setval_pointer(OBJECTPTR, const Field *, int Flags, CPTR , int);
static ERR setval_strview(OBJECTPTR, const Field *, int Flags, CPTR , int);
static ERR setval_double(OBJECTPTR, const Field *, int Flags, CPTR , int);
static ERR setval_long(OBJECTPTR, const Field *, int Flags, CPTR , int);
static ERR setval_function(OBJECTPTR, const Field *, int Flags, CPTR , int);
static ERR set_or_write_vector(OBJECTPTR, const Field *, int Flags, CPTR , int);
static ERR set_or_write_array(OBJECTPTR, const Field *, int Flags, CPTR , int);
static ERR setval_unit(OBJECTPTR, const Field *, int Flags, CPTR , int);

//********************************************************************************************************************

[[nodiscard]] static std::string_view field_string_view(int Flags, CPTR Data) noexcept
{
   if (not Data) return {};
   else return *((std::string_view *)Data);
}

[[nodiscard]] static bool decimal_digits(std::string_view String) noexcept
{
   for (auto ch : String) {
      if ((ch < '0') or (ch > '9')) return false;
   }
   return true;
}

template <class T>
requires std::is_integral_v<T>
[[nodiscard]] static T parse_integer(std::string_view String) noexcept
{
   const auto start = String.find_first_not_of(" \n\r\t");
   if (start IS std::string_view::npos) return 0;
   String.remove_prefix(start);

   bool negative = false;
   if (String.starts_with('-')) {
      negative = true;
      String.remove_prefix(1);
   }
   else if (String.starts_with('+')) String.remove_prefix(1);

   int base = 10;
   if (String.starts_with("0x") or String.starts_with("0X")) {
      base = 16;
      String.remove_prefix(2);
   }
   else if ((String.size() > 1) and (String.front() IS '0')) base = 8;

   uint64_t magnitude = 0;
   auto [ end, error ] = std::from_chars(String.data(), String.data() + String.size(), magnitude, base);
   if (error != std::errc()) return 0;

   if (negative) return T(-int64_t(magnitude));
   else return T(magnitude);
}

[[nodiscard]] static double parse_double(std::string_view String, size_t *End = nullptr) noexcept
{
   const auto original_size = String.size();
   const auto start = String.find_first_not_of(" \n\r\t");
   if (start IS std::string_view::npos) {
      if (End) *End = original_size;
      return 0;
   }

   String.remove_prefix(start);
   if (String.starts_with('+')) String.remove_prefix(1);

   double value = 0;
   auto [ end, error ] = std::from_chars(String.data(), String.data() + String.size(), value);
   if (error != std::errc()) {
      if (End) *End = original_size - String.size();
      return 0;
   }

   if (End) *End = original_size - String.size() + size_t(end - String.data());
   return value;
}

//********************************************************************************************************************
// Used by some of the SetField() range of instructions.

ERR writeval_default(OBJECTPTR Object, const Field *Field, int flags, CPTR Data, int Elements)
{
   kt::Log log("WriteField");

   //log.trace("[%s:%d] Name: %s, SetValue: %c, FieldFlags: $%.8x, SrcFlags: $%.8x", Object->className(), Object->UID, Field->Name, Field->SetValue ? 'Y' : 'N', Field->Flags, flags);

   if (not flags) flags = Field->Flags;

   if (not Field->SetValue) {
      ERR error = ERR::Okay;
      if (Field->Flags & FD_ARRAY) {
         if (Field->Flags & FD_CPP) error = set_or_write_vector(Object, Field, flags, Data, Elements);
         else error = set_or_write_array(Object, Field, flags, Data, Elements);
      }
      else if (Field->Flags & FD_INT)      error = writeval_long(Object, Field, flags, Data, 0);
      else if (Field->Flags & FD_INT64)    error = writeval_large(Object, Field, flags, Data, 0);
      else if (Field->Flags & (FD_DOUBLE|FD_FLOAT)) error = writeval_double(Object, Field, flags, Data, 0);
      else if (Field->Flags & FD_FUNCTION) error = writeval_function(Object, Field, flags, Data, 0);
      else if (Field->Flags & FD_STRING)   error = writeval_cppstr(Object, Field, flags, Data, 0);
      else if (Field->Flags & FD_POINTER)  error = writeval_ptr(Object, Field, flags, Data, 0);
      else if (Field->Flags & FD_UNIT)     error = writeval_unit(Object, Field, flags, Data, 0);
      else log.warning("Unrecognised field flags $%.8x.", Field->Flags);

      if (error != ERR::Okay) {
         log.warning("Error %d on writing to field %s (field type $%.8x, source type $%.8x).", int(error), Field->Name, Field->Flags, flags);
      }
      return error;
   }
   else {
      if (Field->Flags & FD_UNIT)          return setval_unit(Object, Field, flags, Data, 0);
      else if (Field->Flags & FD_ARRAY) {
         if (Field->Flags & FD_CPP) return set_or_write_vector(Object, Field, flags, Data, Elements);
         else return set_or_write_array(Object, Field, flags, Data, Elements);
      }
      else if (Field->Flags & FD_FUNCTION) return setval_function(Object, Field, flags, Data, 0);
      else if (Field->Flags & FD_INT)      return setval_long(Object, Field, flags, Data, 0);
      else if (Field->Flags & (FD_DOUBLE|FD_FLOAT)) return setval_double(Object, Field, flags, Data, 0);
      else if (Field->Flags & FD_STRING)  return setval_strview(Object, Field, flags, Data, 0);
      else if (Field->Flags & FD_POINTER) return setval_pointer(Object, Field, flags, Data, 0);
      else if (Field->Flags & FD_INT64)   return setval_large(Object, Field, flags, Data, 0);
      else return ERR::FieldTypeMismatch;
   }
}

//********************************************************************************************************************
// The writeval() functions are used as optimised calls for all cases where the client has not provided a SetValue()
// function.

[[nodiscard]] inline bool flag_match(const std::string_view CamelFlag, const std::string_view ClientFlag) noexcept
{
   std::size_t i = 0, j = 0;
   while (i < CamelFlag.size() and j < ClientFlag.size()) {
      if (ClientFlag[j] IS '_') {
          j++;
          continue;
      }

      auto ca = std::tolower((unsigned char)CamelFlag[i]);
      auto cb = std::tolower((unsigned char)ClientFlag[j]);

      if (ca != cb) return false;

      i++;
      j++;
   }

   return ((i IS CamelFlag.size()) and (j IS ClientFlag.size()));
}

static ERR writeval_flags(OBJECTPTR Object, const Field *Field, int Flags, CPTR Data, int Elements)
{
   kt::Log log("WriteField");
   int int32;

   // Converts flags to numeric form if the source value is a string.

   if (Flags & FD_STRING) {
      int64_t int64 = 0;
      auto str = field_string_view(Flags, Data);

      // Check if the string is a number
      if (decimal_digits(str)) {
         int64 = parse_integer<int64_t>(str);
      }
      else if (Field->Arg) {
         bool reverse = false;
         int16_t op   = OP_OVERWRITE;
         while (not str.empty()) {
            if (str.front() IS '&')      { op = OP_AND;       str.remove_prefix(1); }
            else if (str.front() IS '!') { op = OP_OR;        str.remove_prefix(1); }
            else if (str.front() IS '^') { op = OP_OVERWRITE; str.remove_prefix(1); }
            else if (str.front() IS '~') { reverse = true;    str.remove_prefix(1); }
            else {
               const auto sep = str.find('|');
               const auto sv  = (sep IS std::string_view::npos) ? str : str.substr(0, sep);

               if (not sv.empty()) {
                  for (auto lk = (FieldDef *)Field->Arg; lk->Name; lk++) {
                     if (flag_match(lk->Name, sv)) {
                        int64 |= lk->Value;
                        break;
                     }
                  }
               }

               if (sep IS std::string_view::npos) str = {};
               else {
                  str.remove_prefix(sep);
                  while (str.starts_with('|')) str.remove_prefix(1);
               }
            }
         }

         if (reverse) int64 = ~int64;

         // Get the current flag values from the field if special ops are requested

         if (op != OP_OVERWRITE) {
            int current_flags;
            if (auto error = Object->get<int>(Field->FieldID, current_flags); !error) {
               if (op IS OP_OR) int64 = current_flags | int64;
               else if (op IS OP_AND) int64 = current_flags & int64;
            }
            else return error;
         }
      }
      else log.warning("Missing flag definitions for field \"%s\"", Field->Name);

      if (Field->Flags & FD_INT) {
         int32 = int64;
         Flags = FD_INT;
         Data  = &int32;
      }
      else if (Field->Flags & FD_INT64) {
         Flags = FD_INT64;
         Data  = &int64;
      }
      else return ERR::SetValueNotArray;
   }

   return writeval_default(Object, Field, Flags, Data, Elements);
}

static ERR writeval_lookup(OBJECTPTR Object, const Field *Field, int Flags, CPTR Data, int Elements)
{
   int int32;

   if (Flags & FD_STRING) {
      auto str = field_string_view(Flags, Data);

      FieldDef *lookup;
      int32 = parse_integer<int>(str); // If the string is a number rather than a lookup, this will extract it
      if ((lookup = (FieldDef *)Field->Arg)) {
         while (lookup->Name) {
            if (iequals(str, lookup->Name)) {
               int32 = lookup->Value;
               break;
            }
            lookup++;
         }
      }
      else kt::Log("WriteField").warning("Missing lookup table definitions for field \"%s\"", Field->Name);

      Flags = FD_INT;
      Data  = &int32;
   }

   return writeval_default(Object, Field, Flags, Data, Elements);
}

static ERR writeval_long(OBJECTPTR Object, const Field *Field, int Flags, CPTR Data, int Elements)
{
   auto offset = (int *)((int8_t *)Object + Field->Offset);
   if (Flags & FD_INT)         *offset = *((int *)Data);
   else if (Flags & FD_INT64)  *offset = (int)(*((int64_t *)Data));
   else if (Flags & (FD_DOUBLE|FD_FLOAT)) *offset = std::lrint(*((double *)Data));
   else if (Flags & FD_STRING) *offset = kt::svtonum<int>(*((std::string_view *)Data));
   else return ERR::SetValueNotNumeric;
   return ERR::Okay;
}

static ERR writeval_large(OBJECTPTR Object, const Field *Field, int Flags, CPTR Data, int Elements)
{
   auto offset = (int64_t *)((int8_t *)Object + Field->Offset);
   if (Flags & FD_INT64)      *offset = *((int64_t *)Data);
   else if (Flags & FD_INT)   *offset = *((int *)Data);
   else if (Flags & (FD_DOUBLE|FD_FLOAT)) *offset = std::lrint(*((double *)Data));
   else if (Flags & FD_STRING) *offset = kt::svtonum<int64_t>(*((std::string_view *)Data));
   else return ERR::SetValueNotNumeric;
   return ERR::Okay;
}

static ERR writeval_double(OBJECTPTR Object, const Field *Field, int Flags, CPTR Data, int Elements)
{
   auto offset = (double *)((int8_t *)Object + Field->Offset);
   if (Flags & (FD_DOUBLE|FD_FLOAT)) *offset = *((double *)Data);
   else if (Flags & FD_INT)    *offset = *((int *)Data);
   else if (Flags & FD_INT64)  *offset = (*((int64_t *)Data));
   else if (Flags & FD_STRING) *offset = kt::svtonum<double>(*((std::string_view *)Data));
   else return ERR::SetValueNotNumeric;
   return ERR::Okay;
}

static ERR writeval_function(OBJECTPTR Object, const Field *Field, int Flags, CPTR Data, int Elements)
{
   auto offset = (FUNCTION *)((int8_t *)Object + Field->Offset);
   if (Flags & FD_FUNCTION) {
      offset[0] = ((FUNCTION *)Data)[0];
   }
   else if (Flags & FD_POINTER) {
      offset[0].Type = (Data) ? CALL::STD_C : CALL::NIL;
      offset[0].Routine = (FUNCTION *)Data;
      offset[0].Context = tlContext.back().obj;
   }
   else return ERR::SetValueNotFunction;
   return ERR::Okay;
}

static ERR writeval_ptr(OBJECTPTR Object, const Field *Field, int Flags, CPTR Data, int Elements)
{
   if (Flags & FD_POINTER) {
      auto offset = (APTR *)((int8_t *)Object + Field->Offset);
      *offset = (APTR)Data;
      return ERR::Okay;
   }
   else return ERR::SetValueNotPointer;
}

static ERR writeval_cppstr(OBJECTPTR Object, const Field *Field, int Flags, CPTR Data, int Elements)
{
   if (Flags & FD_STRING) {
      auto offset = (std::string *)((int8_t *)Object + Field->Offset);
      offset->assign(*((std::string_view *)Data));
      return ERR::Okay;
   }
   else return ERR::SetValueNotPointer;
}

static ERR writeval_unit(OBJECTPTR Object, const Field *Field, int Flags, CPTR Data, int Elements)
{
   auto offset = (Unit *)((int8_t *)Object + Field->Offset);

   if (Flags & FD_UNIT) *offset = *((Unit *)Data);
   else {
      auto unit_type = Flags & (~(FD_INT|FD_INT64|FD_DOUBLE|FD_FLOAT|FD_POINTER|FD_STRING));
      if (Flags & FD_INT64) *offset = Unit(*((int64_t *)Data), unit_type);
      else if (Flags & FD_INT)   *offset = Unit(*((int *)Data), unit_type);
      else if (Flags & (FD_DOUBLE|FD_FLOAT)) *offset = Unit(*((double *)Data), unit_type);
      else if (Flags & FD_STRING) {
         Unit unit;
         auto str = field_string_view(Flags, Data);
         if (Field->Flags & FD_SCALED) {
            size_t end = 0;
            unit.Value = parse_double(str, &end);
            if ((end < str.size()) and (str[end] IS '%')) {
               unit.Type = FD_SCALED;
               unit.Value *= 0.01;
            }
         }
         else unit.Value = parse_double(str);
         *offset = unit;
      }
      else return ERR::SetValueNotNumeric;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

class FieldContext : public extObjectContext {
   bool success;

   public:
   FieldContext(OBJECTPTR Object, const struct Field *Field) : extObjectContext(Object, AC::SetField) {
#ifndef NDEBUG
      if ((tlContext.back().field IS Field) and (tlContext.back().obj IS Object)) { // Detect recursion
         success = false;
         return;
      }
      else success = true;

      Object->ActionDepth++;
#endif
   }

#ifndef NDEBUG
   ~FieldContext() {
      if (success) obj->ActionDepth--;
   }
#endif
};

//********************************************************************************************************************

static ERR setval_unit(OBJECTPTR Object, const Field *Field, int Flags, CPTR Data, int Elements)
{
   // Convert the value to match what the unit will accept, then call the unit field's set function.

   FieldContext ctx(Object, Field);

   if (Flags & (FD_INT|FD_INT64)) {
      auto unit = Unit((Flags & FD_INT) ? *((int *)Data) : *((int64_t *)Data), Flags & (~(FD_INT|FD_INT64|FD_DOUBLE|FD_POINTER|FD_STRING)));
      return ((ERR (*)(APTR, Unit *))(Field->SetValue))(Object, &unit);
   }
   else if (Flags & (FD_DOUBLE|FD_FLOAT)) {
      auto unit = Unit(*((double *)Data), Flags & (~(FD_INT|FD_INT64|FD_DOUBLE|FD_POINTER|FD_STRING)));
      return ((ERR (*)(APTR, Unit *))(Field->SetValue))(Object, &unit);
   }
   else if (Flags & FD_STRING) {
      Unit unit;
      auto str = field_string_view(Flags, Data);
      if (Field->Flags & FD_SCALED) {
         // Percentages are only applicable to numeric variables, and require conversion in advance.
         // NB: If a field needs total control over variable conversion, it should not specify FD_SCALED.
         size_t end = 0;
         unit.Value = parse_double(str, &end);
         if ((end < str.size()) and (str[end] IS '%')) {
            unit.Type = FD_SCALED;
            unit.Value *= 0.01;
         }
      }
      else unit.Value = parse_double(str);
      return ((ERR (*)(APTR, Unit *))(Field->SetValue))(Object, &unit);
   }
   else if (Flags & FD_UNIT) {
      return ((ERR (*)(APTR, APTR))(Field->SetValue))(Object, (APTR)Data);
   }
   else return ERR::FieldTypeMismatch;
}

// Embedded kt::vector<>

template <class T>
static ERR assign_vector_field(OBJECTPTR Object, const Field *Field, CPTR Data, int Elements)
{
   auto dest = (kt::vector<T> *)((int8_t *)Object + Field->Offset);
   if ((Data) and (Elements > 0)) {
      auto source = (const T *)Data;
      dest->assign(source, source + Elements);
   }
   else dest->clear();
   return ERR::Okay;
}

static ERR set_or_write_vector(OBJECTPTR Object, const Field *Field, int Flags, CPTR Data, int Elements)
{
   FieldContext ctx(Object, Field);

   if (Flags & FD_ARRAY) {
      if (Field->SetValue) {
         // Basic type checking
         int src_type = Flags & (FD_INT|FD_INT64|FD_FLOAT|FD_DOUBLE|FD_POINTER|FD_BYTE|FD_WORD|FD_STRUCT);
         if (src_type) {
            int dest_type = Field->Flags & (FD_INT|FD_INT64|FD_FLOAT|FD_DOUBLE|FD_POINTER|FD_BYTE|FD_WORD|FD_STRUCT);
            if (not (src_type & dest_type)) return ERR::SetValueNotArray;
         }
         // Vector arrays are fed direct data pointers and element counts to avoid conversion complications
         return ((ERR (*)(APTR, APTR, int))(Field->SetValue))(Object, (APTR)Data, Elements);
      }
      else if (Field->Flags & FD_CPP) { // Embedded kt::vector<>
         if (Field->Flags & FD_STRING) {
            if (not (Flags & FD_STRING)) return ERR::SetValueNotArray;
            return assign_vector_field<std::string>(Object, Field, Data, Elements);
         }
         else if (Field->Flags & FD_BYTE) {
            if (not (Flags & FD_BYTE)) return ERR::SetValueNotArray;
            return assign_vector_field<uint8_t>(Object, Field, Data, Elements);
         }
         else if (Field->Flags & FD_WORD) {
            if (not (Flags & FD_WORD)) return ERR::SetValueNotArray;
            return assign_vector_field<uint16_t>(Object, Field, Data, Elements);
         }
         else if (Field->Flags & FD_INT) {
            if (not (Flags & FD_INT)) return ERR::SetValueNotArray;
            return assign_vector_field<int>(Object, Field, Data, Elements);
         }
         else if (Field->Flags & FD_INT64) {
            if (not (Flags & FD_INT64)) return ERR::SetValueNotArray;
            return assign_vector_field<int64_t>(Object, Field, Data, Elements);
         }
         else if (Field->Flags & FD_FLOAT) {
            if (not (Flags & FD_FLOAT)) return ERR::SetValueNotArray;
            return assign_vector_field<float>(Object, Field, Data, Elements);
         }
         else if (Field->Flags & FD_DOUBLE) {
            if (not (Flags & FD_DOUBLE)) return ERR::SetValueNotArray;
            return assign_vector_field<double>(Object, Field, Data, Elements);
         }
         else if (Field->Flags & FD_POINTER) {
            if (not (Flags & FD_POINTER)) return ERR::SetValueNotArray;
            return assign_vector_field<APTR>(Object, Field, Data, Elements);
         }
      }
      return ERR::FieldTypeMismatch;
   }
   else {
      kt::Log(__FUNCTION__).warning("Arrays can only be set using the FD_ARRAY type.");
      return ERR::SetValueNotArray;
   }
}

// Embedded basic array (non-vector)

static ERR set_or_write_array(OBJECTPTR Object, const Field *Field, int Flags, CPTR Data, int Elements)
{
   FieldContext ctx(Object, Field);

   if (Flags & FD_ARRAY) {
      // Basic type checking
      int src_type = Flags & (FD_INT|FD_INT64|FD_FLOAT|FD_DOUBLE|FD_POINTER|FD_BYTE|FD_WORD|FD_STRUCT);
      if (src_type) {
         int dest_type = Field->Flags & (FD_INT|FD_INT64|FD_FLOAT|FD_DOUBLE|FD_POINTER|FD_BYTE|FD_WORD|FD_STRUCT);
         if (not (src_type & dest_type)) return ERR::SetValueNotArray;
      }

      if (Field->SetValue) return ((ERR (*)(APTR, APTR, int))(Field->SetValue))(Object, (APTR)Data, Elements);
      else if (Field->Arg > 0) { // An arg value indicates an embedded fixed-size array
         size_t size;
         if ((Elements > Field->Arg) or (Elements <= 0)) Elements = Field->Arg;

         if (Flags & FD_BYTE) size = Elements * sizeof(uint8_t);
         else if (Flags & FD_WORD) size = Elements * sizeof(uint16_t);
         else if (Flags & FD_INT) size = Elements * sizeof(int);
         else if (Flags & (FD_INT64|FD_DOUBLE)) size = Elements * sizeof(double);
         else if (Flags & FD_POINTER) size = Elements * sizeof(APTR);
         else size = 0;

         if (Data) copymem(Data, (int8_t *)Object + Field->Offset, size);
         else return ERR::InvalidValue;

         return ERR::Okay;
      }
      else return ERR::FieldTypeMismatch;
   }
   else if (Flags & FD_STRING) { // Incoming CSV string - DEPRECATED
      kt::Log(__FUNCTION__).warning("CSV support for arrays is deprecated.");
      std::abort();
      return ERR::Failed;
   }
   else return kt::Log(__FUNCTION__).warning(ERR::SetValueNotArray);
}

static ERR setval_function(OBJECTPTR Object, const Field *Field, int Flags, CPTR Data, int Elements)
{
   OBJECTPTR caller = tlContext.back().obj;
   FieldContext ctx(Object, Field);

   if (Flags & FD_FUNCTION) {
      return ((ERR (*)(APTR, APTR))(Field->SetValue))(Object, (APTR)Data);
   }
   else if (Flags & FD_POINTER) {
      FUNCTION func;
      if (Data) {
         func.Type = CALL::STD_C;
         func.Context = caller;
         func.Routine = (APTR)Data;
      }
      else func.clear();
      return ((ERR (*)(APTR, FUNCTION *))(Field->SetValue))(Object, &func);
   }
   else return ERR::SetValueNotFunction;
}

static ERR setval_long(OBJECTPTR Object, const Field *Field, int Flags, CPTR Data, int Elements)
{
   int int32;
   if (Flags & FD_INT64)       int32 = (int)(*((int64_t *)Data));
   else if (Flags & (FD_DOUBLE|FD_FLOAT)) int32 = std::lrint(*((double *)Data));
   else if (Flags & FD_STRING) int32 = kt::svtonum<int>(*((std::string_view *)Data));
   else if (Flags & FD_INT)    int32 = *((int *)Data);
   else if (Flags & FD_UNIT)   int32 = std::lrint(((Unit *)Data)->Value);
   else return ERR::SetValueNotNumeric;

   FieldContext ctx(Object, Field);
   return ((ERR (*)(APTR, int))(Field->SetValue))(Object, int32);
}

static ERR setval_double(OBJECTPTR Object, const Field *Field, int Flags, CPTR Data, int Elements)
{
   double float64;
   if (Flags & FD_INT)         float64 = *((int *)Data);
   else if (Flags & FD_INT64)  float64 = (double)(*((int64_t *)Data));
   else if (Flags & FD_STRING) float64 = kt::svtonum<double>(*((std::string_view *)Data));
   else if (Flags & (FD_DOUBLE|FD_FLOAT)) float64 = *((double *)Data);
   else if (Flags & FD_UNIT)   float64 = ((Unit *)Data)->Value;
   else return ERR::SetValueNotNumeric;

   FieldContext ctx(Object, Field);
   return ((ERR (*)(APTR, double))(Field->SetValue))(Object, float64);
}

static ERR setval_pointer(OBJECTPTR Object, const Field *Field, int Flags, CPTR Data, int Elements)
{
   FieldContext ctx(Object, Field);

   if (Flags & FD_POINTER) {
      return ((ERR (*)(APTR, CPTR))(Field->SetValue))(Object, Data);
   }
   else if (Flags & FD_INT) {
      return ((ERR (*)(APTR, char *))(Field->SetValue))(Object, std::to_string(*((int *)Data)).data());
   }
   else if (Flags & FD_INT64) {
      return ((ERR (*)(APTR, char *))(Field->SetValue))(Object, std::to_string(*((int64_t *)Data)).data());
   }
   else if (Flags & (FD_DOUBLE|FD_FLOAT)) {
      return ((ERR (*)(APTR, char *))(Field->SetValue))(Object, std::to_string(*((double *)Data)).data());
   }
   else return ERR::SetValueNotPointer;
}

static ERR setval_strview(OBJECTPTR Object, const Field *Field, int Flags, CPTR Data, int Elements)
{
   FieldContext ctx(Object, Field);

   if (Flags & FD_STRING) {
      return ((ERR (*)(APTR, std::string_view &))(Field->SetValue))(Object, *((std::string_view *)Data));
   }
   else if (Flags & FD_INT) {
      auto string = std::to_string(*((int *)Data));
      std::string_view view(string);
      return ((ERR (*)(APTR, std::string_view &))(Field->SetValue))(Object, view);
   }
   else if (Flags & FD_INT64) {
      auto string = std::to_string(*((int64_t *)Data));
      std::string_view view(string);
      return ((ERR (*)(APTR, std::string_view &))(Field->SetValue))(Object, view);
   }
   else if (Flags & (FD_DOUBLE|FD_FLOAT)) {
      auto string = std::to_string(*((double *)Data));
      std::string_view view(string);
      return ((ERR (*)(APTR, std::string_view &))(Field->SetValue))(Object, view);
   }
   else return ERR::SetValueNotPointer;
}

static ERR setval_large(OBJECTPTR Object, const Field *Field, int Flags, CPTR Data, int Elements)
{
   int64_t int64;

   if (Flags & FD_INT)        int64 = *((int *)Data);
   else if (Flags & (FD_DOUBLE|FD_FLOAT)) int64 = std::llround(*((double *)Data));
   else if (Flags & FD_STRING) int64 = kt::svtonum<int64_t>(*((std::string_view *)Data));
   else if (Flags & FD_INT64)  int64 = *((int64_t*)Data);
   else if (Flags & FD_UNIT)   int64 = std::llround(((Unit*)Data)->Value);
   else return ERR::SetValueNotNumeric;

   FieldContext ctx(Object, Field);
   return ((ERR (*)(APTR, int64_t))(Field->SetValue))(Object, int64);
}

//********************************************************************************************************************
// This routine configures WriteValue so that it uses the correct set-field function, according to the field type that
// has been defined.

void optimise_write_field(Field &Field)
{
   kt::Log log(__FUNCTION__);

   if (not Field.writeable()) return;

   if (Field.Flags & FD_FLAGS)       Field.WriteValue = writeval_flags;
   else if (Field.Flags & FD_LOOKUP) Field.WriteValue = writeval_lookup;
   else if (not Field.SetValue) {
      if (Field.Flags & FD_ARRAY) {
         Field.WriteValue = (Field.Flags & FD_CPP) ? set_or_write_vector : set_or_write_array;
      }
      else if (Field.Flags & FD_INT)   Field.WriteValue = writeval_long;
      else if (Field.Flags & FD_INT64) Field.WriteValue = writeval_large;
      else if (Field.Flags & (FD_DOUBLE|FD_FLOAT)) Field.WriteValue = writeval_double;
      else if (Field.Flags & FD_FUNCTION) Field.WriteValue = writeval_function;
      else if (Field.Flags & FD_STRING) Field.WriteValue = writeval_cppstr; // Embedded std::string
      else if (Field.Flags & FD_POINTER) {
         if (Field.Flags & FD_STRING) log.warning("C-style string pointers are deprecated; field: %s.", Field.Name);
         Field.WriteValue = writeval_ptr;
      }
      else log.warning("Invalid field flags for %s: $%.8x.", Field.Name, Field.Flags);
   }
   else {
      if (Field.Flags & FD_UNIT)          Field.WriteValue = setval_unit;
      else if (Field.Flags & FD_ARRAY) {
         Field.WriteValue = (Field.Flags & FD_CPP) ? set_or_write_vector : set_or_write_array;
      }
      else if (Field.Flags & FD_FUNCTION) Field.WriteValue = setval_function;
      else if (Field.Flags & FD_INT)      Field.WriteValue = setval_long;
      else if (Field.Flags & (FD_DOUBLE|FD_FLOAT)) Field.WriteValue = setval_double;
      else if (Field.Flags & FD_STRING) Field.WriteValue = setval_strview; // Embedded std::string
      else if (Field.Flags & FD_POINTER) {
         if (Field.Flags & FD_STRING) log.warning("C-style string pointers are deprecated; field: %s.", Field.Name);
         Field.WriteValue = setval_pointer;
      }
      else if (Field.Flags & FD_INT64)    Field.WriteValue = setval_large;
      else log.warning("Invalid field flags for %s: $%.8x.", Field.Name, Field.Flags);
   }
}

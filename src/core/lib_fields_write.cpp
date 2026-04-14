/*********************************************************************************************************************

The source code for Kōtuku is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

-CATEGORY-
Name: Fields
-END-

*********************************************************************************************************************/

#include "defs.h"
#include <kotuku/main.h>

#include <stdarg.h>
#include <stdlib.h>
#include <cmath>

#define OP_OR        0
#define OP_AND       1
#define OP_OVERWRITE 2

static ERR writeval_array(OBJECTPTR, Field *, int, CPTR , int);
static ERR writeval_flags(OBJECTPTR, Field *, int, CPTR , int);
static ERR writeval_long(OBJECTPTR, Field *, int, CPTR , int);
static ERR writeval_large(OBJECTPTR, Field *, int, CPTR , int);
static ERR writeval_double(OBJECTPTR, Field *, int, CPTR , int);
static ERR writeval_function(OBJECTPTR, Field *, int, CPTR , int);
static ERR writeval_ptr(OBJECTPTR, Field *, int, CPTR , int);

static ERR setval_large(OBJECTPTR, Field *, int Flags, CPTR , int);
static ERR setval_pointer(OBJECTPTR, Field *, int Flags, CPTR , int);
static ERR setval_double(OBJECTPTR, Field *, int Flags, CPTR , int);
static ERR setval_long(OBJECTPTR, Field *, int Flags, CPTR , int);
static ERR setval_function(OBJECTPTR, Field *, int Flags, CPTR , int);
static ERR setval_array(OBJECTPTR, Field *, int Flags, CPTR , int);
static ERR setval_brgb(OBJECTPTR, Field *, int Flags, CPTR , int);
static ERR setval_unit(OBJECTPTR, Field *, int Flags, CPTR , int);

//********************************************************************************************************************
// Converts a CSV string into an array (or use "#0x123..." for a hexadecimal byte list)

static int write_array(CSTRING String, int Flags, int16_t ArraySize, APTR Dest)
{
   if (!ArraySize) ArraySize = 0x7fff; // If no ArraySize is specified then there is no imposed limit.

   if ((String[0] IS '#') or ((String[0] IS '0') and (String[1] IS 'x'))) {
      // Array is a sequence of hexadecimal bytes
      String += (String[0] IS '#') ? 1 : 2;
      int i = 0;
      while ((i < ArraySize) and (*String)) {
         uint8_t byte = 0;
         for (int shift=4; shift >= 0; shift -= 4) {
            if (*String) {
               if (std::isdigit(*String)) byte |= (*String - '0') << shift;
               else if (*String >= 'A' and (*String <= 'F')) byte |= (*String - 'A' + 10) << shift;
               else if (*String >= 'a' and (*String <= 'f')) byte |= (*String - 'a' + 10) << shift;
               String++;
            }
         }

         if (Flags & FD_INT)         ((int *)Dest)[i]    = byte;
         else if (Flags & FD_BYTE)   ((int8_t *)Dest)[i] = byte;
         else if (Flags & FD_FLOAT)  ((float *)Dest)[i]  = byte;
         else if (Flags & FD_DOUBLE) ((double *)Dest)[i] = byte;
         i++;
      }
      return i;
   }
   else {
      // Assume String is in CSV format
      char *end;
      int i;
      for (i=0; (i < ArraySize) and (*String); i++) {
          if (Flags & FD_INT)         ((int *)Dest)[i]     = strtol(String, &end, 0);
          else if (Flags & FD_BYTE)   ((uint8_t *)Dest)[i] = strtol(String, &end, 0);
          else if (Flags & FD_FLOAT)  ((float *)Dest)[i]   = strtod(String, &end);
          else if (Flags & FD_DOUBLE) ((double *)Dest)[i]  = strtod(String, &end);
          else if (Flags & FD_STRING) {
             // Not feasible to convert a string into an array of strings
             pf::Log().warning(ERR::InvalidType);
             return 0;
          }
          else {
             pf::Log().warning(ERR::InvalidType);
             return 0;
          }

          String = end;
          while ((*String) and (!std::isdigit(*String)) and (*String != '-')) String++;
      }
      return i;
   }
}

//********************************************************************************************************************
// Used by some of the SetField() range of instructions.

ERR writeval_default(OBJECTPTR Object, Field *Field, int flags, CPTR Data, int Elements)
{
   pf::Log log("WriteField");

   //log.trace("[%s:%d] Name: %s, SetValue: %c, FieldFlags: $%.8x, SrcFlags: $%.8x", Object->className(), Object->UID, Field->Name, Field->SetValue ? 'Y' : 'N', Field->Flags, flags);

   if (!flags) flags = Field->Flags;

   if (!Field->SetValue) {
      ERR error = ERR::Okay;
      if (Field->Flags & FD_ARRAY)         error = writeval_array(Object, Field, flags, Data, Elements);
      else if (Field->Flags & FD_INT)      error = writeval_long(Object, Field, flags, Data, 0);
      else if (Field->Flags & FD_INT64)    error = writeval_large(Object, Field, flags, Data, 0);
      else if (Field->Flags & (FD_DOUBLE|FD_FLOAT)) error = writeval_double(Object, Field, flags, Data, 0);
      else if (Field->Flags & FD_FUNCTION) error = writeval_function(Object, Field, flags, Data, 0);
      else if (Field->Flags & (FD_POINTER|FD_STRING)) error = writeval_ptr(Object, Field, flags, Data, 0);
      else log.warning("Unrecognised field flags $%.8x.", Field->Flags);

      if (error != ERR::Okay) {
         log.warning("Error %d on writing to field %s (field type $%.8x, source type $%.8x).", int(error), Field->Name, Field->Flags, flags);
      }
      return error;
   }
   else {
      if (Field->Flags & FD_UNIT)          return setval_unit(Object, Field, flags, Data, 0);
      else if (Field->Flags & FD_RGB)      return setval_brgb(Object, Field, flags, Data, 0);
      else if (Field->Flags & FD_ARRAY)    return setval_array(Object, Field, flags, Data, Elements);
      else if (Field->Flags & FD_FUNCTION) return setval_function(Object, Field, flags, Data, 0);
      else if (Field->Flags & FD_INT)      return setval_long(Object, Field, flags, Data, 0);
      else if (Field->Flags & (FD_DOUBLE|FD_FLOAT))   return setval_double(Object, Field, flags, Data, 0);
      else if (Field->Flags & (FD_POINTER|FD_STRING)) return setval_pointer(Object, Field, flags, Data, 0);
      else if (Field->Flags & FD_INT64)    return setval_large(Object, Field, flags, Data, 0);
      else return ERR::FieldTypeMismatch;
   }
}

//********************************************************************************************************************
// The writeval() functions are used as optimised calls for all cases where the client has not provided a SetValue()
// function.

static ERR writeval_array(OBJECTPTR Object, Field *Field, int SrcType, CPTR Source, int Elements)
{
   pf::Log log("WriteField");

   if (not Field->writeable()) return ERR::NoFieldAccess;

   // Direct writing to field arrays without a SET function is only supported for the RGB type.  The client should
   // define a SET function for all other cases.

   auto offset = (int8_t *)Object + Field->Offset;

   if ((SrcType & FD_STRING) and (Field->Flags & FD_RGB)) {
      if (!Source) Source = "0,0,0,0"; // A string of nullptr will 'clear' the colour (the alpha value will be zero)
      else if (Field->Flags & FD_INT) ((RGB8 *)offset)->Alpha = 255;
      else if (Field->Flags & FD_BYTE) ((RGB8 *)offset)->Alpha = 255;
      write_array((CSTRING)Source, Field->Flags, 4, offset);
      return ERR::Okay;
   }
   else if ((SrcType & FD_POINTER) and (Field->Flags & FD_RGB)) { // Presume the source is a pointer to an RGB structure
      auto rgb = (RGB8 *)Source;
      ((RGB8 *)offset)->Red   = rgb->Red;
      ((RGB8 *)offset)->Green = rgb->Green;
      ((RGB8 *)offset)->Blue  = rgb->Blue;
      ((RGB8 *)offset)->Alpha = rgb->Alpha;
      return ERR::Okay;
   }

   log.warning("Field array '%s.%s' needs a SET function.", Object->className(), Field->Name);
   return ERR::SanityCheckFailed;
}

[[nodiscard]] inline bool flag_match(const std::string_view CamelFlag, const std::string_view ClientFlag) noexcept
{
   std::size_t i = 0, j = 0;
   while (i < CamelFlag.size() and j < ClientFlag.size()) {
      if (ClientFlag[j] == '_') {
          j++;
          continue;
      }

      auto ca = std::tolower(static_cast<unsigned char>(CamelFlag[i]));
      auto cb = std::tolower(static_cast<unsigned char>(ClientFlag[j]));

      if (ca != cb) return false;

      i++;
      j++;
   }

   return ((i == CamelFlag.size()) and (j == ClientFlag.size()));
}

static ERR writeval_flags(OBJECTPTR Object, Field *Field, int Flags, CPTR Data, int Elements)
{
   pf::Log log("WriteField");
   int j, int32;

   // Converts flags to numeric form if the source value is a string.

   if (Flags & FD_STRING) {
      int64_t int64 = 0;

      if (auto str = (CSTRING)Data) {
         // Check if the string is a number
         for (j=0; str[j] and (str[j] >= '0') and (str[j] <= '9'); j++);
         if (!str[j]) {
            int64 = strtoll(str, nullptr, 0);
         }
         else if (Field->Arg) {
            bool reverse = false;
            int16_t op   = OP_OVERWRITE;
            while (*str) {
               if (*str IS '&')      { op = OP_AND;       str++; }
               else if (*str IS '!') { op = OP_OR;        str++; }
               else if (*str IS '^') { op = OP_OVERWRITE; str++; }
               else if (*str IS '~') { reverse = true;    str++; }
               else {
                  // Find out how long this particular flag name is
                  for (j=0; (str[j]) and (str[j] != '|'); j++);

                  if (j > 0) {
                     std::string_view sv(str, j);
                     for (auto lk = (FieldDef *)Field->Arg; lk->Name; lk++) {
                        if (flag_match(lk->Name, sv)) {
                           int64 |= lk->Value;
                           break;
                        }
                     }
                  }

                  str += j;
                  while (*str IS '|') str++;
               }
            }

            if (reverse) int64 = ~int64;

            // Get the current flag values from the field if special ops are requested

            if (op != OP_OVERWRITE) {
               int current_flags;
               if (auto error = Object->get<int>(Field->FieldID, current_flags); error IS ERR::Okay) {
                  if (op IS OP_OR) int64 = current_flags | int64;
                  else if (op IS OP_AND) int64 = current_flags & int64;
               }
               else return error;
            }
         }
         else log.warning("Missing flag definitions for field \"%s\"", Field->Name);
      }

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

static ERR writeval_lookup(OBJECTPTR Object, Field *Field, int Flags, CPTR Data, int Elements)
{
   pf::Log log("WriteField");
   int int32;

   if (Flags & FD_STRING) {
      if (Data) {
         FieldDef *lookup;
         int32 = strtol((CSTRING)Data, nullptr, 0); // If the Data string is a number rather than a lookup, this will extract it
         if ((lookup = (FieldDef *)Field->Arg)) {
            while (lookup->Name) {
               if (iequals((CSTRING)Data, lookup->Name)) {
                  int32 = lookup->Value;
                  break;
               }
               lookup++;
            }
         }
         else log.warning("Missing lookup table definitions for field \"%s\"", Field->Name);
      }
      else int32 = 0;

      Flags = FD_INT;
      Data  = &int32;
   }

   return writeval_default(Object, Field, Flags, Data, Elements);
}

static ERR writeval_long(OBJECTPTR Object, Field *Field, int Flags, CPTR Data, int Elements)
{
   auto offset = (int *)((int8_t *)Object + Field->Offset);
   if (Flags & FD_INT)         *offset = *((int *)Data);
   else if (Flags & FD_INT64)  *offset = (int)(*((int64_t *)Data));
   else if (Flags & (FD_DOUBLE|FD_FLOAT)) *offset = F2I(*((double *)Data));
   else if (Flags & FD_STRING) *offset = strtol((STRING)Data, nullptr, 0);
   else return ERR::SetValueNotNumeric;
   return ERR::Okay;
}

static ERR writeval_large(OBJECTPTR Object, Field *Field, int Flags, CPTR Data, int Elements)
{
   auto offset = (int64_t *)((int8_t *)Object + Field->Offset);
   if (Flags & FD_INT64)      *offset = *((int64_t *)Data);
   else if (Flags & FD_INT)   *offset = *((int *)Data);
   else if (Flags & (FD_DOUBLE|FD_FLOAT)) *offset = F2I(*((double *)Data));
   else if (Flags & FD_STRING) *offset = strtoll((STRING)Data, nullptr, 0);
   else return ERR::SetValueNotNumeric;
   return ERR::Okay;
}

static ERR writeval_double(OBJECTPTR Object, Field *Field, int Flags, CPTR Data, int Elements)
{
   auto offset = (double *)((int8_t *)Object + Field->Offset);
   if (Flags & (FD_DOUBLE|FD_FLOAT)) *offset = *((double *)Data);
   else if (Flags & FD_INT)    *offset = *((int *)Data);
   else if (Flags & FD_INT64)  *offset = (*((int64_t *)Data));
   else if (Flags & FD_STRING) *offset = strtod((STRING)Data, nullptr);
   else return ERR::SetValueNotNumeric;
   return ERR::Okay;
}

static ERR writeval_function(OBJECTPTR Object, Field *Field, int Flags, CPTR Data, int Elements)
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

static ERR writeval_ptr(OBJECTPTR Object, Field *Field, int Flags, CPTR Data, int Elements)
{
   auto offset = (APTR *)((int8_t *)Object + Field->Offset);
   if (Flags & (FD_POINTER|FD_STRING)) *offset = (void *)Data;
   else return ERR::SetValueNotPointer;
   return ERR::Okay;
}

//********************************************************************************************************************

class FieldContext : public extObjectContext {
   bool success;

   public:
   FieldContext(OBJECTPTR Object, struct Field *Field) : extObjectContext(Object, AC::SetField) {
      if ((tlContext.back().field IS Field) and (tlContext.back().obj IS Object)) { // Detect recursion
         success = false;
         return;
      }
      else success = true;

      Object->ActionDepth++;
   }

   ~FieldContext() {
      if (success) obj->ActionDepth--;
   }
};

//********************************************************************************************************************

static ERR setval_unit(OBJECTPTR Object, Field *Field, int Flags, CPTR Data, int Elements)
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
   else if (Flags & (FD_POINTER|FD_STRING)) {
      Unit unit;
      if (Field->Flags & FD_SCALED) {
         // Percentages are only applicable to numeric variables, and require conversion in advance.
         // NB: If a field needs total control over variable conversion, it should not specify FD_SCALED.
         STRING pct;
         unit.Value = strtod((CSTRING)Data, &pct);
         if (pct[0] IS '%') {
            unit.Type = FD_SCALED;
            unit.Value *= 0.01;
         }
      }
      else unit.Value = strtod((CSTRING)Data, nullptr);
      return ((ERR (*)(APTR, Unit *))(Field->SetValue))(Object, &unit);
   }
   else if (Flags & FD_UNIT) {
      return ((ERR (*)(APTR, APTR))(Field->SetValue))(Object, (APTR)Data);
   }
   else return ERR::FieldTypeMismatch;
}

static ERR setval_brgb(OBJECTPTR Object, Field *Field, int Flags, CPTR Data, int Elements)
{
   if (Field->Flags & FD_BYTE) {
      FieldContext ctx(Object, Field);

      RGB8 rgb;
      rgb.Alpha = 255;
      write_array((CSTRING)Data, FD_BYTE, 4, &rgb);
      ERR error = ((ERR (*)(APTR, RGB8 *, int))(Field->SetValue))(Object, &rgb, 4);

      return error;
   }
   else return ERR::FieldTypeMismatch;
}

static ERR setval_array(OBJECTPTR Object, Field *Field, int Flags, CPTR Data, int Elements)
{
   FieldContext ctx(Object, Field);

   if (Flags & FD_ARRAY) {
      // Basic type checking
      int src_type = Flags & (FD_INT|FD_INT64|FD_FLOAT|FD_DOUBLE|FD_POINTER|FD_BYTE|FD_WORD|FD_STRUCT);
      if (src_type) {
         int dest_type = Field->Flags & (FD_INT|FD_INT64|FD_FLOAT|FD_DOUBLE|FD_POINTER|FD_BYTE|FD_WORD|FD_STRUCT);
         if (!(src_type & dest_type)) return ERR::SetValueNotArray;
      }

      return ((ERR (*)(APTR, APTR, int))(Field->SetValue))(Object, (APTR)Data, Elements);
   }
   else if (Flags & FD_STRING) {
      APTR arraybuffer;
      if ((arraybuffer = malloc(strlen((CSTRING)Data) * 8))) {
         if (!Data) {
            if (Field->Flags & FD_RGB) {
               Data = "0,0,0,0"; // A string of nullptr will 'clear' the colour (the alpha value will be zero)
               Elements = write_array((CSTRING)Data, Field->Flags, Field->Arg, arraybuffer);
            }
            else Elements = 0;
         }
         else if (Field->Flags & FD_RGB) {
            Elements = write_array((CSTRING)Data, Field->Flags, 4, arraybuffer);
            if (Field->Flags & FD_INT)       ((RGB8 *)arraybuffer)->Alpha = 255;
            else if (Field->Flags & FD_BYTE) ((RGB8 *)arraybuffer)->Alpha = 255;
         }
         else Elements = write_array((CSTRING)Data, Field->Flags, 0, arraybuffer);

         auto error = ((ERR (*)(APTR, APTR, int))(Field->SetValue))(Object, arraybuffer, Elements);

         free(arraybuffer);
         return error;
      }
      else return ERR::AllocMemory;
   }
   else {
      pf::Log log(__FUNCTION__);
      log.warning("Arrays can only be set using the FD_ARRAY type.");
      return ERR::SetValueNotArray;
   }
}

static ERR setval_function(OBJECTPTR Object, Field *Field, int Flags, CPTR Data, int Elements)
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

static ERR setval_long(OBJECTPTR Object, Field *Field, int Flags, CPTR Data, int Elements)
{
   int int32;
   if (Flags & FD_INT64)       int32 = (int)(*((int64_t *)Data));
   else if (Flags & (FD_DOUBLE|FD_FLOAT)) int32 = F2I(*((double *)Data));
   else if (Flags & FD_STRING) int32 = strtol((STRING)Data, nullptr, 0);
   else if (Flags & FD_INT)    int32 = *((int *)Data);
   else if (Flags & FD_UNIT)   int32 = F2I(((Unit *)Data)->Value);
   else return ERR::SetValueNotNumeric;

   FieldContext ctx(Object, Field);
   return ((ERR (*)(APTR, int))(Field->SetValue))(Object, int32);
}

static ERR setval_double(OBJECTPTR Object, Field *Field, int Flags, CPTR Data, int Elements)
{
   double float64;
   if (Flags & FD_INT)         float64 = *((int *)Data);
   else if (Flags & FD_INT64)  float64 = (double)(*((int64_t *)Data));
   else if (Flags & FD_STRING) float64 = strtod((CSTRING)Data, nullptr);
   else if (Flags & (FD_DOUBLE|FD_FLOAT)) float64 = *((double *)Data);
   else if (Flags & FD_UNIT)   float64 = ((Unit *)Data)->Value;
   else return ERR::SetValueNotNumeric;

   FieldContext ctx(Object, Field);
   return ((ERR (*)(APTR, double))(Field->SetValue))(Object, float64);
}

static ERR setval_pointer(OBJECTPTR Object, Field *Field, int Flags, CPTR Data, int Elements)
{
   FieldContext ctx(Object, Field);

   if (Flags & (FD_POINTER|FD_STRING)) {
      return ((ERR (*)(APTR, CPTR ))(Field->SetValue))(Object, Data);
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

static ERR setval_large(OBJECTPTR Object, Field *Field, int Flags, CPTR Data, int Elements)
{
   int64_t int64;

   if (Flags & FD_INT)        int64 = *((int *)Data);
   else if (Flags & (FD_DOUBLE|FD_FLOAT)) int64 = std::llround(*((double *)Data));
   else if (Flags & FD_STRING) int64 = strtoll((CSTRING)Data, nullptr, 0);
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
   pf::Log log(__FUNCTION__);

   if (!Field.writeable()) return;

   if (Field.Flags & FD_FLAGS)       Field.WriteValue = writeval_flags;
   else if (Field.Flags & FD_LOOKUP) Field.WriteValue = writeval_lookup;
   else if (!Field.SetValue) {
      if (Field.Flags & FD_ARRAY)      Field.WriteValue = writeval_array;
      else if (Field.Flags & FD_INT)   Field.WriteValue = writeval_long;
      else if (Field.Flags & FD_INT64) Field.WriteValue = writeval_large;
      else if (Field.Flags & (FD_DOUBLE|FD_FLOAT)) Field.WriteValue = writeval_double;
      else if (Field.Flags & FD_FUNCTION) Field.WriteValue = writeval_function;
      else if (Field.Flags & (FD_POINTER|FD_STRING)) Field.WriteValue = writeval_ptr;
      else log.warning("Invalid field flags for %s: $%.8x.", Field.Name, Field.Flags);
   }
   else {
      if (Field.Flags & FD_UNIT) Field.WriteValue = setval_unit;
      else if (Field.Flags & FD_RGB) {
         if (Field.Flags & FD_BYTE) Field.WriteValue = setval_brgb;
         else log.warning("Invalid field flags for %s: $%.8x.", Field.Name, Field.Flags);
      }
      else if (Field.Flags & FD_ARRAY)    Field.WriteValue = setval_array;
      else if (Field.Flags & FD_FUNCTION) Field.WriteValue = setval_function;
      else if (Field.Flags & FD_INT)      Field.WriteValue = setval_long;
      else if (Field.Flags & (FD_DOUBLE|FD_FLOAT))   Field.WriteValue = setval_double;
      else if (Field.Flags & (FD_POINTER|FD_STRING)) Field.WriteValue = setval_pointer;
      else if (Field.Flags & FD_INT64)    Field.WriteValue = setval_large;
      else log.warning("Invalid field flags for %s: $%.8x.", Field.Name, Field.Flags);
   }
}

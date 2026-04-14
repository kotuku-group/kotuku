/*********************************************************************************************************************

The source code for Kōtuku is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

-CATEGORY-
Name: Fields
-END-

*********************************************************************************************************************/

#include "defs.h"
#include <kotuku/main.h>

//********************************************************************************************************************
// This internal function provides a fast binary search of field names via ID.

Field * lookup_id(OBJECTPTR Object, uint32_t FieldID, OBJECTPTR *Target)
{
   auto mc = Object->ExtClass;
   auto &field = mc->FieldLookup;
   *Target = Object;

   unsigned floor = 0;
   unsigned ceiling = mc->BaseCeiling;
   while (floor < ceiling) {
      auto i = (floor + ceiling)>>1;
      if (field[i].FieldID < FieldID) floor = i + 1;
      else if (field[i].FieldID > FieldID) ceiling = i;
      else return &field[i];
   }

   // Sub-class fields (located in the upper register of FieldLookup)

   if (mc->BaseCeiling < mc->FieldLookup.size()) {
      unsigned floor = mc->BaseCeiling;
      auto ceiling = mc->FieldLookup.size();
      while (floor < ceiling) {
         auto i = (floor + ceiling)>>1;
         if (field[i].FieldID < FieldID) floor = i + 1;
         else if (field[i].FieldID > FieldID) ceiling = i;
         else return &field[i];
      }
   }

   // Local object support.  NOTE: This is a fallback mechanism.  The client can optimise their code by
   // directly retrieving a pointer to the local object and then reading the field value from that.

   for (unsigned i=0; mc->Local[i] != 0xff; i++) {
      OBJECTPTR child = *((OBJECTPTR *)(((int8_t *)Object) + mc->FieldLookup[mc->Local[i]].Offset));
      auto &field = child->ExtClass->FieldLookup;
      unsigned floor = 0;
      auto ceiling = child->ExtClass->BaseCeiling;
      while (floor < ceiling) {
         auto j = (floor + ceiling)>>1;
         if (field[j].FieldID < FieldID) floor = j + 1;
         else if (field[j].FieldID > FieldID) ceiling = j;
         else {
            *Target = child;
            return &field[j];
         }
      }
   }

   return nullptr;
}

/*********************************************************************************************************************

-FUNCTION-
FieldName: Resolves a field ID to its registered name.

Resolves a field identifier to its name by checking the internal dictionary.  The field must have previously been
referenced by a class blueprint in order for its name to be registered.

If `FieldID` is not registered, the value is returned as a printable hex string.

-INPUT-
uint FieldID: The unique field hash to resolve.

-RESULT-
cstr: The name of the field is returned.

*********************************************************************************************************************/

extern thread_local char tlFieldName[10]; // $12345678\0

CSTRING FieldName(uint32_t FieldID)
{
   if (auto lock = std::unique_lock{glmFieldKeys, 1s}) {
      if (glFields.contains(FieldID)) return glFields[FieldID].c_str();
   }
   snprintf(tlFieldName, sizeof(tlFieldName), "$%.8x", FieldID);
   return tlFieldName;
}

/*********************************************************************************************************************

-FUNCTION-
FindField: Finds field descriptors for any class, by ID.

The FindField() function checks if an object supports a specified field by scanning its class descriptor for a `FieldID`.
If a matching field is declared, its descriptor is returned.  For example:

<pre>
if (auto field = FindField(Display, FID_Width, NULL)) {
   log.msg("The field name is \"%s\".", field-&gt;Name);
}
</pre>

The resulting !Field structure is immutable.

Note: To lookup the field definition of a @MetaClass, use the @MetaClass.FindField() method.

-INPUT-
obj Object:   The target object.
uint FieldID: The 'FID' number to lookup.
&obj Target:  (Optional) The object that represents the field is returned here (in case a field belongs to an integrated child object).

-RESULT-
struct(Field): Returns a pointer to the !Field descriptor, otherwise `NULL` if not found.
-END-

*********************************************************************************************************************/

// Please note that FieldID is explicitly defined as 32-bit because using the FIELD type would make it 64-bit.
Field * FindField(OBJECTPTR Object, uint32_t FieldID, OBJECTPTR *Target) // Read-only, thread safe function.
{
   OBJECTPTR dummy;
   if (!Target) Target = &dummy;
   return lookup_id(Object, FieldID, Target);
}

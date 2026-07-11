/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

Unit tests for the Object memory layout, compiled into the Core when UNIT_TESTS is enabled and invoked via the public
UnitTests() function.

*********************************************************************************************************************/

#include "../defs.h"

//********************************************************************************************************************

namespace {

struct ExtendedObject final : Object {
   uint32_t Value;

   ExtendedObject() : Object(nullptr, 0), Value(0) { }
};

} // namespace

//********************************************************************************************************************

void object_layout_unit_tests(int &Passed, int &Total)
{
   kt::Log log("ObjectLayout");
   ExtendedObject object;
   const auto *object_start = reinterpret_cast<const char *>(&object);
   const auto *value_start = reinterpret_cast<const char *>(&object.Value);

   Total++;
   if (size_t(value_start - object_start) >= sizeof(Object)) Passed++;
   else log.warning("Extended class storage intrudes into the Object header.");
}

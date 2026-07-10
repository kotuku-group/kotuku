/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

Unit tests for QueueAction() argument serialisation, compiled into the Core when UNIT_TESTS is enabled and invoked
via the public UnitTests() function.  The primary concern is the relocation of self-referential string pointers
when the argument buffer is duplicated into the message queue.

*********************************************************************************************************************/

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../defs.h"

//********************************************************************************************************************
// Queue several SetKey actions with string arguments held in temporary storage, then destroy that storage before
// the messages are processed.  Each QueueAction() call serialises into a temporary buffer that is released on
// return, so successive calls reuse the same heap blocks.  If the serialised messages retain pointers into those
// buffers (or into the caller's storage, which is scribbled after queueing), earlier messages are corrupted.

static bool queued_setkey_relocation_check(kt::Log &Log)
{
   auto task = (OBJECTPTR)CurrentTask();
   if (not task) {
      Log.warning("No current task available.");
      return false;
   }

   static const struct { CSTRING Key; CSTRING Value; } test_keys[] = {
      { "relocation_key_1", "relocation_value_1" },
      { "relocation_key_2", "relocation_value_2" },
      { "relocation_key_3", "relocation_value_3" }
   };

   for (auto &entry : test_keys) {
      auto key   = std::make_unique<char[]>(64);
      auto value = std::make_unique<char[]>(64);
      strcpy(key.get(), entry.Key);
      strcpy(value.get(), entry.Value);

      struct acSetKey args = { std::string_view(key.get()), std::string_view(value.get()) };
      if (QueueAction(AC::SetKey, task->UID, &args) != ERR::Okay) {
         Log.warning("QueueAction() rejected SetKey.");
         return false;
      }

      memset(key.get(), 0xee, 64);
      memset(value.get(), 0xee, 64);
   }

   // Additional heap churn so that the released serialisation buffers are reused before dispatch.

   std::vector<std::unique_ptr<char[]>> churn;
   for (int size=16; size <= 512; size += 8) {
      auto block = std::make_unique<char[]>(size);
      memset(block.get(), 0x55, size);
      churn.push_back(std::move(block));
   }

   ProcessMessages(PMF::NIL, 0);

   for (auto &entry : test_keys) {
      std::string result;
      struct acGetKey get = { entry.Key, &result };
      if (Action(AC::GetKey, task, &get) != ERR::Okay) {
         Log.warning("Queued SetKey for '%s' was not applied to the task.", entry.Key);
         return false;
      }

      if (result != entry.Value) {
         Log.warning("Queued SetKey for '%s' delivered a corrupted value '%s'.", entry.Key, result.c_str());
         return false;
      }
   }

   return true;
}

//********************************************************************************************************************

void queue_action_unit_tests(int &Passed, int &Total)
{
   kt::Log log("QueueAction");

   Total++;
   if (queued_setkey_relocation_check(log)) Passed++;
}

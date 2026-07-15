/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

Unit tests for WaitForObjects(), compiled into the Core when UNIT_TESTS is enabled and invoked via the public
UnitTests() function.  The primary concern is signals and frees that are initiated from a child thread, which must
be deferred to the main thread through the message queue (refer to notify_signal_wfo() and msg_waitforobjects()).

*********************************************************************************************************************/

#include <chrono>
#include <thread>

#include "../defs.h"

//********************************************************************************************************************
// Signal a monitored object from a child thread.  The notification fires on the child thread and must be deferred
// so that glWFOList is only modified by the main thread; WaitForObjects() is expected to wake before its timeout.

static bool cross_thread_signal_check(kt::Log &Log)
{
   OBJECTPTR object;
   if (NewObject(CLASSID::TIME, NF::NIL, &object) != ERR::Okay) {
      Log.warning("Failed to create the test object.");
      return false;
   }

   if (InitObject(object) != ERR::Okay) {
      Log.warning("Failed to initialise the test object.");
      FreeResource(object->UID);
      return false;
   }

   std::thread worker([object]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      Action(AC::Signal, object, nullptr);
   });

   ObjectSignal signals[2] = { { object }, { nullptr } };
   auto error = WaitForObjects(PMF::NIL, 4000, signals);

   worker.join();
   FreeResource(object->UID);

   if (error != ERR::Okay) {
      Log.warning("WaitForObjects() returned '%s' for a child-thread signal.", GetErrorMsg(error));
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Free a monitored object from a child thread.  Freeing is treated as equivalent to a signal; the deferred handler
// must detect that the object no longer exists and still wake WaitForObjects() before its timeout.

static bool cross_thread_free_check(kt::Log &Log)
{
   OBJECTPTR object;
   if (NewObject(CLASSID::TIME, NF::NIL, &object) != ERR::Okay) {
      Log.warning("Failed to create the test object.");
      return false;
   }

   if (InitObject(object) != ERR::Okay) {
      Log.warning("Failed to initialise the test object.");
      FreeResource(object->UID);
      return false;
   }

   std::thread worker([object]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      FreeResource(object->UID);
   });

   ObjectSignal signals[2] = { { object }, { nullptr } };
   auto error = WaitForObjects(PMF::NIL, 4000, signals);

   worker.join();

   if (error != ERR::Okay) {
      Log.warning("WaitForObjects() returned '%s' for a child-thread free.", GetErrorMsg(error));
      return false;
   }

   return true;
}

//********************************************************************************************************************

void wait_for_objects_unit_tests(int &Passed, int &Total)
{
   kt::Log log("WaitForObjects");

   Total++;
   if (cross_thread_signal_check(log)) Passed++;

   Total++;
   if (cross_thread_free_check(log)) Passed++;
}

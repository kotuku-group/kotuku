/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

Unit tests for WaitForObjects(), compiled into the Core when UNIT_TESTS is enabled and invoked via the public
UnitTests() function.  The primary concern is signals and frees that are initiated from a child thread, which must
be deferred to the main thread through the message queue (refer to notify_signal_wfo() and msg_waitforobjects()).

*********************************************************************************************************************/

#include <atomic>
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
// PMF::ANY_SIGNAL must return after the first monitored object is signalled, leaving the other object untouched and
// cleaning the active wait list before returning.

static bool any_signal_check(kt::Log &Log)
{
   OBJECTPTR first = nullptr;
   OBJECTPTR second = nullptr;
   if ((NewObject(CLASSID::TIME, NF::NIL, &first) != ERR::Okay) or
       (NewObject(CLASSID::TIME, NF::NIL, &second) != ERR::Okay)) {
      if (first) FreeResource(first->UID);
      Log.warning("Failed to create objects for the any-signal check.");
      return false;
   }

   if ((InitObject(first) != ERR::Okay) or (InitObject(second) != ERR::Okay)) {
      Log.warning("Failed to initialise objects for the any-signal check.");
      FreeResource(first->UID);
      FreeResource(second->UID);
      return false;
   }

   std::thread worker([first]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      Action(AC::Signal, first, nullptr);
   });

   ObjectSignal signals[3] = { { first }, { second }, { nullptr } };
   auto error = WaitForObjects(PMF::ANY_SIGNAL, 4000, signals);

   worker.join();

   bool okay = true;
   if (error != ERR::Okay) {
      Log.warning("WaitForObjects() returned '%s' in any-signal mode.", GetErrorMsg(error));
      okay = false;
   }
   else if (not glWFOList.empty()) {
      Log.warning("The monitored list was not cleared after an any-signal wait.");
      okay = false;
   }
   else if (second->defined(NF::SIGNALLED)) {
      Log.warning("An unsignalled object was modified by an any-signal wait.");
      okay = false;
   }

   FreeResource(first->UID);
   FreeResource(second->UID);
   return okay;
}

//********************************************************************************************************************
// The default mode must retain the original all-signals contract and must not return after only the first object is
// signalled.

static bool all_signals_check(kt::Log &Log)
{
   OBJECTPTR first = nullptr;
   OBJECTPTR second = nullptr;
   if ((NewObject(CLASSID::TIME, NF::NIL, &first) != ERR::Okay) or
       (NewObject(CLASSID::TIME, NF::NIL, &second) != ERR::Okay)) {
      if (first) FreeResource(first->UID);
      Log.warning("Failed to create objects for the all-signals check.");
      return false;
   }

   if ((InitObject(first) != ERR::Okay) or (InitObject(second) != ERR::Okay)) {
      Log.warning("Failed to initialise objects for the all-signals check.");
      FreeResource(first->UID);
      FreeResource(second->UID);
      return false;
   }

   std::atomic_bool second_signalled = false;
   std::thread worker([first, second, &second_signalled]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      Action(AC::Signal, first, nullptr);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      second_signalled.store(true, std::memory_order_release);
      Action(AC::Signal, second, nullptr);
   });

   ObjectSignal signals[3] = { { first }, { second }, { nullptr } };
   auto error = WaitForObjects(PMF::NIL, 4000, signals);
   bool all_received = second_signalled.load(std::memory_order_acquire);

   worker.join();
   FreeResource(first->UID);
   FreeResource(second->UID);

   if (error != ERR::Okay) {
      Log.warning("WaitForObjects() returned '%s' in all-signals mode.", GetErrorMsg(error));
      return false;
   }
   else if (not all_received) {
      Log.warning("The default wait returned before all monitored objects were signalled.");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// A pre-existing signal must satisfy an any-signal wait immediately.  Placing it second also verifies that any
// subscription created for an earlier entry is cleaned before returning.

static bool pre_signalled_any_check(kt::Log &Log)
{
   OBJECTPTR first = nullptr;
   OBJECTPTR second = nullptr;
   if ((NewObject(CLASSID::TIME, NF::NIL, &first) != ERR::Okay) or
       (NewObject(CLASSID::TIME, NF::NIL, &second) != ERR::Okay)) {
      if (first) FreeResource(first->UID);
      Log.warning("Failed to create objects for the pre-signalled check.");
      return false;
   }

   if ((InitObject(first) != ERR::Okay) or (InitObject(second) != ERR::Okay)) {
      Log.warning("Failed to initialise objects for the pre-signalled check.");
      FreeResource(first->UID);
      FreeResource(second->UID);
      return false;
   }

   Action(AC::Signal, second, nullptr);

   ObjectSignal signals[3] = { { first }, { second }, { nullptr } };
   auto error = WaitForObjects(PMF::ANY_SIGNAL, 1000, signals);

   bool okay = true;
   if (error != ERR::Okay) {
      Log.warning("WaitForObjects() returned '%s' for a pre-signalled object.", GetErrorMsg(error));
      okay = false;
   }
   else if (second->defined(NF::SIGNALLED)) {
      Log.warning("WaitForObjects() did not consume the pre-existing signal.");
      okay = false;
   }
   else if (not glWFOList.empty()) {
      Log.warning("The monitored list was not cleared after a pre-signalled any-signal wait.");
      okay = false;
   }

   FreeResource(first->UID);
   FreeResource(second->UID);
   return okay;
}

//********************************************************************************************************************
// Timing out in any-signal mode must report ERR::TimeOut and remove every dangling subscription/list entry.

static bool any_signal_timeout_check(kt::Log &Log)
{
   OBJECTPTR first = nullptr;
   OBJECTPTR second = nullptr;
   if ((NewObject(CLASSID::TIME, NF::NIL, &first) != ERR::Okay) or
       (NewObject(CLASSID::TIME, NF::NIL, &second) != ERR::Okay)) {
      if (first) FreeResource(first->UID);
      Log.warning("Failed to create objects for the any-signal timeout check.");
      return false;
   }

   if ((InitObject(first) != ERR::Okay) or (InitObject(second) != ERR::Okay)) {
      Log.warning("Failed to initialise objects for the any-signal timeout check.");
      FreeResource(first->UID);
      FreeResource(second->UID);
      return false;
   }

   ObjectSignal signals[3] = { { first }, { second }, { nullptr } };
   auto error = WaitForObjects(PMF::ANY_SIGNAL, 30, signals);

   bool okay = true;
   if (error != ERR::TimeOut) {
      Log.warning("Expected ERR::TimeOut in any-signal mode, got '%s'.", GetErrorMsg(error));
      okay = false;
   }
   else if (not glWFOList.empty()) {
      Log.warning("The monitored list was not cleared after an any-signal timeout.");
      okay = false;
   }

   FreeResource(first->UID);
   FreeResource(second->UID);
   return okay;
}

//********************************************************************************************************************
// A deferred notification can be drained while the child thread is still inside object_free(): NF::FREE is set but
// the object has not yet left the registry.  The handler must treat a terminating object as completed rather than
// leaving the entry monitored, because the consumed message was the only wake-up an indefinite wait would receive.

static bool mid_free_window_check(kt::Log &Log)
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

   // Simulate the mid-free window as msg_waitforobjects() would encounter it.

   glWFOList.insert(std::make_pair(object->UID, ObjectSignal { object }));
   object->setFlag(NF::FREE);

   OBJECTID object_id = object->UID;
   auto result = msg_waitforobjects(nullptr, 0, 0, &object_id, sizeof(object_id));

   object->clearFlag(NF::FREE);

   bool okay = true;
   if (glWFOList.contains(object->UID)) {
      Log.warning("A terminating object was left in the monitored list.");
      glWFOList.erase(object->UID);
      okay = false;
   }
   else if (result != ERR::Terminate) {
      Log.warning("Expected ERR::Terminate for an emptied list, got '%s'.", GetErrorMsg(result));
      okay = false;
   }

   FreeResource(object->UID);
   return okay;
}

//********************************************************************************************************************

void wait_for_objects_unit_tests(int &Passed, int &Total)
{
   kt::Log log("WaitForObjects");

   Total++;
   if (cross_thread_signal_check(log)) Passed++;

   Total++;
   if (cross_thread_free_check(log)) Passed++;

   Total++;
   if (any_signal_check(log)) Passed++;

   Total++;
   if (all_signals_check(log)) Passed++;

   Total++;
   if (pre_signalled_any_check(log)) Passed++;

   Total++;
   if (any_signal_timeout_check(log)) Passed++;

   Total++;
   if (mid_free_window_check(log)) Passed++;
}

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
   auto result = msg_waitforobjects(nullptr, 0, MSGID::WAIT_FOR_OBJECTS,
      std::span<std::byte>((std::byte *)&object_id, sizeof(object_id)));

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

#ifdef __unix__
#include <sys/socket.h>
#include <unistd.h>

struct FDDispatchContext {
   unsigned calls = 0;
   int mode = 0, victim = -1, replacement = -1;
   FDDispatchContext *target = nullptr;
};

static void fd_target_callback(HOSTHANDLE FD, APTR Data)
{
   auto &context = *(FDDispatchContext *)Data;
   ++context.calls;
   char byte;
   (void)read(FD, &byte, 1);
}

static void fd_first_callback(HOSTHANDLE FD, APTR Data)
{
   auto &context = *(FDDispatchContext *)Data;
   ++context.calls;
   char byte;
   (void)read(FD, &byte, 1);
   if (context.mode IS 1) {
      (void)RegisterFD(FD, RFD::REMOVE, nullptr, nullptr);
      return;
   }
   auto filter = context.mode IS 4 ? fd_first_callback : nullptr;
   (void)RegisterFD(context.victim, RFD::REMOVE, filter, nullptr);
   if (context.mode IS 2) close(context.victim);
   if (context.mode IS 3) {
      (void)RegisterFD(FD, RFD::REMOVE, nullptr, nullptr);
      close(context.victim);
      if (dup2(context.replacement, context.victim) != context.victim) return;
      (void)RegisterFD(context.victim, RFD::READ, fd_target_callback, context.target);
   }
}

// Remove a later ready callback, remove self between read/write, retire during recall, reuse an FD and filter removal.
static bool fd_retirement_check(int Mode, kt::Log &Log)
{
   int first[2], victim[2], replacement[2];
   if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, first) != 0) return false;
   auto close_first = kt::Defer([&]() { close(first[0]); close(first[1]); });
   if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, victim) != 0) return false;
   auto close_victim = kt::Defer([&]() { close(victim[0]); close(victim[1]); });
   if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, replacement) != 0) return false;
   auto close_replacement = kt::Defer([&]() { close(replacement[0]); close(replacement[1]); });
   FDDispatchContext initial, old_target, new_target;
   initial.mode = Mode; initial.victim = victim[0]; initial.replacement = replacement[0];
   initial.target = &new_target;
   auto flags = Mode IS 1 ? RFD::READ|RFD::WRITE : Mode IS 2 ? RFD::READ|RFD::RECALL :
      Mode IS 3 ? RFD::ALWAYS_CALL : RFD::READ;
   auto remove = kt::Defer([&]() {
      (void)RegisterFD(first[0], RFD::REMOVE, nullptr, nullptr);
      if (victim[0] >= 0) (void)RegisterFD(victim[0], RFD::REMOVE, nullptr, nullptr);
   });
   if (RegisterFD(first[0], flags, fd_first_callback, &initial) != ERR::Okay or
       RegisterFD(victim[0], RFD::READ, fd_target_callback, &old_target) != ERR::Okay) return false;
   if (write(first[1], "x", 1) != 1 or write(victim[1], "x", 1) != 1 or
       write(replacement[1], "x", 1) != 1 or ProcessMessages(PMF::NIL, 0) != ERR::Okay) return false;
   bool okay = initial.calls IS 1 and old_target.calls IS (Mode IS 1 or Mode IS 4 ? 1u : 0u);
   if (Mode IS 2) victim[0] = -1; // It was closed by the pre-poll callback.
   if (Mode IS 3) {
      // The replacement may run only in the fresh selection after the ALWAYS_CALL retirement was flushed.
      okay = okay and new_target.calls IS 1;
   }
   if (not okay) Log.warning("FD retirement mode %d dispatched stale or duplicate callbacks: %u/%u/%u",
      Mode, initial.calls, old_target.calls, new_target.calls);
   return okay;
}

// A deferred replacement covering both event types must remove both old callback records.
static bool fd_combined_replacement_check(kt::Log &Log)
{
   int sockets[2];
   if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) != 0) return false;
   auto cleanup = kt::Defer([&]() {
      (void)RegisterFD(sockets[0], RFD::REMOVE, nullptr, nullptr);
      close(sockets[0]);
      close(sockets[1]);
   });

   FDDispatchContext read_context, write_context, replacement_context;
   if (RegisterFD(sockets[0], RFD::READ, fd_target_callback, &read_context) != ERR::Okay or
       RegisterFD(sockets[0], RFD::WRITE, fd_target_callback, &write_context) != ERR::Okay) return false;

   ++glFDProtected;
   auto error = RegisterFD(sockets[0], RFD::READ|RFD::WRITE, fd_target_callback, &replacement_context);
   --glFDProtected;
   if (error != ERR::Okay) return false;
   for (auto &record : glRegisterFD) RegisterFD(record.FD, record.Flags, record.Routine, record.Data);
   glRegisterFD.clear();

   unsigned matches = 0;
   bool correct = true;
   for (const auto &record : glFDTable) {
      if (record.FD != sockets[0]) continue;
      ++matches;
      correct = correct and record.Routine IS fd_target_callback and record.Data IS &replacement_context and
         (record.Flags & (RFD::READ|RFD::WRITE)) IS (RFD::READ|RFD::WRITE) and
         (record.Flags & RFD::REMOVE) IS RFD::NIL;
   }
   if (matches IS 1 and correct) return true;
   Log.warning("Combined FD replacement left %u records or a retired callback.", matches);
   return false;
}
#endif

#ifdef _WIN32
struct FDWindowsReplaceContext {
   unsigned calls = 0;
   ERR error = ERR::Okay;
};

static void fd_windows_replace_callback(HOSTHANDLE FD, APTR Data)
{
   auto &context = *(FDWindowsReplaceContext *)Data;
   ++context.calls;
   context.error = RegisterFD(FD, RFD::READ|RFD::WRITE, fd_windows_replace_callback, Data);
}

// The pre-wait callback must be able to replace its own record after an earlier overlapping record.
static bool fd_windows_combined_replacement_check(kt::Log &Log)
{
   WINHANDLE handle = nullptr;
   if (alloc_public_waitlock(&handle, nullptr) != ERR::Okay) return false;
   auto cleanup = kt::Defer([&]() {
      (void)RegisterFD(handle, RFD::REMOVE, nullptr, nullptr);
      free_public_waitlock(handle);
   });

   FDWindowsReplaceContext context;
   if (RegisterFD(handle, RFD::READ, fd_windows_replace_callback, &context) != ERR::Okay or
       RegisterFD(handle, RFD::WRITE|RFD::ALWAYS_CALL, fd_windows_replace_callback, &context) != ERR::Okay) {
      return false;
   }

   if (ProcessMessages(PMF::NIL, 0) != ERR::Okay) return false;

   unsigned matches = 0;
   bool correct = true;
   for (const auto &record : glFDTable) {
      if (record.FD != handle) continue;
      ++matches;
      correct = correct and record.Routine IS fd_windows_replace_callback and record.Data IS &context and
         (record.Flags & (RFD::READ|RFD::WRITE)) IS (RFD::READ|RFD::WRITE) and
         (record.Flags & (RFD::ALWAYS_CALL|RFD::REMOVE)) IS RFD::NIL;
   }
   if (context.calls IS 1 and context.error IS ERR::Okay and matches IS 1 and correct) return true;
   Log.warning("Windows combined FD replacement called %u times and left %u records.", context.calls, matches);
   return false;
}
#endif

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
#ifdef __unix__
   for (int mode = 0; mode < 5; ++mode) {
      ++Total;
      if (fd_retirement_check(mode, log)) ++Passed;
   }
   ++Total;
   if (fd_combined_replacement_check(log)) ++Passed;
#endif
#ifdef _WIN32
   ++Total;
   if (fd_windows_combined_replacement_check(log)) ++Passed;
#endif
}

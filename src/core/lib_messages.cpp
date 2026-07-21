/*********************************************************************************************************************

The source code for Kōtuku is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

-CATEGORY-
Name: Messages
-END-

*********************************************************************************************************************/

#ifdef __unix__
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

#include "defs.h"

#include <deque>

static ERR wake_task(void);
#ifdef _WIN32
static ERR sleep_task(int, int8_t);
#else
static ERR sleep_task(int);
ERR write_nonblock(int Handle, APTR Data, int Size, int64_t EndTime);
#endif

static const int MAX_MSEC = 1000;

static constexpr size_t MESSAGE_BATCH_LIMIT = 30;
static std::mutex glQueueLock;
static std::deque<TaskMessage> glQueue; // Available to all threads, use glQueueLock
static std::atomic_size_t glQueuedMessages = 0;
static bool glWFOAnySignal = false; // Main-thread state for the active WaitForObjects() call
static bool glWFOSignalReceived = false;

template <class T> inline APTR ResolveAddress(T *Pointer, int Offset) {
   return APTR(((int8_t *)Pointer) + Offset);
}

static ERR msghandler_free(ResourceRecord &Resource, APTR Address)
{
   kt::Log log("RemoveMsgHandler");
   log.trace("Handle: %p", Address);

   std::unique_lock lock(glmMsgHandler);
   MsgHandler *h = (MsgHandler *)Address;
   if (h IS glLastMsgHandler) glLastMsgHandler = h->Prev;
   if (h IS glMsgHandlers) glMsgHandlers = h->Next;
   if (h->Next) h->Next->Prev = h->Prev;
   if (h->Prev) h->Prev->Next = h->Next;

   if (h->Function.defined()) {
      if (h->Function.isScript() and (not h->Function.stale())) {
         ((objScript *)h->Function.Context)->derefProcedure(h->Function);
      }
      h->Function.unpin();
      h->Function.disable();
   }

   return ERR::Terminate;
}

static ResourceManager glResourceMsgHandler = { "Message", &msghandler_free, true };

//********************************************************************************************************************
// Handler for WaitForObjects().  If an object on the list is signalled then it is removed from the list.  A
// message is sent once the list of objects that require signalling has been exhausted.

static void notify_signal_wfo(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   if (!tlMainThread) {
      // Signals and frees can be initiated from any thread, but glWFOList is owned by the main thread.  Defer
      // processing by posting the object ID back to the main queue; see msg_waitforobjects() for the follow-up.
      OBJECTID object_id = Object->UID;
      SendMessage(MSGID::WAIT_FOR_OBJECTS, MSF::NIL,
         std::span((const int8_t *)&object_id, sizeof(object_id)));
      return;
   }

   if (auto lref = glWFOList.find(Object->UID); lref != glWFOList.end()) {
      kt::Log log;
      auto &ref = lref->second;
      log.trace("Object #%d has been signalled from action %d.", Object->UID, ActionID);

      // Clean up subscriptions and clear the signal

      UnsubscribeAction(ref.Object, AC::Free, nullptr);
      UnsubscribeAction(ref.Object, AC::Signal, nullptr);
      ref.Object->clearFlag(NF::SIGNALLED);

      glWFOList.erase(lref);

      if ((glWFOAnySignal) or (glWFOList.empty())) {
         glWFOSignalReceived = true;
         log.trace(glWFOAnySignal ? "An object was signalled." : "All objects signalled.");
         SendMessage(MSGID::WAIT_FOR_OBJECTS, MSF::NIL, {}); // Will result in ProcessMessages() terminating
      }
   }
}

//********************************************************************************************************************
// Handler for MSGID::WAIT_FOR_OBJECTS messages, called from ProcessMessages() on the main thread.  A message with
// no payload indicates that the monitored object list is exhausted and the WaitForObjects() message loop can
// terminate.  A message carrying an object ID is a signal or free that occurred on a child thread, deferred by
// notify_signal_wfo() so that glWFOList is only ever modified by the main thread.

ERR msg_waitforobjects(APTR Custom, int MsgID, int MsgType, APTR Message, int MsgSize)
{
   if ((Message) and (MsgSize >= int(sizeof(OBJECTID)))) {
      auto object_id = ((OBJECTID *)Message)[0];
      if (auto lref = glWFOList.find(object_id); lref != glWFOList.end()) {
         kt::Log log;
         bool signal_received = false;
         kt::ScopedObjectLock lock(object_id); // Locking by ID fails safely if the object has been freed
         if (lock.granted()) {
            if (lock.obj->defined(NF::SIGNALLED)) {
               log.trace("Object #%d was signalled from a child thread.", object_id);
               UnsubscribeAction(lock.obj, AC::Free, nullptr);
               UnsubscribeAction(lock.obj, AC::Signal, nullptr);
               lock.obj->clearFlag(NF::SIGNALLED);
               glWFOList.erase(lref);
               signal_received = true;
            }
            // An unsignalled object indicates a stale message from an earlier wait; leave it monitored.
         }
         else if ((lock.error IS ERR::MarkedForDeletion) or (lock.error IS ERR::DoesNotExist) or
                  (lock.error IS ERR::NoMatchingObject)) {
            // The object was freed - or its destruction is already guaranteed - on the child thread.  Freeing is
            // equivalent to a signal, so the entry is removed; any remaining subscriptions die with the object.
            log.trace("Object #%d was freed from a child thread.", object_id);
            glWFOList.erase(lref);
            signal_received = true;
         }
         else {
            // A transient lock failure (e.g. time-out).  Requeue the deferral so that the wake-up is not lost;
            // an indefinite WaitForObjects() would otherwise sleep forever on an already-signalled object.
            log.trace("Deferred signal for object #%d requeued (%s).", object_id, GetErrorMsg(lock.error));
            SendMessage(MSGID::WAIT_FOR_OBJECTS, MSF::NIL,
               std::span((const int8_t *)&object_id, sizeof(object_id)));
         }

         if (((glWFOAnySignal) and (signal_received)) or (glWFOList.empty())) {
            glWFOSignalReceived = true;
            return ERR::Terminate;
         }
      }
      return ERR::Okay; // Never terminate the message loop on a stale deferral
   }

   return ERR::Terminate;
}

/*********************************************************************************************************************

-FUNCTION-
AddMsgHandler: Adds a new message handler for processing incoming messages.

This function allows handlers to be added for the interception of incoming messages.  Message handling works as follows:

During a call to ~ProcessMessages(), each incoming message will be scanned to determine if a message handler is able
to process that message.  All handlers that accept the message type will be called with a copy of the message
structure and any additional data.  The message is then removed from the message queue.

When calling AddMsgHandler(), the `MsgType` acts as a filter so that only messages with the same type identifier will
be passed to the handler.  The `Routine` parameter must point to the function handler, which will follow this definition:

<pre>ERR handler(APTR Meta, MSGID MsgID, INT MsgType, APTR Message, INT MsgSize)</pre>

The handler must return `ERR::Okay` if the message was handled.  This means that the message will not be passed to message
handlers that are yet to receive the message.  Throw `ERR::NothingDone` if the message has been ignored or `ERR::Continue`
if the message was processed but may be analysed by other handlers.  Throw `ERR::Terminate` to break the current
~ProcessMessages() loop.  When using Tiri, this is best achieved by writing `check(errorcode)` in the handler.

The handler will be identified by a unique pointer returned in the Handle parameter.  This handle will be garbage
collected or can be passed to ~FreeResource() once it is no longer required.

-INPUT-
int(MSGID) MsgType: The message type that the handler will intercept.  If zero, all incoming messages are passed to the handler.
ptr(func) Routine: Refers to the function that will handle incoming messages.
!resource(MsgHandler) Handle: The resulting handle of the new message handler - retain for ~FreeResource().

-ERRORS-
Okay: Message handler successfully processed.
NullArgs
AllocMemory
Lock

-TAGS-
caller-owns-result, creates-resource, callback-held, blocking
-END-

*********************************************************************************************************************/

ERR AddMsgHandler(MSGID MsgType, FUNCTION *Routine, MsgHandler **Handle)
{
   kt::Log log(__FUNCTION__);

   if (!Routine) return log.warning(ERR::NullArgs);

   log.branch("MsgType: %d", int(MsgType));

   std::unique_lock lock(glmMsgHandler);
   MsgHandler *handler;
   if (!AllocResource(sizeof(MsgHandler), MEM::NIL, (APTR *)&handler, &glResourceMsgHandler)) {
      handler->Prev     = nullptr;
      handler->Next     = nullptr;
      handler->MsgType  = MsgType;
      handler->Function = *Routine;
      handler->Function.pin();

      if (!glMsgHandlers) glMsgHandlers = handler;
      else {
         if (glLastMsgHandler) glLastMsgHandler->Next = handler;
         handler->Prev = glLastMsgHandler;
      }

      glLastMsgHandler = handler;

      if (Handle) *Handle = handler;
      return ERR::Okay;
   }
   else {
      Routine->consume();
      return log.warning(ERR::AllocMemory);
   }
}

/*********************************************************************************************************************

-FUNCTION-
ProcessMessages: Processes system messages that are queued in the task's message buffer.

The ProcessMessages() function is used to process the task's message queue.  Messages are dispatched to message
handlers in the order in which they arrived and the queue is emptied so that space is available for more messages.

Responding to incoming messages is a vital process - the queue is the standard means of communication between your task
and the rest of the system and other tasks within it.  Failing to call the ProcessMessages() function on a regular
basis may cause a back-log of messages to be generated, as well as causing problems with areas such as the graphical
interface. If an area of your program is likely to loop continuously for a measurable period of time without returning,
consider calling ProcessMessages() at a rate of 50 times per second to ensure that incoming messages are processed.

User messages that are on the queue are passed to message handlers.  If no message handler exists to interpret the
message, then it is removed from the queue without being processed. Message handlers are added with the
~AddMsgHandler() function.  If a message handler returns the error code `ERR::Terminate`, then ProcessMessages()
will stop processing the queue and returns immediately with `ERR::Okay`.

If a message with a `MSGID::QUIT` ID is found on the queue, then the function returns immediately with the error code
`ERR::Terminate`.  The program must respond to the terminate request by exiting immediately.

-INPUT-
int(PMF) Flags: Optional flags are specified here (clients should set a value of zero).
int Timeout: A Timeout value, measured in milliseconds.  If zero, the function will return as soon as all messages on the queue are processed.  If less than zero, the function does not return until a request for termination is received or a user message requires processing.

-ERRORS-
Okay:
OutsideMainThread:
Recursion:
SystemLocked:
Terminate: A `MSGID::QUIT` message type was found on the message queue.
Timeout:
AccessObject
NoSupport

-TAGS-
main-thread-only, blocking, callback-inlines
-END-

*********************************************************************************************************************/

ERR ProcessMessages(PMF Flags, int Timeout)
{
   kt::Log log(__FUNCTION__);

   // Message processing is only possible from the main thread (for system design and synchronisation reasons)
   if (!tlMainThread) return log.warning(ERR::OutsideMainThread);

   // Ensure that all resources allocated by sub-routines are assigned to the Task object by default.
   // Note: Don't use SwitchContext here as it retains a permanent lock on the task (client threads need access to it).

   SetObjectContext(glCurrentTask, nullptr, AC::NIL);

   // This recursion blocker prevents ProcessMessages() from being called to breaking point.  Excessive nesting can
   // occur on occasions where ProcessMessages() sends an action to an object that performs some activity before it
   // makes a nested call to ProcessMessages(), which in turn might result in more processing and then eventually
   // a recursive effect if we are unlucky enough.
   //
   // You can also use this feature to block messages from being processed, e.g. during notifications.  Simply set
   // tlMsgRecursion to 255.

   if (tlMsgRecursion >= 255) {
      //log.msg("Do not call this function when inside a notification routine.");
   }
   else if (tlMsgRecursion > 8) {
      tlContext.pop_back();
      return ERR::Recursion;
   }

   tlMsgRecursion++;

   int64_t timeout_end;
   if (Timeout IS -1) timeout_end = 0x7fffffffffffffffLL; // Infinite loop
   else timeout_end = PreciseTime() + ((int64_t)Timeout * 1000LL);

   log.traceBranch("Flags: $%.8x, Timeout: %d", int(Flags), Timeout);

   ERR returncode = ERR::Okay;
   bool breaking = false;
   ERR error;

   std::unique_lock granted(glmMsgHandler); // A persistent lock on message handlers is optimal

   std::vector<TaskMessage> local_batch;
   local_batch.reserve(MESSAGE_BATCH_LIMIT);

   do { // Standard message handler for the core process.
      // Call all objects on the timer list (managed by SubscribeTimer()).  To manage timer locking cleanly, the loop
      // is restarted after each client call.  To prevent more than one call per cycle, the glTimerCycle is used to
      // prevent secondary calls.

      glTimerCycle++;
timer_cycle:
      if ((glTaskState IS TSTATE::STOPPING) and ((Flags & PMF::SYSTEM_NO_BREAK) IS PMF::NIL));
      else if (glmTimer.try_lock_for(200ms)) {
         int64_t current_time = PreciseTime();
         for (auto timer=glTimers.begin(); timer != glTimers.end(); ) {
            if ((timer->Subscriber) and (not timer->Locked) and (timer->Subscriber->terminating())) {
               // Sweep orphaned subscriptions ahead of the deadline gate; a subscriber freed during its own
               // callback would otherwise retain its entry and weak pin until the next interval elapsed.
               // Locked entries belong to a dispatcher mid-callback and are left for it to remove.
               if (timer->Routine.isScript() and (not timer->Routine.stale())) {
                  ((objScript *)timer->Routine.Context)->derefProcedure(timer->Routine);
               }
               if (timer->Routine.defined()) timer->Routine.unpin();
               timer->Subscriber->unpinWeak();
               timer = glTimers.erase(timer);
               continue;
            }
            if (current_time < timer->NextCall) { timer++; continue; }
            if (timer->Cycle IS glTimerCycle) { timer++; continue; }
            if (timer->Routine.releaseIfStale()) {
               if (timer->Subscriber) timer->Subscriber->unpinWeak();
               timer = glTimers.erase(timer);
               continue;
            }

            int64_t elapsed = current_time - timer->LastCall;

            if (timer->PendingInterval) {
               timer->Interval = timer->PendingInterval;
               timer->PendingInterval = 0;
            }

            timer->NextCall += timer->Interval;
            if (timer->NextCall < current_time) timer->NextCall = current_time;
            timer->LastCall = current_time;
            timer->Cycle = glTimerCycle;

            timer->Locked = true; // Prevents termination of the structure irrespective of having a TL_TIMER lock.

            bool relock = false;
            if ((timer->Subscriber) and (timer->Subscriber->terminating())) {
               // Orphaned entry: the subscriber terminated but object_free() could not acquire glmTimer to remove
               // the subscription.  The weak pin keeps the subscriber's header readable for this detection.
               error = ERR::Terminate;
            }
            else if ((timer->Subscriber) and (timer->Subscriber->collecting())) {
               // The subscriber is marked for deletion; skip the callback and leave entry removal to object_free().
               error = ERR::AccessObject;
            }
            else if (timer->Routine.isC()) {
               if (auto subscriber = timer->Subscriber) {
                  if (!LockObject(subscriber, 50)) {
                     if (subscriber->collecting()) {
                        // Re-validate after locking: the preceding checks can race with object_free(), which may
                        // run to completion in the interim, and LockObject()'s fast path will succeed on a zombie
                        // header.  Once the lock is held any new free attempt is deferred, so this test is final.
                        // Refer to AccessObject() for the same pattern.
                        ReleaseObject(subscriber);
                        error = ERR::AccessObject;
                     }
                     else {
                        kt::SwitchContext context(subscriber);

                        auto routine = (ERR (*)(OBJECTPTR, int64_t, int64_t, APTR))timer->Routine.Routine;
                        glmTimer.unlock();
                        relock = true;

                        error = routine(subscriber, elapsed, current_time, timer->Routine.Meta);

                        ReleaseObject(subscriber);
                     }
                  }
                  else error = ERR::AccessObject;
               }
               else { // Internal subscriptions like process_janitor() don't have a subscriber
                  auto routine = (ERR (*)(OBJECTPTR, int64_t, int64_t, APTR))timer->Routine.Routine;
                  glmTimer.unlock();
                  relock = true;
                  error = routine(nullptr, elapsed, current_time, timer->Routine.Meta);
               }
            }
            else if (timer->Routine.isScript()) {
               OBJECTID subscriber_id = timer->Subscriber ? timer->Subscriber->UID : 0;
               if ((timer->Subscriber) and (not subscriber_id)) error = ERR::Terminate; // Zombie; discard orphan
               else {
                  glmTimer.unlock();
                  relock = true;

                  if (sc::Call(timer->Routine, std::to_array<ScriptArg>({
                        { "Subscriber",  subscriber_id, FDF_OBJECTID },
                        { "Elapsed",     elapsed },
                        { "CurrentTime", current_time }
                     }), error) != ERR::Okay) error = ERR::Terminate;
               }
            }
            else error = ERR::Terminate;

            if (relock) {
               // Reacquire glmTimer before clearing Locked.  The moment an entry is observed unlocked,
               // object_free() on another thread is free to erase it and release its pins, so completing the
               // entry here must be mutually exclusive with the concurrent erasers.  Blocking indefinitely is
               // safe because all other holders of glmTimer acquire it with a timeout.
               glmTimer.lock();
            }

            timer->Locked = false;

            if (error IS ERR::Terminate) {
               if (timer->Routine.isScript() and (not timer->Routine.stale())) {
                  ((objScript *)timer->Routine.Context)->derefProcedure(timer->Routine);
               }
               if (timer->Routine.defined()) timer->Routine.unpin();
               if (timer->Subscriber) timer->Subscriber->unpinWeak();

               timer = glTimers.erase(timer);
            }
            else timer++;

            if (relock) {
               glmTimer.unlock();
               goto timer_cycle;
            }
         } // for

         glmTimer.unlock();
      }
      else log.detail("glmTimer lock failed.");

      // Consume queued messages.  Drain a batch from the shared queue under the lock, then process
      // outside the lock to reduce contention with threads calling SendMessage().

      local_batch.clear();

      {
         const std::lock_guard<std::mutex> lock(glQueueLock);
         auto count = std::min(glQueue.size(), MESSAGE_BATCH_LIMIT);
         if (count > 0) {
            for (size_t n = 0; n < count; n++) {
               local_batch.emplace_back(std::move(glQueue.front()));
               glQueue.pop_front();
            }
            glQueuedMessages.fetch_sub(count, std::memory_order_release);
         }
      }

      for (auto &msg : local_batch) {
         if (msg.Type IS MSGID::BREAK) {
            // MSGID::BREAK is intended for breaking out of recursive calls to ProcessMessages(), but
            // not the top-level UI event loop which is broken by MSGID::QUIT.
            if ((Flags & PMF::EVENT_LOOP) IS PMF::NIL) breaking = true;
            else log.trace("Unable to break from core event loop.");
         }

         tlCurrentMsg = &msg;

         for (auto hdl=glMsgHandlers; hdl; hdl=hdl->Next) {
            if ((hdl->MsgType IS MSGID::NIL) or (hdl->MsgType IS msg.Type)) {
               auto result = ERR::NoSupport;
               if (hdl->Function.stale()) continue;
               else if (hdl->Function.isC()) {
                  auto msghandler = (ERR (*)(APTR, int, MSGID, APTR, int))hdl->Function.Routine;
                  if (msg.Size) result = msghandler(hdl->Function.Meta, msg.UID, msg.Type, msg.getBuffer(), msg.Size);
                  else result = msghandler(hdl->Function.Meta, msg.UID, msg.Type, nullptr, 0);
               }
               else if (hdl->Function.isScript()) {
                  if (sc::Call(hdl->Function, std::to_array<ScriptArg>({
                     { "UID",  msg.UID },
                     { "Type", int(msg.Type) },
                     { "Data", msg.getBuffer(), FD_PTR|FD_BUFFER },
                     { "Size", msg.Size, FD_INT|FD_BUFSIZE }
                  }), result) != ERR::Okay) result = ERR::Terminate;
               }

               if (!result) { // If the message was handled, do not pass it to anyone else
                  break;
               }
               else if (result IS ERR::Terminate) { // Terminate the ProcessMessages() loop, but don't quit the program
                  log.trace("Terminate request received from message handler.");
                  timeout_end = 0; // Set to zero to indicate loop terminated
                  break;
               }
            }
         }

         tlCurrentMsg = nullptr;
      }

      // Check for possibly broken child processes

      if (glValidateProcessID) { validate_process(glValidateProcessID); glValidateProcessID = 0; }

      #ifdef _WIN32
         // Process any incoming window messages that occurred during our earlier processing.

         if (tlMainThread) {
            winProcessMessages();
         }
      #endif

      int64_t wait = 0;
      if ((glQueuedMessages.load(std::memory_order_acquire) > 0) or (breaking) or
          ((glTaskState IS TSTATE::STOPPING) and ((Flags & PMF::SYSTEM_NO_BREAK) IS PMF::NIL)));
      else if (timeout_end > 0) {
         // Wait for someone to communicate with us, or stall until an interrupt is due.

         int64_t sleep_time = timeout_end;
         if (auto lock = std::unique_lock{glmTimer, 200ms}) {
            for (const auto &timer : glTimers) {
               if (timer.NextCall < sleep_time) sleep_time = timer.NextCall;
            }
         }

         wait = sleep_time - PreciseTime();
         if (wait > 60 * 60 * 1000000LL) wait = 60 * 60 * 1000000LL; // One hour (limit required for 64-bit to 32-bit reduction)

         if (wait < 0) wait = 0;
      }

      #ifdef _WIN32
         if (tlMainThread) {
            tlMessageBreak = true;  // Break if the host OS sends us a native message
            sleep_task(wait / 1000LL, false); // Even if wait is zero, we still need to clear FD's and call FD hooks
            tlMessageBreak = false;

            if (wait) {
               winProcessMessages();
            }
         }
         else {
         }
      #else
         sleep_task(wait / 1000LL); // Event if wait is zero, we still need to clear FD's and call FD hooks
      #endif

      // Continue the loop?

      if (glQueuedMessages.load(std::memory_order_acquire) > 0) continue; // There are messages left unprocessed
      else if (((glTaskState IS TSTATE::STOPPING) and ((Flags & PMF::SYSTEM_NO_BREAK) IS PMF::NIL)) or (breaking)) {
         log.trace("Breaking message loop.");
         break;
      }
      else if (PreciseTime() >= timeout_end) {
         if (Timeout) {
            log.trace("Breaking message loop - timeout of %dms.", Timeout);
            if (timeout_end > 0) returncode = ERR::Timeout;
         }
         break;
      }
   } while (true);

   if ((glTaskState IS TSTATE::STOPPING) and ((Flags & PMF::SYSTEM_NO_BREAK) IS PMF::NIL)) returncode = ERR::Terminate;

   tlMsgRecursion--;
   tlContext.pop_back();
   return returncode;
}

/*********************************************************************************************************************

-FUNCTION-
ScanMessages: Scans a message queue for multiple occurrences of a message type.

Use the ScanMessages() function to scan the local message queue for information without affecting the state of the
queue.  To use this function effectively, make repeated calls to ScanMessages() to analyse the queue until it returns
an error code other than `ERR::Okay`.

The following example illustrates a scan for `MSGID::QUIT` messages:

<pre>
while (!ScanMessages(&handle, MSGID::QUIT, {})) {
   ...
}
</pre>

Messages will often (but not always) carry data that is relevant to the message type.  To retrieve this data a buffer
must be supplied.  If the `Buffer` is too small, the message data will be trimmed to fit without any further indication.

-INPUT-
&int Handle: Pointer to a 32-bit value that must initially be set to zero.  The ScanMessages() function will automatically update this variable with each call so that it can remember its analysis position.
int(MSGID) Type:   The message type to filter for, or zero to scan all messages in the queue.
^buf(ptr) Buffer: Optional buffer that is large enough to hold a !Message header and any message data.

-ERRORS-
Okay:
NullArgs:
Args: The supplied buffer is too large for the internal message interface.
OutOfRange:
Search: No more messages are left on the queue, or no messages that match the given `Type` are on the queue.

-TAGS-
mutates-input, blocking
-END-

*********************************************************************************************************************/

ERR ScanMessages(int *Handle, MSGID Type, const std::span<int8_t> &Buffer)
{
   kt::Log log(__FUNCTION__);

   if (!Handle) return log.warning(ERR::NullArgs);
   if (not span_size_fits_int(Buffer.size_bytes())) return log.warning(ERR::Args);

   if (*Handle < 0) {
      *Handle = -1;
      return ERR::Search;
   }

   const std::lock_guard<std::mutex> lock(glQueueLock);

   int index = *Handle;
   if (index >= int(glQueue.size())) return ERR::OutOfRange;

   for (auto it = glQueue.begin() + index; it != glQueue.end(); it++) {
      if ((it->Type != MSGID::NIL) and ((it->Type IS Type) or (Type IS MSGID::NIL))) {
         if (Buffer.size_bytes() >= sizeof(Message)) {
            const auto payload_capacity = Buffer.size_bytes() - sizeof(Message);
            const auto payload_size = std::min(payload_capacity, size_t(it->Size));

            Message message = {};
            message.UID  = it->UID;
            message.Type = it->Type;
            message.Size = int(payload_size);
            message.Time = it->Time;
            copymem(&message, Buffer.data(), sizeof(message));
            copymem(it->getBuffer(), Buffer.data() + sizeof(Message), payload_size);
         }

         *Handle = int(std::distance(glQueue.begin(), it)) + 1;
         return ERR::Okay;
      }
   }

   *Handle = -1;
   return ERR::Search;
}

/*********************************************************************************************************************

-FUNCTION-
SendMessage: Add a message to the local message queue.

The SendMessage() function will add a message to the end of the local message queue.  Messages must be associated
with a `Type` identifier and this can help the receiver process any accompanying Data.  Some common message types are
pre-defined, such as `MSGID::QUIT`.  Custom messages should use a unique type ID obtained from ~AllocateID().

-INPUT-
int(MSGID) Type:  The message Type/ID being sent.  Unique type ID's can be obtained from ~AllocateID().
int(MSF) Flags: Optional flags.
buf(ptr) Data: Optional data to copy to the message queue.  An empty buffer sends a message without payload data.

-ERRORS-
Okay: The message was successfully written to the message queue.
Args:

-TAGS-
copies-input, blocking
-END-

*********************************************************************************************************************/

ERR SendMessage(MSGID Type, MSF Flags, const std::span<const int8_t> &Data)
{
   kt::Log log(__FUNCTION__);

   if ((Type IS MSGID::NIL) or (not span_size_fits_int(Data.size_bytes()))) return log.warning(ERR::Args);

   const auto data_size = int(Data.size_bytes());

   if (glLogLevel >= 9) {
      if ((Type IS MSGID::ACTION) and (Data.size_bytes() >= sizeof(ActionMessage))) {
         ActionMessage action = {};
         copymem(Data.data(), &action, sizeof(action));
         if (action.ActionID > AC::NIL) {
            log.branch("Action: %s, Object: %d, Size: %d", ActionTable[int(action.ActionID)].Name,
               action.ObjectID, data_size);
         }
      }
      else log.branch("Type: %d, Data: %p, Size: %d", int(Type), Data.data(), data_size);
   }

   {
      const std::lock_guard<std::mutex> lock(glQueueLock);

      if ((Flags & (MSF::NO_DUPLICATE|MSF::UPDATE)) != MSF::NIL) {
         for (auto it=glQueue.begin(); it != glQueue.end(); it++) {
            if (it->Type IS Type) {
               if ((Flags & MSF::NO_DUPLICATE) != MSF::NIL) return ERR::Okay;
               else {
                  it->setBuffer((APTR)Data.data(), data_size);
                  return ERR::Okay;
               }
            }
         }
      }

      glQueue.emplace_back(Type, (APTR)Data.data(), data_size); // Deque keeps storage stable for re-entrant handlers.
      glQueuedMessages.fetch_add(1, std::memory_order_release);
   }

   wake_task(); // Alert the process to indicate that there are messages available.

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
WaitForObjects: Process incoming messages while waiting on objects to complete their activities.

WaitForObjects() acts as a front-end to ~ProcessMessages(), with an ability to wait for a list of objects that are
expected to signal an end to their activities.  An object can be signalled via the Signal() action, or via termination.
By default, this function will only return once ALL of the objects are signalled or a time-out occurs.  Use the
`PMF::ANY_SIGNAL` flag to return once ANY of the objects are signalled.

It is guaranteed that the message queue will be consumed by ~ProcessMessages() at least once before this function
returns.

Note that if an object has been signalled prior to entry to this function, its signal flag will be cleared and the
object will not be monitored.

If this function is called recursively, the state of the earlier call will be preserved so that it will not be
affected by subsequent calls.

-INPUT-
int(PMF) Flags: Optional flags are specified here.
int Timeout: A time-out value measured in milliseconds.  If this value is negative then no time-out applies and the function will not return until an incoming message or signal breaks it.
struct(*ObjectSignal) ObjectSignals: A null-terminated array of objects to monitor for signals.

-ERRORS-
Okay
OutsideMainThread
MessageOperation
Recursion
SystemLocked
Terminate
Timeout
ExceptionThreshold

-TAGS-
main-thread-only, blocking, callback-inlines

-END-

*********************************************************************************************************************/

ERR WaitForObjects(PMF Flags, int Timeout, ObjectSignal *ObjectSignals)
{
   // Refer to the Task class for the message interception routines
   kt::Log log(__FUNCTION__);

   std::unordered_map<OBJECTID, ObjectSignal> saved_list;
   bool saved_any_signal = glWFOAnySignal;
   bool saved_signal_received = glWFOSignalReceived;

   // Message processing is only possible from the main thread (for system design and synchronisation reasons)
   if (!tlMainThread) return log.warning(ERR::OutsideMainThread);

   log.branch("Flags: $%.8x, Timeout: %d, Signals: %p", int(Flags), Timeout, ObjectSignals);

   // Set the current task as the context to ensure predictable behaviour.  Note: Don't use SwitchContext here as
   // it retains a lock on the task and we don't want that.

   SetObjectContext(glCurrentTask, nullptr, AC::NIL);

   auto error = ERR::Okay;

   if (!glWFOList.empty()) std::swap(saved_list, glWFOList);
   glWFOList.clear();
   glWFOAnySignal = (Flags & PMF::ANY_SIGNAL) != PMF::NIL;
   glWFOSignalReceived = false;

   if (ObjectSignals) {
      for (int i=0; ((!error) and (not glWFOSignalReceived) and (ObjectSignals[i].Object)); i++) {
         kt::ScopedObjectLock lock(ObjectSignals[i].Object); // For thread safety

         if (lock.granted()) {
            if (ObjectSignals[i].Object->defined(NF::SIGNALLED)) {
               // Objects that have already been signalled do not require monitoring and we switch off the
               // signal flag.
               ObjectSignals[i].Object->clearFlag(NF::SIGNALLED);
               if (glWFOAnySignal) glWFOSignalReceived = true;
            }
            else {
               // NB: An object being freed is treated as equivalent to it receiving a signal.
               // Refer to notify_signal_wfo() for notification handling and clearing of signals.
               log.detail("Monitoring object #%d", ObjectSignals[i].Object->UID);
               if (!SubscribeAction(ObjectSignals[i].Object, AC::Free, C_FUNCTION(notify_signal_wfo))) {
                  if (!SubscribeAction(ObjectSignals[i].Object, AC::Signal, C_FUNCTION(notify_signal_wfo))) {
                     glWFOList.insert(std::make_pair(ObjectSignals[i].Object->UID, ObjectSignals[i]));
                  }
                  else {
                     UnsubscribeAction(ObjectSignals[i].Object, AC::Free, nullptr);
                     error = ERR::MessageOperation;
                  }
               }
               else error = ERR::MessageOperation;
            }
         }
      }
   }

   if ((!error) and (not glWFOSignalReceived) and (not glWFOList.empty())) {
      if (Timeout < 0) { // No time-out will apply
         while ((not glWFOSignalReceived) and (not glWFOList.empty()) and (!error)) {
            error = ProcessMessages(Flags, -1);
         }
      }
      else {
         auto current_time = PreciseTime();
         auto end_time = current_time + (Timeout * 1000LL);
         while ((not glWFOSignalReceived) and (not glWFOList.empty()) and (current_time < end_time) and (!error)) {
            log.detail("Waiting on %d objects.", (int)glWFOList.size());
            error = ProcessMessages(Flags, (end_time - current_time) / 1000LL);
            current_time = PreciseTime();
         }
      }

      if ((!error) and (not glWFOSignalReceived) and (not glWFOList.empty())) error = ERR::Timeout;
   }
   else {
      // At least one call to ProcessMessages() is needed (the caller's message loop may
      // be designed on this basis).
      error = ProcessMessages(Flags, 0);
   }

   if (not glWFOList.empty()) { // Clean up if there are dangling subscriptions
      for (auto &ref : glWFOList) {
         // Lock by ID rather than pointer; a child thread may have freed the object with its
         // deferred notification still unprocessed (see msg_waitforobjects()).
         kt::ScopedObjectLock lock(ref.first);
         if (lock.granted()) {
            UnsubscribeAction(lock.obj, AC::Free, nullptr);
            UnsubscribeAction(lock.obj, AC::Signal, nullptr);
         }
      }
      glWFOList.clear();
   }

   if (!saved_list.empty()) std::swap(glWFOList, saved_list);
   glWFOAnySignal = saved_any_signal;
   glWFOSignalReceived = saved_signal_received;

   if ((error > ERR::ExceptionThreshold) and (error != ERR::Timeout)) log.warning(error);

   tlContext.pop_back();
   return error;
}

//********************************************************************************************************************
// This is the equivalent internal routine to SendMessage(), for the purpose of sending messages to other threads.

#ifdef _WIN32
ERR send_thread_msg(WINHANDLE Handle, MSGID Type, APTR Data, int Size)
#else
ERR send_thread_msg(int Handle, MSGID Type, APTR Data, int Size)
#endif
{
   kt::Log log(__FUNCTION__);
   ERR error;

   log.function("Type: %d, Data: %p, Size: %d", int(Type), Data, Size);

   TaskMessage msg;
   msg.UID  = ++glUniqueMsgID;
   msg.Type = Type;
   msg.Size = Size;
   msg.Time = PreciseTime();

#ifdef _WIN32
   int write = sizeof(msg);
   if (!winWritePipe(Handle, &msg, &write)) { // Write the header first.
      if ((Data) and (Size > 0)) {
         // Write the main message.
         write = Size;
         if (!winWritePipe(Handle, Data, &write)) {
            error = ERR::Okay;
         }
         else error = ERR::Write;
      }
      else error = ERR::Okay;
   }
   else error = ERR::Write;
#else
   int64_t end_time = (PreciseTime() / 1000LL) + 10000LL;
   error = write_nonblock(Handle, &msg, sizeof(msg), end_time);
   if ((!error) and (Data) and (Size > 0)) { // Write the main message.
      error = write_nonblock(Handle, Data, Size, end_time);
   }
#endif

   if (error != ERR::Okay) log.warning(error);
   return error;
}

//********************************************************************************************************************
// Simplifies the process of writing to an FD that is set to non-blocking mode (typically a socket or pipe).  An
// end-time is required so that a timeout will be signalled if the reader isn't keeping the buffer clear.

#ifdef __unix__
ERR write_nonblock(int Handle, APTR Data, int Size, int64_t EndTime)
{
   int offset = 0;
   ERR error = ERR::Okay;

   while ((offset < Size) and (!error)) {
      int write_size = Size - offset;
      if (write_size > 1024) write_size = 1024;  // Limiting the size will make the chance of an EWOULDBLOCK error less likely.
      int len = write(Handle, (char *)Data+offset, write_size);
      if (len >= 0) offset += len;
      if (offset IS Size) break;

      if (len IS -1) { // An error occurred.
         if ((errno IS EAGAIN) or (errno IS EWOULDBLOCK)) { // The write() failed because it would have blocked.  Try again!
            fd_set wfds;
            struct timeval tv;
            while (((PreciseTime() / 1000LL) < EndTime) and (!error)) {
               FD_ZERO(&wfds);
               FD_SET(Handle, &wfds);
               tv.tv_sec = (EndTime - (PreciseTime() / 1000LL)) / 1000LL;
               tv.tv_usec = 0;
               int total = select(Handle + 1, nullptr, &wfds, nullptr, &tv);
               if (total IS -1) error = ERR::SystemCall;
               else if (!total) error = ERR::Timeout;
               else break;
            }
         }
         else if ((errno IS EINVAL) or (errno IS EBADF) or (errno IS EPIPE)) { error = ERR::InvalidHandle; break; }
         else { error = ERR::Write; break; }
      }

      if ((PreciseTime() / 1000LL) > EndTime) {
         error = ERR::Timeout;
         break;
      }
   }

   return error;
}
#endif

/*********************************************************************************************************************

-FUNCTION-
UpdateMessage: Updates the data of any message that is queued.

The UpdateMessage() function provides a facility for updating the content of existing messages on the local queue.
The client must provide the ID of the message to update and the new message Type and/or Data to set against the message.
Non-empty `Data` replaces the complete existing payload, while an empty buffer leaves that payload unchanged.

-INPUT-
int Message:   The ID of the message that will be updated.
int(MSGID) Type: The type of the message.
buf(ptr) Data: Optional replacement data for the message.  An empty buffer leaves the existing payload unchanged.

-ERRORS-
Okay:   The message was successfully updated.
NullArgs:
Args: The supplied data is too large for the internal message interface.
Search: The supplied `Message` ID does not refer to a message in the queue.

-TAGS-
copies-input, blocking
-END-

*********************************************************************************************************************/

ERR UpdateMessage(int MessageID, MSGID Type, const std::span<const int8_t> &Data)
{
   if (!MessageID) return ERR::NullArgs;
   if (not span_size_fits_int(Data.size_bytes())) return ERR::Args;

   const auto data_size = int(Data.size_bytes());

   const std::lock_guard<std::mutex> lock(glQueueLock);

   for (auto it=glQueue.begin(); it != glQueue.end(); it++) {
      if (it->UID != MessageID) continue;
      if (not Data.empty()) it->setBuffer((APTR)Data.data(), data_size);
      if (Type != MSGID::NIL) it->Type = Type;
      return ERR::Okay;
   }

   return ERR::Search;
}

//********************************************************************************************************************
// sleep_task() - Unix version

#ifdef __unix__
ERR sleep_task(int Timeout)
{
   kt::Log log(__FUNCTION__);

   if (!tlMainThread) {
      log.warning("Only the main thread can call this function.");
      return ERR::MessageOperation;
   }

   register_sleep(Timeout);

   struct timeval tv;
   struct timespec time;
   fd_set fread, fwrite;

   //log.trace("Time-out: %d", Timeout);

   int maxfd = -1;
   if (!glFDTable.empty()) {
      FD_ZERO(&fread);
      FD_ZERO(&fwrite);
      for (auto &fd : glFDTable) {
         if ((fd.Flags & RFD::STOP_RECURSE) != RFD::NIL) continue; // This is an internally managed flag to prevent recursion
         if ((fd.Flags & RFD::READ) != RFD::NIL) FD_SET(fd.FD, &fread);
         if ((fd.Flags & RFD::WRITE) != RFD::NIL) FD_SET(fd.FD, &fwrite);
         //log.trace("Listening to %d, Read: %d, Write: %d, Routine: %p, Flags: $%.2x", fd.FD, (fd.Flags & RFD::READ) ? 1 : 0, (fd.Flags & RFD::WRITE) ? 1 : 0, fd.Routine, fd.Flags);
         if (fd.FD > maxfd) maxfd = fd.FD;

         if ((fd.Flags & RFD::ALWAYS_CALL) != RFD::NIL) {
            if (fd.Routine) fd.Routine(fd.FD, fd.Data);
         }
         else if ((fd.Flags & RFD::RECALL) != RFD::NIL) {
            // If the RECALL flag is set against an FD, it was done so because the subscribed routine needs to manually check
            // for incoming/outgoing data.  These are considered 'one-off' checks, so the subscriber will need to set the RECALL flag
            // again if it wants this service maintained.
            //
            // See the SSL support routines as an example of this requirement.

            fd.Flags &= ~RFD::RECALL; // Turn off the recall flag as each call is a one-off

            if ((fd.Flags & RFD::ALLOW_RECURSION) IS RFD::NIL) {
               fd.Flags |= RFD::STOP_RECURSE;
            }

            if (fd.Routine) {
               fd.Routine(fd.FD, fd.Data);

               if ((fd.Flags & RFD::RECALL) != RFD::NIL) {
                  // If the RECALL flag was re-applied by the subscriber, we need to employ a reduced timeout so that the subscriber doesn't get 'stuck'.

                  if (Timeout > 10) Timeout = 10;
               }
            }

            fd.Flags &= ~RFD::STOP_RECURSE;
         }
      }
   }

   int result = 0;
   if (Timeout < 0) { // Sleep indefinitely
      if (!glFDTable.empty()) result = select(maxfd + 1, &fread, &fwrite, nullptr, nullptr);
      else pause();
   }
   else if (Timeout IS 0) { // A zero second timeout means that we just poll the FD's and call them if they have data.  This is really useful for periodically flushing the FD's.
      if (!glFDTable.empty()) {
         tv.tv_sec = 0;
         tv.tv_usec = 0;
         result = select(maxfd + 1, &fread, &fwrite, nullptr, &tv);
      }
   }
   else {
      if (!glFDTable.empty()) {
         tv.tv_sec = Timeout / 1000;
         tv.tv_usec = (Timeout - (tv.tv_sec * 1000)) * 1000;
         result = select(maxfd + 1, &fread, &fwrite, nullptr, &tv);
      }
      else {
         if (Timeout > MAX_MSEC) Timeout = MAX_MSEC; // Do not sleep too long, in case the Linux kernel doesn't wake us when signalled (kernel 2.6)
         time.tv_sec  = Timeout / 1000;
         time.tv_nsec = (Timeout - (time.tv_sec * 1000)) * 1000;
         nanosleep(&time, nullptr);
      }
   }

   uint8_t buffer[64];
   if (result > 0) {
      glFDProtected++;
      for (auto &fd : glFDTable) {
         if ((fd.Flags & RFD::READ) != RFD::NIL) {  // Readable FD support
            if (FD_ISSET(fd.FD, &fread)) {
               if ((fd.Flags & RFD::ALLOW_RECURSION) IS RFD::NIL) {
                  fd.Flags |= RFD::STOP_RECURSE;
               }

               if (fd.Routine) {
                  fd.Routine(fd.FD, fd.Data);
               }
               else if (fd.FD IS glSocket) {
                  socklen_t socklen;
                  struct sockaddr_un *sockpath = get_socket_path(glProcessID, &socklen);
                  recvfrom(glSocket, &buffer, sizeof(buffer), 0, (struct sockaddr *)sockpath, &socklen);
               }
               else while (read(fd.FD, &buffer, sizeof(buffer)) > 0);
            }

            fd.Flags &= ~RFD::STOP_RECURSE;
         }

         if ((fd.Flags & RFD::WRITE) != RFD::NIL) { // Writeable FD support
            if (FD_ISSET(fd.FD, &fwrite)) {
               if ((fd.Flags & RFD::ALLOW_RECURSION) IS RFD::NIL) fd.Flags |= RFD::STOP_RECURSE;
               if (fd.Routine) fd.Routine(fd.FD, fd.Data);
            }

            fd.Flags &= ~RFD::STOP_RECURSE;
         }
      }
      glFDProtected--;

      if ((!glRegisterFD.empty()) and (!glFDProtected)) {
         for (auto &record : glRegisterFD) {
            RegisterFD(record.FD, record.Flags, record.Routine, record.Data);
         }
         glRegisterFD.clear();
      }
   }
   else if (result IS -1) {
      if (errno IS EINTR); // Interrupt caught during sleep
      else if (errno IS EBADF) {
         // At least one of the file descriptors is invalid - it is most likely that the file descriptor was closed and the
         // code responsible did not de-register the descriptor.

         struct stat info;
         for (auto &fd : glFDTable) {
            if (fstat(fd.FD, &info) < 0) {
               if (errno IS EBADF) {
                  log.warning("FD %d was closed without a call to deregister it.", fd.FD);
                  RegisterFD(fd.FD, RFD::REMOVE|RFD::READ|RFD::WRITE|RFD::EXCEPT, nullptr, nullptr);
                  break;
               }
            }
         }
      }
      else log.warning("select() error %d: %s", errno, strerror(errno));
   }

   deregister_sleep();
   return ERR::Okay;
}
#endif

//********************************************************************************************************************
// sleep_task() - Windows version

#ifdef _WIN32
ERR sleep_task(int Timeout, int8_t SystemOnly)
{
   kt::Log log(__FUNCTION__);

   if (!tlMainThread) {
      log.warning("Only the main thread can call this function.");
      return ERR::MessageOperation;
   }

   //log.traceBranch("Time-out: %d, TotalFDs: %d", Timeout, glTotalFDs);

   register_sleep(Timeout);

   int64_t time_end;
   if (Timeout < 0) {
      Timeout = -1; // A value of -1 means to wait indefinitely
      time_end = 0x7fffffffffffffffLL;
   }
   else time_end = (PreciseTime() / 1000LL) + Timeout;

   while (true) {
      // This subroutine will wait until either:
      //   Something is received on a registered WINHANDLE
      //   The thread-lock is released by another task (see wake_task).
      //   A window message is received (if tlMessageBreak is true)

      WINHANDLE stack_handles[32];
      auto heap_storage = (glFDTable.size() + 1 > 32)
         ? std::make_unique<WINHANDLE[]>(glFDTable.size() + 1) : nullptr;
      auto handles = heap_storage ? heap_storage.get() : stack_handles;
      handles[0] = get_threadlock();
      int total = 1;

      if ((SystemOnly) and (!tlMessageBreak)) {
         log.trace("Sleeping on process semaphore only.");
      }
      else {
         for (auto it = glFDTable.begin(); it != glFDTable.end(); ) {
            auto &fd = *it;
            if ((fd.Flags & RFD::SOCKET) != RFD::NIL); // Network sockets are managed by the Network module backend.
            else if ((fd.Flags & RFD::ALWAYS_CALL) != RFD::NIL) {
               if (fd.Routine) fd.Routine(fd.FD, fd.Data);
            }
            else if ((fd.Flags & (RFD::READ|RFD::WRITE|RFD::EXCEPT)) != RFD::NIL) {
               handles[total++] = fd.FD;
            }
            else {
               log.warning("FD %" PRId64 " has no READ/WRITE/EXCEPT flag setting - de-registering.", (int64_t)fd.FD);
               it = glFDTable.erase(it);
               continue;
            }
            it++;
         }
      }

      //if (Timeout > 0) log.trace("Sleeping on %d handles for up to %dms.  MsgBreak: %d", total, Timeout, tlMessageBreak);

      int sleeptime = time_end - (PreciseTime() / 1000LL);
      if (sleeptime < 0) sleeptime = 0;

      int i = winWaitForObjects(total, handles, sleeptime, tlMessageBreak);

      // Return Codes/Reasons for breaking:
      //
      //   -1 = Timed out
      //   -2 = Error (usually a bad handle, like a recently freed handle is still on handle list).
      //   -3 = Message received in windows message queue or event system.
      //    0 = Task semaphore signalled
      //   >0 = Handle signalled

      if ((i > 0) and (i < total)) {
         // Process only the handle routine that was signalled: NOTE: This is potentially an issue if the handle is
         // early on in the list and is being frequently signalled - it will mean that the other handles aren't
         // going to get signalled until the earlier one stops being signalled.

         glFDProtected++;
         for (auto it = glFDTable.begin(); it != glFDTable.end(); it++) {
            if (it->FD != handles[i]) continue;
            if (it->Routine) it->Routine(it->FD, it->Data);
            glFDTable.splice(glFDTable.end(), glFDTable, it); // Move this record to the end of glFDTable
            break;
         }
         glFDProtected--;

         if ((!glRegisterFD.empty()) and (!glFDProtected)) {
            for (auto &record : glRegisterFD) {
               RegisterFD(record.FD, record.Flags, record.Routine, record.Data);
            }
            glRegisterFD.clear();
         }

         break;
      }
      else if (i IS -2) {
         log.warning("WaitForObjects() failed, bad handle %" PRId64 ".  Deregistering automatically.", (int64_t)handles[0]);
         RegisterFD((HOSTHANDLE)handles[0], RFD::REMOVE|RFD::READ|RFD::WRITE|RFD::EXCEPT, nullptr, nullptr);
      }
      else if (i IS -4) {
         log.warning("WaitForObjects() failure - error not handled.");
      }
      else if (i IS -1) {
         // On Windows we can sometimes receive a timeout return code despite no change to the system time, so don't break.
         // The most common reason for this is that a callback-based interrupt that uses a timer has been triggered.
      }
      else {
         //log.trace("A message in the windows message queue needs processing.");
         break; // Message in windows message queue that needs to be processed, so break
      }

      int64_t systime = (PreciseTime() / 1000LL);
      if (systime < time_end) Timeout = time_end - systime;
      else break;
   }

   deregister_sleep();
   return ERR::Okay;
}

#endif // _WIN32

//********************************************************************************************************************
// This function complements sleep_task().  It is useful for waking the main thread of a process when it is waiting for
// new messages to come in.
//
// It's not a good idea to call wake_task() while locks are active because the Core might choose to instantly switch
// to the foreign task when we wake it up.  Having a lock would then increase the likelihood of delays and time-outs.

#ifdef __unix__ // TLS data for the wake_task() socket.
static pthread_key_t keySocket;
static pthread_once_t keySocketOnce = PTHREAD_ONCE_INIT;
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
static void thread_socket_free(void *Socket) { close(PTR_TO_HOST(Socket)); }
static void thread_socket_init(void) { pthread_key_create(&keySocket, thread_socket_free); }
#endif

static ERR wake_task(void)
{
   kt::Log log(__FUNCTION__);

   if (!glCurrentTask) return ERR::Okay;

#ifdef __unix__

   // Sockets are the preferred method because they use FD's.  This plays nice with the traditional message and
   // locking system employed by sleep_task() and can be used in conjunction with FD's for things like incoming
   // network messages.
   //
   // NOTE: If sockets are not available on the host then you can use a mutex for sleeping, BUT this would mean that
   // every FD has to be given its own thread for processing.

   uint8_t msg = 1;

   // Each thread gets its own comm socket for dispatch, because allowing them to all use the same socket has been
   // discovered to cause problems.  The use of pthread keys also ensures that the socket FD is automatically closed
   // when the thread is removed.

   #pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
   HOSTHANDLE tlSendSocket;
   pthread_once(&keySocketOnce, thread_socket_init);
   if (!(tlSendSocket = PTR_TO_HOST(pthread_getspecific(keySocket)))) {
      if ((tlSendSocket = socket(PF_UNIX, SOCK_DGRAM, 0)) != -1) {
         fcntl(tlSendSocket, F_SETFL, O_NONBLOCK);
         pthread_setspecific(keySocket, (void *)tlSendSocket);
      }
      else {
         log.warning("Failed to create a new socket communication point.");
         return ERR::SystemCall;
      }
   }

   // Place a single character in the destination task's socket to indicate that there are messages to be processed.

   socklen_t socklen;
   struct sockaddr_un *sockpath = get_socket_path(glProcessID, &socklen);
   if (sendto(tlSendSocket, &msg, sizeof(msg), MSG_DONTWAIT, (struct sockaddr *)sockpath, socklen) IS -1) {
      if (errno != EAGAIN) {
         log.warning("sendto() failed: %s", strerror(errno));
      }
   }

#elif _WIN32

   wake_waitlock(glTaskLock, 1);

#endif

   return ERR::Okay;
}

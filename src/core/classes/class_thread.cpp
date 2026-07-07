/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
Thread: Threads are created and managed by the Thread class.

The Thread class provides the means to execute and manage threads within an application.

The following code illustrates how to create a temporary thread that is automatically destroyed after the
`thread_entry()` function has completed:

<pre>
static ERR thread_entry(objThread *Thread) {
   return ERR::Okay;
}

objThread::create thread = { fl::Routine(thread_entry), fl::Flags(THF::AUTO_FREE) };
if (thread.ok()) thread->activate();
</pre>

To initialise the thread with data, call #SetData() prior to execution and read the #Data field from within the
thread routine.

-END-

*********************************************************************************************************************/

#ifdef __unix__
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "../defs.h"
#include <kotuku/main.h>

thread_local int8_t tlThreadCrashed;
thread_local extThread *tlThreadRef;

static void thread_entry_cleanup(void *);

struct ThreadEntryCleanupGuard {
   ~ThreadEntryCleanupGuard() {
      thread_entry_cleanup(nullptr);
   }
};

//********************************************************************************************************************
// Called whenever a MSGID::THREAD_CALLBACK message is caught by ProcessMessages().  See thread_entry() for usage.

ERR msg_threadcallback(APTR Custom, int MsgID, int MsgType, APTR Message, int MsgSize)
{
   kt::Log log(__FUNCTION__);

   auto msg = (ThreadMessage *)Message;
   auto uid = msg->ThreadID;

   log.branch("Executing completion callback for thread #%d", uid);

   if (msg->Callback.stale()) clear_callback(msg->Callback);
   else if (msg->Callback.isC()) {
      auto callback = (void (*)(OBJECTID, APTR))msg->Callback.Routine;
      callback(uid, msg->Callback.Meta);
   }
   else if (msg->Callback.isScript()) {
      ScopedObjectLock script(msg->Callback.Context, 5000);
      if (script.granted()) sc::Call(msg->Callback, std::to_array<ScriptArg>({ { "Thread", uid, FD_OBJECTID } }));
   }

   if (msg->Callback.defined()) {
      msg->Callback.unpin();
      msg->Callback.clear();
   }

   // NB: Assume 'msg' is unstable after this point because the callback may have modified the message table.

   ScopedObjectLock<extThread> thread(uid, 10000);
   if (thread.granted()) {
      thread->InterruptThreadID.store(0, std::memory_order_release);
      // NB: If a client wants notification of the thread ending, they can use WaitForObjects()
      // if not using callbacks.
      thread->Active = false;
      if ((thread->Flags & THF::AUTO_FREE) != THF::NIL) FreeResource(*thread);
      else acSignal(*thread); // Convenience for the client
   }
   else log.warning(ERR::AccessObject);

   return ERR::Okay;
}

//********************************************************************************************************************
// Cleanup on completion of a thread.  Note that this will also run in the event that the thread throws an exception.

static void thread_entry_cleanup(void *Arg)
{
   if (tlThreadCrashed) {
      kt::Log("thread_cleanup").error("A thread in this program has crashed.");
      if (tlThreadRef) {
         tlThreadRef->InterruptThreadID.store(0, std::memory_order_release);
         tlThreadRef->Active = false;
      }
   }

   deregister_thread();

   #ifdef _WIN32
      free_threadlock();
   #endif
}

/*********************************************************************************************************************
-ACTION-
Activate: Spawn a new thread that calls the function referenced in the #Routine field.
-END-
*********************************************************************************************************************/

static ERR THREAD_Activate(extThread *Self)
{
   kt::Log log;

   if (Self->Active) return ERR::ThreadAlreadyActive;

   // Thread objects MUST be locked prior to activation.  There is otherwise a genuine risk that the object
   // could be terminated by code operating outside of the thread space while it is active.

   if (Self->Queue.load() < 1) return log.warning(ERR::ThreadNotLocked);

   Self->Active = true; // Indicate that the thread is running

   std::thread::id thread_id = std::this_thread::get_id();
   Self->InterruptThreadID.store(0, std::memory_order_release);

   Self->CPPThread = new (std::nothrow) std::jthread([Self](std::stop_token StopToken) {
      auto uid = Self->UID;

      // Note that the Active flag will have been set to true prior to entry, and will remain until msg_threadcallback()
      // is called.

      // Configure crash indicators and a cleanup operation

      tlThreadCrashed = true;
      tlThreadRef     = Self;
      ThreadEntryCleanupGuard cleanup_guard;
      Self->InterruptThreadID.store(GetThreadID(), std::memory_order_release);

      ThreadMessage msg = { .ThreadID = uid, .Callback = Self->Callback };
      if (msg.Callback.defined()) msg.Callback.pin();

      {
         // Replace the default dummy context with one that pertains to the thread
         extObjectContext thread_ctx(Self, AC::NIL);

         if (StopToken.stop_requested()) {
            Self->Error = ERR::Cancelled;
         }
         else if (Self->Routine.stale()) {
            clear_callback(Self->Routine);
            Self->Error = ERR::Terminate;
         }
         else if (Self->Routine.isC()) {
            auto routine = (ERR (*)(extThread *, APTR))Self->Routine.Routine;
            Self->Error = routine(Self, Self->Routine.Meta);
         }
         else if (Self->Routine.isScript()) {
            ScopedObjectLock script(Self->Routine.Context, 5000);
            if (script.granted()) {
               sc::Call(Self->Routine, std::to_array<ScriptArg>({ { "Thread", Self, FD_OBJECTPTR } }));
            }
         }
      }

      // Please no references to Self after this point.  It is possible that the Thread object has been forcibly removed
      // if the client routine is persistently running during shutdown.

      // See msg_threadcallback()
      SendMessage(MSGID::THREAD_CALLBACK, MSF::NIL, &msg, sizeof(msg));

      // Reset the crash indicators and invoke the cleanup code.
      tlThreadRef     = nullptr;
      tlThreadCrashed = false;
   });

   if (Self->CPPThread) {
      Self->ThreadID = Self->CPPThread->get_id();
      Self->Handle   = Self->CPPThread->native_handle();
      return ERR::Okay;
   }
   else {
      Self->Active = false;
      return log.warning(ERR::SystemCall);
   }
}

/*********************************************************************************************************************
-ACTION-
Deactivate: Stops a thread.

Deactivating an active thread will cause it to stop immediately.  Stopping a thread in this manner is dangerous and
could result in an unstable application.
-END-
*********************************************************************************************************************/

static ERR THREAD_Deactivate(extThread *Self)
{
   if (Self->Active and Self->CPPThread) {
      Self->CPPThread->request_stop();

      auto thread_id = Self->InterruptThreadID.load(std::memory_order_acquire);
      if (thread_id > 0) WakeThread(thread_id, true);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR THREAD_FreeWarning(extThread *Self)
{
   if (!Self->Active) return ERR::Okay;
   else {
      kt::Log log;
      log.detail("Thread is still running, marking for auto termination.");
      Self->Flags |= THF::AUTO_FREE;
      return ERR::InUse;
   }
}

/*********************************************************************************************************************

-METHOD-
SetData: Attaches data to the thread.

Use the SetData() method prior to activating a thread so that it can be initialised with user data.  The thread will be
able to read the data from the #Data field.

A copy of the provided data buffer will be stored with the thread object, so there is no need to retain the original
data after this method has returned.  In some cases it may be desirable to store a direct pointer value and bypass the
copy operation.  To do this, set the Size parameter to zero.

-INPUT-
buf(ptr) Data: Pointer to the data buffer.
bufsize Size: Size of the data buffer.  If zero, the pointer is stored directly, with no copy operation taking place.

-ERRORS-
Okay
NullArgs
Args
AllocMemory

-TAGS-
mutates-object, copies-input
-END-

*********************************************************************************************************************/

static ERR THREAD_SetData(extThread *Self, struct th::SetData *Args)
{
   kt::Log log;

   if ((!Args) or (!Args->Data)) return log.warning(ERR::NullArgs);
   if (Args->Size < 0) return log.warning(ERR::Args);

   if (Self->Data) {
      FreeResource(Self->Data);
      Self->Data = nullptr;
      Self->DataSize = 0;
   }

   if (!Args->Size) { // If no size is provided, we simply copy the provided pointer.
      Self->Data = Args->Data;
      Self->DataSize = 0;
      return ERR::Okay;
   }
   else if (!AllocMemory(Args->Size, MEM::DATA, &Self->Data)) {
      Self->DataSize = Args->Size;
      copymem(Args->Data, Self->Data, Args->Size);
      return ERR::Okay;
   }
   else return log.warning(ERR::AllocMemory);
}

/*********************************************************************************************************************

-FIELD-
Callback: This function will be called when the thread finishes.

Set a function reference here to receive a notification when the thread finishes processing.  The
callback will be executed in the context of the main program loop to minimise resource locking issues.

The prototype for the callback routine is `void Callback(objThread *Thread)`.

-FIELD-
Data: Pointer to initialisation data for the thread.

The Data field will point to a data buffer if the #SetData() method has previously been called to store data in
the thread object.  It is paired with the #DataSize field, which reflects the size of the data buffer.

*********************************************************************************************************************/

static ERR GET_Data(extThread *Self, std::span<uint8_t> &Value)
{
   Value = std::span<uint8_t>((uint8_t *)Self->Data, Self->DataSize);
   return ERR::Okay;
}

static ERR SET_Callback(extThread *Self, FUNCTION *Value)
{
   clear_callback(Self->Callback);
   if (Value) {
      Self->Callback = *Value;
      if (Self->Callback.defined()) Self->Callback.pin();
   }
   return ERR::Okay;
}

static ERR SET_Routine(extThread *Self, FUNCTION *Value)
{
   clear_callback(Self->Routine);
   if (Value) {
      Self->Routine = *Value;
      if (Self->Routine.defined()) Self->Routine.pin();
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
DataSize: The size of the buffer referenced in the Data field.

-FIELD-
Error: Reflects the error code returned by the thread routine.

-FIELD-
Flags: Optional flags can be defined here.
Lookup: THF

-FIELD-
Routine: This function will be called when the thread starts.

The routine that will be executed when the thread is activated must be specified here.  The function prototype is
`ERR routine(objThread *Thread)`.

When the routine is called, a reference to the thread object is passed as a parameter.  Once the routine has
finished processing, the resulting error code will be stored in the thread object's #Error field.

*********************************************************************************************************************/

extThread::~extThread()
{
   if ((Data) and (DataSize > 0)) {
      FreeResource(Data);
      Data = nullptr;
      DataSize = 0;
   }

   if (CPPThread) { delete CPPThread; CPPThread = nullptr; }

   clear_callback(Callback);
   clear_callback(Routine);
}

//********************************************************************************************************************

#include "class_thread_def.c"

static const FieldArray clFields[] = {
   { "Callback",  FDF_FUNCTION|FDF_RW, nullptr, SET_Callback },
   { "Routine",   FDF_FUNCTION|FDF_RW, nullptr, SET_Routine },
   { "Data",      FDF_ARRAY|FDF_BYTE|FDF_R|FDF_PURE, GET_Data },
   { "DataSize",  FDF_INT|FDF_R },
   { "Error",     FDF_INT|FDF_R },
   { "Flags",     FDF_INT|FDF_RI, nullptr, nullptr, &clThreadFlags },
   END_FIELD
};

//********************************************************************************************************************

extern ERR add_thread_class(void)
{
   glThreadClass = objMetaClass::create::global(
      fl::ClassVersion(VER_THREAD),
      fl::Name("Thread"),
      fl::Category(CCF::SYSTEM),
      fl::Actions(clThreadActions),
      fl::Methods(clThreadMethods),
      fl::Fields(clFields),
      fl::Size(sizeof(extThread)),
      fl::Path("modules:core"));

   return glThreadClass ? ERR::Okay : ERR::AddClass;
}

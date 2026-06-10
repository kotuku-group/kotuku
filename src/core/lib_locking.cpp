/*********************************************************************************************************************

The source code for Kōtuku is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

*********************************************************************************************************************/

#include "defs.h"
#include <atomic>
#include <chrono>
#include <concepts>
#include <array>
#include <mutex>

using namespace kt;
using namespace std::chrono;

#ifdef _WIN32
thread_local bool tlMessageBreak = false; // This variable is set by ProcessMessages() to allow breaking when Windows sends OS messages
#endif

//********************************************************************************************************************
// C++20 Concepts for type safety

template<typename T>
concept Lockable = requires(T t) {
   { t.Queue } -> std::convertible_to<std::atomic<char>&>;
   { t.ThreadID } -> std::convertible_to<int&>;
   { t.SleepQueue } -> std::convertible_to<std::atomic<char>&>;
   { t.UID } -> std::convertible_to<OBJECTID>;
};

//********************************************************************************************************************
// Thread lock management

#ifdef _WIN32
class ThreadLockManager {
private:
   std::array<std::atomic<WINHANDLE>, MAX_THREADS> thread_locks{};
   std::atomic<int> next_index{1};
   std::once_flag init_flag;

   WINHANDLE allocate_lock() {
      for (int attempts = 0; attempts < MAX_THREADS; ++attempts) {
         int index = next_index.fetch_add(1, std::memory_order_relaxed) % MAX_THREADS;
         if (index IS 0) index = 1; // Skip index 0

         WINHANDLE expected = WINHANDLE(0);
         WINHANDLE new_lock;

         if (alloc_public_waitlock(&new_lock, nullptr) IS ERR::Okay) {
            if (thread_locks[index].compare_exchange_weak(expected, new_lock, std::memory_order_acquire)) {
               kt::Log log("ThreadLockManager");
               log.trace("Allocated thread-lock #%d for thread #%d", index, get_thread_id());
               return new_lock;
            }
            free_public_waitlock(new_lock);
         }
      }

      return WINHANDLE(0); // Graceful failure instead of exit(0)
   }

public:
   ThreadLockManager() {
      std::call_once(init_flag, [this]() {
         // Initialize each atomic individually since they can't be copied
         for (auto& lock : thread_locks) {
            lock.store(WINHANDLE(0), std::memory_order_relaxed);
         }
      });
   }

   WINHANDLE get_thread_lock() {
      thread_local WINHANDLE tl_lock = allocate_lock();
      return tl_lock;
   }

   void free_all_locks() {
      for (auto& lock_atomic : thread_locks) {
         WINHANDLE lock = lock_atomic.exchange(WINHANDLE(0), std::memory_order_acquire);
         if (lock) {
            free_public_waitlock(lock);
         }
      }
   }

   void free_thread_lock(WINHANDLE Lock) {
      if (not Lock) return;

      for (auto& lock_atomic : thread_locks) {
         WINHANDLE expected = Lock;
         if (lock_atomic.compare_exchange_weak(expected, WINHANDLE(0), std::memory_order_release)) {
            winCloseHandle(Lock);
            break;
         }
      }
   }
};

static ThreadLockManager glThreadLockManager;
#endif

//********************************************************************************************************************

struct WaitLock {
   THREADID ThreadID; // The thread represented by this wait-lock
   #ifdef _WIN32
   WINHANDLE Lock;
   #endif
   int64_t WaitingTime;
   THREADID WaitingForThreadID;
   int  WaitingForResourceID;
   int  WaitingForResourceType;
   uint8_t Flags; // WLF flags

   #define WLF_REMOVED 0x01  // Set if the resource was removed by the thread that was holding it.

   WaitLock() : ThreadID(0), Flags(0) { }
   WaitLock(THREADID pThread) : ThreadID(pThread), Flags(0) { }

   void setThread(const THREADID pThread) { ThreadID = pThread; }

   void notWaiting() {
      Flags = 0;
      WaitingForResourceID = 0;
      WaitingForResourceType = 0;
      WaitingForThreadID = THREADID(0);  // NB: Important that you clear this last if you are to avoid threading conflicts.
   }
};

static thread_local int16_t glWLIndex = -1; // The current thread's index within glWaitLocks
static std::vector<WaitLock> glWaitLocks;
static std::mutex glWaitLockMutex;

//********************************************************************************************************************

inline void register_waitlock(THREADID OurThread, THREADID OtherThread, int ResourceID, int ResourceType)
{
   if (glWLIndex IS -1) { // New thread that isn't registered yet
      unsigned i = 0;
      for (; i < glWaitLocks.size(); i++) {
         if (not glWaitLocks[i].ThreadID.defined()) break;
      }

      glWLIndex = i;
      if (i IS glWaitLocks.size()) glWaitLocks.push_back(OurThread);
      else glWaitLocks[glWLIndex].setThread(OurThread);
   }

   // Record the wait *before* checking for cycles, so that would_deadlock() sees the complete graph.

   glWaitLocks[glWLIndex].Flags                  = 0;
   glWaitLocks[glWLIndex].WaitingForThreadID     = OtherThread;
   glWaitLocks[glWLIndex].WaitingForResourceID   = ResourceID;
   glWaitLocks[glWLIndex].WaitingForResourceType = ResourceType;
}

//********************************************************************************************************************
// Deadlock detection via resource-based cycle detection.
//
// Walks the glWaitLocks table to detect circular wait chains.  Each entry records which thread is waiting and
// which thread currently holds the contested resource (WaitingForThreadID).  A deadlock exists when following
// the holder chain leads back to the requesting thread.
//
// Must be called while glWaitLockMutex is held.

static bool would_deadlock(THREADID Requester, THREADID Holder)
{
   THREADID current = Holder;

   for (int depth = 0; depth < int(glWaitLocks.size()); depth++) {
      if (current IS Requester) return true;

      // Find the WaitLock entry for 'current' to see if it is itself waiting on another thread
      bool found = false;
      for (auto &wl : glWaitLocks) {
         if (wl.ThreadID IS current) {
            if (wl.WaitingForThreadID.defined()) {
               current = wl.WaitingForThreadID;
               found = true;
            }
            else if ((wl.WaitingForResourceType IS RT_SLEEP) and (wl.WaitingTime < 0)) {
               // The holder is sleeping indefinitely (e.g. in ProcessMessages with no timeout).  It holds
               // an object lock that it cannot release until woken, so waiting on it will deadlock.
               return true;
            }
            break;
         }
      }

      if (not found) return false; // The holder isn't waiting on anything — no cycle
   }

   return false; // Exceeded maximum traversal depth without finding a cycle
}

//********************************************************************************************************************
// Register the current thread as sleeping (e.g. in ProcessMessages / sleep_task).  This allows the deadlock
// detector to recognise that the thread is blocked and cannot release any object locks it holds.
// Call wake_waitlock_entry() when the sleep completes.

void register_sleep(int Timeout)
{
   const std::lock_guard<std::mutex> lock(glWaitLockMutex);

   register_waitlock(get_thread_id(), THREADID(0), 0, RT_SLEEP);
   glWaitLocks[glWLIndex].WaitingTime = Timeout;
}

void deregister_sleep(void)
{
   const std::lock_guard<std::mutex> lock(glWaitLockMutex);

   if (glWLIndex >= 0) {
      glWaitLocks[glWLIndex].WaitingForResourceType = 0;
   }
}

//********************************************************************************************************************
// Prepare a thread for going to sleep on a resource.  Checks for deadlocks in advance.  Once a thread has added a
// WakeLock entry, it must keep it until either the thread or process is destroyed.
//
// Used by LockObject()

ERR init_sleep(THREADID OtherThreadID, int ResourceID, int ResourceType)
{
   //log.trace("Sleeping on thread %d for resource #%d, Total Threads: %d", OtherThreadID, ResourceID, int(glWaitLocks.size()));

   auto our_thread = get_thread_id();
   if (OtherThreadID IS our_thread) return ERR::Args;

   const std::lock_guard<std::mutex> lock(glWaitLockMutex);

   register_waitlock(our_thread, OtherThreadID, ResourceID, ResourceType);

   #ifdef _WIN32
   glWaitLocks[glWLIndex].Lock = get_threadlock();
   #endif

   if (would_deadlock(our_thread, OtherThreadID)) {
      kt::Log log(__FUNCTION__);
      log.warning("Deadlock: Thread %d contends with thread %d for resource #%d.", int(our_thread), int(OtherThreadID), ResourceID);
      glWaitLocks[glWLIndex].notWaiting();
      return ERR::DeadLock;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Remove all the wait-locks for the current process (affects all threads).  Lingering wait-locks are indicative of
// serious problems, as all should have been released on shutdown.

void remove_process_waitlocks(void)
{
   kt::Log log("Shutdown");
   log.trace("Removing process waitlocks...");

   auto const our_thread = get_thread_id();

   const std::lock_guard<std::mutex> lock(glWaitLockMutex);

   for (unsigned i=0; i < glWaitLocks.size(); i++) {
      if (glWaitLocks[i].ThreadID IS our_thread) {
         glWaitLocks[i].notWaiting();
      }
      else if (glWaitLocks[i].WaitingForThreadID IS our_thread) { // A thread is waiting on us, wake it up.
         #ifdef _WIN32
            log.warning("Waking thread %d", int(glWaitLocks[i].ThreadID));
            glWaitLocks[i].notWaiting();
            wake_waitlock(glWaitLocks[i].Lock, 1);
         #endif
      }
   }
}

//********************************************************************************************************************
// Windows thread-lock support.  Each thread gets its own semaphore.  Note that this is intended for handling public
// resources only.  Internally, use critical sections for synchronisation between threads.

#ifdef _WIN32
// Modern thread lock functions using RAII manager

WINHANDLE get_threadlock(void)
{
   return glThreadLockManager.get_thread_lock();
}

void free_threadlocks(void)
{
   glThreadLockManager.free_all_locks();
}

void free_threadlock(void)
{
   // Thread-local cleanup is now handled automatically by the manager
   // Individual thread lock cleanup happens when the thread terminates
}
#endif

/*********************************************************************************************************************

-FUNCTION-
AccessObject: Grants exclusive access to objects via unique ID.
Category: Objects

This function resolves an object ID to its address and acquires a lock on the object so that other threads cannot use
it simultaneously.

If the `Object` is already locked, the function will wait until it becomes available.   This must occur within the amount
of time specified in the `Milliseconds` parameter.  If the time expires, the function will return with an `ERR::TimeOut`
error code.  If successful, `ERR::Okay` is returned and a reference to the object's address is stored in the `Result`
variable.

It is crucial that calls to AccessObject() are followed with a call to ~ReleaseObject() once the lock is no
longer required.  Calls to AccessObject() will also nest, so they must be paired with ~ReleaseObject()
correctly.

It is recommended that C++ developers use the `ScopedObjectLock` class to acquire object locks rather than making
direct calls to AccessObject().  The following example illustrates lock acquisition within a 1 second time limit:

<pre>
{
   kt::ScopedObjectLock&lt;OBJECTPTR&gt; obj(my_object_id, 1000);
   if (lock.granted()) {
      obj.acDraw();
   }
}
</pre>


-INPUT-
oid Object: The unique ID of the target object.
int MilliSeconds: The limit in milliseconds before a timeout occurs.  The maximum limit is `60000`, and `100` is recommended.
&obj Result: A pointer storage variable that will store the resulting object address.

-ERRORS-
Okay
NullArgs
Args
NoMatchingObject
TimeOut
SystemLocked
Cancelled: The thread has been requested to stop whilst sleeping.
MarkedForDeletion: The object is being removed and cannot be locked.
DoesNotExist: The object was removed while waiting for the lock.
LockFailed: Failed to initialise the sleep record for the waiting thread.

-TAGS-
blocking
-END-

*********************************************************************************************************************/

ERR AccessObject(OBJECTID ObjectID, int MilliSeconds, OBJECTPTR *Result)
{
   kt::Log log(__FUNCTION__);

   if ((not Result) or (not ObjectID)) return log.warning(ERR::NullArgs);
   if (MilliSeconds <= 0) log.warning(ERR::Args); // Warn but do not fail

   *Result = nullptr;

   if (ObjectID IS glMetaClass.UID) { // Access to the MetaClass requires this special case handler.
      if (auto error = LockObject(&glMetaClass, MilliSeconds); error IS ERR::Okay) {
         *Result = &glMetaClass;
         return ERR::Okay;
      }
      else return error;
   }

   OBJECTPTR object = nullptr;

   {
      std::lock_guard lock(glmResources);

      auto resource = glResources.find(ObjectID);
      if ((resource IS glResources.end()) or (not resource->second.Address)) return ERR::NoMatchingObject;
      if (resource->second.Manager != &glResourceObject) return ERR::NoMatchingObject;
      if (resource->second.CollectOnUnlock or resource->second.Terminating) return ERR::MarkedForDeletion;

      object = (OBJECTPTR)resource->second.Address;
      if (object->collecting()) return ERR::MarkedForDeletion;

      object->pin();
   }

   auto error = LockObject(object, MilliSeconds);

   // Sanity check in case a thread called FreeResource() on the object before LockObject()

   if ((!error) and (object->collecting())) {
      ReleaseObject(object);
      error = ERR::MarkedForDeletion;
   }

   object->unpin(error != ERR::Okay);

   if (!error) *Result = object;
   return error;
}

/*********************************************************************************************************************

-FUNCTION-
LockObject: Lock an object to prevent contention between threads.
Category: Objects

Use LockObject() to gain exclusive access to an object at thread-level.  This function provides identical behaviour
to that of ~AccessObject(), but with a slight speed advantage as the object ID does not need to be resolved to an
address.  Calls to LockObject() will nest, and must be matched with a call to ~ReleaseObject() to unlock the object.

Be aware that while this function is faster than ~AccessObject(), it is unsafe if other threads could terminate the
object without a suitable barrier in place.

If it is guaranteed that an object is not being shared between threads, object locking is unnecessary.

-INPUT-
obj Object: The address of the object to lock.
int MilliSeconds: The total number of milliseconds to wait before giving up.  If `-1`, the function will wait indefinitely.

-ERRORS-
Okay:
NullArgs:
MarkedForDeletion:
SystemLocked:
TimeOut:
Cancelled: The thread has been requested to stop and cannot pause.
DoesNotExist: The object was removed while waiting for the lock.
LockFailed: Failed to initialise the sleep record for the waiting thread.

-TAGS-
blocking
-END-

*********************************************************************************************************************/

ERR LockObject(OBJECTPTR Object, int Timeout)
{
   if (not Object) {
      DEBUG_BREAK
      return ERR::NullArgs;
   }

   auto our_thread = get_thread_id();

   // Using an atomic increment we can achieve a 'quick lock' of the object without having to resort to locks.
   // This is quite safe so long as the developer is being careful with use of the object between threads (i.e. not
   // destroying the object when other threads could potentially be using it).

   // Use proper atomic compare-and-swap for thread-safe lock acquisition

   char expected = 0;
   if (Object->Queue.compare_exchange_weak(expected, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
      Object->ThreadID = int(our_thread);
      return ERR::Okay;
   }

   // Support nested locks - check if we already own the lock

   if (int(our_thread) IS Object->ThreadID) {
      Object->Queue.fetch_add(1, std::memory_order_relaxed);
      return ERR::Okay;
   }

   if (Object->collecting()) return ERR::MarkedForDeletion; // If the object is currently being removed by another thread, sleeping on it is pointless.

   // Problem: What if ReleaseObject() in another thread were to release the object prior to our glmObjectLocking lock?  This means that we would never receive the wake signal.
   // Solution: Prior to wait_until(), increment the object queue to attempt a lock.  This is *slightly* less efficient than doing it after the cond_wait(), but
   //           it will prevent us from sleeping on a signal that we would never receive.

   // Indefinite waiting is not permitted if the thread is in a stopping state.

   if (Timeout < 0) {
      if (auto record = get_thread_record(); record) {
         if (record->state.load(std::memory_order_acquire) IS TSTATE::STOPPING) Timeout = 1000;
      }
   }

   steady_clock::time_point end_time;
   if (Timeout < 0) end_time = steady_clock::time_point::max();
   else end_time = steady_clock::now() + milliseconds(Timeout);

   Object->SleepQueue.fetch_add(1, std::memory_order_relaxed); // Increment the sleep queue first so that ReleaseObject() will know that another thread is expecting a wake-up.

   // When Timeout is negative (indefinite wait), use a blocking lock.  A negative chrono duration
   // passed to try_lock_for() is treated as a non-blocking attempt per the C++ standard, which
   // would cause the acquisition to fail spuriously under contention.

   auto lock = std::unique_lock<std::timed_mutex>(glmObjectLocking, std::defer_lock);
   if (Timeout < 0) lock.lock();
   else (void)lock.try_lock_for(std::chrono::milliseconds(Timeout));

   if (lock.owns_lock()) {
      kt::Log log(__FUNCTION__);

      //log.function("TID: %d, Sleeping on #%d, Timeout: %d, Queue: %d, Locked By: %d", our_thread, Object->UID, Timeout, Object->Queue, Object->ThreadID);

      ERR error = ERR::TimeOut;
      if (init_sleep(THREADID(Object->ThreadID), Object->UID, RT_OBJECT) IS ERR::Okay) { // Indicate that our thread is sleeping.
         auto record = get_thread_record();
         record->state.store(TSTATE::PAUSED, std::memory_order_release);

         while (steady_clock::now() < end_time) {
            // Check if woken or stopped by WakeThread() before blocking
            {
               if (record->interrupted.load(std::memory_order_acquire) or
                   record->state.load(std::memory_order_acquire) IS TSTATE::STOPPING) break;
            }

            if (glWaitLocks[glWLIndex].Flags & WLF_REMOVED) {
               glWaitLocks[glWLIndex].notWaiting();
               Object->SleepQueue.fetch_sub(1, std::memory_order_release);
               auto expected = TSTATE::PAUSED;
               record->state.compare_exchange_strong(expected, TSTATE::RUNNING, std::memory_order_acq_rel);
               return ERR::DoesNotExist;
            }

            // Use proper atomic compare-and-swap for lock acquisition
            char expected = 0;
            if (Object->Queue.compare_exchange_weak(expected, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
               glWaitLocks[glWLIndex].notWaiting();
               Object->ThreadID = int(our_thread);
               Object->SleepQueue.fetch_sub(1, std::memory_order_release);
               auto exp = TSTATE::PAUSED;
               record->state.compare_exchange_strong(exp, TSTATE::RUNNING, std::memory_order_acq_rel);
               return ERR::Okay;
            }

            if (Timeout < 0) {
               cvObjects.wait(glmObjectLocking); // Indefinite wait; avoids time_point overflow with wait_for()
            }
            else {
               auto timeout_remaining = end_time - steady_clock::now();
               if (timeout_remaining <= milliseconds(0)) break;
               if (cvObjects.wait_for(glmObjectLocking, timeout_remaining) IS std::cv_status::timeout) break;
            }
         }

         // Failure: Either a timeout occurred, the object no longer exists, or the thread is stopping.

         if (glWaitLocks[glWLIndex].Flags & WLF_REMOVED) {
            log.warning("TID %d: The resource no longer exists.", int(get_thread_id()));
            error = ERR::DoesNotExist;
         }
         else if ((record->interrupted.load(std::memory_order_acquire) or
                  record->state.load(std::memory_order_acquire) IS TSTATE::STOPPING)) {
            error = ERR::Cancelled;
            record->interrupted.store(false, std::memory_order_release);
         }
         else {
            log.traceWarning("TID: %d, #%d, Timeout occurred.", our_thread, Object->UID);
            error = ERR::TimeOut;
         }

         glWaitLocks[glWLIndex].notWaiting();

         auto exp = TSTATE::PAUSED;
         record->state.compare_exchange_strong(exp, TSTATE::RUNNING, std::memory_order_acq_rel);
      }
      else error = log.error(ERR::LockFailed);

      Object->SleepQueue.fetch_sub(1, std::memory_order_release);
      return error;
   }
   else {
      Object->SleepQueue.fetch_sub(1, std::memory_order_release);
      return ERR::SystemLocked;
   }
}

/*********************************************************************************************************************

-FUNCTION-
ReleaseObject: Release a locked object.
Category: Objects

Release a lock previously obtained from ~AccessObject() or ~LockObject().  Locks will nest, so a release is required
for every lock that has been granted.

-INPUT-
obj Object: Pointer to the object to be released.

-TAGS-
blocking

*********************************************************************************************************************/

void ReleaseObject(OBJECTPTR Object)
{
   if (not Object) return;

   #ifndef NDEBUG
   if (Object->Queue.load(std::memory_order_relaxed) <= 0) {
      kt::Log("ReleaseObject").warning("Queue underflow on #%d (%s), Queue: %d, ThreadID: %d, OurThread: %d",
         Object->UID, Object->className(), Object->Queue.load(), Object->ThreadID.load(), int(get_thread_id()));
      DEBUG_BREAK
   }
   #endif

   if (Object->Queue.fetch_sub(1, std::memory_order_release) > 1) return;

   if (Object->SleepQueue > 0) { // Other threads are waiting on this object
      kt::Log log(__FUNCTION__);
      log.traceBranch("Waking %d threads for this object.", Object->SleepQueue.load());

      {
         std::unique_lock lock(glmObjectLocking);
         if (Object->collecting()) { // We have to tell other threads that the object is marked for deletion.
            // NB: A lock on glWaitLocks is not required because we're already protected by the glmObjectLocking
            // barrier (which is common between LockObject() and ReleaseObject()
            for (unsigned i=0; i < glWaitLocks.size(); i++) {
               if ((glWaitLocks[i].WaitingForResourceID IS Object->UID) and
                   (glWaitLocks[i].WaitingForResourceType IS RT_OBJECT)) {
                  glWaitLocks[i].Flags |= WLF_REMOVED;
               }
            }
         }

         // Destroy the object if marked for deletion and not pinned by reference counting.

         if (Object->defined(NF::FREE_ON_UNLOCK) and (not Object->defined(NF::FREE)) and (not Object->isPinned())) {
            Object->clearFlag(NF::FREE_ON_UNLOCK);
            FreeResource(Object);
         }

         cvObjects.notify_all(); // Multiple threads may be waiting on this object
      }
   }
   else if (Object->defined(NF::FREE_ON_UNLOCK) and (not Object->defined(NF::FREE)) and (not Object->isPinned())) {
      Object->clearFlag(NF::FREE_ON_UNLOCK);
      FreeResource(Object);
   }
}

/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

This program tests resource locking and termination behaviour.

*********************************************************************************************************************/

#include <atomic>
#include <cstdint>
#include <thread>
#include <kotuku/startup.h>

using namespace kt;

CSTRING ProgName = "ResourceLocking";
static APTR glTerminatingResource = nullptr;
static std::atomic_bool glManagerEntered = false;
static std::atomic_bool glManagerCanFinish = false;
static ERR glConcurrentFreeError = ERR::Okay;
static std::atomic_int glAllocFreeFailures = 0;

//********************************************************************************************************************

static ERR terminating_resource_free(ResourceRecord &, APTR)
{
   glManagerEntered.store(true, std::memory_order_release);
   while (not glManagerCanFinish.load(std::memory_order_acquire)) WaitTime(0.001);
   return ERR::Terminate;
}

static ResourceManager glTerminatingResourceManager = {
   "TerminatingTest",
   &terminating_resource_free,
   true
};

//********************************************************************************************************************

static void free_terminating_resource()
{
   glConcurrentFreeError = FreeResource(glTerminatingResource);
}

//********************************************************************************************************************

static void alloc_free_worker(int Base)
{
   for (int i=0; i < 250; i++) {
      APTR memory = nullptr;
      if (AllocResource(32 + ((Base + i) % 96), MEM::NIL, &memory, nullptr) != ERR::Okay) {
         glAllocFreeFailures.fetch_add(1, std::memory_order_relaxed);
         continue;
      }

      if (((uintptr_t)memory & 31) != 0) {
         glAllocFreeFailures.fetch_add(1, std::memory_order_relaxed);
      }

      auto memory_id = GetMemoryID(memory);
      if (CheckResourceExists(memory_id) != ERR::True) {
         glAllocFreeFailures.fetch_add(1, std::memory_order_relaxed);
      }

      if (FreeResource(memory) != ERR::Okay) {
         glAllocFreeFailures.fetch_add(1, std::memory_order_relaxed);
      }

      if (CheckResourceExists(memory_id) != ERR::False) {
         glAllocFreeFailures.fetch_add(1, std::memory_order_relaxed);
      }
   }
}

//********************************************************************************************************************

static int run_concurrent_alloc_free_check(void)
{
   kt::Log log(__FUNCTION__);

   static constexpr int thread_count = 4;
   std::thread threads[thread_count];

   glAllocFreeFailures.store(0, std::memory_order_release);

   for (int i=0; i < thread_count; i++) threads[i] = std::thread(&alloc_free_worker, i * 1000);
   for (int i=0; i < thread_count; i++) threads[i].join();

   if (glAllocFreeFailures.load(std::memory_order_acquire) != 0) {
      log.warning("%d concurrent allocation/free checks failed.", glAllocFreeFailures.load(std::memory_order_relaxed));
      return -1;
   }

   return 0;
}

//********************************************************************************************************************

static int run_owned_resource_cleanup_check(void)
{
   kt::Log log(__FUNCTION__);

   OBJECTPTR parent = nullptr;
   OBJECTPTR child = nullptr;
   APTR memory = nullptr;

   if (NewObject(CLASSID::CONFIG, NF::NIL, &parent) != ERR::Okay) {
      log.warning("Failed to create parent Config object for ownership cleanup check.");
      return -1;
   }

   if (NewObject(CLASSID::CONFIG, NF::NIL, &child) != ERR::Okay) {
      FreeResource(parent);
      log.warning("Failed to create child Config object for ownership cleanup check.");
      return -1;
   }

   if (SetOwner(child, parent) != ERR::Okay) {
      FreeResource(child);
      FreeResource(parent);
      log.warning("SetOwner() failed for ownership cleanup check.");
      return -1;
   }

   if (AllocResource(64, MEM::NIL, &memory, nullptr) != ERR::Okay) {
      FreeResource(child);
      FreeResource(parent);
      log.warning("AllocMemory() failed for ownership cleanup check.");
      return -1;
   }

   const auto child_id = child->UID;
   const auto memory_id = GetMemoryID(memory);

   if (auto error = TrackResource(memory_id, memory, parent->UID, nullptr); error != ERR::Okay) {
      FreeResource(memory);
      FreeResource(child);
      FreeResource(parent);
      log.warning("TrackResource() failed for ownership cleanup check: %s.", GetErrorMsg(error));
      return -1;
   }

   if (FreeResource(parent) != ERR::Okay) {
      FreeResource(memory);
      FreeResource(child);
      log.warning("FreeResource() failed for parent ownership cleanup check.");
      return -1;
   }

   OBJECTPTR locked = nullptr;
   auto error = AccessObject(child_id, 1000, &locked);
   if ((error != ERR::NoMatchingObject) and (error != ERR::MarkedForDeletion)) {
      if (!error) ReleaseObject(locked);
      log.warning("AccessObject() returned %s for a child freed with its parent.", GetErrorMsg(error));
      return -1;
   }

   if (CheckResourceExists(memory_id) != ERR::False) {
      log.warning("Tracked memory resource still exists after parent cleanup.");
      return -1;
   }

   return 0;
}

//********************************************************************************************************************

static int run_terminating_resource_check(void)
{
   kt::Log log(__FUNCTION__);

   APTR memory = nullptr;
   if (AllocResource(64, MEM::NIL, &memory, nullptr) != ERR::Okay) {
      log.warning("AllocMemory() failed for terminating resource test.");
      return -1;
   }

   glTerminatingResource = memory;
   glManagerEntered.store(false, std::memory_order_release);
   glManagerCanFinish.store(false, std::memory_order_release);
   glConcurrentFreeError = ERR::Okay;

   if (auto error = TrackResource(GetMemoryID(memory), memory, 0, &glTerminatingResourceManager); error != ERR::Okay) {
      FreeResource(memory);
      log.warning("TrackResource() failed for terminating resource test: %s.", GetErrorMsg(error));
      return -1;
   }

   std::thread thread(&free_terminating_resource);

   while (not glManagerEntered.load(std::memory_order_acquire)) WaitTime(0.001);

   auto second_error = FreeResource(memory);

   glManagerCanFinish.store(true, std::memory_order_release);
   thread.join();

   if (second_error != ERR::InUse) {
      log.warning("FreeResource() returned %s for a terminating resource.", GetErrorMsg(second_error));
      return -1;
   }

   if (glConcurrentFreeError != ERR::Okay) {
      log.warning("Initial FreeResource() returned %s for terminating resource test.", GetErrorMsg(glConcurrentFreeError));
      return -1;
   }

   glTerminatingResource = nullptr;
   return 0;
}

//********************************************************************************************************************

static int run_free_object_checks(void)
{
   kt::Log log(__FUNCTION__);

   OBJECTPTR object = nullptr;
   if (NewObject(CLASSID::CONFIG, NF::NIL, &object) != ERR::Okay) {
      log.warning("Failed to create Config object for direct FreeObject() check.");
      return -1;
   }

   auto object_id = object->UID;
   if (FreeObject(object_id) != ERR::Okay) {
      log.warning("FreeObject() failed for an unlocked object.");
      return -1;
   }

   if (CheckResourceExists(object_id) != ERR::False) {
      log.warning("Directly freed object still exists.");
      return -1;
   }

   if (FreeObject(object_id) != ERR::DoesNotExist) {
      log.warning("FreeObject() did not reject an already freed object.");
      return -1;
   }

   if (NewObject(CLASSID::CONFIG, NF::NIL, &object) != ERR::Okay) {
      log.warning("Failed to create Config object for deferred FreeObject() check.");
      return -1;
   }

   object_id = object->UID;
   OBJECTPTR locked = nullptr;
   if (AccessObject(object_id, 1000, &locked) != ERR::Okay) {
      FreeObject(object_id);
      log.warning("Failed to lock Config object for deferred FreeObject() check.");
      return -1;
   }

   if (FreeObject(object_id) != ERR::InUse) {
      ReleaseObject(locked);
      log.warning("FreeObject() did not defer destruction of a locked object.");
      return -1;
   }

   if (CheckResourceExists(object_id) != ERR::False) {
      ReleaseObject(locked);
      log.warning("Deferred object remains visible through CheckResourceExists().");
      return -1;
   }

   if (FreeObject(object_id) != ERR::InUse) {
      ReleaseObject(locked);
      log.warning("Repeated FreeObject() did not report a locked object as in use.");
      return -1;
   }

   ReleaseObject(locked);

   if (FreeObject(object_id) != ERR::DoesNotExist) {
      log.warning("Deferred object was not collected on its final unlock.");
      return -1;
   }

   if (NewObject(CLASSID::CONFIG, NF::NIL, &object) != ERR::Okay) {
      log.warning("Failed to create Config object for FreeResource() dispatch check.");
      return -1;
   }

   object_id = object->UID;
   if (FreeResource(object_id) != ERR::Okay) {
      log.warning("FreeResource() failed to dispatch an object identifier.");
      return -1;
   }

   if (FreeObject(object_id) != ERR::DoesNotExist) {
      log.warning("FreeResource() dispatch did not remove the object registry entry.");
      return -1;
   }

   return 0;
}

//********************************************************************************************************************

static int run_pinned_forced_object_free_check(void)
{
   kt::Log log(__FUNCTION__);

   OBJECTPTR object = nullptr;
   if (NewObject(CLASSID::CONFIG, NF::NIL, &object) != ERR::Okay) {
      log.warning("Failed to create Config object for pinned forced free check.");
      return -1;
   }

   const auto object_id = object->UID;
   object->pin();
   object->setFlag(NF::PERMIT_TERMINATE);

   if (FreeResource(object) != ERR::Okay) {
      object->unpin();
      log.warning("FreeResource() failed for pinned forced free check.");
      return -1;
   }

   if ((not object->defined(NF::FREE)) or (not object->defined(NF::ZOMBIE))) {
      object->unpin();
      log.warning("Pinned forced free did not leave a zombie header.");
      return -1;
   }

   if (object->RefCount.load(std::memory_order_acquire) != 1) {
      object->unpin();
      log.warning("Unexpected zombie RefCount after forced free: %d.", int(object->RefCount.load()));
      return -1;
   }

   if (CheckResourceExists(object_id) != ERR::False) {
      object->unpin();
      log.warning("Zombie object resource still exists after forced free.");
      return -1;
   }

   object->unpin();
   return 0;
}

//********************************************************************************************************************
// Weak pins must never defer termination: an explicit FreeResource() without PERMIT_TERMINATE proceeds directly to
// full teardown, retaining only the zombie header until the weak pin is released.

static int run_weak_pinned_object_free_check(void)
{
   kt::Log log(__FUNCTION__);

   OBJECTPTR object = nullptr;
   if (NewObject(CLASSID::CONFIG, NF::NIL, &object) != ERR::Okay) {
      log.warning("Failed to create Config object for weak pin check.");
      return -1;
   }

   const auto object_id = object->UID;
   object->pinWeak();

   if (FreeResource(object) != ERR::Okay) {
      object->unpinWeak();
      log.warning("FreeResource() failed for weak pinned free check.");
      return -1;
   }

   if (object->defined(NF::FREE_ON_UNLOCK)) {
      object->unpinWeak();
      log.warning("Weak pin incorrectly deferred object termination.");
      return -1;
   }

   if ((not object->defined(NF::FREE)) or (not object->defined(NF::ZOMBIE))) {
      object->unpinWeak();
      log.warning("Weak pinned free did not leave a zombie header.");
      return -1;
   }

   if (object->RefCount.load(std::memory_order_acquire) != Object::WEAK_PINS) {
      object->unpinWeak();
      log.warning("Unexpected zombie RefCount after weak pinned free: %d.", int(object->RefCount.load()));
      return -1;
   }

   if (CheckResourceExists(object_id) != ERR::False) {
      object->unpinWeak();
      log.warning("Zombie object resource still exists after weak pinned free.");
      return -1;
   }

   object->unpinWeak();
   return 0;
}

//********************************************************************************************************************
// Cascade frees are how callback contexts usually die.  A weak-pinned child collected by its parent's free must be
// zombified with the same guarantees as a direct free: header retained, NF::FREE|NF::ZOMBIE set, block released on
// the last unpinWeak().

static int run_weak_pinned_cascade_free_check(void)
{
   kt::Log log(__FUNCTION__);

   OBJECTPTR parent = nullptr;
   OBJECTPTR child = nullptr;

   if (NewObject(CLASSID::CONFIG, NF::NIL, &parent) != ERR::Okay) {
      log.warning("Failed to create parent Config object for weak pinned cascade check.");
      return -1;
   }

   if (NewObject(CLASSID::CONFIG, NF::NIL, &child) != ERR::Okay) {
      FreeResource(parent);
      log.warning("Failed to create child Config object for weak pinned cascade check.");
      return -1;
   }

   if (SetOwner(child, parent) != ERR::Okay) {
      FreeResource(child);
      FreeResource(parent);
      log.warning("SetOwner() failed for weak pinned cascade check.");
      return -1;
   }

   const auto child_id = child->UID;
   child->pinWeak();

   if (FreeResource(parent) != ERR::Okay) {
      child->unpinWeak();
      log.warning("FreeResource() failed for parent in weak pinned cascade check.");
      return -1;
   }

   if ((not child->defined(NF::FREE)) or (not child->defined(NF::ZOMBIE))) {
      child->unpinWeak();
      log.warning("Cascade free of a weak pinned child did not leave a zombie header.");
      return -1;
   }

   if (child->RefCount.load(std::memory_order_acquire) != Object::WEAK_PINS) {
      child->unpinWeak();
      log.warning("Unexpected zombie RefCount after cascade free: %d.", int(child->RefCount.load()));
      return -1;
   }

   if (CheckResourceExists(child_id) != ERR::False) {
      child->unpinWeak();
      log.warning("Cascaded child resource still exists in weak pinned cascade check.");
      return -1;
   }

   child->unpinWeak();
   return 0;
}

//********************************************************************************************************************
// A strong-pinned child must defer its collection during a parent cascade (NF::FREE_ON_UNLOCK) and the deferred
// collection via unpin(true) must then follow the same zombie path as a direct free.  A weak pin retains the header
// for post-collection inspection.

static int run_pinned_deferred_cascade_check(void)
{
   kt::Log log(__FUNCTION__);

   OBJECTPTR parent = nullptr;
   OBJECTPTR child = nullptr;

   if (NewObject(CLASSID::CONFIG, NF::NIL, &parent) != ERR::Okay) {
      log.warning("Failed to create parent Config object for deferred cascade check.");
      return -1;
   }

   if (NewObject(CLASSID::CONFIG, NF::NIL, &child) != ERR::Okay) {
      FreeResource(parent);
      log.warning("Failed to create child Config object for deferred cascade check.");
      return -1;
   }

   if (SetOwner(child, parent) != ERR::Okay) {
      FreeResource(child);
      FreeResource(parent);
      log.warning("SetOwner() failed for deferred cascade check.");
      return -1;
   }

   const auto child_id = child->UID;
   child->pin();
   child->pinWeak();

   if (FreeResource(parent) != ERR::Okay) {
      child->unpinWeak();
      child->unpin();
      log.warning("FreeResource() failed for parent in deferred cascade check.");
      return -1;
   }

   if (child->defined(NF::FREE)) {
      child->unpinWeak();
      child->unpin();
      log.warning("Strong pinned child was terminated by a parent cascade.");
      return -1;
   }

   if (not child->defined(NF::FREE_ON_UNLOCK)) {
      child->unpinWeak();
      child->unpin();
      log.warning("Strong pinned child was not marked for deferred collection.");
      return -1;
   }

   child->unpin(true); // freeIfReady() collects the deferred child immediately

   if ((not child->defined(NF::FREE)) or (not child->defined(NF::ZOMBIE))) {
      child->unpinWeak();
      log.warning("Deferred collection did not leave a zombie header.");
      return -1;
   }

   if (child->RefCount.load(std::memory_order_acquire) != Object::WEAK_PINS) {
      child->unpinWeak();
      log.warning("Unexpected zombie RefCount after deferred collection: %d.", int(child->RefCount.load()));
      return -1;
   }

   if (CheckResourceExists(child_id) != ERR::False) {
      child->unpinWeak();
      log.warning("Deferred child resource still exists after collection.");
      return -1;
   }

   child->unpinWeak();
   return 0;
}

//********************************************************************************************************************
// Threaded stress: worker threads release weak pins while the main thread frees the object.  Whichever side crosses
// zero must perform exactly one block release; correctness is confirmed by the absence of crashes, unbalanced-unpin
// warnings and shutdown leak reports.

static constexpr int ZOMBIE_STRESS_THREADS = 4;
static constexpr int ZOMBIE_STRESS_PINS = 4; // Weak pins released per thread
static OBJECTPTR glStressObject = nullptr;
static std::atomic_bool glStressStart = false;

static void zombie_stress_worker()
{
   while (not glStressStart.load(std::memory_order_acquire));
   for (int i=0; i < ZOMBIE_STRESS_PINS; i++) glStressObject->unpinWeak();
}

static int run_threaded_zombie_release_check(void)
{
   kt::Log log(__FUNCTION__);

   for (int iteration=0; iteration < 200; iteration++) {
      OBJECTPTR object = nullptr;
      if (NewObject(CLASSID::CONFIG, NF::NIL, &object) != ERR::Okay) {
         log.warning("Failed to create Config object for threaded zombie check, iteration %d.", iteration);
         return -1;
      }

      const auto object_id = object->UID;
      for (int i=0; i < ZOMBIE_STRESS_THREADS * ZOMBIE_STRESS_PINS; i++) object->pinWeak();

      glStressObject = object;
      glStressStart.store(false, std::memory_order_release);

      std::thread threads[ZOMBIE_STRESS_THREADS];
      for (int i=0; i < ZOMBIE_STRESS_THREADS; i++) threads[i] = std::thread(&zombie_stress_worker);

      glStressStart.store(true, std::memory_order_release);

      auto error = FreeResource(object);

      for (int i=0; i < ZOMBIE_STRESS_THREADS; i++) threads[i].join();

      if (error != ERR::Okay) {
         log.warning("FreeResource() returned %s in threaded zombie check, iteration %d.", GetErrorMsg(error), iteration);
         return -1;
      }

      if (CheckResourceExists(object_id) != ERR::False) {
         log.warning("Object resource still exists after threaded zombie check, iteration %d.", iteration);
         return -1;
      }
   }

   return 0;
}

//********************************************************************************************************************

static int run_access_object_checks(void)
{
   kt::Log log(__FUNCTION__);

   APTR memory = nullptr;
   if (AllocResource(64, MEM::NIL, &memory, nullptr) != ERR::Okay) {
      log.warning("AllocMemory() failed for AccessObject() resource rejection test.");
      return -1;
   }

   OBJECTPTR locked = nullptr;
   auto error = AccessObject(GetMemoryID(memory), 1000, &locked);
   if (!error) {
      ReleaseObject(locked);
      FreeResource(memory);
      log.warning("AccessObject() incorrectly accepted a memory resource ID.");
      return -1;
   }
   else if (error != ERR::NoMatchingObject) {
      FreeResource(memory);
      log.warning("AccessObject() returned %s for a memory resource ID.", GetErrorMsg(error));
      return -1;
   }

   FreeResource(memory);

   OBJECTPTR object = nullptr;
   if (NewObject(CLASSID::CONFIG, NF::NIL, &object) != ERR::Okay) {
      log.warning("Failed to create Config object for AccessObject() checks.");
      return -1;
   }

   auto object_id = object->UID;
   error = AccessObject(object_id, 1000, &locked);
   if (error != ERR::Okay) {
      FreeResource(object);
      log.warning("AccessObject() failed for a valid object ID: %s.", GetErrorMsg(error));
      return -1;
   }

   if (locked != object) {
      ReleaseObject(locked);
      FreeResource(object);
      log.warning("AccessObject() returned the wrong object pointer.");
      return -1;
   }

   ReleaseObject(locked);

   object->pin();
   object->setFlag(NF::FREE_ON_UNLOCK);

   locked = nullptr;
   error = AccessObject(object_id, 1000, &locked);

   object->clearFlag(NF::FREE_ON_UNLOCK);
   object->unpin();

   if (!error) {
      ReleaseObject(locked);
      FreeResource(object);
      log.warning("AccessObject() accepted an object marked for deletion.");
      return -1;
   }
   else if (error != ERR::MarkedForDeletion) {
      FreeResource(object);
      log.warning("AccessObject() returned %s for an object marked for deletion.", GetErrorMsg(error));
      return -1;
   }

   FreeResource(object);

   locked = nullptr;
   error = AccessObject(object_id, 1000, &locked);
   if ((error != ERR::NoMatchingObject) and (error != ERR::MarkedForDeletion)) {
      if (!error) ReleaseObject(locked);
      log.warning("AccessObject() returned %s for a freed object ID.", GetErrorMsg(error));
      return -1;
   }

   return 0;
}

int main(int argc, CSTRING *argv)
{
   if (auto msg = init_kotuku(argc, argv)) {
      printf("%s\n", msg);
      return -1;
   }

   if (run_access_object_checks() != 0) {
      close_kotuku();
      return -1;
   }

   if (run_owned_resource_cleanup_check() != 0) {
      close_kotuku();
      return -1;
   }

   if (run_concurrent_alloc_free_check() != 0) {
      close_kotuku();
      return -1;
   }

   if (run_terminating_resource_check() != 0) {
      close_kotuku();
      return -1;
   }

   if (run_free_object_checks() != 0) {
      close_kotuku();
      return -1;
   }

   if (run_pinned_forced_object_free_check() != 0) {
      close_kotuku();
      return -1;
   }

   if (run_weak_pinned_object_free_check() != 0) {
      close_kotuku();
      return -1;
   }

   if (run_weak_pinned_cascade_free_check() != 0) {
      close_kotuku();
      return -1;
   }

   if (run_pinned_deferred_cascade_check() != 0) {
      close_kotuku();
      return -1;
   }

   if (run_threaded_zombie_release_check() != 0) {
      close_kotuku();
      return -1;
   }

   printf("Testing complete.\n");

   close_kotuku();
}

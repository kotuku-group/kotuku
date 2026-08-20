/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

This program tests resource locking and termination behaviour.

*********************************************************************************************************************/

#include <atomic>
#include <barrier>
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
static std::atomic_int glPinManagerCalls = 0;
static std::atomic_bool glFailFirstManagerCall = false;

//********************************************************************************************************************

static ERR pin_counting_resource_free(ResourceRecord &, APTR)
{
   const auto call = glPinManagerCalls.fetch_add(1, std::memory_order_acq_rel);
   if (glFailFirstManagerCall.exchange(false, std::memory_order_acq_rel) and (call IS 0)) return ERR::Failed;
   return ERR::Okay;
}

static ERR pin_terminating_resource_free(ResourceRecord &, APTR)
{
   glPinManagerCalls.fetch_add(1, std::memory_order_relaxed);
   return ERR::Terminate;
}

static ResourceManager glPinCountingManager = {
   "PinCountingTest",
   &pin_counting_resource_free,
   false
};

static ResourceManager glPinTerminatingManager = {
   "PinTerminatingTest",
   &pin_terminating_resource_free,
   false
};

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

static int run_resource_pin_functional_checks(void)
{
   kt::Log log(__FUNCTION__);

   if (PinResource(RESOURCEID(0)) != ERR::NullArgs) {
      log.warning("PinResource() did not reject a zero resource ID.");
      return -1;
   }

   if (UnpinResource(RESOURCEID(0)) != ERR::NullArgs) {
      log.warning("UnpinResource() did not reject a zero resource ID.");
      return -1;
   }

   const auto missing_id = AllocateID(IDTYPE::RESOURCE);
   if (PinResource(missing_id) != ERR::DoesNotExist) {
      log.warning("PinResource() did not reject a missing resource.");
      return -1;
   }

   if (UnpinResource(missing_id) != ERR::DoesNotExist) {
      log.warning("UnpinResource() did not reject a missing resource.");
      return -1;
   }

   APTR memory = nullptr;
   if (AllocResource(64, MEM::NIL, &memory, nullptr) != ERR::Okay) return -1;

   const auto memory_id = GetMemoryID(memory);
   if (UnpinResource(memory_id) != ERR::ResourceNotLocked) {
      FreeResource(memory_id);
      log.warning("UnpinResource() did not reject an unbalanced release.");
      return -1;
   }

   if ((PinResource(memory) != ERR::Okay) or (PinResource(memory_id) != ERR::Okay)) {
      FreeResource(memory_id);
      log.warning("Failed to acquire nested resource pins.");
      return -1;
   }

   if ((FreeResource(memory_id) != ERR::InUse) or (FreeResource(memory_id) != ERR::InUse)) {
      UnpinResource(memory_id);
      UnpinResource(memory_id);
      log.warning("FreeResource() did not defer repeated frees for a pinned resource.");
      return -1;
   }

   if ((CheckResourceExists(memory_id) != ERR::False) or
       (PinResource(memory_id) != ERR::MarkedForDeletion)) {
      UnpinResource(memory_id);
      UnpinResource(memory_id);
      log.warning("A deferred resource remained visible or accepted another pin.");
      return -1;
   }

   if ((UnpinResource(memory_id) != ERR::Okay) or (CheckResourceExists(memory_id) != ERR::False)) {
      UnpinResource(memory_id);
      log.warning("A nested pin did not retain a deferred resource.");
      return -1;
   }

   if ((UnpinResource(memory_id) != ERR::Okay) or (CheckResourceExists(memory_id) != ERR::False)) {
      log.warning("The final pin did not collect a deferred resource.");
      return -1;
   }

   memory = nullptr;
   if (AllocResource(64, MEM::NIL, &memory, nullptr) != ERR::Okay) return -1;
   const auto ordinary_id = GetMemoryID(memory);

   if ((PinResource(memory) != ERR::Okay) or (UnpinResource(memory) != ERR::Okay) or
       (CheckResourceExists(ordinary_id) != ERR::True) or (FreeResource(ordinary_id) != ERR::Okay)) {
      log.warning("Ordinary resource pin release changed the resource lifetime.");
      return -1;
   }

   OBJECTPTR object = nullptr;
   if (NewObject(CLASSID::CONFIG, NF::NIL, &object) != ERR::Okay) return -1;
   const auto object_id = object->UID;
   if (PinResource(object_id) != ERR::DoesNotExist) {
      FreeResource(object);
      log.warning("PinResource() accepted an object identifier.");
      return -1;
   }
   FreeResource(object);

   return 0;
}

//********************************************************************************************************************

static int run_custom_resource_pin_checks(void)
{
   kt::Log log(__FUNCTION__);
   int payload = 1;
   const auto direct_id = AllocateID(IDTYPE::RESOURCE);

   glPinManagerCalls.store(0, std::memory_order_release);
   if (TrackResource(direct_id, &payload, 0, &glPinCountingManager) != ERR::Okay) return -1;

   if ((PinResource(direct_id) != ERR::Okay) or (FreeResource(direct_id) != ERR::InUse)) {
      UnpinResource(direct_id);
      log.warning("Custom resource destruction was not deferred.");
      return -1;
   }

   if ((glPinManagerCalls.load(std::memory_order_acquire) != 0) or
       (UnpinResource(direct_id) != ERR::Okay) or
       (glPinManagerCalls.load(std::memory_order_acquire) != 1)) {
      log.warning("Custom resource manager was not called exactly once on the final unpin.");
      return -1;
   }

   APTR memory = nullptr;
   if (AllocResource(64, MEM::NIL, &memory, nullptr) != ERR::Okay) return -1;
   const auto terminate_id = GetMemoryID(memory);
   glPinManagerCalls.store(0, std::memory_order_release);

   if (TrackResource(terminate_id, memory, 0, &glPinTerminatingManager) != ERR::Okay) {
      FreeResource(terminate_id);
      return -1;
   }

   if ((PinResource(terminate_id) != ERR::Okay) or (FreeResource(terminate_id) != ERR::InUse) or
       (UnpinResource(terminate_id) != ERR::Okay) or
       (glPinManagerCalls.load(std::memory_order_acquire) != 1) or
       (CheckResourceExists(terminate_id) != ERR::False)) {
      log.warning("ERR::Terminate fallback was not deferred or collected correctly.");
      return -1;
   }

   int failed_payload = 2;
   const auto failed_id = AllocateID(IDTYPE::RESOURCE);
   glPinManagerCalls.store(0, std::memory_order_release);
   glFailFirstManagerCall.store(true, std::memory_order_release);
   if (TrackResource(failed_id, &failed_payload, 0, &glPinCountingManager) != ERR::Okay) return -1;

   if ((PinResource(failed_id) != ERR::Okay) or (FreeResource(failed_id) != ERR::InUse) or
       (UnpinResource(failed_id) != ERR::Failed)) {
      log.warning("A deferred manager failure was not returned to the final unpin caller.");
      return -1;
   }

   if ((CheckResourceExists(failed_id) != ERR::True) or (PinResource(failed_id) != ERR::Okay) or
       (UnpinResource(failed_id) != ERR::Okay) or (FreeResource(failed_id) != ERR::Okay) or
       (glPinManagerCalls.load(std::memory_order_acquire) != 2)) {
      log.warning("A failed deferred collection did not restore a retryable resource.");
      return -1;
   }

   return 0;
}

//********************************************************************************************************************

static int run_pinned_owner_cascade_check(void)
{
   kt::Log log(__FUNCTION__);
   OBJECTPTR owner = nullptr;
   APTR memory = nullptr;

   if (NewObject(CLASSID::CONFIG, NF::NIL, &owner) != ERR::Okay) return -1;
   if (AllocResource(64, MEM::NIL, &memory, nullptr) != ERR::Okay) {
      FreeResource(owner);
      return -1;
   }

   const auto owner_id = owner->UID;
   const auto memory_id = GetMemoryID(memory);
   if ((TrackResource(memory_id, memory, owner_id, nullptr) != ERR::Okay) or
       (PinResource(memory_id) != ERR::Okay)) {
      FreeResource(memory_id);
      FreeResource(owner);
      return -1;
   }

   if ((FreeResource(owner) != ERR::Okay) or (CheckResourceExists(owner_id) != ERR::False) or
       (CheckResourceExists(memory_id) != ERR::False)) {
      UnpinResource(memory_id);
      log.warning("Owner cleanup did not defer its pinned resource.");
      return -1;
   }

   if ((UnpinResource(memory_id) != ERR::Okay) or (CheckResourceExists(memory_id) != ERR::False)) {
      log.warning("Pinned resource did not safely outlive and follow its owner.");
      return -1;
   }

   return 0;
}

//********************************************************************************************************************

static int run_pinned_record_mutation_check(void)
{
   kt::Log log(__FUNCTION__);
   int first = 1;
   int second = 2;
   const auto resource_id = AllocateID(IDTYPE::RESOURCE);

   glPinManagerCalls.store(0, std::memory_order_release);
   if (TrackResource(resource_id, &first, 0, &glPinCountingManager) != ERR::Okay) return -1;
   if (PinResource(resource_id) != ERR::Okay) return -1;

   if ((TrackResource(resource_id, &second, 0, &glPinTerminatingManager) != ERR::InUse) or
       (FreeResource(resource_id) != ERR::InUse) or
       (TrackResource(resource_id, &second, 0, &glPinTerminatingManager) != ERR::InUse)) {
      UnpinResource(resource_id);
      log.warning("TrackResource() mutated a pinned or deferred resource.");
      return -1;
   }

   if ((UnpinResource(resource_id) != ERR::Okay) or
       (glPinManagerCalls.load(std::memory_order_acquire) != 1)) {
      log.warning("Record mutation check did not retain the original manager.");
      return -1;
   }

   return 0;
}

//********************************************************************************************************************

static int run_threaded_resource_pin_checks(void)
{
   kt::Log log(__FUNCTION__);
   static constexpr int thread_count = 4;

   int payload = 1;
   auto resource_id = RESOURCEID(AllocateID(IDTYPE::RESOURCE));
   glPinManagerCalls.store(0, std::memory_order_release);
   if (TrackResource(resource_id, &payload, 0, &glPinCountingManager) != ERR::Okay) return -1;

   for (int i=0; i < thread_count; i++) {
      if (PinResource(resource_id) != ERR::Okay) return -1;
   }

   if (FreeResource(resource_id) != ERR::InUse) return -1;

   std::barrier release_start(thread_count + 1);
   std::atomic_int release_failures = 0;
   std::thread release_threads[thread_count];

   for (int i=0; i < thread_count; i++) {
      release_threads[i] = std::thread([&]() {
         release_start.arrive_and_wait();
         if (UnpinResource(resource_id) != ERR::Okay) release_failures.fetch_add(1, std::memory_order_relaxed);
      });
   }

   release_start.arrive_and_wait();
   for (auto &thread : release_threads) thread.join();

   if ((release_failures.load(std::memory_order_acquire) != 0) or
       (glPinManagerCalls.load(std::memory_order_acquire) != 1) or
       (CheckResourceExists(resource_id) != ERR::False)) {
      log.warning("Concurrent final unpins did not perform exactly one deferred collection.");
      return -1;
   }

   for (int iteration=0; iteration < 100; iteration++) {
      resource_id = AllocateID(IDTYPE::RESOURCE);
      glPinManagerCalls.store(0, std::memory_order_release);
      if (TrackResource(resource_id, &payload, 0, &glPinCountingManager) != ERR::Okay) return -1;

      std::barrier race_start(3);
      ERR pin_error = ERR::Failed;
      ERR free_error = ERR::Failed;

      std::thread pin_thread([&]() {
         race_start.arrive_and_wait();
         pin_error = PinResource(resource_id);
      });
      std::thread free_thread([&]() {
         race_start.arrive_and_wait();
         free_error = FreeResource(resource_id);
      });

      race_start.arrive_and_wait();
      pin_thread.join();
      free_thread.join();

      if (pin_error IS ERR::Okay) {
         if ((free_error != ERR::InUse) or (UnpinResource(resource_id) != ERR::Okay)) {
            log.warning("Pin/free race did not defer collection, iteration %d.", iteration);
            return -1;
         }
      }
      else if ((free_error != ERR::Okay) or
               ((pin_error != ERR::MarkedForDeletion) and (pin_error != ERR::DoesNotExist))) {
         log.warning("Pin/free race produced invalid results, iteration %d.", iteration);
         return -1;
      }

      if ((glPinManagerCalls.load(std::memory_order_acquire) != 1) or
          (CheckResourceExists(resource_id) != ERR::False)) {
         log.warning("Pin/free race did not collect exactly once, iteration %d.", iteration);
         return -1;
      }
   }

   for (int iteration=0; iteration < 100; iteration++) {
      resource_id = AllocateID(IDTYPE::RESOURCE);
      glPinManagerCalls.store(0, std::memory_order_release);
      if ((TrackResource(resource_id, &payload, 0, &glPinCountingManager) != ERR::Okay) or
          (PinResource(resource_id) != ERR::Okay)) return -1;

      std::barrier final_start(thread_count + 1);
      std::thread final_threads[thread_count];
      final_threads[0] = std::thread([&]() {
         final_start.arrive_and_wait();
         UnpinResource(resource_id);
      });

      for (int i=1; i < thread_count; i++) {
         final_threads[i] = std::thread([&]() {
            final_start.arrive_and_wait();
            FreeResource(resource_id);
         });
      }

      final_start.arrive_and_wait();
      for (auto &thread : final_threads) thread.join();

      if ((glPinManagerCalls.load(std::memory_order_acquire) != 1) or
          (CheckResourceExists(resource_id) != ERR::False)) {
         log.warning("Final unpin/free race did not collect exactly once, iteration %d.", iteration);
         return -1;
      }
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
   auto pin_error = PinResource(GetMemoryID(memory));

   glManagerCanFinish.store(true, std::memory_order_release);
   thread.join();

   if (second_error != ERR::InUse) {
      log.warning("FreeResource() returned %s for a terminating resource.", GetErrorMsg(second_error));
      return -1;
   }

   if (pin_error != ERR::MarkedForDeletion) {
      log.warning("PinResource() returned %s for a terminating resource.", GetErrorMsg(pin_error));
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

   if (run_resource_pin_functional_checks() != 0) {
      close_kotuku();
      return -1;
   }

   if (run_custom_resource_pin_checks() != 0) {
      close_kotuku();
      return -1;
   }

   if (run_pinned_owner_cascade_check() != 0) {
      close_kotuku();
      return -1;
   }

   if (run_pinned_record_mutation_check() != 0) {
      close_kotuku();
      return -1;
   }

   if (run_threaded_resource_pin_checks() != 0) {
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

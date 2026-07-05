/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

This program tests resource locking and termination behaviour.

*********************************************************************************************************************/

#include <pthread.h>
#include <atomic>
#include <cstdint>
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
   nullptr,
   nullptr,
   true
};

//********************************************************************************************************************

static void * free_terminating_resource(void *)
{
   glConcurrentFreeError = FreeResource(glTerminatingResource);
   return nullptr;
}

//********************************************************************************************************************

static void * alloc_free_worker(void *Arg)
{
   auto base = int((intptr_t)Arg);

   for (int i=0; i < 250; i++) {
      APTR memory = nullptr;
      if (AllocMemory(32 + ((base + i) % 96), MEM::DATA, &memory) != ERR::Okay) {
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

   return nullptr;
}

//********************************************************************************************************************

static int run_concurrent_alloc_free_check(void)
{
   kt::Log log(__FUNCTION__);

   static constexpr int thread_count = 4;
   pthread_t threads[thread_count];

   glAllocFreeFailures.store(0, std::memory_order_release);

   for (int i=0; i < thread_count; i++) {
      if (pthread_create(&threads[i], nullptr, &alloc_free_worker, (void *)(intptr_t)(i * 1000)) != 0) {
         log.warning("pthread_create() failed for concurrent allocation worker %d.", i);
         return -1;
      }
   }

   for (int i=0; i < thread_count; i++) pthread_join(threads[i], nullptr);

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

   if (AllocMemory(64, MEM::DATA, &memory) != ERR::Okay) {
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
   if (AllocMemory(64, MEM::DATA, &memory) != ERR::Okay) {
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

   pthread_t thread;
   pthread_create(&thread, nullptr, &free_terminating_resource, nullptr);

   while (not glManagerEntered.load(std::memory_order_acquire)) WaitTime(0.001);

   auto second_error = FreeResource(memory);

   glManagerCanFinish.store(true, std::memory_order_release);
   pthread_join(thread, nullptr);

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

static int run_access_object_checks(void)
{
   kt::Log log(__FUNCTION__);

   APTR memory = nullptr;
   if (AllocMemory(64, MEM::DATA, &memory) != ERR::Okay) {
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

   if (run_pinned_forced_object_free_check() != 0) {
      close_kotuku();
      return -1;
   }

   printf("Testing complete.\n");

   close_kotuku();
}

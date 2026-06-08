/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

This program tests resource locking and termination behaviour.

*********************************************************************************************************************/

#include <pthread.h>
#include <atomic>
#include <kotuku/startup.h>

using namespace kt;

CSTRING ProgName = "ResourceLocking";
static APTR glTerminatingResource = nullptr;
static std::atomic_bool glManagerEntered = false;
static std::atomic_bool glManagerCanFinish = false;
static ERR glConcurrentFreeError = ERR::Okay;

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

   if (auto error = TrackResource(GetMemoryID(memory), memory, 0, &glTerminatingResourceManager, 64); error != ERR::Okay) {
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
   if (error IS ERR::Okay) {
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

   if (error IS ERR::Okay) {
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
      if (error IS ERR::Okay) ReleaseObject(locked);
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

   if (run_terminating_resource_check() != 0) {
      close_kotuku();
      return -1;
   }

   printf("Testing complete.\n");

   close_kotuku();
}

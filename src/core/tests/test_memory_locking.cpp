/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

This program tests the locking of memory between threads.

*********************************************************************************************************************/

#include <pthread.h>
#include <atomic>
#include <kotuku/startup.h>
#include <kotuku/strings.hpp>

using namespace kt;

CSTRING ProgName = "MemoryLocking";
static volatile MEMORYID glMemoryID = 0;
static uint32_t glTotalThreads = 2;
static uint32_t glLockAttempts = 20;
static int glAccessGap = 2000;
static bool glTerminateMemory = false;
static bool glTestAllocation = false;
static APTR glTerminatingResource = nullptr;
static std::atomic_bool glManagerEntered = false;
static std::atomic_bool glManagerCanFinish = false;
static ERR glConcurrentFreeError = ERR::Okay;

struct thread_info{
   pthread_t thread;
   int index;
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

//********************************************************************************************************************

static void * test_locking(void *Arg)
{
   kt::Log log(__FUNCTION__);
   auto info = (thread_info *)Arg;

   info->index = GetResource(RES::THREAD_ID);
   log.msg("----- Thread %d is starting now.", info->index);

   for (unsigned i=0; i < glLockAttempts; i++) {
      if (!glMemoryID) break;
      //log.branch("Attempt %d.%d: Acquiring the memory.", info->index, i);

      int8_t *memory;
      if (auto error = AccessMemory(glMemoryID, MEM::READ_WRITE, 30000, (APTR *)&memory); error IS ERR::Okay) {
         memory[0]++;
         log.msg("%d.%d: Memory acquired.", info->index, i);
         WaitTime(0.002); // Wait 2 milliseconds
         if (memory[0] > 1) log.warning("--- MAJOR ERROR %d: More than one thread has access to this memory!", info->index);
         memory[0]--;

         // Test that object removal works in ReleaseObject() and that waiting threads fail peacefully.

         if (glTerminateMemory) {
            if (i >= glLockAttempts-2) {
               FreeResource(memory);
               ReleaseMemory(glMemoryID);
               memory = nullptr;
               break;
            }
         }

         ReleaseMemory(glMemoryID);

         log.msg("%d: Memory released.", info->index);

         #ifdef __unix__
            sched_yield();
         #endif
         if (glAccessGap > 0) WaitTime(glAccessGap / 1000000.0); // Convert microseconds to seconds
      }
      else log.msg("Attempt %d.%d: Failed to acquire a lock, error: %s", info->index, i, GetErrorMsg(error));
   }

   log.msg("----- Thread %d is finished.", info->index);
   return nullptr;
}

//********************************************************************************************************************
// Allocate and free sets of memory blocks at random intervals.

static constexpr int glTotalAlloc = 2000;

static void * test_allocation(void *Arg)
{
   APTR memory[glTotalAlloc];

   int i, j;
   int start = 0;
   for (i=0; i < glTotalAlloc; i++) {
      AllocMemory(1024, MEM::DATA|MEM::NO_CLEAR, &memory[i]);
      if (rand() % 10 > 7) {
         for (j=start; j < i; j++) {
            FreeResource(memory[j]);
         }
         start = j;
      }
   }

   for (j=start; j < i; j++) {
      FreeResource(memory[j]);
   }

   return nullptr;
}

//********************************************************************************************************************

int main(int argc, CSTRING *argv)
{
   if (auto msg = init_kotuku(argc, argv)) {
      printf("%s\n", msg);
      return -1;
   }

   kt::vector<std::string> *args;
   if ((CurrentTask()->get(FID_Parameters, args) IS ERR::Okay) and (args)) {
      for (unsigned i=0; i < args->size(); i++) {
         if (iequals(args[0][i], "-threads")) {
            if (++i < args->size()) glTotalThreads = strtol(args[0][i].c_str(), nullptr, 0);
            else break;
         }
         else if (iequals(args[0][i], "-attempts")) {
            if (++i < args->size()) glLockAttempts = strtol(args[0][i].c_str(), nullptr, 0);
            else break;
         }
         else if (iequals(args[0][i], "-gap")) {
            if (++i < args->size()) glAccessGap = strtol(args[0][i].c_str(), nullptr, 0);
            else break;
         }
         else if (iequals(args[0][i], "-terminate")) glTerminateMemory = true;
         else if (iequals(args[0][i], "-alloc")) glTestAllocation = true;
      }
   }

   if (run_access_object_checks() != 0) {
      close_kotuku();
      return -1;
   }

   if (run_terminating_resource_check() != 0) {
      close_kotuku();
      return -1;
   }

   APTR mem = nullptr;
   if (AllocMemory(10000, MEM::DATA, &mem) != ERR::Okay) return -1;
   glMemoryID = GetMemoryID(mem);

   printf("Spawning %d threads...\n", glTotalThreads);

   thread_info glThreads[glTotalThreads];

   for (unsigned i=0; i < glTotalThreads; i++) {
      glThreads[i].index = i;
      if (glTestAllocation) pthread_create(&glThreads[i].thread, nullptr, &test_allocation, &glThreads[i]);
      else pthread_create(&glThreads[i].thread, nullptr, &test_locking, &glThreads[i]);
   }

   // Main block now waits for both threads to terminate, before it exits.  If main block exits, both threads exit,
   // even if the threads have not finished their work

   printf("Waiting for thread completion.\n");

   for (unsigned i=0; i < glTotalThreads; i++) {
      pthread_join(glThreads[i].thread, nullptr);
   }

   FreeResource(mem);

   printf("Testing complete.\n");

   close_kotuku();
}

/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

This program tests the locking of memory between threads.

*********************************************************************************************************************/

#include <pthread.h>
#include <kotuku/startup.h>
#include <kotuku/strings.hpp>

using namespace pf;

CSTRING ProgName = "MemoryLocking";
static volatile MEMORYID glMemoryID = 0;
static uint32_t glTotalThreads = 2;
static uint32_t glLockAttempts = 20;
static int glAccessGap = 2000;
static bool glTerminateMemory = false;
static bool glTestAllocation = false;

struct thread_info{
   pthread_t thread;
   int index;
};

//********************************************************************************************************************

static void * test_locking(void *Arg)
{
   pf::Log log(__FUNCTION__);
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
      AllocMemory(1024, MEM::DATA|MEM::NO_CLEAR, &memory[i], nullptr);
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

   pf::vector<std::string> *args;
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

   AllocMemory(10000, MEM::DATA, nullptr, (MEMORYID *)&glMemoryID);

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

   FreeResource(glMemoryID);

   printf("Testing complete.\n");

   close_kotuku();
}

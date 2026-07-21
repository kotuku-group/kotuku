/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
Task: System processes are managed by the Task class.

Tasks, also known as processes, form the basis of process execution in an operating system.  By creating a task object,
it is possible to execute a program from within the host system.

To execute a compiled program, set the #Location field to point to the executable file before initialising the
task.  Arguments can be passed to the executable by setting the #Parameters field.  After initialising the task,
use the #Activate() action to run the executable.  If the program executes successfully, the task object can be
removed and this will not impact the running program.

The task object that represents the active process can be acquired from ~Core.CurrentTask().

-END-

*********************************************************************************************************************/

#define PRV_TASK

#ifdef __unix__
 #include <unistd.h>
 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <fcntl.h>
 #include <time.h>
 #include <errno.h>
 #include <sys/resource.h>
 #include <sys/utsname.h>
 #include <sys/poll.h>
 #include <sys/time.h>
 #include <sys/wait.h>
 #include <sched.h>
#endif

#ifdef _WIN32
 #include <direct.h>
 #include <stdio.h>
#endif

#include <algorithm>
#include <bit>
#include <fstream>
#include <vector>

#include "../defs.h"
#include <kotuku/main.h>

// Buffer size constants
static constexpr size_t TASK_STDIN_BUFFER_SIZE = 4096;
static constexpr size_t TASK_IO_BUFFER_SIZE = 2048;
static constexpr size_t TASK_WIN_BUFFER_SIZE = 4096;

extern "C" void CloseCore(void);

// Helper function to cleanup file descriptors in Unix task activation
#ifdef __unix__
static void cleanup_task_fds(int input_fd, int out_fd, int out_errfd, int in_fd, int in_errfd) {
   if (input_fd != -1)  close(input_fd);
   if (out_fd != -1)    close(out_fd);
   if (out_errfd != -1) close(out_errfd);
   if (in_fd != -1)     close(in_fd);
   if (in_errfd != -1)  close(in_errfd);
}

static void set_task_pipe_nonblocking(int FD)
{
   auto flags = fcntl(FD, F_GETFL);
   if (flags != -1) fcntl(FD, F_SETFL, flags | O_NONBLOCK);
}
#endif

#ifdef __unix__

static void task_stdout(HOSTHANDLE FD, APTR);
static void task_stderr(HOSTHANDLE FD, APTR);

#elif _WIN32

#define DLLCALL // __declspec(dllimport)
#define WINAPI  __stdcall
#define HKEY_CLASSES_ROOT	   (0x80000000)
#define HKEY_CURRENT_USER	   (0x80000001)
#define HKEY_LOCAL_MACHINE	   (0x80000002)
#define HKEY_USERS	         (0x80000003)
#define HKEY_PERFORMANCE_DATA	(0x80000004)
#define HKEY_CURRENT_CONFIG	(0x80000005)
#define HKEY_DYN_DATA	      (0x80000006)

constexpr int REG_DWORD = 4;
constexpr int REG_DWORD_BIG_ENDIAN = 5;
constexpr int REG_QWORD = 11;
constexpr int REG_SZ = 1;
constexpr int REG_EXPAND_SZ = 2;

#define KEY_READ  0x20019
#define KEY_WRITE 0x20006

constexpr int MAX_PATH = 260;
#define INVALID_HANDLE_VALUE (void *)(-1)

extern "C" DLLCALL int WINAPI RegOpenKeyExA(int,CSTRING,int,int,APTR *);
extern "C" DLLCALL int WINAPI RegQueryValueExA(APTR,CSTRING,int *,int *,int8_t *,int *);
extern "C" DLLCALL int WINAPI RegSetValueExA(APTR hKey, CSTRING lpValueName, int Reserved, int dwType, const void *lpData, int cbData);
extern "C" DLLCALL int WINAPI RegEnumValueA(APTR hKey, int dwIndex, STRING lpValueName, int *lpcchValueName, int *lpReserved, int *lpType, int8_t *lpData, int *lpcbData);
extern "C" DLLCALL int WINAPI RegEnumKeyExA(APTR hKey, int dwIndex, STRING lpName, int *lpcchName, int *lpReserved, STRING lpClass, int *lpcchClass, void *lpftLastWriteTime);
extern "C" DLLCALL int WINAPI GetFileVersionInfoSizeA(CSTRING,int *);
extern "C" DLLCALL int WINAPI GetFileVersionInfoA(CSTRING,int,int,APTR);

static MSGID glProcessBreak = MSGID::NIL;
#endif

static ERR TASK_Activate(extTask *);
static ERR TASK_GetEnv(extTask *, struct task::GetEnv *);
static ERR TASK_GetKey(extTask *, struct acGetKey *);
static ERR TASK_Init(extTask *);
static ERR TASK_New(extTask *);
static ERR TASK_Query(extTask *);
static ERR TASK_SetEnv(extTask *, struct task::SetEnv *);
static ERR TASK_SetKey(extTask *, struct acSetKey *);
static ERR TASK_Write(extTask *, struct acWrite *);

static ERR TASK_AddArgument(extTask *, struct task::AddArgument *);
static ERR TASK_Expunge(extTask *);
static ERR TASK_Quit(extTask *);

#include "class_task_def.c"
#include "../microsoft/pe_metadata.cpp"

//********************************************************************************************************************

static void task_stdinput_callback(HOSTHANDLE FD, void *Task)
{
   auto Self = (extTask *)Task;
   char buffer[TASK_STDIN_BUFFER_SIZE];
   ERR error;

#ifdef _WIN32
   int bytes_read;
   auto result = winReadStdInput(FD, buffer, sizeof(buffer)-1, &bytes_read);
   if (not result) error = ERR::Okay;
   else if (result IS 1) return;
   else if (result IS -2) {
      error = ERR::Finished;
      RegisterFD(winGetStdInput(), RFD::READ|RFD::REMOVE, &task_stdinput_callback, Self);
   }
   else return;
#else
   int bytes_read = read(fileno(stdin), buffer, sizeof(buffer)-1);
   if (bytes_read >= 0) error = ERR::Okay;
   else error = ERR::Finished;
#endif

   if ((bytes_read > 0) and (unsigned(bytes_read) < sizeof(buffer))) {
      buffer[bytes_read] = 0;
   }
   else {
      bytes_read = 0;
      buffer[0] = 0;
   }

   if (Self->InputCallback.stale()) clear_callback(Self->InputCallback);
   else if (Self->InputCallback.isC()) {
      auto routine = (void (*)(extTask *, APTR, int, ERR, APTR))Self->InputCallback.Routine;
      routine(Self, buffer, bytes_read, error, Self->InputCallback.Meta);
   }
   else if (Self->InputCallback.isScript()) {
      sc::Call(Self->InputCallback, std::to_array<ScriptArg>({
         { "Task",       Self },
         { "Buffer",     buffer, FD_PTRBUFFER },
         { "BufferSize", bytes_read, FD_INT|FD_BUFSIZE },
         { "Status",     int(error), FD_ERROR }
      }));
   }
}

#ifdef __unix__
static void check_incoming(extTask *Self)
{
   struct pollfd fd;

   if (Self->InFD != -1) {
      fd.fd = Self->InFD;
      fd.events = POLLIN;
      if ((poll(&fd, 1, 0) > 0) and (fd.revents & POLLIN)) {
         task_stdout(Self->InFD, Self);
      }
   }

   if (Self->ErrFD != -1) {
      fd.fd = Self->ErrFD;
      fd.events = POLLIN;
      if ((poll(&fd, 1, 0) > 0) and (fd.revents & POLLIN)) {
         task_stderr(Self->ErrFD, Self);
      }
   }
}
#endif

//********************************************************************************************************************
// Data output from the executed process is passed via data channels to the object specified in Task->OutputID, and/or
// sent to a callback function.

#ifdef __unix__
static int read_task_stdout(HOSTHANDLE FD, APTR Task)
{
   int len;
   char buffer[TASK_IO_BUFFER_SIZE];
   if ((len = read(FD, buffer, sizeof(buffer)-1)) > 0) {
      buffer[len] = 0;

      auto task = (extTask *)Task;
      if (task->OutputCallback.stale()) clear_callback(task->OutputCallback);
      else if (task->OutputCallback.isC()) {
         auto routine = (void (*)(extTask *, APTR, int, APTR))task->OutputCallback.Routine;
         routine(task, buffer, len, task->OutputCallback.Meta);
      }
      else if (task->OutputCallback.isScript()) {
         sc::Call(task->OutputCallback, std::to_array<ScriptArg>({
            { "Task",       Task,   FD_OBJECTPTR },
            { "Buffer",     buffer, FD_PTRBUFFER },
            { "BufferSize", len,    FD_INT|FD_BUFSIZE }
         }));
      }
   }

   return len;
}

static void close_task_stdout(extTask *Task, HOSTHANDLE FD)
{
   if (Task->InFD IS FD) Task->InFD = -1;
   RegisterFD(FD, RFD::READ|RFD::REMOVE, &task_stdout, Task);
   close(FD);
}

static int process_task_stdout(HOSTHANDLE FD, APTR Task, bool Drain)
{
   thread_local uint8_t recursive = 0;

   if (recursive) return -1;

   int len = -1;
   recursive++;
   do {
      len = read_task_stdout(FD, Task);
      if ((not Drain) or (len <= 0)) break;
   } while (true);
   recursive--;

   return len;
}

static void task_stdout(HOSTHANDLE FD, APTR Task)
{
   if (process_task_stdout(FD, Task, false) IS 0) close_task_stdout((extTask *)Task, FD);
}

static bool drain_task_stdout(HOSTHANDLE FD, APTR Task)
{
   return process_task_stdout(FD, Task, true) IS 0;
}

static int read_task_stderr(HOSTHANDLE FD, APTR Task)
{
   char buffer[TASK_IO_BUFFER_SIZE];
   int len;

   if ((len = read(FD, buffer, sizeof(buffer)-1)) > 0) {
      buffer[len] = 0;

      auto task = (extTask *)Task;
      if (task->ErrorCallback.stale()) clear_callback(task->ErrorCallback);
      else if (task->ErrorCallback.isC()) {
         auto routine = (void (*)(extTask *, APTR, int, APTR))task->ErrorCallback.Routine;
         routine(task, buffer, len, task->ErrorCallback.Meta);
      }
      else if (task->ErrorCallback.isScript()) {
         sc::Call(task->ErrorCallback, std::to_array<ScriptArg>({
            { "Task", Task, FD_OBJECTPTR },
            { "Data", buffer, FD_PTRBUFFER },
            { "Size", len, FD_INT|FD_BUFSIZE }
         }));
      }
   }

   return len;
}

static void close_task_stderr(extTask *Task, HOSTHANDLE FD)
{
   if (Task->ErrFD IS FD) Task->ErrFD = -1;
   RegisterFD(FD, RFD::READ|RFD::REMOVE, &task_stderr, Task);
   close(FD);
}

static int process_task_stderr(HOSTHANDLE FD, APTR Task, bool Drain)
{
   thread_local uint8_t recursive = 0;

   if (recursive) return -1;

   int len = -1;
   recursive++;
   do {
      len = read_task_stderr(FD, Task);
      if ((not Drain) or (len <= 0)) break;
   } while (true);
   recursive--;

   return len;
}

static void task_stderr(HOSTHANDLE FD, APTR Task)
{
   if (process_task_stderr(FD, Task, false) IS 0) close_task_stderr((extTask *)Task, FD);
}

static bool drain_task_stderr(HOSTHANDLE FD, APTR Task)
{
   return process_task_stderr(FD, Task, true) IS 0;
}
#endif

//********************************************************************************************************************
// task_incoming_stdout() and task_incoming_stderr() are callbacks that are activated when data is incoming from a
// process that we've launched.

#ifdef _WIN32
static void output_callback(extTask *Task, FUNCTION *Callback, APTR Buffer, int Size)
{
   if (Callback->isC()) {
      auto routine = (void (*)(extTask *, APTR, int, APTR))Callback->Routine;
      routine(Task, Buffer, Size, Callback->Meta);
   }
   else if (Callback->isScript()) {
      sc::Call(*Callback, std::to_array<ScriptArg>({
         { "Task", Task, FD_OBJECTPTR },
         { "Data", Buffer, FD_PTRBUFFER },
         { "Size", Size, FD_INT|FD_BUFSIZE }
      }));
   }
}

static void task_incoming_stdout(WINHANDLE Handle, extTask *Task)
{
   kt::Log log(__FUNCTION__);
   thread_local uint8_t recursive = 0;

   if (recursive) return;
   if (not Task->Platform) return;

   log.traceBranch();

   char buffer[TASK_WIN_BUFFER_SIZE];
   int size = sizeof(buffer) - 1;
   winResetStdOut(Task->Platform, buffer, &size);

   if (size > 0) {
      recursive = 1;
      buffer[size] = 0;
      output_callback(Task, &Task->OutputCallback, buffer, size);
      recursive = 0;
   }
}

static void task_incoming_stderr(WINHANDLE Handle, extTask *Task)
{
   kt::Log log(__FUNCTION__);
   thread_local uint8_t recursive = 0;

   if (recursive) return;
   if (not Task->Platform) return;

   log.traceBranch();

   char buffer[TASK_WIN_BUFFER_SIZE];
   int size = sizeof(buffer) - 1;
   winResetStdErr(Task->Platform, buffer, &size);

   if (size > 0) {
      recursive = 1;
      buffer[size] = 0;
      output_callback(Task, &Task->ErrorCallback, buffer, size);
      recursive = 0;
   }
}

//********************************************************************************************************************
// These functions arrange for callbacks to be made whenever one of our process-connected pipes receives data.

extern "C" void task_register_stdout(extTask *Task, WINHANDLE Handle)
{
   kt::Log log(__FUNCTION__);
   log.traceBranch("Handle: %d", (int)(MAXINT)Handle);
   RegisterFD(Handle, RFD::READ, (void (*)(void *, void *))&task_incoming_stdout, Task);
}

extern "C" void task_register_stderr(extTask *Task, WINHANDLE Handle)
{
   kt::Log log(__FUNCTION__);
   log.traceBranch("Handle: %d", (int)(MAXINT)Handle);
   RegisterFD(Handle, RFD::READ, (void (*)(void *, void *))&task_incoming_stderr, Task);
}

//********************************************************************************************************************

extern "C" void task_deregister_incoming(WINHANDLE Handle)
{
   RegisterFD(Handle, RFD::REMOVE|RFD::READ|RFD::WRITE|RFD::EXCEPT, nullptr, nullptr);
}
#endif

//********************************************************************************************************************

static CSTRING action_id_name(ACTIONID ActionID)
{
   static char idname[20];
   if ((ActionID > AC::NIL) and (ActionID < AC::END)) {
      return ActionTable[int(ActionID)].Name;
   }
   else {
      snprintf(idname, sizeof(idname), "%d", int(ActionID));
      return idname;
   }
}

//********************************************************************************************************************

static ERR msg_action(APTR Custom, int MsgID, int MsgType, APTR Message, int MsgSize)
{
   kt::Log log("ProcessMessages");
   ActionMessage *action;
   const FunctionField *copied_fields = nullptr;
   bool executed = false;

   if (not (action = (ActionMessage *)Message)) {
      log.warning("No data attached to MSGID::ACTION message.");
      return ERR::Okay;
   }

   copied_fields = action->Fields;

   #ifdef DBG_INCOMING
      log.function("Executing action %s on object #%d, Data: %p, Size: %d", action_id_name(action->ActionID), action->ObjectID, Message, MsgSize);
   #endif

   if ((action->ObjectID) and (action->ActionID != AC::NIL)) {
      OBJECTPTR obj;
      if (auto error = AccessObject(action->ObjectID, 5000, &obj); !error) {
         if (action->SendArgs IS false) {
            glCurrentActionMsg = action;
            Action(action->ActionID, obj, nullptr);
            glCurrentActionMsg = nullptr;
            ReleaseObject(obj);
         }
         else {
            const FunctionField *fields;
            if (action->ActionID > AC::NIL) fields = ActionTable[int(action->ActionID)].Args;
            else {
               auto cl = obj->ExtClass;
               if (cl->Base) cl = cl->Base;
               fields = cl->Methods[-int(action->ActionID)].Args;
            }

            if (fields) {
               // Argument pointers were serialised as offsets by QueueAction(); rebase them against this copy.
               make_args_absolute(fields, action->ArgsSize, (int8_t *)(action + 1));
               glCurrentActionMsg = action;
               Action(action->ActionID, obj, action+1);
               executed = true;
               glCurrentActionMsg = nullptr;
               ReleaseObject(obj);
            }
         }
      }
      else if ((error != ERR::NoMatchingObject) and (error != ERR::MarkedForDeletion)) {
         if (action->ActionID > AC::NIL) log.warning("Could not gain access to object %d to execute action %s.", action->ObjectID, action_id_name(action->ActionID));
         else log.warning("Could not gain access to object %d to execute method %d.", action->ObjectID, int(action->ActionID));
      }
   }
   else log.warning("Action message %s specifies an object ID of #%d.", action_id_name(action->ActionID), action->ObjectID);

   if (action->SendArgs and copied_fields) {
      release_copied_args(copied_fields, action->ArgsSize, (int8_t *)(action + 1), not executed);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR msg_quit(APTR Custom, int MsgID, int MsgType, APTR Message, int MsgSize)
{
   kt::Log log(__FUNCTION__);
   log.function("Processing quit message");
   glTaskState = TSTATE::STOPPING;
   return ERR::Okay;
}

//********************************************************************************************************************

#ifdef __unix__
static void task_process_end(extTask *Task, int ReturnCode, bool Returned)
{
   if ((Task->InFD != -1) and (drain_task_stdout(Task->InFD, Task))) close_task_stdout(Task, Task->InFD);
   if ((Task->ErrFD != -1) and (drain_task_stderr(Task->ErrFD, Task))) close_task_stderr(Task, Task->ErrFD);

   if (Returned) {
      Task->ReturnCode = ReturnCode;
      Task->ReturnCodeSet = true;
   }

   if (Task->ExitCallback.stale()) clear_callback(Task->ExitCallback);
   else if (Task->ExitCallback.isC()) {
      auto routine = (void (*)(extTask *, APTR))Task->ExitCallback.Routine;
      routine(Task, Task->ExitCallback.Meta);
   }
   else if (Task->ExitCallback.isScript()) {
      sc::Call(Task->ExitCallback, std::to_array<ScriptArg>({ { "Task", Task, FD_OBJECTPTR } }));
   }
}
#endif

//********************************************************************************************************************
// Determine whether or not a process is alive

extern "C" ERR validate_process(int ProcessID)
{
   kt::Log log(__FUNCTION__);

   log.function("PID: %d", ProcessID);

   if (glValidateProcessID IS ProcessID) glValidateProcessID = 0;
   if ((ProcessID IS glProcessID) or (not ProcessID)) return ERR::Okay;

   #ifdef _WIN32
      // On Windows we don't check if the process is alive because validation can often occur during the final shutdown
      // phase of the other process.
   #elif __unix__
      if ((kill(ProcessID, 0) IS -1) and (errno IS ESRCH));
      else return ERR::Okay;
   #else
      log.error("This platform does not support validate_process()");
      return ERR::Okay;
   #endif

   OBJECTID task_id = 0;
   int return_code = 0;
   bool returned = false;

   for (auto it = glTasks.begin(); it != glTasks.end(); it++) {
      if (it->ProcessID IS ProcessID) {
         task_id = it->TaskID;
         return_code = it->ReturnCode;
         returned = it->Returned;
         glTasks.erase(it);
         break;
      }
   }

   if (not task_id) return ERR::False;

   #ifdef __unix__
      if (auto task = (extTask *)GetObjectPtr(task_id)) task_process_end(task, return_code, returned);
   #endif

   evTaskRemoved task_removed = { GetEventID(EVG::SYSTEM, "task", "removed"), task_id, ProcessID };
   BroadcastEvent(&task_removed, sizeof(task_removed));

   return ERR::False; // Return ERR::False to indicate that the task was not healthy
}

//********************************************************************************************************************
// This function is called when a WIN32 process that we launched has been terminated.
//
// For the linux equivalent, refer to internal.c validate_processID().

#ifdef _WIN32
static void task_process_end(WINHANDLE FD, extTask *Task)
{
   kt::Log log(__FUNCTION__);

   winGetExitCodeProcess(Task->Platform, &Task->ReturnCode);
   if (Task->ReturnCode != 259) {
      Task->ReturnCodeSet = true;
      log.branch("Process %" PF64 " ended, return code: %d.", (int64_t)FD, Task->ReturnCode);
   }
   else log.branch("Process %" PF64 " signalled exit too early.", (int64_t)FD);

   if (Task->Platform) {
      char buffer[TASK_WIN_BUFFER_SIZE];
      int size;

      // Process remaining data

      do {
         size = sizeof(buffer);
         if ((not winReadStd(Task->Platform, TSTD_OUT, buffer, &size)) and (size)) {
            log.msg("Processing %d remaining bytes on stdout.", size);
            output_callback(Task, &Task->OutputCallback, buffer, size);
         }
         else break;
      } while (size IS sizeof(buffer));

      do {
         size = sizeof(buffer);
         if ((not winReadStd(Task->Platform, TSTD_ERR, buffer, &size)) and (size)) {
            log.msg("Processing %d remaining bytes on stderr.", size);
            output_callback(Task, &Task->ErrorCallback, buffer, size);
         }
         else break;
      } while (size IS sizeof(buffer));

      winFreeProcess(Task->Platform);
      Task->Platform = nullptr;
   }
   else winCloseHandle(FD); // winFreeProcess() normally does this with Process->Handle

   // Call ExitCallback, if specified

   if (Task->ExitCallback.stale()) clear_callback(Task->ExitCallback);
   else if (Task->ExitCallback.isC()) {
      auto routine = (void (*)(extTask *, APTR))Task->ExitCallback.Routine;
      routine(Task, Task->ExitCallback.Meta);
   }
   else if (Task->ExitCallback.isScript()) {
      sc::Call(Task->ExitCallback, std::to_array<ScriptArg>({ { "Task", Task, FD_OBJECTPTR } }));
   }

   // Post an event for the task's closure

   evTaskRemoved task_removed = { EVID_SYSTEM_TASK_REMOVED, Task->UID, Task->ProcessID };
   BroadcastEvent(&task_removed, sizeof(task_removed));

   // Send a break if we're waiting for this process to end

   if (((Task->Flags & TSF::WAIT) != TSF::NIL) and (Task->TimeOut > 0)) SendMessage(glProcessBreak, MSF::NIL, {});
}
#endif

//********************************************************************************************************************

#ifdef _WIN32
extern "C" void register_process_pipes(extTask *Self, WINHANDLE ProcessHandle)
{
   kt::Log log;
   log.traceBranch("Process: %d", (int)(MAXINT)ProcessHandle);
   RegisterFD(ProcessHandle, RFD::READ, (void (*)(void *, void *))&task_process_end, Self);
}

extern "C" void deregister_process_pipes(extTask *Self, WINHANDLE ProcessHandle)
{
   kt::Log log;
   log.traceBranch("Process: %d", (int)(MAXINT)ProcessHandle);
   if (ProcessHandle) RegisterFD(ProcessHandle, RFD::REMOVE|RFD::READ|RFD::WRITE|RFD::EXCEPT, nullptr, nullptr);
}
#endif

/*********************************************************************************************************************

-ACTION-
Activate: Activating a task object will execute it.

Activating a task results in the execution of the file referenced in the #Location field.

On successful execution, the ProcessID will refer to the ID of the executed process.  This ID is compatible with the
hosting platform's unique process numbers.

If the `WAIT` flag is specified, this action will not return until the executed process has returned or the
#TimeOut (if specified) has expired.  Messages are processed as normal during this time, ensuring that your
process remains responsive while waiting.

The process' return code can be read from the #ReturnCode field after the process has completed its execution.

In Microsoft Windows, output can be redirected to a file if the redirection symbol is used to direct output in one of
the task arguments.  For instance `&gt;C:\output.txt` will redirect both stderr and stdout to
`c:\output.txt`.  The use of `1&gt;` to redirect stdout and `2&gt;` to redirect stderr
independently of each other is also acceptable.

When running a DOS program in Microsoft Windows, the `SHELL` flag can be set in the #Flags field to prevent the
DOS window from appearing.  The DOS window will also be hidden if the stdout or stderr pipes are redirected.

-ERRORS-
Okay
MissingPath: The Location field has not been set.
Failed
TimeOut:     Can be returned if the `WAIT` flag is used.  Indicates that the process was launched, but the timeout expired before the process returned.
ProcessCreation

-END-

*********************************************************************************************************************/

static ERR TASK_Activate(extTask *Self)
{
   kt::Log log;
   int i;
   ERR error;
#ifdef _WIN32
   std::string launchdir;
   bool hide_output;
   int winerror;
#endif
#ifdef __unix__
   int pid;
   int8_t privileged, shell;
   int8_t requested_shell;
#endif

   Self->ReturnCodeSet = false;

   if (Self->Location.empty()) return log.warning(ERR::MissingPath);

   #ifndef __unix__
      // This is a backup in case SIGCHLD signals don't work.  It terminates automatically if no processes remain.
      if (not glJanitorActive) {
         kt::SwitchContext ctx(glCurrentTask);
         auto call = C_FUNCTION(process_janitor);
         SubscribeTimer(60, &call, &glProcessJanitor);
         glJanitorActive = true;
      }
   #endif

#ifdef _WIN32
   // Determine the launch folder

   if (not Self->LaunchPath.empty()) {
      std::string rpath;
      if (!ResolvePath(Self->LaunchPath, RSF::APPROXIMATE|RSF::PATH, &rpath)) {
         launchdir.assign(rpath);
      }
      else launchdir.assign(Self->LaunchPath);
   }
   else if ((Self->Flags & TSF::RESET_PATH) != TSF::NIL) {
      std::string rpath;
      if (!ResolvePath(Self->Location, RSF::APPROXIMATE|RSF::PATH, &rpath)) {
         launchdir.assign(rpath);
      }
      else launchdir.assign(Self->Location);

      if (auto i = launchdir.rfind('\\'); i != std::string::npos) launchdir.resize(i);
      else launchdir.clear();
   }

   // Resolve the location of the executable (may contain a volume) and copy it to the command line buffer.

   i = 0;
   std::ostringstream buffer;
   buffer << '"';
   std::string rpath;
   if (!ResolvePath(Self->Location, RSF::APPROXIMATE|RSF::PATH, &rpath)) {
      buffer << rpath;
   }
   else buffer << Self->Location;
   buffer << '"';

   // Following the executable path are any arguments that have been used

   std::string redirect_stdout;
   std::string redirect_stderr;
   hide_output = false;

   for (auto &param : Self->Parameters) {
      if (param[0] IS '>') {
         // Redirection argument detected

         auto sv = std::string_view(param.begin()+1, param.end());
         if (!ResolvePath(sv, RSF::NO_FILE_CHECK, &redirect_stdout)) {
            redirect_stderr.assign(redirect_stdout);
         }

         log.msg("StdOut/Err redirected to %s", redirect_stdout.c_str());

         hide_output = true;
         continue;
      }
      else if ((param[0] IS '2') and (param[1] IS '>')) {
         log.msg("StdErr redirected to %s", param.c_str() + 2);
         auto sv = std::string_view(param.begin()+2, param.end());
         ResolvePath(sv, RSF::NO_FILE_CHECK, &redirect_stderr);
         hide_output = true;
         continue;
      }
      else if ((param[0] IS '1') and (param[1] IS '>')) {
         log.msg("StdOut redirected to %s", param.c_str() + 2);
         auto sv = std::string_view(param.begin()+2, param.end());
         ResolvePath(sv, RSF::NO_FILE_CHECK, &redirect_stdout);
         hide_output = true;
         continue;
      }

      buffer << ' ';

      if (param.find(' ') != std::string::npos) buffer << '"' << param << '"';
      else buffer << param;
   }

   // Convert single quotes into double quotes

   std::string final_buffer = buffer.str();
   bool whitespace = true;
   for (i=0; i < std::ssize(final_buffer); i++) {
      if (whitespace) {
         if (final_buffer[i] IS '"') {
            // Skip everything inside double quotes
            i++;
            while ((i < std::ssize(final_buffer)) and (final_buffer[i] != '"')) i++;
            if (i >= std::ssize(final_buffer)) break;
            whitespace = false;
            continue;
         }
         else if (final_buffer[i] IS '\'') {
            for (auto j=i+1; final_buffer[j]; j++) {
               if (final_buffer[j] IS '\'') {
                  if (final_buffer[j+1] <= 0x20) {
                     final_buffer[i] = '"';
                     final_buffer[j] = '"';
                  }
                  i = j;
                  break;
               }
               else if (final_buffer[j] IS '"') break;
            }
         }
      }

      if (final_buffer[i] <= 0x20) whitespace = true;
      else whitespace = false;
   }

   log.trace("Exec: %s", final_buffer.c_str());

   // Hide window if this is designated a shell program (i.e. hide the DOS window).
   // NB: If you hide a non-shell program, this usually results in the first GUI window that pops up being hidden.

   if ((Self->Flags & TSF::SHELL) != TSF::NIL) hide_output = true;

   // Determine whether this new process will be a member of the parent process' group.  This can be forced with the TSF::DETACHED/ATTACHED flags,
   // otherwise it will be determined automatically according to the status of our current task.

   bool group;

   if ((Self->Flags & TSF::ATTACHED) != TSF::NIL) group = true;
   else if ((Self->Flags & TSF::DETACHED) != TSF::NIL) group = false;
   else group = true;

   int internal_redirect = 0;
   if (Self->OutputCallback.defined()) internal_redirect |= TSTD_OUT;
   if (Self->ErrorCallback.defined()) internal_redirect |= TSTD_ERR;
   if ((Self->Flags & TSF::PIPE) != TSF::NIL) internal_redirect |= TSTD_IN;

   if (not (winerror = winLaunchProcess(Self, final_buffer.data(), (not launchdir.empty()) ? launchdir.data() : 0, group,
         internal_redirect, &Self->Platform, hide_output, (not redirect_stdout.empty()) ? redirect_stdout.data() : nullptr,
         (not redirect_stderr.empty()) ? redirect_stderr.data() : nullptr, &Self->ProcessID))) {

      error = ERR::Okay;
      if (((Self->Flags & TSF::WAIT) != TSF::NIL) and (Self->TimeOut > 0)) {
         log.msg("Waiting for process to exit.  TimeOut: %.2f sec", Self->TimeOut);

         //if (not glProcessBreak) glProcessBreak = AllocateID(IDTYPE_MESSAGE);
         glProcessBreak = MSGID::BREAK;

         auto wait_error = ProcessMessages(PMF::NIL, Self->TimeOut * 1000.0);
         if (wait_error != ERR::Okay) error = wait_error;

         if ((not Self->ReturnCodeSet) and (Self->Platform)) {
            winGetExitCodeProcess(Self->Platform, &Self->ReturnCode);
            if (Self->ReturnCode != 259) Self->ReturnCodeSet = true;
         }
      }
   }
   else {
      log.warning("Launch Error: %s", winFormatMessage(winerror).c_str());
      error = ERR::ProcessCreation;
   }

   return error;

#elif __unix__

   // Add a 'cd' command so that the application starts in its own folder

   auto shell_quote = [](std::string_view Value) {
      std::string quoted;
      quoted.reserve(Value.size() + 2);
      quoted += '\'';
      for (auto ch : Value) {
         if (ch != '\'') quoted += ch;
         else quoted.append("'\\''");
      }
      quoted += '\'';
      return quoted;
   };

   std::string_view path = Self->LaunchPath;
   requested_shell = ((Self->Flags & TSF::SHELL) != TSF::NIL) ? 1 : 0;

   std::ostringstream buffer;
   std::string fallback_path;

   i = 0;
   if (((Self->Flags & TSF::RESET_PATH) != TSF::NIL) or (not path.empty())) {
      Self->Flags |= TSF::SHELL;

      buffer << "cd ";

      if (path.empty()) {
         fallback_path.assign(Self->Location);
         if (auto i = fallback_path.find_last_of("/\\:"); i != std::string::npos) fallback_path.resize(i+1);
         else fallback_path.clear();
         path = fallback_path;
      }
      std::string rpath;
      if (!ResolvePath(path, RSF::APPROXIMATE|RSF::PATH, &rpath)) {
         while (rpath.ends_with('/')) rpath.pop_back();
         buffer << shell_quote(rpath);
      }
      else {
         auto p = path;
         while (p.ends_with('/')) p.remove_suffix(1);
         buffer << shell_quote(p);
      }

      buffer << " && ";
   }

   // Resolve the location of the executable (may contain an volume) and copy it to the command line buffer.

   std::string rpath;
   if (!ResolvePath(Self->Location, RSF::APPROXIMATE|RSF::PATH, &rpath)) {
      if (((Self->Flags & TSF::SHELL) != TSF::NIL) and (not requested_shell)) buffer << shell_quote(rpath);
      else buffer << rpath;
   }
   else {
      if (((Self->Flags & TSF::SHELL) != TSF::NIL) and (not requested_shell)) buffer << shell_quote(Self->Location);
      else buffer << Self->Location;
   }

   // Following the executable path are any arguments that have been used. NOTE: This isn't needed if TSF::SHELL is used,
   // however it is extremely useful in the debug printout to see what is being executed.

   std::ostringstream params;
   if ((Self->Flags & TSF::SHELL) != TSF::NIL) {
      for (auto &param : Self->Parameters) {
         params << ' ' << shell_quote(param);
      }
   }

   auto final_buffer = buffer.str();

   log.msg("%s", final_buffer.c_str());

   // If we're not going to run in shell mode, create an argument list for passing to the program.

   CSTRING argslist[Self->Parameters.size()+2];
   if ((Self->Flags & TSF::SHELL) IS TSF::NIL) {
      argslist[0] = final_buffer.c_str();
      unsigned i;
      for (i=0; i < Self->Parameters.size(); i++) {
         argslist[i+1] = Self->Parameters[i].c_str();
      }
      argslist[i+1] = nullptr;

      if ((Self->Flags & TSF::VERBOSE) != TSF::NIL) {
         for (i=1; argslist[i]; i++) {
            log.msg("Arg %d: %s", i, argslist[i]);
         }
      }
   }

   int outpipe[2],errpipe[2];
   int out_errfd, in_errfd, out_fd, in_fd, input_fd;

   out_errfd = -1;
   out_fd    = -1;
   in_errfd  = -1;
   in_fd     = -1;

   // File descriptor management for Unix process execution:
   // - input_fd: Connected to /dev/null to prevent child reading from parent stdin
   // - out_fd/in_fd: Pipe pair for capturing child stdout
   // - out_errfd/in_errfd: Pipe pair for capturing child stderr
   // All file descriptors are properly cleaned up on error via cleanup_task_fds()
   input_fd = open("/dev/null", O_RDONLY); // Input is always NULL, we don't want the child process reading from our own stdin stream

   if (Self->OutputCallback.defined()) {
      log.trace("Output will be sent to callback.");
      if (not pipe(outpipe)) {
         out_fd = outpipe[1]; // for writing
         in_fd  = outpipe[0]; // for reading
      }
      else {
         log.warning("Failed to create pipe: %s", strerror(errno));
         cleanup_task_fds(input_fd, out_fd, -1, in_fd, -1);
         return ERR::ProcessCreation;
      }
   }

   if ((out_fd IS -1) and ((Self->Flags & TSF::QUIET) != TSF::NIL)) {
      log.msg("Output will go to NULL");
      out_fd = open("/dev/null", O_WRONLY);
   }

   if (Self->ErrorCallback.defined()) {
      log.trace("Error output will be sent to a callback.");
      if (not pipe(errpipe)) {
         out_errfd = errpipe[1];
         in_errfd  = errpipe[0];
      }
      else {
         log.warning("Failed to create pipe: %s", strerror(errno));
         cleanup_task_fds(input_fd, out_fd, -1, in_fd, -1);
         return ERR::ProcessCreation;
      }
   }

   if ((out_errfd IS -1) and ((Self->Flags & TSF::QUIET) != TSF::NIL)) {
      out_errfd = open("/dev/null", O_WRONLY);
   }

   // Fork a new task.  Remember that forking produces an exact duplicate of the process that made the fork.

   privileged = ((Self->Flags & TSF::PRIVILEGED) != TSF::NIL) ? 1 : 0;
   shell = ((Self->Flags & TSF::SHELL) != TSF::NIL) ? 1 : 0;

   pid = fork();

   if (pid IS -1) {
      cleanup_task_fds(input_fd, out_fd, out_errfd, in_fd, in_errfd);
      if (errno IS EAGAIN) log.warning("Failed in an attempt to fork(): process limit or system resources exhausted.");
      else log.warning("Failed in an attempt to fork(): %s", strerror(errno));
      return ERR::ProcessCreation;
   }

   if (pid) {
      // The following code is executed by the initiating process thread

      log.msg("Created new process %d.  Shell: %d", pid, shell);

      Self->ProcessID = pid; // Record the native process ID

      glTasks.emplace_back(Self);

      if (in_fd != -1) {
         set_task_pipe_nonblocking(in_fd);
         RegisterFD(in_fd, RFD::READ, &task_stdout, Self);
         Self->InFD = in_fd;
         close(out_fd);
      }

      if (in_errfd != -1) {
         set_task_pipe_nonblocking(in_errfd);
         RegisterFD(in_errfd, RFD::READ, &task_stderr, Self);
         Self->ErrFD = in_errfd;
         close(out_errfd);
      }

      // input_fd has no relevance to the parent process

      if (input_fd != -1) {
         close(input_fd);
         input_fd = -1;
      }

      error = ERR::Okay;
      if ((Self->Flags & TSF::WAIT) != TSF::NIL) {
         log.branch("Waiting for process to turn into a zombie in %.2fs.", Self->TimeOut);

         // Wait for the child process to turn into a zombie.  NB: A parent process or our own child handler may
         // potentially pick this up but that's fine as waitpid() will just fail with -1 in that case.

         int status = 0;
         int wait_result = 0;
         int64_t ticks = PreciseTime() + int64_t(Self->TimeOut * 1000000.0);
         while ((wait_result = waitpid(pid, &status, WNOHANG)) IS 0) {
            ProcessMessages(PMF::NIL, 100);

            auto remaining = ticks - PreciseTime();
            if (remaining <= 0) {
               error = log.warning(ERR::TimeOut);
               break;
            }
         }

         // Find out what error code was returned

         if (wait_result IS pid) {
            bool returned = false;
            int return_code = 0;

            if (WIFEXITED(status)) {
               return_code = (int8_t)WEXITSTATUS(status);
               returned = true;
            }
            else if (WIFSIGNALED(status)) {
               return_code = 128 + WTERMSIG(status);
               returned = true;
            }

            if (returned) {
               Self->ReturnCode = return_code;
               Self->ReturnCodeSet = true;

               for (auto &task : glTasks) {
                  if (task.ProcessID IS pid) {
                     task.ReturnCode = return_code;
                     task.Returned = true;
                     break;
                  }
               }

               validate_process(pid);
            }
         }
      }

      check_incoming(Self);

      return error;
   }

   // The following code is executed by the newly forked process. Using execl() is the easiest way to clean up after a
   // fork because it will replace the process image, which means we don't have to worry about freeing memory and the
   // like.

   if (input_fd != -1) { // stdin
      close(0);
      dup2(input_fd, 0);
      close(input_fd);
   }

   // Duplicate our parent's output FD's for stdout and stderr

   if (out_fd != -1) { // stdout
      close(1);
      dup2(out_fd, 1);
      close(out_fd);
   }

   if (out_errfd != -1) { // stderr
      close(2);
      dup2(out_errfd, 2);
      close(out_errfd);
   }

   // Close the read-only end of the pipe as it's not relevant to the forked process.

   if (in_fd != -1)    close(in_fd);
   if (in_errfd != -1) close(in_errfd);

   if (not privileged) { // Drop privileges so that the program runs as normal
      seteuid(glUID);
      setegid(glGID);
      setuid(glUID);
      setgid(glGID);
   }

   final_buffer.append(params.str());
   if (shell) {
      execl("/bin/sh", "sh", "-c", final_buffer.c_str(), (char *)nullptr);
   }
   else execv(final_buffer.c_str(), (char * const *)&argslist);

   _exit(EXIT_FAILURE);
#endif
}

/*********************************************************************************************************************

-METHOD-
AddArgument: Adds a new argument to the Parameters field.

This method will add a new argument to the end of the #Parameters field array.  If the string is surrounded by quotes,
they will be removed automatically.

-INPUT-
strview Argument: The new argument string.

-ERRORS-
Okay
NullArgs

-TAGS-
mutates-object, copies-input

*********************************************************************************************************************/

static ERR TASK_AddArgument(extTask *Self, struct task::AddArgument *Args)
{
   if ((not Args) or Args->Argument.empty()) return ERR::NullArgs;

   auto src = Args->Argument;
   if ((src.front() IS '"') or (src.front() IS '\'')) {
      auto quote = src.front();
      src.remove_prefix(1);
      if (auto end = src.find(quote); end != std::string_view::npos) {
         Self->Parameters.emplace_back(src.substr(0, end));
      }
      else Self->Parameters.emplace_back(src);
   }
   else Self->Parameters.emplace_back(Args->Argument);

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Expunge: Forces a Task to expunge unused code.

The Expunge() method releases all loaded libraries that are no longer in use by the active process.

-ERRORS-
Okay

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR TASK_Expunge(extTask *Self)
{
   Expunge(false);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetEnv: Retrieves environment variables for the active process.

On platforms that support environment variables, GetEnv() returns the value of the environment variable matching the
`Name` string.  If there is no matching variable, `ERR::DoesNotExist` is returned.

In Windows, it is possible to look up registry keys if the string starts with one of the following (in all other
cases, the system's environment variables are queried):

<pre>
\HKEY_LOCAL_MACHINE\
\HKEY_CURRENT_USER\
\HKEY_CLASSES_ROOT\
\HKEY_USERS\
</pre>

If the `Name` string ends with a trailing backslash, all keys and sub-keys held at that location are returned as a
tab-separated list in the form `Name1\tName2...`.  Sub-key names are distinguished from values by a trailing
backslash, e.g. `SubKey\\`.  This is useful for enumerating the complete contents of a registry key without needing
to know names in advance.

NOTE: If your programming language uses backslash as an escape character, remember to use double-backslashes in the
`Name` string.

-INPUT-
strview Name: The name of the environment variable to retrieve.
^&string Value: The value of the environment variable is returned in this parameter.

-ERRORS-
Okay
NullArgs
DoesNotExist: The environment variable is undefined.
NoSupport: The platform does not support environment variables.
ExecViolation
Syntax

-TAGS-
pure-query, caller-owns-result
-END-

*********************************************************************************************************************/

static ERR TASK_GetEnv(extTask *Self, struct task::GetEnv *Args)
{
   kt::Log log;

   if ((not Args) or Args->Name.empty() or (not Args->Value)) return log.warning(ERR::NullArgs);
   Args->Value->clear();

#ifdef _WIN32

   if (glCurrentTask != Self) return ERR::ExecViolation;

   if (Args->Name[0] IS '\\') {
      struct key {
         uint32_t ID;
         std::string_view HKey;
      } keys[] = {
         { HKEY_LOCAL_MACHINE,  "\\HKEY_LOCAL_MACHINE\\" },
         { HKEY_CURRENT_USER,   "\\HKEY_CURRENT_USER\\" },
         { HKEY_CLASSES_ROOT,   "\\HKEY_CLASSES_ROOT\\" },
         { HKEY_USERS,          "\\HKEY_USERS\\" }
      };

      std::string full_path(Args->Name);

      auto format_value = [](int Type, int8_t *Buffer, int Length, std::string &Output) -> bool {
         switch(Type) {
            case REG_DWORD:
               if (unsigned(Length) >= sizeof(int)) Output = std::to_string(((int *)Buffer)[0]);
               return true;

            case REG_DWORD_BIG_ENDIAN:
               if (unsigned(Length) >= sizeof(int)) {
                  if constexpr (std::endian::native IS std::endian::little) {
                     Output = std::to_string(reverse_long(((int *)Buffer)[0]));
                  }
                  else Output = std::to_string(((int *)Buffer)[0]);
               }
               return true;

            case REG_QWORD:
               if (unsigned(Length) >= sizeof(int64_t)) {
                  Output = std::to_string(((int64_t *)Buffer)[0]);
               }
               return true;

            case REG_SZ:
            case REG_EXPAND_SZ:
               Output.assign((char *)Buffer, Length);
               while ((not Output.empty()) and (Output.back() IS 0)) Output.pop_back();
               return true;

            default:
               return false;
         }
      };

      for (auto &key : keys) {
         if (not full_path.starts_with(key.HKey)) continue;

         // A trailing backslash signals enumeration of all values at the given sub-key.

         bool enumerate = full_path.back() IS '\\';

         auto sep = full_path.find_last_of('\\');
         if (sep IS std::string::npos) return log.warning(ERR::Syntax);

         std::string folder;
         std::string name;
         if (enumerate) {
            folder = full_path.substr(key.HKey.size());
         }
         else {
            folder = full_path.substr(key.HKey.size(), sep - key.HKey.size() + 1);
            name = full_path.substr(sep+1);
         }

         APTR keyhandle;
         if (not RegOpenKeyExA(key.ID, folder.c_str(), 0, KEY_READ, &keyhandle)) {
            if (enumerate) {
               char value_name[256];
               int8_t buffer[4096];

               // Enumerate all key-value names stored at this key.

               for (int index = 0; ; index++) {
                  int name_len = sizeof(value_name);
                  int data_len = sizeof(buffer);
                  int type;
                  if (RegEnumValueA(keyhandle, index, value_name, &name_len, 0, &type, buffer, &data_len)) break;
                  if (not Args->Value->empty()) Args->Value->push_back('\t');
                  Args->Value->append(value_name, name_len);
               }

               // Enumerate all sub-keys (folders) at this key.  Each sub-key name is
               // suffixed with '\' so callers can distinguish folders from values.

               for (int index = 0; ; index++) {
                  int name_len = sizeof(value_name);
                  if (RegEnumKeyExA(keyhandle, index, value_name, &name_len, 0, nullptr, nullptr, nullptr)) break;

                  if (not Args->Value->empty()) Args->Value->push_back('\t');
                  Args->Value->append(value_name, name_len);
                  Args->Value->push_back('\\');
               }

               winCloseHandle(keyhandle);
               if (Args->Value->empty()) return ERR::DoesNotExist;
               return ERR::Okay;
            }
            else {
               int type;
               int8_t buffer[4096];
               int envlen = sizeof(buffer);
               bool found = false;
               if (not RegQueryValueExA(keyhandle, name.c_str(), 0, &type, buffer, &envlen)) {
                  if (not format_value(type, buffer, envlen, *Args->Value)) {
                     log.warning("Unsupported registry type %d for key %.*s", type, int(Args->Name.size()),
                        Args->Name.data());
                  }
                  else found = true;
               }
               winCloseHandle(keyhandle);

               if (found) return ERR::Okay;
               else return ERR::DoesNotExist;
            }
         }
         else return ERR::DoesNotExist;
      }
   }

   std::string name(Args->Name);
   winGetEnv(name.c_str(), *Args->Value);
   if (Args->Value->empty()) return ERR::DoesNotExist;
   return ERR::Okay;

#elif __unix__

   std::string name(Args->Name);
   if (auto value = getenv(name.c_str())) {
      Args->Value->assign(value);
      return ERR::Okay;
   }
   else return ERR::DoesNotExist;
#else
   #warn Write support for GetEnv()
   return ERR::NoSupport;
#endif
}

/*********************************************************************************************************************
-ACTION-
GetKey: Retrieves custom key values.
-END-
*********************************************************************************************************************/

static ERR TASK_GetKey(extTask *Self, struct acGetKey *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Value)) return log.warning(ERR::NullArgs);

   if (auto it = Self->Fields.find(Args->Key); it != Self->Fields.end()) {
      Args->Value->assign(it->second);
      return ERR::Okay;
   }
   else return log.warning(ERR::UnsupportedField);
}

/*********************************************************************************************************************

-ACTION-
Query: Reads executable metadata from the file referenced in Location.

Query() reads Windows VERSIONINFO metadata from the executable referenced by the #Location field.  Discovered string
properties are stored in the task's key-value list and can be retrieved with #GetKey().  Existing keys with matching
names are overwritten, while unrelated custom keys are preserved.

The #Name field is updated from the ProductName, FileDescription or InternalName metadata when one of those properties
is available.

-ERRORS-
Okay
MissingPath: The Location field has not been set.
File:        The executable file could not be opened.
Query:       The executable did not contain readable VERSIONINFO metadata.

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR TASK_Query(extTask *Self)
{
   kt::Log log;

   if (Self->Location.empty()) return log.warning(ERR::MissingPath);

   std::string path;
   if (ResolvePath(Self->Location, RSF::APPROXIMATE|RSF::PATH, &path) != ERR::Okay) return log.warning(ERR::ResolvePath);

   TaskVersionMetadata metadata;
   if (auto error = load_task_version_metadata(path, metadata); error != ERR::Okay) return log.warning(error);

   for (auto &field : metadata.Fields) Self->Fields[field.first] = field.second;

   // Promote relevant metadata to the Name field

   if (auto it = metadata.Fields.find("ProductName"); it != metadata.Fields.end()) Self->Name = it->second;
   else if (auto desc = metadata.Fields.find("FileDescription"); desc != metadata.Fields.end()) Self->Name = desc->second;
   else if (auto name = metadata.Fields.find("InternalName"); name != metadata.Fields.end()) Self->Name = name->second;

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR TASK_Init(extTask *Self)
{
   kt::Log log;

   if (not fs_initialised) { // Perform the following if this is a Task representing the current process
      Self->ProcessID = glProcessID;

#ifdef _WIN32
      glTaskLock = get_threadlock(); // This lock can be used by other threads to wake the main task.

      char buffer[300];
      if (winGetExeDirectory(sizeof(buffer), buffer)) {
         int len = strlen(buffer);
         while ((len > 1) and (buffer[len-1] != '/') and (buffer[len-1] != '\\') and (buffer[len-1] != ':')) len--;
         Self->ProcessPath.assign(buffer, len);
      }

      if (auto len = winGetCurrentDirectory(sizeof(buffer), buffer)) {
         Self->Path.assign(buffer, len);
         if (not Self->Path.ends_with('\\')) Self->Path += '\\';
      }

#elif __unix__

         char buffer[256], procfile[50];
         int i, len;

         // This method of path retrieval only works on Linux (most types of Unix don't provide any support for this).

         snprintf(procfile, sizeof(procfile), "/proc/%d/exe", glProcessID);

         buffer[0] = 0;
         if ((i = readlink(procfile, buffer, sizeof(buffer)-1)) > 0) {
            buffer[i] = 0;
            while (i > 0) { // Strip the process name
               if (buffer[i] IS '/') {
                  buffer[i+1] = 0;
                  break;
               }
               i--;
            }

            for (len=0; buffer[len]; len++);
            while ((len > 1) and (buffer[len-1] != '/') and (buffer[len-1] != '\\') and (buffer[len-1] != ':')) len--;
            Self->ProcessPath.assign(buffer, len);
         }

         if (Self->Path.empty()) { // Set the working folder
            if (getcwd(buffer, sizeof(buffer))) {
               for (len=0; buffer[len]; len++);
               Self->Path.assign(buffer, len);
               Self->Path += '/';
            }
         }
#endif

      // Initialise message handlers so that the task can process messages.

      MsgHandler *handler;
      FUNCTION call;
      call.Type = CALL::STD_C;
      call.Routine = (APTR)msg_action;
      call.Context = Self;
      AddMsgHandler(MSGID::ACTION, &call, &handler);
      Self->MsgAction.reset(handler);

      call.Routine = (APTR)msg_free;
      AddMsgHandler(MSGID::FREE, &call, &handler);
      Self->MsgFree.reset(handler);

      call.Routine = (APTR)msg_quit;
      AddMsgHandler(MSGID::QUIT, &call, &handler);
      Self->MsgQuit.reset(handler);

      call.Routine = (APTR)msg_waitforobjects; // lib_messages.cpp
      AddMsgHandler(MSGID::WAIT_FOR_OBJECTS, &call, &handler);
      Self->MsgWaitForObjects.reset(handler);

      call.Routine = (APTR)msg_event; // lib_events.c
      AddMsgHandler(MSGID::EVENT, &call, &handler);
      Self->MsgEvent.reset(handler);

      call.Routine = (APTR)msg_threadcallback; // class_thread.c
      AddMsgHandler(MSGID::THREAD_CALLBACK, &call, &handler);
      Self->MsgThreadCallback.reset(handler);

      call.Routine = (APTR)msg_threadaction; // lib_objects.cpp
      AddMsgHandler(MSGID::THREAD_ACTION, &call, &handler);
      Self->MsgThreadAction.reset(handler);

      log.msg("Process Path: %s", Self->ProcessPath.c_str());
      log.msg("Working Path: %s", Self->Path.c_str());
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Quit: Sends a quit message to a task.

The Quit() method can be used as a convenient way of sending a task a quit message.  This will normally result in the
destruction of the task, so long as it is still functioning correctly and has been coded to respond to the
`MSGID::QUIT` message type.  It is legal for a task to ignore a quit request if it is programmed to stay alive.

Signal Handling on Unix: When terminating a foreign process on Unix systems, the quit behavior follows a two-stage
approach for safe process termination: The first call sends `SIGTERM` to allow the process to shutdown gracefully;
A second call sends `SIGKILL` to force immediate termination if the process is unresponsive.

On Windows systems, the method uses `winTerminateApp()` with a timeout for process termination.

-ERRORS-
Okay

-TAGS-
blocking, mutates-object
-END-

*********************************************************************************************************************/

static ERR TASK_Quit(extTask *Self)
{
   kt::Log log;

   if ((Self->ProcessID) and (Self->ProcessID != glProcessID)) {
      #ifdef __unix__
         if (not Self->QuitCalled) { // First call: send SIGTERM for graceful termination
            log.msg("Sending SIGTERM to process %d (graceful termination)", Self->ProcessID);
            kill(Self->ProcessID, SIGTERM);
            Self->QuitCalled = true;
         }
         else { // Second call: send SIGKILL for forced termination
            log.msg("Sending SIGKILL to process %d (forced termination)", Self->ProcessID);
            kill(Self->ProcessID, SIGKILL);
         }
      #elif _WIN32
         log.msg("Terminating foreign process %d", Self->ProcessID);
         winTerminateApp(Self->ProcessID, 1000);
      #else
         #warning Add code to kill foreign processes.
      #endif
   }
   else {
      log.branch("Sending QUIT message to self.");
      SendMessage(MSGID::QUIT, MSF::NIL, {});
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SetEnv: Sets environment variables for the active process.

On platforms that support environment variables, SetEnv() is used for defining values for named variables.  A `Name`
and accompanying `Value` string are required.  If the `Value` is empty, the environment variable is removed if it
already exists.

In Windows, it is possible to set registry keys if the string starts with one of the following (in all other cases, the
system's environment variables are queried):

<pre>
\HKEY_LOCAL_MACHINE\
\HKEY_CURRENT_USER\
\HKEY_CLASSES_ROOT\
\HKEY_USERS\
</pre>

When setting a registry key, the function will always set the Value as a string type unless the key already exists.  If
the existing key value is a number such as `DWORD` or `QWORD`, then the Value will be converted to an integer before the
key is set.

-INPUT-
strview Name:  The name of the environment variable to set.
strview Value: The value to assign to the environment variable.  If empty, the variable is removed.

-ERRORS-
Okay
NullArgs
NoSupport: The platform does not support environment variables.
Syntax
TaskExecutionFailed

-TAGS-
mutates-object, copies-input
-END-

*********************************************************************************************************************/

static ERR TASK_SetEnv(extTask *Self, struct task::SetEnv *Args)
{
   kt::Log log;

   if ((not Args) or Args->Name.empty()) return log.warning(ERR::NullArgs);

   std::string name(Args->Name);
   std::string value(Args->Value);

#ifdef _WIN32

   if (name.starts_with('\\')) {
      int ki, len;
      const struct {
         uint32_t ID;
         CSTRING HKey;
      } keys[] = {
         { HKEY_LOCAL_MACHINE,  "\\HKEY_LOCAL_MACHINE\\" },
         { HKEY_CURRENT_USER,   "\\HKEY_CURRENT_USER\\" },
         { HKEY_CLASSES_ROOT,   "\\HKEY_CLASSES_ROOT\\" },
         { HKEY_USERS,          "\\HKEY_USERS\\" }
      };

      log.msg("Registry: %s = %s", name.c_str(), value.c_str());

      for (ki=0; ki < std::ssize(keys); ki++) {
         if (startswith(keys[ki].HKey, name)) {
            CSTRING str = name.c_str() + strlen(keys[ki].HKey); // str = Kotuku\Something

            for (len=strlen(str); (len > 0) and (str[len] != '\\'); len--);

            if (len > 0) {
               std::string path(str, len);
               APTR keyhandle;
               if (not RegOpenKeyExA(keys[ki].ID, path.c_str(), 0, KEY_READ|KEY_WRITE, &keyhandle)) {
                  int type;
                  if (not RegQueryValueExA(keyhandle, str+len+1, 0, &type, nullptr, nullptr)) {
                     // Numerical registry types can be converted into strings

                     switch(type) {
                        case REG_DWORD: {
                           int int32 = strtol(value.c_str(), nullptr, 0);
                           RegSetValueExA(keyhandle, str+len+1, 0, REG_DWORD, &int32, sizeof(int32));
                           break;
                        }

                        case REG_QWORD: {
                           int64_t int64 = strtoll(value.c_str(), nullptr, 0);
                           RegSetValueExA(keyhandle, str+len+1, 0, REG_QWORD, &int64, sizeof(int64));
                           break;
                        }

                        default: {
                           RegSetValueExA(keyhandle, str+len+1, 0, REG_SZ, value.c_str(), value.size() + 1);
                        }
                     }
                  }
                  else RegSetValueExA(keyhandle, str+len+1, 0, REG_SZ, value.c_str(), value.size() + 1);

                  winCloseHandle(keyhandle);
               }

               return ERR::Okay;
            }
            else return log.warning(ERR::Syntax);
         }
      }

      return log.warning(ERR::TaskExecutionFailed);
   }
   else {
      winSetEnv(name.c_str(), value.empty() ? nullptr : value.c_str());
      return ERR::Okay;
   }

#elif __unix__

   if (value.empty()) unsetenv(name.c_str());
   else setenv(name.c_str(), value.c_str(), 1);
   return ERR::Okay;

#else

   #warn Write support for SetEnv()
   return ERR::NoSupport;

#endif
}

/*********************************************************************************************************************
-ACTION-
SetKey: Variable fields are supported for the general storage of program variables.
-END-
*********************************************************************************************************************/

static ERR TASK_SetKey(extTask *Self, struct acSetKey *Args)
{
   if (not Args) return ERR::NullArgs;

   Self->Fields[Args->Key] = Args->Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Write: Send raw data to a launched process' stdin descriptor.

If a process is successfully launched with the `PIPE` set in #Flags, data can be sent to its stdin pipe by calling the
Write() action.  Setting the `Buffer` parameter to `NULL` will result in the pipe being closed (this will signal to the
process that no more data is incoming).

*********************************************************************************************************************/

static ERR TASK_Write(extTask *Task, struct acWrite *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);

#ifdef _WIN32
   if (Task->Platform) {
      if (auto winerror = winWriteStd(Task->Platform, Args->Buffer, Args->Length); !winerror) {
         return ERR::Okay;
      }
      else return log.warning(ERR::Write);
   }
   else return log.warning(ERR::TaskExecutionFailed);
#else
   return log.warning(ERR::NoSupport);
#endif
}

/*********************************************************************************************************************

-FIELD-
Actions: Used to gain direct access to a task's actions.

This field provides direct access to the actions of a task, and is intended for use with the active task object
returned from ~Core.CurrentTask().  Hooking into the action table allows the running executable to 'blend-in' with
Kotuku's object oriented design.

The Actions field points to a lookup table of !ActionEntry items.  To create an action hook, set its `AC`
table index with a pointer to the action routine.  For example:

<pre>
if (!AccessObject(CurrentTask(), 5000, &task)) {
   task->getActions(actions);
   actions[AC::Seek] = PROGRAM_Seek;
   ReleaseObject(task);
}
</pre>

*********************************************************************************************************************/

static ERR GET_Actions(extTask *Self, std::span<ActionEntry> &Value)
{
   Value = std::span<ActionEntry>(Self->Actions.data(), Self->Actions.size());
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
AffinityMask: Controls which CPU cores the process can run on.

The AffinityMask field sets the CPU affinity for the current process, determining which CPU cores the process
is allowed to run on. This is expressed as a bitmask where each bit represents a CPU core (bit 0 = core 0,
bit 1 = core 1, etc.).

Setting CPU affinity is particularly useful for benchmarking applications where consistent performance timing
is required, as it prevents the OS from moving the process between different CPU cores during execution.

Note: This field affects the current process only and requires appropriate system privileges on some platforms.

*********************************************************************************************************************/

static ERR GET_AffinityMask(extTask *Self, int64_t *Value)
{
#ifdef __unix__
   cpu_set_t cpuset;
   if (sched_getaffinity(0, sizeof(cpuset), &cpuset) != 0) {
      return ERR::SystemCall;
   }

   // Convert cpu_set_t to bitmask
   int64_t mask = 0;
   constexpr int max_mask_bits = sizeof(mask) * 8;
   const int max_cpu = (CPU_SETSIZE < max_mask_bits) ? CPU_SETSIZE : max_mask_bits;
   for (int cpu = 0; cpu < max_cpu; cpu++) {
      if (CPU_ISSET(cpu, &cpuset)) {
         mask |= (1LL << cpu);
      }
   }
   *Value = mask;
#elif _WIN32
   auto mask = winGetProcessAffinityMask();
   if (mask IS 0) return ERR::SystemCall;
   *Value = mask;
#endif

   return ERR::Okay;
}

static ERR SET_AffinityMask(extTask *Self, int64_t Value)
{
   if (Value <= 0) return ERR::InvalidValue;

   Self->AffinityMask = Value;

#ifdef __unix__
   cpu_set_t cpuset;
   CPU_ZERO(&cpuset);

   // Convert bitmask to cpu_set_t
   auto mask = uint64_t(Value);
   while (mask) {
      int cpu = int(std::countr_zero(mask));
      CPU_SET(cpu, &cpuset);
      mask &= mask - 1;
   }

   // Set affinity for current process
   if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
      return ERR::SystemCall;
   }
#elif _WIN32
   if (winSetProcessAffinityMask(Value) != 0) return ERR::SystemCall;
#endif

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Args: Command line arguments (string format).

This field allows command line arguments to be set using a single string, whereby each value is separated by whitespace.
The string will be disassembled and the arguments will be available to read from the #Parameters field.

If an argument needs to include whitespace, use double-quotes to encapsulate the value.

Security Limits: To prevent buffer overflow attacks, the following limits are enforced:

<list type="bullet">
<li>Maximum total input length: 64KB (65,536 bytes)</li>
<li>Maximum individual argument length: 8KB (8,192 bytes)</li>
<li>Malformed quotes are detected and cause `ERR::Syntax` to be returned.</li>
</list>

*********************************************************************************************************************/

static ERR SET_Args(extTask *Self, const std::string_view &Value)
{
   if (Value.empty()) return ERR::Okay;

   const size_t MAX_INPUT_LEN = 65536;
   size_t input_len = Value.size();
   if (input_len > MAX_INPUT_LEN) return ERR::BufferOverflow;

   auto cursor = Value.data();
   auto end = cursor + Value.size();

   while (cursor < end) {
      while ((cursor < end) and (*cursor <= 0x20)) cursor++; // Skip whitespace

      if (cursor < end) { // Extract the argument
         std::string buffer;
         buffer.reserve(512); // Pre-allocate reasonable size

         bool in_quotes = false;
         while ((cursor < end) and (in_quotes or (*cursor > 0x20))) {
            if (*cursor IS '"') {
               in_quotes = !in_quotes;
               cursor++;
            }
            else {
               buffer += *cursor++;
               // Prevent buffer overflow from malicious input
               if (buffer.size() > 8192) return ERR::BufferOverflow; // 8KB max per argument
            }
         }

         if (in_quotes) return kt::Log().warning(ERR::Syntax);
         if (cursor < end) while ((cursor < end) and (*cursor > 0x20)) cursor++;
         Self->addArgument(buffer.c_str());
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ErrorCallback: This callback returns incoming data from STDERR.

The ErrorCallback field can be set with a function reference that will be called when an active process sends data via
STDERR.  The callback must follow the prototype `Function(*Task, APTR Data, int Size)`

The information read from STDERR will be returned in the Data pointer and the byte-length of the data will be
indicated by the `Size`.  The data pointer is temporary and will be invalid once the callback function has returned.

*********************************************************************************************************************/

static ERR GET_ErrorCallback(extTask *Self, FUNCTION * &Value)
{
   if (Self->ErrorCallback.defined()) {
      Value = &Self->ErrorCallback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_ErrorCallback(extTask *Self, FUNCTION *Value)
{
   clear_callback(Self->ErrorCallback);
   if (Value) {
      Self->ErrorCallback = *Value;
      if (Self->ErrorCallback.defined()) Self->ErrorCallback.pin();
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ExitCallback: The callback is activated when the process is terminated.

The ExitCallback field can be set with a function reference that will be called when the executed process is
terminated.  The callback must follow the prototype `Function(*Task)`.

Please keep in mind that if the `Task` is freed when the process is still executing, the ExitCallback routine will not be
called on termination because the `Task` object no longer exists for the control of the process.

*********************************************************************************************************************/

static ERR GET_ExitCallback(extTask *Self, FUNCTION * &Value)
{
   if (Self->ExitCallback.defined()) {
      Value = &Self->ExitCallback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_ExitCallback(extTask *Self, FUNCTION *Value)
{
   clear_callback(Self->ExitCallback);
   if (Value) {
      Self->ExitCallback = *Value;
      if (Self->ExitCallback.defined()) Self->ExitCallback.pin();
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Keys: Returns a list of all key names available to the GetKeys() action.

*********************************************************************************************************************/

static ERR GET_Keys(extTask *Self, std::span<std::string> &Value)
{
   Self->Keys.clear();
   Self->Keys.reserve(Self->Fields.size());
   for (const auto &kv : Self->Fields) Self->Keys.emplace_back(kv.first);
   Value = std::span<std::string>(Self->Keys.data(), Self->Keys.size());
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
InputCallback: This callback returns incoming data from STDIN.

The InputCallback field is available to the active task object only (i.e. the current process).
The referenced function will be called when process receives data from STDIN.  The callback must match the
prototype `void Function(*Task, APTR Data, int Size, ERR Status)`.  In Tiri the prototype is
'function callback(Task, Array, Status)` where `Array` is an array interface.

The information read from STDOUT will be returned in the `Data` pointer and the byte-length of the data will be indicated
by the `Size`.  The data buffer is temporary and will be invalid once the callback function has returned.

A status of `ERR::Finished` is sent if the stdinput handle has been closed.

*********************************************************************************************************************/

static ERR GET_InputCallback(extTask *Self, FUNCTION * &Value)
{
   if (Self->InputCallback.defined()) {
      Value = &Self->InputCallback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_InputCallback(extTask *Self, FUNCTION *Value)
{
   if (Self != glCurrentTask) return ERR::ExecViolation;

   if (Value) {
      #ifdef __unix__
      fcntl(fileno(stdin), F_SETFL, fcntl(fileno(stdin), F_GETFL) | O_NONBLOCK);
      if (auto error = RegisterFD(fileno(stdin), RFD::READ, &task_stdinput_callback, Self); !error) {
      #elif _WIN32
      if (auto error = RegisterFD(winGetStdInput(), RFD::READ, &task_stdinput_callback, Self); !error) {
      #endif
         clear_callback(Self->InputCallback);
         Self->InputCallback = *Value;
         if (Self->InputCallback.defined()) Self->InputCallback.pin();
      }
      else return error;
   }
   else {
      #ifdef _WIN32
      if (Self->InputCallback.defined()) RegisterFD(winGetStdInput(), RFD::READ|RFD::REMOVE, &task_stdinput_callback, Self);
      #else
      if (Self->InputCallback.defined()) RegisterFD(fileno(stdin), RFD::READ|RFD::REMOVE, &task_stdinput_callback, Self);
      #endif
      clear_callback(Self->InputCallback);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OutputCallback: This callback returns incoming data from STDOUT.

The OutputCallback field can be set with a function reference that will be called when an active process sends data via
STDOUT.  For C++ the callback must match the prototype `void Function(*Task, APTR Data, int Size)`.  In Tiri the
prototype is 'function callback(Task, Array)` where `Array` is an array interface.

The information read from STDOUT will be returned in the `Data` pointer and the byte-length of the data will be indicated
by the `Size`.  The `Data` pointer is temporary and will be invalid once the callback function has returned.

*********************************************************************************************************************/

static ERR GET_OutputCallback(extTask *Self, FUNCTION * &Value)
{
   if (Self->OutputCallback.defined()) {
      Value = &Self->OutputCallback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_OutputCallback(extTask *Self, FUNCTION *Value)
{
   clear_callback(Self->OutputCallback);
   if (Value) {
      Self->OutputCallback = *Value;
      if (Self->OutputCallback.defined()) Self->OutputCallback.pin();
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Flags: Optional flags.
Lookup: TSF

-FIELD-
LaunchPath: Launched executables will start in the path specified here.

Use the LaunchPath field to specify the folder that a launched executable will start in when the task object is
activated.  This will override all other path options, such as the `RESET_PATH` flag.

-FIELD-
Location: Location of an executable file to launch.

When a task object is activated, the Location field will be checked for a valid filename.  If the path is valid, the
executable code will be loaded from this source.  The source must be in an executable format recognised by the
native platform.

Leading spaces will be ignored by the string parser.  The Location string can be enclosed with quotes, in which case
only the quoted portion of the string will be used as the source path.

*********************************************************************************************************************/

static ERR SET_Location(extTask *Self, const std::string_view &Value)
{
   if (not Value.empty()) {
      auto cursor = Value.data();
      auto end = cursor + Value.size();
      while ((cursor < end) and (*cursor <= 0x20)) cursor++;
      if ((cursor < end) and (*cursor IS '"')) {
         cursor++;
         auto start = cursor;
         while ((cursor < end) and (*cursor != '"')) cursor++;
         Self->Location.assign(start, cursor - start);
      }
      else Self->Location.assign(cursor, end - cursor);
   }
   else Self->Location.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Name: Name of the task.

This field specifies the task's name, which may be derived from the source program's metadata, if available.

-FIELD-
Parameters: Command line arguments (list format).

Command line arguments for a program can be defined here as a vector list, whereby each argument is an independent
string.  To illustrate, the following command-line string:

<pre>1&gt; YourProgram PREFS MyPrefs -file "documents:readme.txt"</pre>

Would be represented as a list containing `"PREFS"`, `"MyPrefs"`, `"-file"` and `"documents:readme.txt"`.

The list is compatible with Tiri, and can be iterated as follows:

<pre>
for index, value in processing.task().parameters do
   print(index, ' = ', value)
end
</pre>

-FIELD-
ProcessID: Reflects the process ID when an executable is launched.

If a task object launches an executable file via #Activate(), the ProcessID will be set to the 'pid' that was assigned
to the new process by the host system.  At all other times the ProcessID is set to zero.

-FIELD-
Path: The current working folder of the active process.

The Path specifies the 'working folder' that determines where files are loaded from when an absolute path is not
otherwise specified for file access.  Initially the working folder is usually set to the folder of the parent
process, such as that of a terminal shell.

The working folder can be changed at any time by updating the Path with a new folder location.  If changing to the
new folder fails for any reason, the working folder will remain unchanged and the path value will not be updated.

The path string must be fully qualified with a slash or colon at the end of the string.  Non-viable characters are
truncated.

*********************************************************************************************************************/

static ERR SET_Path(extTask *Self, const std::string_view &Value)
{
   std::string new_path;

   kt::Log log;

   log.trace("ChDir: %.*s", int(Value.size()), Value.data());

   ERR error = ERR::Okay;
   if (not Value.empty()) {
      auto len = Value.find_last_of(":/\\");
      if (len IS std::string::npos) return ERR::InvalidPath;
      new_path.assign(Value, 0, len + 1);

#ifdef __unix__
         std::string path;
         if (!ResolvePath(new_path, RSF::NO_FILE_CHECK, &path)) {
            if (chdir(path.c_str())) {
               error = ERR::InvalidPath;
               log.msg("Failed to switch current path to: %s", path.c_str());
            }
         }
         else error = log.warning(ERR::ResolvePath);
#elif _WIN32
         std::string path;
         if (!ResolvePath(new_path, RSF::NO_FILE_CHECK|RSF::PATH, &path)) {
            if (chdir(path.c_str())) {
               error = ERR::InvalidPath;
               log.msg("Failed to switch current path to: %s", path.c_str());
            }
         }
         else error = log.warning(ERR::ResolvePath);
#else
#warn Support required for changing the current path.
#endif
   }
   else error = ERR::EmptyString;

   if (!error) Self->Path.assign(new_path);

   return error;
}

/*********************************************************************************************************************

-FIELD-
ProcessPath: The path of the executable that is associated with the task.

The ProcessPath is set to the path of the executable file that is associated with the task (not including the
executable file name).  This value is managed internally and cannot be altered.

In Microsoft Windows it is not always possible to determine the origins of an executable, in which case the
ProcessPath is set to the working folder in use at the time the process was launched.

-FIELD-
Priority: The task priority in relation to other tasks is be defined here.

Set the Priority field to change the priority of the process associated with the task.  The default value for all
processes is zero.  High positive values will give the process more CPU time while negative values will yield
CPU time to other active processes.

Behaviour varies between platforms, but for consistency the client can assume that the behaviour is equivalent
to the niceness value on Unix-like systems, and the available range is -20 to 20.

Note that depending on the platform, there may be limits as to whether one process can change the priority level
of a foreign process.  Other factors such as the scheduler used by the host system should be considered in the
effect of prioritisation.

*********************************************************************************************************************/

static ERR GET_Priority(extTask *Self, int *Value)
{
#ifdef __unix__
   *Value = -getpriority(PRIO_PROCESS, 0);
#elif _WIN32
   auto priorityClass = winGetProcessPriority();
   if (priorityClass < 0) return ERR::SystemCall;
   *Value = priorityClass;
#endif
   return ERR::Okay;
}

static ERR SET_Priority(extTask *Self, int Value)
{
#ifdef __unix__
   auto priority = -getpriority(PRIO_PROCESS, 0);
   nice(-(Value - priority));
#elif _WIN32
   if (winSetProcessPriority(Value) != 0) return ERR::SystemCall;
#endif
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ReturnCode: The task's return code can be retrieved following execution.

Once a process has completed execution then its return code can be read from this field.  If process is still running,
the error code `ERR::TaskStillExists` will be returned.

-ERRORS-
Okay
TaskStillExists: The task is still running and has no return code at this stage.
DoesNotExist: The task is yet to be successfully launched with the #Activate() action.

*********************************************************************************************************************/

static ERR GET_ReturnCode(extTask *Self, int *Value)
{
   kt::Log log;

   if (Self->ReturnCodeSet) {
      *Value = Self->ReturnCode;
      return ERR::Okay;
   }

   if (not Self->ProcessID) {
      log.msg("Task hasn't been launched yet.");
      return ERR::DoesNotExist;
   }

#ifdef __unix__
   // Please note that ProcessMessages() will typically kill zombie processes.  This means waitpid() may not return the
   // status (although it will inform us that the task no longer exists).  This issue is resolved by getting
   // ProcessMessages() to set the ReturnCode field when it detects zombie tasks.

   int status = 0;
   int result = waitpid(Self->ProcessID, &status, WNOHANG);

   if (result IS Self->ProcessID) {
      // The process has exited.  Find out what error code was returned and pass it as the result.

      if (WIFEXITED(status)) {
         Self->ReturnCode = (int8_t)WEXITSTATUS(status);
         Self->ReturnCodeSet = true;
      }
      else if (WIFSIGNALED(status)) {
         Self->ReturnCode = 128 + WTERMSIG(status);
         Self->ReturnCodeSet = true;
      }

      *Value = Self->ReturnCode;
      for (auto &task : glTasks) {
         if (task.ProcessID IS Self->ProcessID) {
            task.ReturnCode = Self->ReturnCode;
            task.Returned = true;
            break;
         }
      }

      validate_process(Self->ProcessID);
      return ERR::Okay;
   }
   else if (result IS -1) {
      return ERR::TaskStillExists;
   }
   else return ERR::TaskStillExists;

#elif _WIN32

   winGetExitCodeProcess(Self->Platform, &Self->ReturnCode);
   if (Self->ReturnCode IS 259) return ERR::TaskStillExists;
   else {
      Self->ReturnCodeSet = true;
      *Value = Self->ReturnCode;
      return ERR::Okay;
   }

#else

   return ERR::NoSupport;

#endif
}

static ERR SET_ReturnCode(extTask *Self, int Value)
{
   Self->ReturnCodeSet = true;
   Self->ReturnCode = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
TimeOut: Limits the amount of time to wait for a launched process to return.

This field can be set in conjunction with the `WAIT` flag to define the time limit when waiting for a launched
process to return.  The time out is defined in seconds.

*********************************************************************************************************************/

extTask::~extTask()
{
#ifdef __unix__
   check_incoming(this);

   if (InFD != -1) {
      RegisterFD(InFD, RFD::REMOVE, nullptr, nullptr);
      close(InFD);
      InFD = -1;
   }

   if (ErrFD != -1) {
      RegisterFD(ErrFD, RFD::REMOVE, nullptr, nullptr);
      close(ErrFD);
      ErrFD = -1;
   }

   if (InputCallback.defined()) RegisterFD(fileno(stdin), RFD::READ|RFD::REMOVE, &task_stdinput_callback, this);
#endif

#ifdef _WIN32
   if (Platform) { winFreeProcess(Platform); Platform = nullptr; }
   if (InputCallback.defined()) RegisterFD(winGetStdInput(), RFD::READ|RFD::REMOVE, &task_stdinput_callback, this);
#endif

   clear_callback(ErrorCallback);
   clear_callback(OutputCallback);
   clear_callback(ExitCallback);
   clear_callback(InputCallback);
}

//********************************************************************************************************************

static const FieldArray clFields[] = {
   { "LaunchPath",      FDF_CPPSTRING|FDF_RW },
   { "Name",            FDF_CPPSTRING|FDF_RW },
   { "Location",        FDF_CPPSTRING|FDF_RW, nullptr, SET_Location },
   { "Src",             FDF_SYNONYM },
   { "Path",            FDF_CPPSTRING|FDF_RW, nullptr, SET_Path },
   { "ProcessPath",     FDF_CPPSTRING|FDF_R },
   { "TimeOut",         FDF_DOUBLE|FDF_RW },
   { "Parameters",      FDF_VECTOR|FDF_CPPSTRING|FDF_RW },
   { "Flags",           FDF_INTFLAGS|FDF_RI, nullptr, nullptr, &clTaskFlags },
   { "ReturnCode",      FDF_INT|FDF_RW, GET_ReturnCode, SET_ReturnCode },
   { "ProcessID",       FDF_INT|FDF_RI },
   // Virtual fields
   { "Actions",         FDF_VIRTUAL|FDF_ARRAY|FDF_STRUCT|FDF_R|FDF_PURE, GET_Actions, nullptr, "ActionEntry" },
   { "AffinityMask",    FDF_VIRTUAL|FDF_INT64|FDF_RW|FDF_PURE,       GET_AffinityMask, SET_AffinityMask },
   { "Args",            FDF_VIRTUAL|FDF_CPPSTRING|FDF_W,             nullptr, SET_Args },
   { "Keys",            FDF_VIRTUAL|FDF_VECTOR|FDF_CPPSTRING|FDF_R,  GET_Keys },
   { "ErrorCallback",   FDF_VIRTUAL|FDF_FUNCTION|FDF_RI|FDF_PURE,    GET_ErrorCallback,   SET_ErrorCallback }, // STDERR
   { "ExitCallback",    FDF_VIRTUAL|FDF_FUNCTION|FDF_RW|FDF_PURE,    GET_ExitCallback,    SET_ExitCallback },
   { "InputCallback",   FDF_VIRTUAL|FDF_FUNCTION|FDF_RW|FDF_PURE,    GET_InputCallback,   SET_InputCallback }, // STDIN
   { "OutputCallback",  FDF_VIRTUAL|FDF_FUNCTION|FDF_RI|FDF_PURE,    GET_OutputCallback,  SET_OutputCallback }, // STDOUT
   { "Priority",        FDF_VIRTUAL|FDF_INT|FDF_RW,                  GET_Priority, SET_Priority },
   END_FIELD
};

//********************************************************************************************************************

extern ERR add_task_class(void)
{
   glTaskClass = objMetaClass::create::global(
      fl::ClassVersion(VER_TASK),
      fl::Name("Task"),
      fl::Category(CCF::SYSTEM),
      fl::FileExtension("exe|bat|com"),
      fl::FileDescription("Executable File"),
      fl::FileHeader("[0:$4d5a]|[0:$7f454c46]"),
      fl::Icon("items/launch"),
      fl::Actions(clTaskActions),
      fl::Methods(clTaskMethods),
      fl::Fields(clFields),
      fl::Size(sizeof(extTask)),
      fl::Path("modules:core"));

   return glTaskClass ? ERR::Okay : ERR::AddClass;
}

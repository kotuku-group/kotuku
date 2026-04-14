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

The task object that represents the active process can be acquired from ~CurrentTask().

-END-

*********************************************************************************************************************/

#define PRV_TASK

#ifdef __CYGWIN__
#undef __unix__
#endif

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
 #ifdef __CYGWIN__
  #include <unistd.h>
 #else
  #include <direct.h>
 #endif
 #include <stdio.h>
#endif

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
constexpr int REG_EXPAND_SZ = 0x00020000;

#define KEY_READ  0x20019
#define KEY_WRITE 0x20006

constexpr int MAX_PATH = 260;
#define INVALID_HANDLE_VALUE (void *)(-1)

extern "C" DLLCALL int WINAPI RegOpenKeyExA(int,CSTRING,int,int,APTR *);
extern "C" DLLCALL int WINAPI RegQueryValueExA(APTR,CSTRING,int *,int *,int8_t *,int *);
extern "C" DLLCALL int WINAPI RegSetValueExA(APTR hKey, CSTRING lpValueName, int Reserved, int dwType, const void *lpData, int cbData);

static MSGID glProcessBreak = MSGID::NIL;
#endif

static ERR GET_LaunchPath(extTask *, CSTRING *);

static ERR TASK_Activate(extTask *);
static ERR TASK_Free(extTask *);
static ERR TASK_GetEnv(extTask *, struct task::GetEnv *);
static ERR TASK_GetKey(extTask *, struct acGetKey *);
static ERR TASK_Init(extTask *);
static ERR TASK_NewPlacement(extTask *);
static ERR TASK_SetEnv(extTask *, struct task::SetEnv *);
static ERR TASK_SetKey(extTask *, struct acSetKey *);
static ERR TASK_Write(extTask *, struct acWrite *);

static ERR TASK_AddArgument(extTask *, struct task::AddArgument *);
static ERR TASK_Expunge(extTask *);
static ERR TASK_Quit(extTask *);

static const FieldDef clFlags[] = {
   { "Wait",       TSF::WAIT },
   { "Shell",      TSF::SHELL },
   { "ResetPath",  TSF::RESET_PATH },
   { "Privileged", TSF::PRIVILEGED },
   { "LogAll",     TSF::VERBOSE },
   { "Quiet",      TSF::QUIET },
   { "Attached",   TSF::ATTACHED },
   { "Detached",   TSF::DETACHED },
   { "Pipe",       TSF::PIPE },
   { nullptr, 0 }
};

static const ActionArray clActions[] = {
   { AC::Activate,      TASK_Activate },
   { AC::Free,          TASK_Free },
   { AC::GetKey,        TASK_GetKey },
   { AC::NewPlacement,  TASK_NewPlacement },
   { AC::SetKey,        TASK_SetKey },
   { AC::Init,          TASK_Init },
   { AC::Write,         TASK_Write },
   { AC::NIL, nullptr }
};

#include "class_task_def.c"

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

   if (Self->InputCallback.isC()) {
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
static void task_stdout(HOSTHANDLE FD, APTR Task)
{
   thread_local uint8_t recursive = 0;

   if (recursive) return;

   recursive++;

   int len;
   char buffer[TASK_IO_BUFFER_SIZE];
   if ((len = read(FD, buffer, sizeof(buffer)-1)) > 0) {
      buffer[len] = 0;

      auto task = (extTask *)Task;
      if (task->OutputCallback.isC()) {
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
   recursive--;
}

static void task_stderr(HOSTHANDLE FD, APTR Task)
{
   char buffer[TASK_IO_BUFFER_SIZE];
   int len;
   thread_local uint8_t recursive = 0;

   if (recursive) return;

   recursive++;
   if ((len = read(FD, buffer, sizeof(buffer)-1)) > 0) {
      buffer[len] = 0;

      auto task = (extTask *)Task;
      if (task->ErrorCallback.isC()) {
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
   recursive--;
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
   pf::Log log(__FUNCTION__);
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
   pf::Log log(__FUNCTION__);
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
   pf::Log log(__FUNCTION__);
   log.traceBranch("Handle: %d", (int)(MAXINT)Handle);
   RegisterFD(Handle, RFD::READ, (void (*)(void *, void *))&task_incoming_stdout, Task);
}

extern "C" void task_register_stderr(extTask *Task, WINHANDLE Handle)
{
   pf::Log log(__FUNCTION__);
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

static ERR msg_waitforobjects(APTR Custom, int MsgID, int MsgType, APTR Message, int MsgSize)
{
   return ERR::Terminate;
}

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
   pf::Log log("ProcessMessages");
   ActionMessage *action;

   if (not (action = (ActionMessage *)Message)) {
      log.warning("No data attached to MSGID::ACTION message.");
      return ERR::Okay;
   }

   #ifdef DBG_INCOMING
      log.function("Executing action %s on object #%d, Data: %p, Size: %d", action_id_name(action->ActionID), action->ObjectID, Message, MsgSize);
   #endif

   if ((action->ObjectID) and (action->ActionID != AC::NIL)) {
      OBJECTPTR obj;
      ERR error;
      if ((error = AccessObject(action->ObjectID, 5000, &obj)) IS ERR::Okay) {
         if (action->SendArgs IS false) {
            obj->setFlag(NF::MESSAGE);
            Action(action->ActionID, obj, nullptr);
            obj->clearFlag(NF::MESSAGE);
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
               obj->setFlag(NF::MESSAGE);
               Action(action->ActionID, obj, action+1);
               obj->clearFlag(NF::MESSAGE);
               ReleaseObject(obj);
            }
         }
      }
      else {
         if ((error != ERR::NoMatchingObject) and (error != ERR::MarkedForDeletion)) {
            if (action->ActionID > AC::NIL) log.warning("Could not gain access to object %d to execute action %s.", action->ObjectID, action_id_name(action->ActionID));
            else log.warning("Could not gain access to object %d to execute method %d.", action->ObjectID, int(action->ActionID));
         }
      }
   }
   else log.warning("Action message %s specifies an object ID of #%d.", action_id_name(action->ActionID), action->ObjectID);

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR msg_quit(APTR Custom, int MsgID, int MsgType, APTR Message, int MsgSize)
{
   pf::Log log(__FUNCTION__);
   log.function("Processing quit message");
   glTaskState = TSTATE::STOPPING;
   return ERR::Okay;
}

//********************************************************************************************************************
// Determine whether or not a process is alive

extern "C" ERR validate_process(int ProcessID)
{
   pf::Log log(__FUNCTION__);

   log.function("PID: %d", ProcessID);

   if (glValidateProcessID IS ProcessID) glValidateProcessID = 0;
   if ((ProcessID IS glProcessID) or (!ProcessID)) return ERR::Okay;

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
   for (auto it = glTasks.begin(); it != glTasks.end(); it++) {
      if (it->ProcessID IS ProcessID) {
         task_id = it->TaskID;
         glTasks.erase(it);
         break;
      }
   }

   if (not task_id) return ERR::False;

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
   pf::Log log(__FUNCTION__);

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
         if ((!winReadStd(Task->Platform, TSTD_OUT, buffer, &size)) and (size)) {
            log.msg("Processing %d remaining bytes on stdout.", size);
            output_callback(Task, &Task->OutputCallback, buffer, size);
         }
         else break;
      } while (size IS sizeof(buffer));

      do {
         size = sizeof(buffer);
         if ((!winReadStd(Task->Platform, TSTD_ERR, buffer, &size)) and (size)) {
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

   if (Task->ExitCallback.isC()) {
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

   if (((Task->Flags & TSF::WAIT) != TSF::NIL) and (Task->TimeOut > 0)) SendMessage(glProcessBreak, MSF::NIL, nullptr, 0);
}
#endif

//********************************************************************************************************************

#ifdef _WIN32
extern "C" void register_process_pipes(extTask *Self, WINHANDLE ProcessHandle)
{
   pf::Log log;
   log.traceBranch("Process: %d", (int)(MAXINT)ProcessHandle);
   RegisterFD(ProcessHandle, RFD::READ, (void (*)(void *, void *))&task_process_end, Self);
}

extern "C" void deregister_process_pipes(extTask *Self, WINHANDLE ProcessHandle)
{
   pf::Log log;
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
-END-

*********************************************************************************************************************/

static ERR TASK_Activate(extTask *Self)
{
   pf::Log log;
   int i, j;
   ERR error;
   #ifdef _WIN32
      std::string launchdir;
      bool hide_output;
      int winerror;
   #endif
   #ifdef __unix__
      int pid;
      int8_t privileged, shell;
   #endif

   Self->ReturnCodeSet = false;

   if (Self->Location.empty()) return log.warning(ERR::MissingPath);

   if (not glJanitorActive) {
      pf::SwitchContext ctx(glCurrentTask);
      auto call = C_FUNCTION(process_janitor);
      SubscribeTimer(60, &call, &glProcessJanitor);
      glJanitorActive = true;
   }

#ifdef _WIN32
   // Determine the launch folder

   if (not Self->LaunchPath.empty()) {
      std::string rpath;
      if (ResolvePath(Self->LaunchPath, RSF::APPROXIMATE|RSF::PATH, &rpath) IS ERR::Okay) {
         launchdir.assign(rpath);
      }
      else launchdir.assign(Self->LaunchPath);
   }
   else if ((Self->Flags & TSF::RESET_PATH) != TSF::NIL) {
      std::string rpath;
      if (ResolvePath(Self->Location, RSF::APPROXIMATE|RSF::PATH, &rpath) IS ERR::Okay) {
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
   if (ResolvePath(Self->Location, RSF::APPROXIMATE|RSF::PATH, &rpath) IS ERR::Okay) {
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
         if (ResolvePath(sv, RSF::NO_FILE_CHECK, &redirect_stdout) IS ERR::Okay) {
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
            for (j=i+1; final_buffer[j]; j++) {
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

   if (not (winerror = winLaunchProcess(Self, final_buffer.data(), (!launchdir.empty()) ? launchdir.data() : 0, group,
         internal_redirect, &Self->Platform, hide_output, redirect_stdout.data(), redirect_stderr.data(), &Self->ProcessID))) {

      error = ERR::Okay;
      if (((Self->Flags & TSF::WAIT) != TSF::NIL) and (Self->TimeOut > 0)) {
         log.msg("Waiting for process to exit.  TimeOut: %.2f sec", Self->TimeOut);

         //if (not glProcessBreak) glProcessBreak = AllocateID(IDTYPE_MESSAGE);
         glProcessBreak = MSGID::BREAK;

         ProcessMessages(PMF::NIL, Self->TimeOut * 1000.0);

         winGetExitCodeProcess(Self->Platform, &Self->ReturnCode);
         if (Self->ReturnCode != 259) Self->ReturnCodeSet = true;
      }
   }
   else {
      log.warning("Launch Error: %s", winFormatMessage(winerror).c_str());
      error = ERR::ProcessCreation;
   }

   return error;

#elif __unix__

   // Add a 'cd' command so that the application starts in its own folder

   CSTRING path = nullptr;
   GET_LaunchPath(Self, &path);

   std::ostringstream buffer;

   i = 0;
   if (((Self->Flags & TSF::RESET_PATH) != TSF::NIL) or (path)) {
      Self->Flags |= TSF::SHELL;

      buffer << "cd ";

      if (not path) path = Self->Location.c_str();
      std::string rpath;
      if (ResolvePath(path, RSF::APPROXIMATE|RSF::PATH, &rpath) IS ERR::Okay) {
         while (rpath.ends_with('/')) rpath.pop_back();
         buffer << rpath;
      }
      else {
         auto p = std::string_view(path);
         while (p.ends_with('/')) p.remove_suffix(1);
         buffer << p;
      }
   }

   // Resolve the location of the executable (may contain an volume) and copy it to the command line buffer.

   std::string rpath;
   if (ResolvePath(Self->Location, RSF::APPROXIMATE|RSF::PATH, &rpath) IS ERR::Okay) {
      buffer << rpath;
   }
   else buffer << Self->Location;

   // Following the executable path are any arguments that have been used. NOTE: This isn't needed if TSF::SHELL is used,
   // however it is extremely useful in the debug printout to see what is being executed.

   std::ostringstream params;
   if ((Self->Flags & TSF::SHELL) != TSF::NIL) {
      for (auto &param : Self->Parameters) {
         params << ' ';
         if (param.find(' ') != std::string::npos) params << '"' << param << '"';
         else params << param;
      }
   }

   // Convert single quotes into double quotes

   auto final_buffer = buffer.str();
   for (int i=0; i < std::ssize(final_buffer); i++) if (final_buffer[i] IS '\'') final_buffer[i] = '"';

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
      out_fd = open("/dev/null", O_RDONLY);
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
      out_errfd = open("/dev/null", O_RDONLY);
   }

   // Fork a new task.  Remember that forking produces an exact duplicate of the process that made the fork.

   privileged = ((Self->Flags & TSF::PRIVILEGED) != TSF::NIL) ? 1 : 0;
   shell = ((Self->Flags & TSF::SHELL) != TSF::NIL) ? 1 : 0;

   // Check system resource limits before forking
   struct rlimit rlim;
   if (getrlimit(RLIMIT_NPROC, &rlim) IS 0) {
      if (rlim.rlim_cur != RLIM_INFINITY) {
         // Count current processes to see if we're near the limit
         // Leave some margin (10% or at least 5 processes) before hitting the limit
         auto margin = std::max(5UL, rlim.rlim_cur / 10);
         if ((rlim.rlim_cur + margin) >= rlim.rlim_max) {
            log.warning("Too close to process limit (%lu/%lu), refusing to fork", rlim.rlim_cur, rlim.rlim_max);
            cleanup_task_fds(input_fd, out_fd, out_errfd, in_fd, in_errfd);
            return ERR::ProcessCreation;
         }
      }
   }

   pid = fork();

   if (pid IS -1) {
      cleanup_task_fds(input_fd, out_fd, out_errfd, in_fd, in_errfd);
      log.warning("Failed in an attempt to fork(): %s", strerror(errno));
      return ERR::ProcessCreation;
   }

   if (pid) {
      // The following code is executed by the initiating process thread

      log.msg("Created new process %d.  Shell: %d", pid, shell);

      Self->ProcessID = pid; // Record the native process ID

      glTasks.emplace_back(Self);

      if (in_fd != -1) {
         RegisterFD(in_fd, RFD::READ, &task_stdout, Self);
         Self->InFD = in_fd;
         close(out_fd);
      }

      if (in_errfd != -1) {
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
         int64_t ticks = PreciseTime() + int64_t(Self->TimeOut * 1000000.0);
         while (!waitpid(pid, &status, WNOHANG)) {
            ProcessMessages(PMF::NIL, 100);

            auto remaining = ticks - PreciseTime();
            if (remaining <= 0) {
               error = log.warning(ERR::TimeOut);
               break;
            }
         }

         // Find out what error code was returned

         if (WIFEXITED(status)) {
            Self->ReturnCode = (int8_t)WEXITSTATUS(status);
            Self->ReturnCodeSet = true;
         }

         if (kill(pid, 0)) {
            for (auto it = glTasks.begin(); it != glTasks.end(); it++) {
               if (it->ProcessID IS pid) {
                  glTasks.erase(it);
                  break;
               }
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
   if (shell) { // For some reason, bash terminates the argument list if it encounters a # symbol, so we'll strip those out.
      for (j=0,i=0; i < std::ssize(final_buffer); i++) {
         if (final_buffer[i] != '#') final_buffer[j++] = final_buffer[i];
      }

      execl("/bin/sh", "sh", "-c", final_buffer.c_str(), (char *)nullptr);
   }
   else execv(final_buffer.c_str(), (char * const *)&argslist);

   exit(EXIT_FAILURE);
#endif
}

/*********************************************************************************************************************

-METHOD-
AddArgument: Adds a new argument to the Parameters field.

This method will add a new argument to the end of the #Parameters field array.  If the string is surrounded by quotes,
they will be removed automatically.

-INPUT-
cstr Argument: The new argument string.

-ERRORS-
Okay
NullArgs

*********************************************************************************************************************/

static ERR TASK_AddArgument(extTask *Self, struct task::AddArgument *Args)
{
   if ((!Args) or (!Args->Argument) or (!*Args->Argument)) return ERR::NullArgs;

   auto src = Args->Argument;
   if ((*src IS '"') or (*src IS '\'')) {
      auto end = *src++;
      int len = 0;
      while ((src[len]) and (src[len] != end)) len++;
      Self->Parameters.emplace_back(std::string(src, len));
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

*********************************************************************************************************************/

static ERR TASK_Expunge(extTask *Self)
{
   Expunge(false);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR TASK_Free(extTask *Self)
{
   pf::Log log;

#ifdef __unix__
   check_incoming(Self);

   if (Self->InFD != -1) {
      RegisterFD(Self->InFD, RFD::REMOVE, nullptr, nullptr);
      close(Self->InFD);
      Self->InFD = -1;
   }

   if (Self->ErrFD != -1) {
      RegisterFD(Self->ErrFD, RFD::REMOVE, nullptr, nullptr);
      close(Self->ErrFD);
      Self->ErrFD = -1;
   }

   if (Self->InputCallback.defined()) RegisterFD(fileno(stdin), RFD::READ|RFD::REMOVE, &task_stdinput_callback, Self);
#endif

#ifdef _WIN32
   if (Self->Platform) { winFreeProcess(Self->Platform); Self->Platform = nullptr; }
   if (Self->InputCallback.defined()) RegisterFD(winGetStdInput(), RFD::READ|RFD::REMOVE, &task_stdinput_callback, Self);
#endif

   if (Self->MessageMID)        { FreeResource(Self->MessageMID);        Self->MessageMID         = 0; }
   if (Self->MsgAction)         { FreeResource(Self->MsgAction);         Self->MsgAction          = nullptr; }
   if (Self->MsgDebug)          { FreeResource(Self->MsgDebug);          Self->MsgDebug           = nullptr; }
   if (Self->MsgWaitForObjects) { FreeResource(Self->MsgWaitForObjects); Self->MsgWaitForObjects  = nullptr; }
   if (Self->MsgQuit)           { FreeResource(Self->MsgQuit);           Self->MsgQuit            = nullptr; }
   if (Self->MsgFree)           { FreeResource(Self->MsgFree);           Self->MsgFree            = nullptr; }
   if (Self->MsgEvent)          { FreeResource(Self->MsgEvent);          Self->MsgEvent           = nullptr; }
   if (Self->MsgThreadCallback) { FreeResource(Self->MsgThreadCallback); Self->MsgThreadCallback  = nullptr; }
   if (Self->MsgThreadAction)   { FreeResource(Self->MsgThreadAction);   Self->MsgThreadAction    = nullptr; }

   Self->~extTask();
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

Here is a valid example for reading the 'Kotuku' key value `\HKEY_CURRENT_USER\Software\Kotuku`

Caution: If your programming language uses backslash as an escape character (true for Tiri developers), remember to
use double-backslashes as the key value separator in your Name string.

-INPUT-
cstr Name:  The name of the environment variable to retrieve.
&cstr Value: The value of the environment variable is returned in this parameter.

-ERRORS-
Okay
Args
DoesNotExist: The environment variable is undefined.
NoSupport: The platform does not support environment variables.
-END-

*********************************************************************************************************************/

static ERR TASK_GetEnv(extTask *Self, struct task::GetEnv *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Name)) return log.warning(ERR::NullArgs);

#ifdef _WIN32

   Args->Value = nullptr;

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
      for (auto &key : keys) {
         if (not full_path.starts_with(key.HKey)) continue;

         auto sep = full_path.find_last_of('\\');
         if (sep IS std::string::npos) return log.warning(ERR::Syntax);

         std::string folder = full_path.substr(key.HKey.size(), sep - key.HKey.size() + 1);

         APTR keyhandle;
         if (not RegOpenKeyExA(key.ID, folder.c_str(), 0, KEY_READ, &keyhandle)) {
            int type;
            int8_t buffer[4096];
            int envlen = sizeof(buffer);
            std::string name = full_path.substr(sep+1);
            if (not RegQueryValueExA(keyhandle, name.c_str(), 0, &type, buffer, &envlen)) {
               // Numerical registry types can be converted into strings

               switch(type) {
                  case REG_DWORD:
                     if (unsigned(envlen) >= sizeof(int)) Self->Env = std::to_string(((int *)buffer)[0]);
                     break;

                  case REG_DWORD_BIG_ENDIAN:
                     if (unsigned(envlen) >= sizeof(int)) {
                        if constexpr (std::endian::native == std::endian::little) {
                           Self->Env = std::to_string(reverse_long(((int *)buffer)[0]));
                        }
                        else Self->Env = std::to_string(((int *)buffer)[0]);
                     }
                     break;

                  case REG_QWORD:
                     if (unsigned(envlen) >= sizeof(int64_t)) {
                        Self->Env = std::to_string(((int64_t *)buffer)[0]);
                     }
                     break;

                  case REG_SZ:
                  case REG_EXPAND_SZ:
                     Self->Env.assign((char *)buffer, envlen);
                     // Remove any trailing null characters
                     while ((!Self->Env.empty()) and (Self->Env.back() IS 0)) Self->Env.pop_back();
                     break;

                  default:
                     log.warning("Unsupported registry type %d for key %s", type, Args->Name);
                     break;
               }

               Args->Value = Self->Env.c_str();
            }
            winCloseHandle(keyhandle);

            if (Args->Value) return ERR::Okay;
            else return ERR::DoesNotExist;
         }
         else return ERR::DoesNotExist;
      }
   }

   winGetEnv(Args->Name, Self->Env);
   if (Self->Env.empty()) return ERR::DoesNotExist;
   Args->Value = Self->Env.c_str();
   return ERR::Okay;

#elif __unix__
   if ((Args->Value = getenv(Args->Name))) {
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
   pf::Log log;
   int j;

   if ((!Args) or (!Args->Value) or (Args->Size <= 0)) return log.warning(ERR::NullArgs);

   auto it = Self->Fields.find(Args->Key);
   if (it != Self->Fields.end()) {
      for (j=0; (it->second[j]) and (j < Args->Size-1); j++) Args->Value[j] = it->second[j];
      Args->Value[j++] = 0;

      if (j >= Args->Size) return ERR::BufferOverflow;
      else return ERR::Okay;
   }

   log.warning("The variable \"%s\" does not exist.", Args->Key);

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR TASK_Init(extTask *Self)
{
   pf::Log log;

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

      FUNCTION call;
      call.Type = CALL::STD_C;
      call.Routine = (APTR)msg_action;
      AddMsgHandler(MSGID::ACTION, &call, &Self->MsgAction);

      call.Routine = (APTR)msg_free;
      AddMsgHandler(MSGID::FREE, &call, &Self->MsgFree);

      call.Routine = (APTR)msg_quit;
      AddMsgHandler(MSGID::QUIT, &call, &Self->MsgQuit);

      call.Routine = (APTR)msg_waitforobjects;
      AddMsgHandler(MSGID::WAIT_FOR_OBJECTS, &call, &Self->MsgWaitForObjects);

      call.Routine = (APTR)msg_event; // lib_events.c
      AddMsgHandler(MSGID::EVENT, &call, &Self->MsgEvent);

      call.Routine = (APTR)msg_threadcallback; // class_thread.c
      AddMsgHandler(MSGID::THREAD_CALLBACK, &call, &Self->MsgThreadCallback);

      call.Routine = (APTR)msg_threadaction; // lib_objects.cpp
      AddMsgHandler(MSGID::THREAD_ACTION, &call, &Self->MsgThreadAction);

      log.msg("Process Path: %s", Self->ProcessPath.c_str());
      log.msg("Working Path: %s", Self->Path.c_str());
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR TASK_NewPlacement(extTask *Self)
{
   new (Self) extTask; // See constructor for initialisation
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
-END-

*********************************************************************************************************************/

static ERR TASK_Quit(extTask *Self)
{
   pf::Log log;

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
      SendMessage(MSGID::QUIT, MSF::NIL, nullptr, 0);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SetEnv: Sets environment variables for the active process.

On platforms that support environment variables, SetEnv() is used for defining values for named variables.  A `Name`
and accompanying `Value` string are required.  If the `Value` is `NULL`, the environment variable is removed if it
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
cstr Name:  The name of the environment variable to set.
cstr Value: The value to assign to the environment variable.

-ERRORS-
Okay
Args
NoSupport: The platform does not support environment variables.
-END-

*********************************************************************************************************************/

static ERR TASK_SetEnv(extTask *Self, struct task::SetEnv *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Name)) return log.warning(ERR::NullArgs);

#ifdef _WIN32

   if (Args->Name[0] IS '\\') {
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

      log.msg("Registry: %s = %s", Args->Name, Args->Value);

      for (ki=0; ki < std::ssize(keys); ki++) {
         if (startswith(keys[ki].HKey, Args->Name)) {
            CSTRING str = Args->Name + strlen(keys[ki].HKey); // str = Kotuku\Something

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
                           int int32 = strtol(Args->Value, nullptr, 0);
                           RegSetValueExA(keyhandle, str+len+1, 0, REG_DWORD, &int32, sizeof(int32));
                           break;
                        }

                        case REG_QWORD: {
                           int64_t int64 = strtoll(Args->Value, nullptr, 0);
                           RegSetValueExA(keyhandle, str+len+1, 0, REG_QWORD, &int64, sizeof(int64));
                           break;
                        }

                        default: {
                           RegSetValueExA(keyhandle, str+len+1, 0, REG_SZ, Args->Value, strlen(Args->Value)+1);
                        }
                     }
                  }
                  else RegSetValueExA(keyhandle, str+len+1, 0, REG_SZ, Args->Value, strlen(Args->Value)+1);

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
      winSetEnv(Args->Name, Args->Value);
      return ERR::Okay;
   }

#elif __unix__

   if (Args->Value) setenv(Args->Name, Args->Value, 1);
   else unsetenv(Args->Name);
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
   if ((!Args) or (!Args->Key) or (!Args->Value)) return ERR::NullArgs;

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
   pf::Log log;

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
returned from ~CurrentTask().  Hooking into the action table allows the running executable to 'blend-in' with
Kotuku's object oriented design.

The Actions field points to a lookup table of !ActionEntry items.  Hooking into an action involves writing its `AC`
index in the table with a pointer to the action routine.  For example:

<pre>
if (not AccessObject(CurrentTask(), 5000, &task)) {
   task->get(FID_Actions, actions);
   actions[AC::Seek] = PROGRAM_Seek;
   ReleaseObject(task);
}
</pre>

*********************************************************************************************************************/

static ERR GET_Actions(extTask *Self, struct ActionEntry **Value)
{
   *Value = Self->Actions;
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
   for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
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
   for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
      if (Value & (1LL << cpu)) {
         CPU_SET(cpu, &cpuset);
      }
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

static ERR SET_Args(extTask *Self, CSTRING Value)
{
   if ((!Value) or (!*Value)) return ERR::Okay;

   const size_t MAX_INPUT_LEN = 65536;
   size_t input_len = strlen(Value);
   if (input_len > MAX_INPUT_LEN) return ERR::BufferOverflow;

   while (*Value) {
      while (*Value <= 0x20) Value++; // Skip whitespace

      if (*Value) { // Extract the argument
         std::string buffer;
         buffer.reserve(512); // Pre-allocate reasonable size

         bool in_quotes = false;
         while (*Value and (in_quotes or (*Value > 0x20))) {
            if (*Value IS '"') {
               in_quotes = !in_quotes;
               Value++;
            }
            else {
               buffer += *Value++;
               // Prevent buffer overflow from malicious input
               if (buffer.size() > 8192) return ERR::BufferOverflow; // 8KB max per argument
            }
         }

         if (in_quotes) return pf::Log().warning(ERR::Syntax);
         if (*Value) while (*Value > 0x20) Value++;
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

static ERR GET_ErrorCallback(extTask *Self, FUNCTION **Value)
{
   if (Self->ErrorCallback.defined()) {
      *Value = &Self->ErrorCallback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_ErrorCallback(extTask *Self, FUNCTION *Value)
{
   if (Value) Self->ErrorCallback = *Value;
   else Self->ErrorCallback.clear();
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

static ERR GET_ExitCallback(extTask *Self, FUNCTION **Value)
{
   if (Self->ExitCallback.defined()) {
      *Value = &Self->ExitCallback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_ExitCallback(extTask *Self, FUNCTION *Value)
{
   if (Value) Self->ExitCallback = *Value;
   else Self->ExitCallback.clear();
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

static ERR GET_InputCallback(extTask *Self, FUNCTION **Value)
{
   if (Self->InputCallback.defined()) {
      *Value = &Self->InputCallback;
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
      if (auto error = RegisterFD(fileno(stdin), RFD::READ, &task_stdinput_callback, Self); error IS ERR::Okay) {
      #elif _WIN32
      if (auto error = RegisterFD(winGetStdInput(), RFD::READ, &task_stdinput_callback, Self); error IS ERR::Okay) {
      #endif
         Self->InputCallback = *Value;
      }
      else return error;
   }
   else {
      #ifdef _WIN32
      if (Self->InputCallback.defined()) RegisterFD(winGetStdInput(), RFD::READ|RFD::REMOVE, &task_stdinput_callback, Self);
      #else
      if (Self->InputCallback.defined()) RegisterFD(fileno(stdin), RFD::READ|RFD::REMOVE, &task_stdinput_callback, Self);
      #endif
      Self->InputCallback.clear();
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

static ERR GET_OutputCallback(extTask *Self, FUNCTION **Value)
{
   if (Self->OutputCallback.defined()) {
      *Value = &Self->OutputCallback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_OutputCallback(extTask *Self, FUNCTION *Value)
{
   if (Value) Self->OutputCallback = *Value;
   else Self->OutputCallback.clear();
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

*********************************************************************************************************************/

static ERR GET_LaunchPath(extTask *Self, CSTRING *Value)
{
   *Value = Self->LaunchPath.c_str();
   return ERR::Okay;
}

static ERR SET_LaunchPath(extTask *Self, CSTRING Value)
{
   if ((Value) and (*Value)) Self->LaunchPath.assign(Value);
   else Self->LaunchPath.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Location: Location of an executable file to launch.

When a task object is activated, the Location field will be checked for a valid filename.  If the path is valid, the
executable code will be loaded from this source.  The source must be in an executable format recognised by the
native platform.

Leading spaces will be ignored by the string parser.  The Location string can be enclosed with quotes, in which case
only the quoted portion of the string will be used as the source path.

*********************************************************************************************************************/

static ERR GET_Location(extTask *Self, CSTRING *Value)
{
   *Value = Self->Location.c_str();
   return ERR::Okay;
}

static ERR SET_Location(extTask *Self, CSTRING Value)
{
   if ((Value) and (*Value)) {
      while ((*Value) and (*Value <= 0x20)) Value++;
      if (*Value IS '"') {
         Value++;
         const char* start = Value;
         while (*Value && *Value != '"') ++Value;
         Self->Location.assign(start, Value - start);
      }
      else Self->Location.assign(Value);
   }
   else Self->Location.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Name: Name of the task.

This field specifies the name of the task or program that has been initialised. It is up to the developer of the
program to set the Name which will appear in this field.  If there is no name for the task then the system may
assign a randomly generated name.

*********************************************************************************************************************/

static ERR GET_Name(extTask *Self, STRING *Value)
{
   *Value = Self->Name;
   return ERR::Okay;
}

static ERR SET_Name(extTask *Self, CSTRING Value)
{
   strcopy(Value, Self->Name, sizeof(Self->Name));
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Parameters: Command line arguments (list format).

Command line arguments for a program can be defined here as a vector list, whereby each argument is an independent
string.  To illustrate, the following command-line string:

<pre>1&gt; YourProgram PREFS MyPrefs -file "documents:readme.txt"</pre>

Would be represented as follows:

<pre>
pf::vector&lt;std::string&gt; Args = {
   "PREFS",
   "MyPrefs",
   "-file",
   "documents:readme.txt"
};
</pre>

NOTE: Scripts should use the #Args field instead.

*********************************************************************************************************************/

static ERR GET_Parameters(extTask *Self, pf::vector<std::string> **Value, int *Elements)
{
   *Value = &Self->Parameters;
   *Elements = Self->Parameters.size();
   return ERR::Okay;
}

static ERR SET_Parameters(extTask *Self, const pf::vector<std::string> *Value, int Elements)
{
   if (Value) Self->Parameters = Value[0];
   else Self->Parameters.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

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

*********************************************************************************************************************/

static ERR GET_Path(extTask *Self, CSTRING *Value)
{
   *Value = Self->Path.c_str();
   return ERR::Okay;
}

static ERR SET_Path(extTask *Self, CSTRING Value)
{
   std::string new_path;

   pf::Log log;

   log.trace("ChDir: %s", Value);

   ERR error = ERR::Okay;
   if ((Value) and (*Value)) {
      int len = strlen(Value);
      while ((len > 1) and (Value[len-1] != '/') and (Value[len-1] != '\\') and (Value[len-1] != ':')) len--;
      new_path.assign(Value, len);

#ifdef __unix__
         std::string path;
         if (ResolvePath(new_path.c_str(), RSF::NO_FILE_CHECK, &path) IS ERR::Okay) {
            if (chdir(path.c_str())) {
               error = ERR::InvalidPath;
               log.msg("Failed to switch current path to: %s", path.c_str());
            }
         }
         else error = log.warning(ERR::ResolvePath);
#elif _WIN32
         std::string path;
         if (ResolvePath(new_path, RSF::NO_FILE_CHECK|RSF::PATH, &path) IS ERR::Okay) {
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

   if (error IS ERR::Okay) Self->Path.assign(new_path);

   return error;
}

/*********************************************************************************************************************

-FIELD-
ProcessPath: The path of the executable that is associated with the task.

The ProcessPath is set to the path of the executable file that is associated with the task (not including the
executable file name).  This value is managed internally and cannot be altered.

In Microsoft Windows it is not always possible to determine the origins of an executable, in which case the
ProcessPath is set to the working folder in use at the time the process was launched.

*********************************************************************************************************************/

static ERR GET_ProcessPath(extTask *Self, CSTRING *Value)
{
   *Value = Self->ProcessPath.c_str();
   return ERR::Okay;
}

/*********************************************************************************************************************

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
   pf::Log log;

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

   if ((result IS -1) or (result IS Self->ProcessID)) {
      // The process has exited.  Find out what error code was returned and pass it as the result.

      if (WIFEXITED(status)) {
         Self->ReturnCode = (int8_t)WEXITSTATUS(status);
         Self->ReturnCodeSet = true;
      }

      *Value = Self->ReturnCode;
      return ERR::Okay;
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

static const FieldArray clFields[] = {
   { "TimeOut",         FDF_DOUBLE|FDF_RW },
   { "Flags",           FDF_INTFLAGS|FDF_RI, nullptr, nullptr, &clFlags },
   { "ReturnCode",      FDF_INT|FDF_RW, GET_ReturnCode, SET_ReturnCode },
   { "ProcessID",       FDF_INT|FDF_RI },
   // Virtual fields
   { "Actions",        FDF_POINTER|FDF_R,  GET_Actions },
   { "AffinityMask",   FDF_INT64|FDF_RW,   GET_AffinityMask, SET_AffinityMask },
   { "Args",           FDF_STRING|FDF_W,   nullptr, SET_Args },
   { "Parameters",     FDF_CPP|FDF_ARRAY|FDF_STRING|FDF_RW, GET_Parameters, SET_Parameters },
   { "ErrorCallback",  FDF_FUNCTIONPTR|FDF_RI, GET_ErrorCallback,   SET_ErrorCallback }, // STDERR
   { "ExitCallback",   FDF_FUNCTIONPTR|FDF_RW, GET_ExitCallback,    SET_ExitCallback },
   { "InputCallback",  FDF_FUNCTIONPTR|FDF_RW, GET_InputCallback,   SET_InputCallback }, // STDIN
   { "LaunchPath",     FDF_STRING|FDF_RW,      GET_LaunchPath,      SET_LaunchPath },
   { "Location",       FDF_STRING|FDF_RW,      GET_Location,        SET_Location },
   { "Name",           FDF_STRING|FDF_RW,      GET_Name,            SET_Name },
   { "OutputCallback", FDF_FUNCTIONPTR|FDF_RI, GET_OutputCallback,  SET_OutputCallback }, // STDOUT
   { "Path",           FDF_STRING|FDF_RW,      GET_Path,            SET_Path },
   { "ProcessPath",    FDF_STRING|FDF_R,       GET_ProcessPath },
   { "Priority",       FDF_INT|FDF_RW,         GET_Priority, SET_Priority },
   // Synonyms
   { "Src",            FDF_SYNONYM|FDF_STRING|FDF_RW, GET_Location, SET_Location },
   END_FIELD
};

//********************************************************************************************************************

extern ERR add_task_class(void)
{
   glTaskClass = objMetaClass::create::global(
      fl::ClassVersion(VER_TASK),
      fl::Name("Task"),
      fl::Category(CCF::SYSTEM),
      fl::FileExtension("*.exe|*.bat|*.com"),
      fl::FileDescription("Executable File"),
      fl::FileHeader("[0:$4d5a]|[0:$7f454c46]"),
      fl::Icon("items/launch"),
      fl::Actions(clActions),
      fl::Methods(clTaskMethods),
      fl::Fields(clFields),
      fl::Size(sizeof(extTask)),
      fl::Path("modules:core"));

   return glTaskClass ? ERR::Okay : ERR::AddClass;
}

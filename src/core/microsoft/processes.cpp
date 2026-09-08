
static bool assign_group(HANDLE Process);

static uint64_t unique_pipe_id(void)
{
   static std::atomic_uint64_t glPipeID = 0;
   return ++glPipeID;
}

static void close_handle(HANDLE *Handle)
{
   if ((!Handle) or (!*Handle) or (*Handle IS INVALID_HANDLE_VALUE)) return;
   CloseHandle(*Handle);
   *Handle = nullptr;
}

static void cancel_overlapped_read(HANDLE Handle, OVERLAPPED *Overlap)
{
   if ((!Handle) or (Handle IS INVALID_HANDLE_VALUE) or (!Overlap) or (!Overlap->hEvent)) return;

   if (CancelIoEx(Handle, Overlap)) {
      DWORD transferred = 0;
      GetOverlappedResult(Handle, Overlap, &transferred, TRUE);
   }
}

static HANDLE open_nul_handle(DWORD Access)
{
   SECURITY_ATTRIBUTES sa;

   sa.nLength              = sizeof(sa);
   sa.lpSecurityDescriptor = nullptr;
   sa.bInheritHandle       = TRUE;

   return CreateFile("NUL", Access, FILE_SHARE_READ|FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
      nullptr);
}

static int duplicate_or_nul_std_handle(DWORD StdID, DWORD Access, HANDLE *Handle)
{
   *Handle = nullptr;

   auto source = GetStdHandle(StdID);
   if ((source) and (source != INVALID_HANDLE_VALUE)) {
      if (DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), Handle, 0, TRUE,
            DUPLICATE_SAME_ACCESS)) {
         return 0;
      }
   }

   if ((*Handle = open_nul_handle(Access)) != INVALID_HANDLE_VALUE) return 0;

   auto winerror = GetLastError();
   *Handle = nullptr;
   return winerror;
}

extern "C" void deregister_process_pipes(APTR Self, HANDLE ProcessHandle);
extern "C" void register_process_pipes(APTR Self, HANDLE ProcessHandle);
extern "C" void task_register_stdout(APTR Task, HANDLE Handle);
extern "C" void task_register_stderr(APTR Task, HANDLE Handle);
extern "C" void task_deregister_incoming(HANDLE);

//********************************************************************************************************************

extern "C" void winFreeProcess(struct winprocess *Process)
{
   if (!Process) return;

   task_deregister_incoming(Process->StdOutEvent);
   task_deregister_incoming(Process->StdErrEvent);

   deregister_process_pipes(Process->Task, Process->Handle);

   cancel_overlapped_read(Process->PipeOut.Read, &Process->OutOverlap);
   cancel_overlapped_read(Process->PipeErr.Read, &Process->ErrOverlap);

   close_handle(&Process->PipeOut.Write);
   close_handle(&Process->PipeErr.Write);
   close_handle(&Process->PipeIn.Read);

   close_handle(&Process->PipeOut.Read);
   close_handle(&Process->PipeErr.Read);
   close_handle(&Process->PipeIn.Write);

   close_handle(&Process->StdOutEvent);
   close_handle(&Process->StdErrEvent);
   close_handle(&Process->Handle);

   free(Process);
}

//********************************************************************************************************************
// Assigns a newly created process to a group that belongs to the current process.  This has the benefit of closing the
// child process when the parent is destroyed.

#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 0x00002000

//LONG WINAPI SetInformationJobObject(HANDLE, JOBOBJECTINFOCLASS, PVOID, ULONG);

static bool assign_group(HANDLE Process)
{
   static HANDLE glJob = 0;
   bool result = false;

   EnterCriticalSection(&csJob);

   if (!glJob) {

      // Create the job object

      if (!(glJob = CreateJobObject(nullptr, nullptr))) {
         LeaveCriticalSection(&csJob);
         return false;
      }

      {
         JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
         ZeroMemory(&jeli, sizeof(jeli));
         jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
         if (!SetInformationJobObject(glJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
            close_handle(&glJob);
            LeaveCriticalSection(&csJob);
            return false;
         }
      }
/*
      {
         if (!glIOPort) glIOPort = CreateIOCompletionPort...;

         JOBOBJECT_ASSOCIATE_COMPLETION_PORT port;
         port.CompletionKey  = glProcessID;
         port.CompletionPort = glIOPort;
         SetInformationJobObject(job, JobObject..., &port, sizeof(port));
      }
*/
   }

   result = AssignProcessToJobObject(glJob, Process);
   LeaveCriticalSection(&csJob);
   return result;
}

//********************************************************************************************************************

extern "C" void winResetStdOut(struct winprocess *Process, char *Buffer, DWORD *Size)
{
   MSG("winResetStdOut()\n");

   if (Process->StdOutEvent) ResetEvent(Process->StdOutEvent); // Turn off the most recent signal

   Buffer[0] = Process->OutBuffer[0];

   DWORD avail = 0; // Check if there is data available on the pipe
   if (PeekNamedPipe(Process->PipeOut.Read, nullptr, 0, nullptr, &avail, nullptr)) {
      if (!avail) {
         *Size = 1;
         return;
      }

      *Size = *Size - 1;

      if (*Size < avail) avail = *Size;

      if (ReadFile(Process->PipeOut.Read, Buffer+1, avail, Size, 0)) {
         *Size = *Size + 1;
         // Prepare for the next input report
         ReadFile(Process->PipeOut.Read, Process->OutBuffer, 1, &Process->OutTotalRead, &Process->OutOverlap);
      }
      else *Size = 1;
   }
   else {
      MSG("PeekNamedPipe() failed: %s\n", winFormatMessage().c_str());
      *Size = 1;
   }
}

//********************************************************************************************************************

extern "C" void winResetStdErr(struct winprocess *Process, char *Buffer, DWORD *Size)
{
   MSG("winResetStdErr(%p)\n", Process);

   if (Process->StdErrEvent) ResetEvent(Process->StdErrEvent);  // Turn off the most recent signal

   Buffer[0] = Process->ErrBuffer[0]; // A byte is always read into this buffer due to the overlapped ReadFile() call

   DWORD avail = 0;  // Check if there is more data available on the pipe and read it
   if (PeekNamedPipe(Process->PipeErr.Read, nullptr, 0, nullptr, &avail, nullptr)) {
      if (!avail) {
         MSG("Nothing more available on the pipe.");
         *Size = 1;
         return;
      }

      *Size = *Size - 1;

      if (*Size < avail) avail = *Size;

      if (ReadFile(Process->PipeErr.Read, Buffer+1, avail, Size, 0)) {
         *Size = *Size + 1;
         // Prepare for the next input report
         ReadFile(Process->PipeErr.Read, Process->ErrBuffer, 1, &Process->ErrTotalRead, &Process->ErrOverlap);
      }
      else *Size = 1;
   }
   else {
      MSG("PeekNamedPipe() failed: %s\n", winFormatMessage().c_str());
      *Size = 1;
   }
}

//********************************************************************************************************************

extern "C" int winLaunchProcess(APTR Task, LPSTR commandline, LPSTR InitialDir, int8_t Group, int8_t InternalRedirect,
   struct winprocess **ProcessResult, char HideWindow, char *RedirectStdOut, char *RedirectStdErr, int *ProcessID)
{
   SECURITY_ATTRIBUTES sa;

   if (!ProcessResult) return 0;

   int winerror = 0;
   int pid = 0;

   // Note that dwFlags must include STARTF_USESHOWWINDOW if we use the wShowWindow flags. This also assumes that the
   // CreateProcess() call will use CREATE_NEW_CONSOLE.

   STARTUPINFO start;
   for (unsigned i=0; i < sizeof(STARTUPINFO); i++) ((char *)&start)[i] = 0;
   start.cb = sizeof(STARTUPINFO);
   //start.dwFlags = STARTF_FORCEOFFFEEDBACK; // Stops the mouse pointer from showing the hourglass

   if (HideWindow) {
      // Hiding is useful if you don't want the application's window or the DOS window to be displayed.

      start.wShowWindow = SW_HIDE;
      start.dwFlags |= STARTF_USESHOWWINDOW;
   }

   auto process = (struct winprocess *)calloc(1, sizeof(struct winprocess));
   if (!process) return ERROR_NOT_ENOUGH_MEMORY;

   if (InternalRedirect) {
      HANDLE inherited_stdout = nullptr;
      HANDLE inherited_stderr = nullptr;
      auto pipe_id = unique_pipe_id();
      char stdout_pipe[128];
      char stderr_pipe[128];

      snprintf(stdout_pipe, sizeof(stdout_pipe), "\\\\.\\pipe\\ktout-%lu-%llu",
         GetCurrentProcessId(), (unsigned long long)pipe_id);
      snprintf(stderr_pipe, sizeof(stderr_pipe), "\\\\.\\pipe\\kterr-%lu-%llu",
         GetCurrentProcessId(), (unsigned long long)pipe_id);

      sa.nLength = sizeof(sa);
      sa.lpSecurityDescriptor = nullptr;
      sa.bInheritHandle = TRUE;

      start.dwFlags |= STARTF_USESTDHANDLES;

      // STDOUT
      if (InternalRedirect & TSTD_OUT) {
         HANDLE newhd;
         process->PipeOut.Read = CreateNamedPipe(stdout_pipe, PIPE_ACCESS_INBOUND|FILE_FLAG_OVERLAPPED,
            PIPE_READMODE_BYTE, 1, 4096, 4096, 1000, &sa);
         if (process->PipeOut.Read != INVALID_HANDLE_VALUE) {
            process->PipeOut.Write = CreateFile(stdout_pipe, FILE_WRITE_DATA|SYNCHRONIZE, 0, &sa, OPEN_EXISTING,
               FILE_ATTRIBUTE_NORMAL, 0);
            if (process->PipeOut.Write != INVALID_HANDLE_VALUE) {
               if (DuplicateHandle(GetCurrentProcess(), process->PipeOut.Read, GetCurrentProcess(), &newhd, 0, FALSE,
                     DUPLICATE_SAME_ACCESS)) {
                  CloseHandle(process->PipeOut.Read);
                  process->PipeOut.Read = newhd;
                  start.hStdOutput = process->PipeOut.Write; // The child process will be writing to stdout

                  if ((process->StdOutEvent = CreateEvent(nullptr, TRUE, TRUE, nullptr))) {
                     process->OutOverlap.hEvent     = process->StdOutEvent;
                     process->OutOverlap.Offset     = 0;
                     process->OutOverlap.OffsetHigh = 0;

                     task_register_stdout(Task, process->StdOutEvent);
                     if (ReadFile(process->PipeOut.Read, process->OutBuffer, 1, &process->OutTotalRead,
                           &process->OutOverlap)) {
                        MSG("Warning: ReadFile() succeeded on asynchronous file.\n");
                     }
                     else if (auto error = GetLastError(); error != ERROR_IO_PENDING) winerror = error;
                  }
                  else { MSG("CreateEvent() failed."); winerror = GetLastError(); }
               }
               else { MSG("DuplicateHandle() failed.\n"); winerror = GetLastError(); }
            }
            else { MSG("CreateFile() failed.\n"); winerror = GetLastError(); }
         }
         else { MSG("CreateNamedPipe(%s) failed.\n", stdout_pipe); winerror = GetLastError(); }
      }
      else if (!winerror) {
         winerror = duplicate_or_nul_std_handle(STD_OUTPUT_HANDLE, GENERIC_WRITE, &inherited_stdout);
         start.hStdOutput = inherited_stdout;
      }

      if ((InternalRedirect & TSTD_ERR) and (!winerror)) {
         // STDERR
         HANDLE newhd;
         process->PipeErr.Read = CreateNamedPipe(stderr_pipe, PIPE_ACCESS_INBOUND|FILE_FLAG_OVERLAPPED,
            PIPE_READMODE_BYTE, 1, 4096, 4096, 1000, &sa);
         if (process->PipeErr.Read != INVALID_HANDLE_VALUE) {
            process->PipeErr.Write = CreateFile(stderr_pipe, FILE_WRITE_DATA|SYNCHRONIZE, 0, &sa, OPEN_EXISTING,
               FILE_ATTRIBUTE_NORMAL, 0);
            if (process->PipeErr.Write != INVALID_HANDLE_VALUE) {
               if (DuplicateHandle(GetCurrentProcess(), process->PipeErr.Read, GetCurrentProcess(), &newhd, 0, FALSE,
                     DUPLICATE_SAME_ACCESS)) {
                  CloseHandle(process->PipeErr.Read);
                  process->PipeErr.Read = newhd;
                  start.hStdError  = process->PipeErr.Write; // The child process will be writing to stderr

                  if ((process->StdErrEvent = CreateEvent(nullptr, TRUE, TRUE, nullptr))) {
                     process->ErrOverlap.hEvent     = process->StdErrEvent;
                     process->ErrOverlap.Offset     = 0;
                     process->ErrOverlap.OffsetHigh = 0;

                     task_register_stderr(Task, process->StdErrEvent);
                     if (ReadFile(process->PipeErr.Read, process->ErrBuffer, 1, &process->ErrTotalRead,
                           &process->ErrOverlap)) {
                        MSG("Warning: ReadFile() succeeded on asynchronous file.\n");
                     }
                     else if (auto error = GetLastError(); error != ERROR_IO_PENDING) winerror = error;
                  }
                  else { MSG("CreateEvent() failed."); winerror = GetLastError(); }
               }
               else { MSG("DuplicateHandle() failed.\n"); winerror = GetLastError(); }
            }
            else { MSG("CreateFile() failed.\n"); winerror = GetLastError(); }
         }
         else { MSG("CreateNamedPipe(%s) failed.\n", stderr_pipe); winerror = GetLastError(); }
      }
      else if (!winerror) {
         winerror = duplicate_or_nul_std_handle(STD_ERROR_HANDLE, GENERIC_WRITE, &inherited_stderr);
         start.hStdError = inherited_stderr;
      }

      // STDIN.  Some programs get upset if an FD for stdin isn't present, so provide
      // one and close our end of the pipe if the user does not intend to send the
      // sub-process any data.

      if (CreatePipe(&process->PipeIn.Read, &process->PipeIn.Write, &sa, 4096)) {
         SetHandleInformation(process->PipeIn.Write, HANDLE_FLAG_INHERIT, 0);
         start.hStdInput = process->PipeIn.Read;   // The child process will be reading from stdin

         if (!(InternalRedirect & TSTD_IN)) {
            CloseHandle(process->PipeIn.Write);
            process->PipeIn.Write = 0;
         }
      }
      else { MSG("CreateNamedPipe(ktin) failed.\n"); winerror = GetLastError(); }

      if (!winerror) {
         // Event handling for incoming data

         PROCESS_INFORMATION info;
         if (CreateProcess(0, commandline, 0, 0, TRUE, CREATE_NEW_CONSOLE|CREATE_SUSPENDED, 0, InitialDir, &start,
               &info)) {
            pid = info.dwProcessId;
            process->Handle = info.hProcess;
            process->Task = Task;

            register_process_pipes(Task, process->Handle);

            if ((Group) and (!assign_group(info.hProcess))) MSG("AssignProcessToJobObject() failed.\n");

            ResumeThread(info.hThread); // Required as process was created with CREATE_SUSPENDED

            CloseHandle(info.hThread);
         }
         else { MSG("CreateProcess() failed.\n"); winerror = GetLastError(); }
      }

      close_handle(&inherited_stdout);
      close_handle(&inherited_stderr);

      if (!pid) {
         winFreeProcess(process);
         process = nullptr;
      }
   }
   else {
      // Think CREATE_NEW_CONSOLE means that if you run the program from DOS, a new
      // console is opened on the display rather than outputting to the current DOS window.

      SECURITY_ATTRIBUTES sa;
      int inherit = FALSE;
      bool stderr_shared = false;
      HANDLE inherited_stdin = nullptr;
      HANDLE inherited_stdout = nullptr;
      HANDLE inherited_stderr = nullptr;

      if ((!RedirectStdOut) and (!RedirectStdErr) and (HideWindow)) {
         //start.dwFlags |= STARTF_USESTDHANDLES;
         // To nullify all output, ensure hStdInput, hStdOutput, and hStdError in &start are all zero.
      }

      if (RedirectStdOut) {
         start.dwFlags |= STARTF_USESTDHANDLES;

         sa.nLength              = sizeof(SECURITY_ATTRIBUTES);
         sa.lpSecurityDescriptor = nullptr;
         sa.bInheritHandle       = TRUE;

         start.hStdOutput = CreateFile(RedirectStdOut, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
         if (start.hStdOutput != INVALID_HANDLE_VALUE) inherit = TRUE;
         else {
            winerror = GetLastError();
            start.hStdOutput = nullptr;
         }
      }

      if ((RedirectStdErr) and (!winerror)) {
         if ((RedirectStdOut) and (lstrcmpiA(RedirectStdErr, RedirectStdOut) IS 0)) {
            start.hStdError = start.hStdOutput;
            stderr_shared = true;
         }
         else {
            start.dwFlags |= STARTF_USESTDHANDLES;

            sa.nLength              = sizeof(SECURITY_ATTRIBUTES);
            sa.lpSecurityDescriptor = nullptr;
            sa.bInheritHandle       = TRUE;
            start.hStdError = CreateFile(RedirectStdErr, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (start.hStdError != INVALID_HANDLE_VALUE) inherit = TRUE;
            else {
               winerror = GetLastError();
               start.hStdError = nullptr;
            }
         }
      }

      if (((start.dwFlags & STARTF_USESTDHANDLES) != 0) and (!winerror)) {
         if (!start.hStdInput) {
            winerror = duplicate_or_nul_std_handle(STD_INPUT_HANDLE, GENERIC_READ, &inherited_stdin);
            start.hStdInput = inherited_stdin;
         }

         if ((!start.hStdOutput) and (!winerror)) {
            winerror = duplicate_or_nul_std_handle(STD_OUTPUT_HANDLE, GENERIC_WRITE, &inherited_stdout);
            start.hStdOutput = inherited_stdout;
         }

         if ((!start.hStdError) and (!winerror)) {
            winerror = duplicate_or_nul_std_handle(STD_ERROR_HANDLE, GENERIC_WRITE, &inherited_stderr);
            start.hStdError = inherited_stderr;
         }

         if (!winerror) inherit = TRUE;
      }

      PROCESS_INFORMATION info;
      if ((!winerror) and (CreateProcess(0 /* appname */, commandline, 0, 0,
            inherit, // inherit handles
            CREATE_NEW_CONSOLE|CREATE_SUSPENDED, // creation flags
            0, InitialDir, &start, &info))) {
         pid = info.dwProcessId;
         process->Handle = info.hProcess;
         process->Task = Task;

         register_process_pipes(Task, process->Handle);

         if ((Group) and (!assign_group(info.hProcess))) MSG("AssignProcessToJobObject() failed.\n");

         ResumeThread(info.hThread); // Required as process was created with CREATE_SUSPENDED
         CloseHandle(info.hThread);
      }
      else if (!winerror) winerror = GetLastError();

      if ((!stderr_shared) and (start.hStdError != inherited_stderr)) close_handle(&start.hStdError);
      else start.hStdError = nullptr;

      if (start.hStdOutput != inherited_stdout) close_handle(&start.hStdOutput);
      else start.hStdOutput = nullptr;

      close_handle(&inherited_stdin);
      close_handle(&inherited_stdout);
      close_handle(&inherited_stderr);

      if (!pid) {
         winFreeProcess(process);
         process = nullptr;
      }
   }

   if (pid) {
      *ProcessResult = process;
      *ProcessID = pid;
   }
   else {
      *ProcessResult = NULL;
      *ProcessID = 0;
   }

   return winerror;
}

//********************************************************************************************************************

extern "C" ERR winGetExitCodeProcess(struct winprocess *Process, LPDWORD Code)
{
   if (Process) {
      GetExitCodeProcess(Process->Handle, Code);
      return ERR::Okay;
   }
   else {
      *Code = 0;
      return ERR::NullArgs;
   }
}

//********************************************************************************************************************

extern "C" int winWriteStd(struct winprocess *Platform, APTR Buffer, DWORD Size)
{
   if (!Buffer) {
      // Close the process' stdin FD
      if (Platform->PipeIn.Write) CloseHandle(Platform->PipeIn.Write);
      if (Platform->PipeIn.Read) CloseHandle(Platform->PipeIn.Read);
      Platform->PipeIn.Write = nullptr;
      Platform->PipeIn.Read = nullptr;
      return 0;
   }

   DWORD result;
   if (WriteFile(Platform->PipeIn.Write, Buffer, Size, &result, nullptr)) {
      return 0;
   }
   else return GetLastError();
}

//********************************************************************************************************************
// Designed for reading from stdin/out/err pipes.  Returns -1 on general error, -2 if the pipe is broken, e.g. child
// process is dead.

extern "C" int winReadStd(struct winprocess *Platform, int Type, APTR Buffer, DWORD *Size)
{
   if (!Platform) {
      MSG("winReadStd() No Platform parameter specified.\n");
      *Size = 0;
      return 0;
   }

   HANDLE FD;
   if (Type IS TSTD_OUT)      FD = Platform->PipeOut.Read;
   else if (Type IS TSTD_ERR) FD = Platform->PipeErr.Read;
   else if (Type IS TSTD_IN)  FD = Platform->PipeIn.Read;
   else {
      MSG("winReadStd() Invalid STD Type %d specified.\n", (int)Type);
      *Size = 0;
      return -1;
   }

   if (!FD) {
      // No FD type, not really an error
      MSG("winReadStd() No FD present for STD %d.\n", (int)Type);
      *Size = 0;
      return 0;
   }

   DWORD avail = 0;
   if (!PeekNamedPipe(FD, nullptr, 0, nullptr, &avail, nullptr)) {
      MSG("winReadStd() PeekNamedPipe() failed.\n");
      if (GetLastError() IS ERROR_BROKEN_PIPE) return -2;
      else return -1;
   }

   if (!avail) {
      MSG("winReadStd() no data to read.\n");
      *Size = 0;
      return 0;
   }

   DWORD len = *Size;
   if (avail < len) len = avail;
   *Size = 0;
   if (ReadFile(FD, Buffer, len, Size, 0)) {
      // Success
      return 0;
   }
   else {
      if (GetLastError() IS ERROR_BROKEN_PIPE) {
         if (*Size <= 0) return -2;
         else return 0;
      }
      else {
         *Size = 0;
         return -1;
      }
   }
}

//********************************************************************************************************************

extern "C" HANDLE winCreateThread(LPTHREAD_START_ROUTINE Function, APTR Arg, int StackSize, DWORD *ID)
{
   HANDLE handle;

   if ((handle = CreateThread(0, StackSize, Function, Arg, 0, ID))) {
      return handle;
   }
   else return 0;
}

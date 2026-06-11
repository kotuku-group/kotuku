// Information on the Windows Integrity Mechanism:  https://msdn.microsoft.com/en-us/library/bb625957.aspx
//
// Low-integrity processes can write and create subfolders under %USER PROFILE%\AppData\LocalLow
// Reading files at any location will generally work, opening files with write access will not.
// Executing other programs is possible, but they will inherit the same low integrity permissions as the parent.
// If the low-integrity process needs to write to files outside of LocalLow, call set_low_file()

#define _WIN32_WINNT _WIN32_WINNT_WIN6

#include <windows.h>
#include <stdio.h>
#include <sddl.h>
#include <aclapi.h>
#include <string>

#ifndef PLATFORM_CONFIG_H
#include <kotuku/config.h>
#endif

#include <kotuku/system/errors.h>

#include "common.h"

IntegrityLevel get_integrity_level();
static const char * GetIntegrityLevelString(IntegrityLevel integrity_level);

#ifdef __GNUC__
[[maybe_unused]] static int str_copy(const char *String, char *Dest, int Length);
[[maybe_unused]] static ERR set_low_file(LPCWSTR pwszFileName);
#endif

static int str_copy(const char *String, char *Dest, int Length)
{
   if (Length < 0) return(0);
   int i = 0;
   if ((String) and (Dest)) {
      while ((i < Length) and (*String)) {
         Dest[i++] = *String++;
      }
      if ((*String) and (i >= Length)) Dest[i-1] = 0;
      else Dest[i] = 0;
   }
   return(i);
}

//********************************************************************************************************************

static int get_command_line_args(char ** *argv)
{
   int argc = 0;
   auto wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
   if (!wargv) { *argv = nullptr; return 0; }

   // Calculate total space for UTF-8 strings
   int n = 0;
   for (int i=0; i < argc; i++) n += WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr) + 1;

   *argv = (char **)malloc((argc + 1) * sizeof(char *) + n);
   auto arg = (char *)&((*argv)[argc + 1]);
   for (int i=0; i < argc; i++) {
      (*argv)[i] = arg;
      arg += WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, arg, n, nullptr, nullptr) + 1;
   }
   (*argv)[argc] = nullptr;
   return argc;
}

extern "C" int main(int argc, char **argv);

int WINAPI WinMain(HINSTANCE hinstance, HINSTANCE hprevinstance, LPSTR lpcmdline, int ncmdshow)
{
   char **argv;
   auto argc = get_command_line_args(&argv);
   return main(argc, argv);
}

//********************************************************************************************************************

int get_exe(char *Buffer, int Size)
{
   return(GetModuleFileName(nullptr, Buffer, Size));
}

//********************************************************************************************************************

static const char * GetIntegrityLevelString(IntegrityLevel integrity_level)
{
   switch (integrity_level) {
      case INTEGRITY_LEVEL_SYSTEM:     return "S-1-16-16384";
      case INTEGRITY_LEVEL_HIGH:       return "S-1-16-12288";
      case INTEGRITY_LEVEL_MEDIUM:     return "S-1-16-8192";
      case INTEGRITY_LEVEL_MEDIUM_LOW: return "S-1-16-6144";
      case INTEGRITY_LEVEL_LOW:        return "S-1-16-4096";
      case INTEGRITY_LEVEL_BELOW_LOW:  return "S-1-16-2048";
      case INTEGRITY_LEVEL_UNTRUSTED:  return "S-1-16-0";
      case INTEGRITY_LEVEL_UNKNOWN:
      case INTEGRITY_LEVEL_LAST:       return nullptr;
   }

   return nullptr;
}

//********************************************************************************************************************
// Returns the integrity level of the running process, expressed as an INTEGRITY constant.

IntegrityLevel get_integrity_level(void)
{
   HANDLE hToken;
   HANDLE hProcess = GetCurrentProcess();
   IntegrityLevel result = INTEGRITY_LEVEL_UNKNOWN;

   if (OpenProcessToken(hProcess, TOKEN_QUERY | TOKEN_QUERY_SOURCE, &hToken)) {
      // Get the Integrity level.
      DWORD dwLengthNeeded;
      if (!GetTokenInformation(hToken, TokenIntegrityLevel, nullptr, 0, &dwLengthNeeded)) {
         DWORD dwError = GetLastError();
         if (dwError == ERROR_INSUFFICIENT_BUFFER) {
            PTOKEN_MANDATORY_LABEL pTIL;
            pTIL = (PTOKEN_MANDATORY_LABEL)LocalAlloc(0, dwLengthNeeded);
            if (pTIL) {
               if (GetTokenInformation(hToken, TokenIntegrityLevel, pTIL, dwLengthNeeded, &dwLengthNeeded)) {
                  DWORD dwIntegrityLevel = *GetSidSubAuthority(pTIL->Label.Sid, (DWORD)(UCHAR)(*GetSidSubAuthorityCount(pTIL->Label.Sid)-1));
                  if (dwIntegrityLevel < SECURITY_MANDATORY_MEDIUM_RID) {
                     result = INTEGRITY_LEVEL_LOW;
                  }
                  else if (dwIntegrityLevel >= SECURITY_MANDATORY_MEDIUM_RID && dwIntegrityLevel < SECURITY_MANDATORY_HIGH_RID) {
                     result = INTEGRITY_LEVEL_MEDIUM;
                  }
                  else if (dwIntegrityLevel >= SECURITY_MANDATORY_HIGH_RID) {
                     result = INTEGRITY_LEVEL_HIGH;
                  }
               }
               LocalFree(pTIL);
            }
         }
      }
      CloseHandle(hToken);
   }

   return result;
}

//********************************************************************************************************************
// Execute a process at low priority.

ERR create_low_process(std::string_view ExePath, int8_t SharedOutput)
{
   ERR result = ERR::Failed;
   HANDLE hToken = nullptr;
   HANDLE hNewToken = nullptr;
   PSID pIntegritySid = nullptr;
   PROCESS_INFORMATION proc_info;
   STARTUPINFOEX startup_info;

   ZeroMemory(&startup_info, sizeof(startup_info));
   startup_info.StartupInfo.cb = sizeof(startup_info);
   ZeroMemory(&proc_info, sizeof(proc_info));

   if (SharedOutput) {
      startup_info.StartupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
      startup_info.StartupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
      startup_info.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
   }

   if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &hToken)) goto exit;
   if (!DuplicateTokenEx(hToken, 0, nullptr, SecurityImpersonation, TokenPrimary, &hNewToken)) goto exit;
   if (!ConvertStringSidToSid(GetIntegrityLevelString(INTEGRITY_LEVEL_LOW), &pIntegritySid)) goto exit;

   {
      TOKEN_MANDATORY_LABEL TIL = {0};
      TIL.Label.Attributes = SE_GROUP_INTEGRITY; // Target group integrity.
      TIL.Label.Sid        = pIntegritySid; // Low priority SID

      // Set the integrity level for the new process.

      if (!SetTokenInformation(hNewToken, TokenIntegrityLevel, &TIL, sizeof(TOKEN_MANDATORY_LABEL) + GetLengthSid(pIntegritySid))) {
         goto exit;
      }
   }

   // Create the new process at Low integrity

   if (CreateProcessAsUser(hNewToken,
         nullptr,
         (LPSTR)ExePath.data(), // Command line
         nullptr, // Process attributes
         nullptr, // Thread attributes
         (SharedOutput) ? TRUE : FALSE, // Inherit handles
         EXTENDED_STARTUPINFO_PRESENT,
         nullptr, // Environment
         nullptr, // Directory
         (LPSTARTUPINFO)&startup_info,
         &proc_info)) {

      // Successfully created the process.  Wait for it to finish.
      WaitForSingleObject(proc_info.hProcess, INFINITE);

      // Get the exit code.
      DWORD rc, exitCode;
      rc = GetExitCodeProcess(proc_info.hProcess, &exitCode);

      result = ERR::Okay;
   }

exit:
   if (proc_info.hProcess != nullptr) CloseHandle(proc_info.hProcess);
   if (proc_info.hThread != nullptr) CloseHandle(proc_info.hThread);
   LocalFree(pIntegritySid);
   if (hNewToken != nullptr) CloseHandle(hNewToken);
   if (hToken != nullptr) CloseHandle(hToken);
   return result;
}

//********************************************************************************************************************
// Changes the integrity of the target file so that low-integrity processes can write to it.
//
// This code could potentially target a file or directory, network share, registry key, semaphore, event, mutex, file mapping
// or timer (refer SetNamedSecurityInfo()).

static ERR set_low_file(LPCWSTR pwszFileName)
{
   PSECURITY_DESCRIPTOR pSD = nullptr;
   if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;;NW;;;LW)", SDDL_REVISION_1, &pSD, nullptr)) {
      PACL pSacl = nullptr; // not allocated
      BOOL fSaclPresent = FALSE;
      BOOL fSaclDefaulted = FALSE;
      if (GetSecurityDescriptorSacl(pSD, &fSaclPresent, &pSacl, &fSaclDefaulted)) {
         // Note that psidOwner, psidGroup, and pDacl are all NULL and set the new LABEL_SECURITY_INFORMATION
         SetNamedSecurityInfoW((LPWSTR) pwszFileName, SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, pSacl);
      }
      LocalFree(pSD);
      return(ERR::Okay);
   }
   else return(ERR::Failed);
}

// Query the real Windows build independently of application manifest compatibility settings.
#include <windows.h>
#include <cstdint>

uint32_t webrtc_windows_build()
{
   using RtlGetVersionFn = LONG (WINAPI *)(OSVERSIONINFOW *);
   auto ntdll = GetModuleHandleW(L"ntdll.dll");
   auto get_version = ntdll ? (RtlGetVersionFn)GetProcAddress(ntdll, "RtlGetVersion") : nullptr;
   OSVERSIONINFOW version{sizeof(version)};
   if (not get_version or get_version(&version) != 0 or version.dwMajorVersion < 10) return 0;
   return version.dwBuildNumber;
}

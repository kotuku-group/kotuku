
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>
#include <algorithm>
#include <atomic>
#include <new>
#include <string>
#include <vector>
#include "wasapi.h"

#define IS ==
#include "audio_endpoint.h"

//********************************************************************************************************************

struct WasapiStream {
   HANDLE Thread = nullptr;
   HANDLE Shutdown = CreateEventW(nullptr, TRUE, FALSE, nullptr);
   HANDLE Wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
   HANDLE Render = CreateEventW(nullptr, FALSE, FALSE, nullptr);
   HANDLE Ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
   HANDLE Notify = CreateEventW(nullptr, FALSE, FALSE, nullptr);
   std::atomic<bool> Started{false};
   std::atomic<bool> Reopen{false};
   HRESULT Result = E_FAIL;
   WasapiFormat Format;
   unsigned RequestedPeriod = 0;
   std::wstring Device;
   void *Context;
   WasapiRender Produce;
   WasapiFailure Fail;
   WasapiDiagnostics Diagnostics;
   uint64_t InitialCPU = 0;
   ULONGLONG InitialTime = GetTickCount64();
#ifdef AUDIO_TEST_BACKEND
   bool Simulated = false;
   std::string CapturePath;
   std::vector<float> Capture;
   size_t Captured = 0;
#endif
};

//********************************************************************************************************************

static uint64_t process_time()
{
   FILETIME created, exited, kernel, user;
   if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
   return ((uint64_t(kernel.dwHighDateTime) << 32) + kernel.dwLowDateTime +
      (uint64_t(user.dwHighDateTime) << 32) + user.dwLowDateTime) / 10;
}

#ifdef AUDIO_TEST_BACKEND
// Test-only paced endpoint.  Capture storage is prepared before publication and written after join.
static DWORD WINAPI simulated_thread(void *Data)
{
   auto self = (WasapiStream *)Data;
   std::vector<float> buffer(self->Format.Capacity * self->Format.Channels);
   self->Result = S_OK;
   SetEvent(self->Ready);
   HANDLE events[] = {self->Shutdown, self->Wake};
   unsigned iteration = 0;
   while (WaitForMultipleObjects(2, events, FALSE, 5) != WAIT_OBJECT_0) {
      if (!self->Started) continue;
      if (self->Reopen) { self->Fail(self->Context, int(AUDCLNT_E_DEVICE_INVALIDATED)); break; }
      // Variable packet sizes catch assumptions that every render request is one period.
      const unsigned frames = (++iteration % 3 IS 0) ? 127 : self->Format.Period;
      if (!self->Produce(self->Context, buffer.data(), frames, 0)) continue;
      const size_t count = std::min(size_t(frames) * self->Format.Channels,
         self->Capture.size() - self->Captured);
      if (count) std::copy_n(buffer.data(), count, self->Capture.data() + self->Captured);
      self->Captured += count;
   }
   return 0;
}
#endif

//********************************************************************************************************************
// The owner keeps this callback alive until unregistration.  Callbacks only publish a wake request.

class EndpointNotifications final : public IMMNotificationClient {
   LONG references = 1;
   WasapiStream *stream;

public:
   explicit EndpointNotifications(WasapiStream *Stream) : stream(Stream) { }
   ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references); }
   ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&references); }

   HRESULT STDMETHODCALLTYPE QueryInterface(REFIID ID, void **Result) override {
      if (!Result) return E_POINTER;
      *Result = nullptr;
      if (IsEqualIID(ID, __uuidof(IUnknown)) or IsEqualIID(ID, __uuidof(IMMNotificationClient))) {
         *Result = (IMMNotificationClient *)this;
         AddRef();
         return S_OK;
      }
      return E_NOINTERFACE;
   }

   HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow Flow, ERole Role, LPCWSTR) override {
      if (Flow IS eRender and Role IS eConsole and stream->Device.empty()) {
         stream->Reopen = true;
         SetEvent(stream->Wake);
      }
      return S_OK;
   }

   HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
   HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
   HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
   HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

//********************************************************************************************************************

static HRESULT select_endpoint(IMMDeviceEnumerator *Enumerator, const std::wstring &Name, IMMDevice **Device)
{
   if (Name.empty()) return Enumerator->GetDefaultAudioEndpoint(eRender, eConsole, Device);
   const HRESULT direct = Enumerator->GetDevice(Name.c_str(), Device);
   if (SUCCEEDED(direct)) return direct;

   // Endpoint IDs are opaque.  Also permit a unique friendly name from the active endpoint enumeration.

   IMMDeviceCollection *devices = nullptr;
   HRESULT result = Enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
   if (FAILED(result)) return result;
   UINT count = 0;
   result = devices->GetCount(&count);
   unsigned matches = 0;

   if (SUCCEEDED(result)) {
      for (UINT i = 0; i < count; ++i) {
         IMMDevice *candidate = nullptr;
         if (FAILED(devices->Item(i, &candidate))) continue;
         IPropertyStore *properties = nullptr;
         bool match = false;
         if (SUCCEEDED(candidate->OpenPropertyStore(STGM_READ, &properties))) {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) and name.vt IS VT_LPWSTR)
               match = _wcsicmp(Name.c_str(), name.pwszVal) IS 0;
            PropVariantClear(&name);
            properties->Release();
         }

         if (match) ++matches;
         if (match and matches IS 1) *Device = candidate;
         else candidate->Release();
      }
   }

   devices->Release();
   if (matches IS 1) return S_OK;
   if (*Device) { (*Device)->Release(); *Device = nullptr; }
   return matches ? E_INVALIDARG : direct;
}

//********************************************************************************************************************
// Activation, Get/ReleaseBuffer, service release and COM teardown all execute in this apartment.

static DWORD WINAPI render_thread(void *Data)
{
   auto self = (WasapiStream *)Data;
   EndpointNotifications notifications(self);
   bool registered = false;
   const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
   IMMDeviceEnumerator *enumerator = nullptr;
   IMMDevice *device = nullptr;
   IAudioClient *client = nullptr;
   IAudioClient3 *modern = nullptr;
   IAudioRenderClient *render = nullptr;
   WAVEFORMATEX *format = nullptr;
   HANDLE priority = nullptr;
   std::vector<float> scratch;
   AudioEndpointFormat endpoint;
   HRESULT result = com;

   if (SUCCEEDED(result)) result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
      __uuidof(IMMDeviceEnumerator), (void **)&enumerator);

   if (SUCCEEDED(result)) {
      registered = SUCCEEDED(enumerator->RegisterEndpointNotificationCallback(&notifications));
      result = select_endpoint(enumerator, self->Device, &device);
   }

   if (SUCCEEDED(result)) result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client);

   if (SUCCEEDED(result)) {
      IPropertyStore *properties = nullptr;
      if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
         PROPVARIANT name;
         PropVariantInit(&name);
         if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) and name.vt IS VT_LPWSTR)
            WideCharToMultiByte(CP_UTF8, 0, name.pwszVal, -1, self->Format.Endpoint, 256, nullptr, nullptr);
         PropVariantClear(&name);
         properties->Release();
      }
   }

   if (SUCCEEDED(result)) result = client->GetMixFormat(&format);

   if (SUCCEEDED(result)) {
      endpoint.Channels = format->nChannels;
      endpoint.Bits = endpoint.ValidBits = format->wBitsPerSample;
      endpoint.Floating = format->wFormatTag IS WAVE_FORMAT_IEEE_FLOAT;
      if (format->wFormatTag IS WAVE_FORMAT_EXTENSIBLE and format->cbSize >= 22) {
         auto extended = (WAVEFORMATEXTENSIBLE *)format;
         endpoint.Floating = IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
         endpoint.ValidBits = extended->Samples.wValidBitsPerSample;
         endpoint.Mask = extended->dwChannelMask;
         if (!endpoint.Floating and !IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
            result = AUDCLNT_E_UNSUPPORTED_FORMAT;
      }
      else if (!endpoint.Floating and format->wFormatTag != WAVE_FORMAT_PCM)
         result = AUDCLNT_E_UNSUPPORTED_FORMAT;
      if (!endpoint.valid() or format->nBlockAlign != format->nChannels * (format->wBitsPerSample / 8))
         result = AUDCLNT_E_UNSUPPORTED_FORMAT;
   }

   if (SUCCEEDED(result)) {
      self->Format.Rate = format->nSamplesPerSec;
      self->Format.Channels = format->nChannels IS 1 ? 1 : 2;
      HRESULT initialised = E_NOINTERFACE;
      if (SUCCEEDED(client->QueryInterface(__uuidof(IAudioClient3), (void **)&modern))) {
         UINT32 standard, fundamental, minimum, maximum;
         if (SUCCEEDED(modern->GetSharedModeEnginePeriod(format, &standard, &fundamental, &minimum, &maximum))) {
            unsigned selected = standard;
            if (self->RequestedPeriod and self->RequestedPeriod < standard and fundamental) {
               const auto requested = std::max(minimum, self->RequestedPeriod);
               selected = std::min(maximum, ((requested + fundamental - 1) / fundamental) * fundamental);
            }
            self->Format.Period = selected;
            initialised = modern->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
               selected, format, nullptr);
         }
      }

      if (FAILED(initialised)) {
         // A failed modern initialisation must not leave a partially initialised client in use.
         if (modern) { modern->Release(); modern = nullptr; }
         client->Release(); client = nullptr;
         result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client);
         REFERENCE_TIME period = 0;
         if (SUCCEEDED(result)) result = client->GetDevicePeriod(&period, nullptr);
         if (SUCCEEDED(result)) {
            self->Format.Period = unsigned((period * format->nSamplesPerSec + 9999999) / 10000000);
            result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
               0, 0, format, nullptr);
         }
      }

      if (SUCCEEDED(result)) result = client->GetBufferSize(&self->Format.Capacity);
      if (SUCCEEDED(result)) result = client->SetEventHandle(self->Render);
      if (SUCCEEDED(result)) result = client->GetService(__uuidof(IAudioRenderClient), (void **)&render);
   }

   if (SUCCEEDED(result)) scratch.resize(size_t(self->Format.Capacity) * self->Format.Channels);

   self->Result = result;
   SetEvent(self->Ready);
   if (SUCCEEDED(result)) {
      HANDLE events[] = {self->Shutdown, self->Wake, self->Render};
      DWORD task = 0;
      priority = AvSetMmThreadCharacteristicsW(L"Audio", &task);
      bool running = false;
      LARGE_INTEGER frequency;
      QueryPerformanceFrequency(&frequency);

      while (true) {
         const DWORD event = WaitForMultipleObjects(3, events, FALSE, INFINITE);
         if (event IS WAIT_OBJECT_0) break;
         if (event IS WAIT_FAILED) { result = HRESULT_FROM_WIN32(GetLastError()); break; }
         if (!self->Started) continue;
         if (self->Reopen) { result = AUDCLNT_E_DEVICE_INVALIDATED; break; }
         ++self->Diagnostics.Wakeups;
         UINT32 padding = 0;
         result = client->GetCurrentPadding(&padding);
         if (FAILED(result)) break;
         const auto frames = self->Format.Capacity - std::min(padding, self->Format.Capacity);
         if (!frames) continue;
         BYTE *buffer = nullptr;
         result = render->GetBuffer(frames, &buffer);
         if (FAILED(result)) break;
         LARGE_INTEGER begin, end;
         QueryPerformanceCounter(&begin);

         // mix_data retains its master scratch, clips/converts each command window into this packet,
         // and never retains the endpoint pointer after this callback.

         auto output = (float *)buffer;
         const bool direct = endpoint.Floating and endpoint.Channels <= 2;
         auto destination = direct ? output : scratch.data();
         const bool active = self->Produce(self->Context, destination, frames, padding);

         if (!active) {
            result = render->ReleaseBuffer(0, 0);
            if (FAILED(result)) break;
         }

         if (!active and !padding) {
            if (running) {
               result = client->Stop();
               if (FAILED(result)) break;
               result = client->Reset();
               if (FAILED(result)) break;
               running = false;
            }
            continue;
         }

         if (!active) continue; // Drain submitted audio before stopping.
         if (!direct) endpoint.write(output, scratch.data(), frames);
         QueryPerformanceCounter(&end);
         const uint64_t duration = uint64_t(end.QuadPart - begin.QuadPart) * 1000000 / frequency.QuadPart;
         auto &stats = self->Diagnostics;
         ++stats.Packets;
         if (running and !padding) ++stats.EmptyQueues;
         stats.QueuedTotal += padding + frames;
         stats.RenderMicroseconds += duration;
         stats.MaximumMicroseconds = std::max(stats.MaximumMicroseconds, duration);
         ++stats.Histogram[std::min(uint64_t(63), duration / 100)];
         result = render->ReleaseBuffer(frames, 0);
         if (FAILED(result)) break;
         if (!running) {
            result = client->Start();
            if (FAILED(result)) break;
            running = true;
         }
      }
      client->Stop();
      if (FAILED(result)) self->Fail(self->Context, int(result));
   }

   if (priority) AvRevertMmThreadCharacteristics(priority);
   if (render) render->Release();
   if (modern) modern->Release();
   if (client) client->Release();
   if (format) CoTaskMemFree(format);
   if (device) device->Release();
   if (registered) enumerator->UnregisterEndpointNotificationCallback(&notifications);
   if (enumerator) enumerator->Release();
   if (SUCCEEDED(com)) CoUninitialize();
   return 0;
}

//********************************************************************************************************************

WasapiStream *wasapi_open(const char *Device, WasapiFormat &Format, void *Context, WasapiRender Produce,
   WasapiFailure Fail)
{
   auto self = new (std::nothrow) WasapiStream;
   if (!self) return nullptr;
   self->Context = Context; self->Produce = Produce; self->Fail = Fail;
   self->InitialCPU = process_time();
   self->RequestedPeriod = Format.Period;

#ifdef AUDIO_TEST_BACKEND
   const std::string_view name = Device ? Device : "";
   self->Simulated = name IS "null" or name.starts_with("capture:");
   if (self->Simulated) {
      self->Format = {Format.Rate ? Format.Rate : 48000, Format.Channels ? Format.Channels : 2, 256, 768};
      if (name.starts_with("capture:")) {
         self->CapturePath = name.substr(8);
         self->Capture.resize(size_t(self->Format.Rate) * 2 * 30);
      }
   }
#endif

   if (Device and *Device and std::string_view(Device) != "default") {
      const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Device, -1, nullptr, 0);
      if (!length) { wasapi_close(self); return nullptr; }
      self->Device.resize(length);
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Device, -1, self->Device.data(), length);
   }

   if (self->Shutdown and self->Wake and self->Render and self->Ready and self->Notify) {
      auto routine = render_thread;
      #ifdef AUDIO_TEST_BACKEND
      if (self->Simulated) routine = simulated_thread;
      #endif
      self->Thread = CreateThread(nullptr, 0, routine, self, 0, nullptr);
   }

   if (!self->Thread) { wasapi_close(self); return nullptr; }
   WaitForSingleObject(self->Ready, INFINITE);
   if (FAILED(self->Result)) { wasapi_close(self); return nullptr; }
   Format = self->Format;
   return self;
}

//********************************************************************************************************************

void wasapi_close(WasapiStream *Self, WasapiDiagnostics *Diagnostics)
{
   if (!Self) return;
   SetEvent(Self->Shutdown);
   if (Self->Thread) { WaitForSingleObject(Self->Thread, INFINITE); CloseHandle(Self->Thread); }
   Self->Diagnostics.ProcessMicroseconds = process_time() - Self->InitialCPU;
   Self->Diagnostics.ElapsedMicroseconds = (GetTickCount64() - Self->InitialTime) * 1000;
   if (Diagnostics) *Diagnostics = Self->Diagnostics;
   #ifdef AUDIO_TEST_BACKEND
   if (!Self->CapturePath.empty()) {
      HANDLE file = CreateFileA(Self->CapturePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
         FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file != INVALID_HANDLE_VALUE) {
         DWORD written = 0;
         WriteFile(file, Self->Capture.data(), DWORD(Self->Captured * sizeof(float)), &written, nullptr);
         CloseHandle(file);
      }
   }
   #endif
   for (auto event : {Self->Shutdown, Self->Wake, Self->Render, Self->Ready, Self->Notify})
      if (event) CloseHandle(event);
   delete Self;
}

void wasapi_start(WasapiStream *Self) { Self->Started = true; SetEvent(Self->Wake); }
void wasapi_wake(WasapiStream *Self) { if (Self) SetEvent(Self->Wake); }
void wasapi_notify(WasapiStream *Self) { if (Self) SetEvent(Self->Notify); }
void *wasapi_notification(WasapiStream *Self) { return Self->Notify; }
bool wasapi_beep(int Pitch, int Duration) { return Beep(Pitch, Duration); }
#ifdef AUDIO_TEST_BACKEND
void wasapi_test_invalidate(WasapiStream *Self) { Self->Reopen = true; SetEvent(Self->Wake); }
#endif

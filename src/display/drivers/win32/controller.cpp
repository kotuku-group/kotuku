#define _WIN32_WINNT 0x0600
#define WINVER 0x0600
#define NO_STRICT
#define NOMINMAX

#include <windows.h>
#include <xinput.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <dinputd.h>
#include <wbemidl.h>

#include <kotuku/system/errors.h>

#include "windows.h"
#include "controller.h"
#include "controller_mapping.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace display {
namespace {

using Clock = std::chrono::steady_clock;

struct DirectDevice {
   std::string Identity;
   std::string Name;
   GUID InstanceGuid = { };
   IDirectInputDevice8 *Device = nullptr;
   controller::AxisProfile Profile;

   void release_device()
   {
      if (Device) {
         Device->Unacquire();
         Device->Release();
         Device = nullptr;
      }
   }

   void release()
   {
      release_device();
      Identity.clear();
      Name.clear();
      InstanceGuid = { };
      Profile = { };
   }
};

struct DeviceDescription {
   DIDEVICEINSTANCE Instance = { };
   std::string Identity;
};

struct Notifications {
   std::array<bool, controller::MAX_PORTS> Old = { };
   std::array<bool, controller::MAX_PORTS> Current = { };
   bool Changed = false;
};

static void publish_notifications(const Notifications &Changes);

static std::string guid_identity(const GUID &Guid)
{
   char buffer[64];
   snprintf(buffer, sizeof(buffer), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      unsigned long(Guid.Data1), unsigned(Guid.Data2), unsigned(Guid.Data3), unsigned(Guid.Data4[0]),
      unsigned(Guid.Data4[1]), unsigned(Guid.Data4[2]), unsigned(Guid.Data4[3]), unsigned(Guid.Data4[4]),
      unsigned(Guid.Data4[5]), unsigned(Guid.Data4[6]), unsigned(Guid.Data4[7]));
   return buffer;
}

static bool get_joystick_hardware_flags(uint32_t ProductID, DWORD &Flags)
{
   const auto vendor_id = ProductID & 0xffffu;
   const auto product_id = ProductID >> 16;
   char path[256];
   snprintf(path, sizeof(path),
      "SYSTEM\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\VID_%04X&PID_%04X",
      unsigned(vendor_id), unsigned(product_id));

   const HKEY roots[2] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
   for (const auto root : roots) {
      HKEY key = nullptr;
      if (RegOpenKeyExA(root, path, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) continue;
      JOYREGHWSETTINGS settings = { };
      DWORD type = 0;
      DWORD size = sizeof(settings);
      const auto result = RegQueryValueExA(key, "OEMData", nullptr, &type, (BYTE *)&settings, &size);
      RegCloseKey(key);
      if ((result IS ERROR_SUCCESS) and (type IS REG_BINARY) and (size >= sizeof(settings))) {
         Flags = settings.dwFlags;
         return true;
      }
   }
   return false;
}

static controller::RawAxis axis_from_offset(DWORD Offset)
{
   switch (Offset) {
      case offsetof(DIJOYSTATE2, lX):           return controller::RawAxis::X;
      case offsetof(DIJOYSTATE2, lY):           return controller::RawAxis::Y;
      case offsetof(DIJOYSTATE2, lZ):           return controller::RawAxis::Z;
      case offsetof(DIJOYSTATE2, lRx):          return controller::RawAxis::RX;
      case offsetof(DIJOYSTATE2, lRy):          return controller::RawAxis::RY;
      case offsetof(DIJOYSTATE2, lRz):          return controller::RawAxis::RZ;
      case offsetof(DIJOYSTATE2, rglSlider[0]): return controller::RawAxis::SLIDER_0;
      case offsetof(DIJOYSTATE2, rglSlider[1]): return controller::RawAxis::SLIDER_1;
      default:               return controller::RawAxis::END;
   }
}

static int32_t direct_axis_value(const DIJOYSTATE2 &State, controller::RawAxis Axis)
{
   switch (Axis) {
      case controller::RawAxis::X:        return State.lX;
      case controller::RawAxis::Y:        return State.lY;
      case controller::RawAxis::Z:        return State.lZ;
      case controller::RawAxis::RX:       return State.lRx;
      case controller::RawAxis::RY:       return State.lRy;
      case controller::RawAxis::RZ:       return State.lRz;
      case controller::RawAxis::SLIDER_0: return State.rglSlider[0];
      case controller::RawAxis::SLIDER_1: return State.rglSlider[1];
      case controller::RawAxis::END:      return 0;
   }
   return 0;
}

struct ObjectContext {
   IDirectInputDevice8 *Device;
   bool Configure;
   std::array<bool, size_t(controller::RawAxis::END)> Present = { };
};

static BOOL CALLBACK enumerate_objects(const DIDEVICEOBJECTINSTANCE *Instance, VOID *Context)
{
   auto &context = *(ObjectContext *)Context;
   const auto axis = axis_from_offset(Instance->dwOfs);
   if (axis != controller::RawAxis::END) {
      context.Present[size_t(axis)] = true;
      if (context.Configure) {
         DIPROPRANGE range = { };
         range.diph.dwSize = sizeof(range);
         range.diph.dwHeaderSize = sizeof(range.diph);
         range.diph.dwObj = Instance->dwType;
         range.diph.dwHow = DIPH_BYID;
         range.lMin = -32768;
         range.lMax = 32767;
         context.Device->SetProperty(DIPROP_RANGE, &range.diph);
      }
   }
   return DIENUM_CONTINUE;
}

static BOOL CALLBACK enumerate_devices(const DIDEVICEINSTANCE *Instance, VOID *Context)
{
   auto &devices = *(std::vector<DeviceDescription> *)Context;
   DeviceDescription description;
   description.Instance = *Instance;
   description.Identity = guid_identity(Instance->guidInstance);
   devices.push_back(description);
   return DIENUM_CONTINUE;
}

static std::string narrow_device_id(const wchar_t *Value)
{
   std::string result;
   if (not Value) return result;
   while (*Value) {
      result.push_back((*Value <= 0x7f) ? char(*Value) : '?');
      Value++;
   }
   return result;
}

static bool collect_xinput_products(std::set<uint32_t> &Products)
{
   const auto init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
   const bool uninitialise = SUCCEEDED(init_result);
   if (FAILED(init_result) and (init_result != RPC_E_CHANGED_MODE)) return false;

   IWbemLocator *locator = nullptr;
   IWbemServices *services = nullptr;
   IEnumWbemClassObject *enumerator = nullptr;
   bool success = false;

   const auto security_result = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
   if (FAILED(security_result) and (security_result != RPC_E_TOO_LATE)) goto cleanup;

   if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
         (void **)&locator))) goto cleanup;

   {
      BSTR name_space = SysAllocString(L"ROOT\\CIMV2");
      if (not name_space) goto cleanup;
      const auto result = locator->ConnectServer(name_space, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
      SysFreeString(name_space);
      if (FAILED(result)) goto cleanup;
   }

   if (FAILED(CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
         RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE))) goto cleanup;

   {
      BSTR language = SysAllocString(L"WQL");
      BSTR query = SysAllocString(L"SELECT PNPDeviceID FROM Win32_PNPEntity");
      if ((not language) or (not query)) {
         if (language) SysFreeString(language);
         if (query) SysFreeString(query);
         goto cleanup;
      }
      const auto result = services->ExecQuery(language, query,
         WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator);
      SysFreeString(language);
      SysFreeString(query);
      if (FAILED(result)) goto cleanup;
   }

   while (true) {
      IWbemClassObject *object = nullptr;
      ULONG returned = 0;
      const auto result = enumerator->Next(5000, 1, &object, &returned);
      if (FAILED(result)) goto cleanup;
      if (returned IS 0) break;

      VARIANT value;
      VariantInit(&value);
      BSTR property = SysAllocString(L"PNPDeviceID");
      if (property) {
         if (SUCCEEDED(object->Get(property, 0, &value, nullptr, nullptr)) and (value.vt IS VT_BSTR)) {
            const auto product = controller::parseXInputDeviceID(narrow_device_id(value.bstrVal));
            if (product != 0xffffffffu) Products.insert(product);
         }
         SysFreeString(property);
      }
      VariantClear(&value);
      object->Release();
   }
   success = true;

cleanup:
   if (enumerator) enumerator->Release();
   if (services) services->Release();
   if (locator) locator->Release();
   if (uninitialise) CoUninitialize();
   return success;
}

static ERR direct_input_error(HRESULT Result)
{
   if ((Result IS DIERR_UNPLUGGED) or (Result IS DI_NOTATTACHED)) return ERR::Disconnected;
   if (Result IS DIERR_OTHERAPPHASPRIO) return ERR::AccessObject;
   if ((Result IS E_INVALIDARG) or (Result IS DIERR_INVALIDPARAM)) return ERR::Args;
   return ERR::SystemCall;
}

class ControllerManager {
public:
   ~ControllerManager()
   {
      shutdown();
   }

   ERR read(int Port, double *Values, CON &Buttons, Notifications &Changes)
   {
      if (not Values) return ERR::Args;
      std::fill(Values, Values + 6, 0.0);
      Buttons = CON::NIL;
      if ((Port < -1) or (Port >= controller::MAX_PORTS)) return ERR::OutOfRange;

      std::lock_guard lock(Lock);
      Changes.Old = occupied();
      if (not CooperativeWindow) return ERR::NotInitialised;

      int selected = Port;
      if (selected IS -1) {
         selected = controller::selectPrimary(Changes.Old, PrimaryPort);
         if (selected < 0) return ERR::Disconnected;
      }

      controller::ControllerState state;
      ERR error;
      if (selected < controller::XINPUT_PORTS) error = read_xinput(selected, state);
      else error = read_direct_input(selected, state);

      if (error IS ERR::Okay) {
         std::copy(state.Axes.begin(), state.Axes.end(), Values);
         Buttons = CON(state.Buttons);
         PrimaryPort = selected;
      }
      else if (selected IS PrimaryPort) PrimaryPort = -1;

      Changes.Current = occupied();
      Changes.Changed = Changes.Old != Changes.Current;
      return error;
   }

   ERR total(int &Value)
   {
      std::unique_lock lock(Lock);
      if (not CooperativeWindow) {
         Value = 0;
         return ERR::NotInitialised;
      }

      if (Dirty) {
         request_refresh();
         RefreshComplete.wait(lock, [this]() { return (not Dirty) or (not CooperativeWindow) or Stopping; });
         if ((not CooperativeWindow) or Stopping) {
            Value = 0;
            return ERR::NotInitialised;
         }
      }

      Value = controller::totalPorts(occupied());
      return ERR::Okay;
   }

   void set_window(HWND Window, bool Enabled)
   {
      std::lock_guard lock(Lock);
      const auto found = std::find(Windows.begin(), Windows.end(), Window);
      if (Enabled and (found IS Windows.end())) Windows.push_back(Window);
      else if ((not Enabled) and (found != Windows.end())) Windows.erase(found);

      update_cooperative_window(select_cooperative_window());
      Dirty = true;
      LastEmptyProbe = Clock::time_point();
      request_refresh();
   }

   void remove_window(HWND Window)
   {
      std::lock_guard lock(Lock);
      const auto found = std::find(Windows.begin(), Windows.end(), Window);
      if (found != Windows.end()) Windows.erase(found);
      if (not has_cooperative_window(CooperativeWindow)) {
         update_cooperative_window(select_cooperative_window());
         Dirty = true;
         LastEmptyProbe = Clock::time_point();
         request_refresh();
      }
   }

   void activate_window(HWND Window)
   {
      std::lock_guard lock(Lock);
      if (std::find(Windows.begin(), Windows.end(), Window) != Windows.end()) {
         update_cooperative_window(top_level_window(Window));
      }
      Dirty = true;
      LastEmptyProbe = Clock::time_point();
      request_refresh();
   }

   void mark_dirty()
   {
      std::lock_guard lock(Lock);
      Dirty = true;
      LastEmptyProbe = Clock::time_point();
      request_refresh();
   }

   void refresh(Notifications &Changes)
   {
      std::lock_guard lock(Lock);
      Changes.Old = occupied();
      if ((not Dirty) or (not CooperativeWindow)) {
         Changes.Current = Changes.Old;
         return;
      }

      refresh_xinput();
      refresh_direct_input();
      Dirty = false;
      Changes.Current = occupied();
      Changes.Changed = Changes.Old != Changes.Current;
      if ((PrimaryPort >= 0) and (not Changes.Current[PrimaryPort])) PrimaryPort = -1;
   }

   void shutdown()
   {
      {
         std::lock_guard lock(Lock);
         Stopping = true;
         WorkerSignal.notify_all();
         RefreshComplete.notify_all();
      }
      if (Worker.joinable()) Worker.join();

      {
         std::lock_guard lock(Lock);
         for (auto &device : DirectDevices) device.release();
         if (DirectInput) {
            DirectInput->Release();
            DirectInput = nullptr;
         }
         Windows.clear();
         CooperativeWindow = nullptr;
         XInputConnected.fill(false);
         PrimaryPort = -1;
         Dirty = true;
         RefreshRequested = false;
         Stopping = false;
      }
   }

private:
   void request_refresh()
   {
      RefreshRequested = true;
      if (not Worker.joinable()) Worker = std::thread([this]() { worker_loop(); });
      WorkerSignal.notify_one();
   }

   void worker_loop()
   {
      // DirectInput requires COM to remain initialised until its interfaces have been released.
      const auto init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      const bool uninitialise = SUCCEEDED(init_result);

      while (true) {
         {
            std::unique_lock lock(Lock);
            WorkerSignal.wait(lock, [this]() { return RefreshRequested or Stopping; });
            if (Stopping) {
               for (auto &device : DirectDevices) device.release();
               if (DirectInput) {
                  DirectInput->Release();
                  DirectInput = nullptr;
               }
               lock.unlock();
               if (uninitialise) CoUninitialize();
               return;
            }
            RefreshRequested = false;
         }

         Notifications changes;
         refresh(changes);
         RefreshComplete.notify_all();
         publish_notifications(changes);
      }
   }

   std::array<bool, controller::MAX_PORTS> occupied() const
   {
      std::array<bool, controller::MAX_PORTS> result = { };
      for (int port=0; port < controller::XINPUT_PORTS; port++) result[port] = XInputConnected[port];
      for (int port=controller::XINPUT_PORTS; port < controller::MAX_PORTS; port++) {
         result[port] = not DirectDevices[port].Identity.empty();
      }
      return result;
   }

   static HWND top_level_window(HWND Window)
   {
      if (not Window) return nullptr;
      const auto root = GetAncestor(Window, GA_ROOT);
      return root ? root : Window;
   }

   bool has_cooperative_window(HWND Window) const
   {
      if (not Window) return false;
      return std::find_if(Windows.begin(), Windows.end(), [&](HWND Registered) {
         return top_level_window(Registered) IS Window;
      }) != Windows.end();
   }

   HWND select_cooperative_window() const
   {
      const auto foreground = top_level_window(GetForegroundWindow());
      if (foreground and has_cooperative_window(foreground)) return foreground;
      if (has_cooperative_window(CooperativeWindow)) return CooperativeWindow;
      return Windows.empty() ? nullptr : top_level_window(Windows.front());
   }

   void update_cooperative_window(HWND Window)
   {
      if (Window IS CooperativeWindow) return;
      CooperativeWindow = Window;
      for (int port=controller::XINPUT_PORTS; port < controller::MAX_PORTS; port++) {
         auto &record = DirectDevices[port];
         record.release_device();
      }
   }

   void refresh_xinput()
   {
      const auto now = Clock::now();
      const bool probe_empty = (LastEmptyProbe IS Clock::time_point()) or
         ((now - LastEmptyProbe) >= std::chrono::seconds(3));
      for (DWORD port=0; port < XUSER_MAX_COUNT; port++) {
         if ((not XInputConnected[port]) and (not probe_empty)) continue;
         XINPUT_CAPABILITIES capabilities = { };
         XInputConnected[port] = XInputGetCapabilities(port, XINPUT_FLAG_GAMEPAD, &capabilities) IS ERROR_SUCCESS;
      }
      if (probe_empty) LastEmptyProbe = now;
   }

   bool ensure_direct_input()
   {
      if (DirectInput) return true;
      return SUCCEEDED(DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8,
         (void **)&DirectInput, nullptr));
   }

   void refresh_direct_input()
   {
      std::set<uint32_t> xinput_products;
      if (not collect_xinput_products(xinput_products)) {
         MsgControllerLog(true,
            "WMI controller classification failed; DirectInput discovery is disabled for this refresh.");
         for (int port=controller::XINPUT_PORTS; port < controller::MAX_PORTS; port++) DirectDevices[port].release();
         return;
      }
      if (not ensure_direct_input()) {
         MsgControllerLog(true, "DirectInput8 initialisation failed; XInput remains available.");
         return;
      }

      std::vector<DeviceDescription> enumerated;
      if (FAILED(DirectInput->EnumDevices(DI8DEVCLASS_GAMECTRL, enumerate_devices, &enumerated, DIEDFL_ATTACHEDONLY))) {
         MsgControllerLog(true, "DirectInput controller enumeration failed.");
         return;
      }

      std::vector<DeviceDescription> accepted;
      std::vector<std::string> identities;
      for (const auto &description : enumerated) {
         const auto product = controller::directInputProductID(description.Instance.guidProduct.Data1);
         if (xinput_products.find(product) != xinput_products.end()) continue;
         if (not is_usable_direct_device(description)) continue;
         accepted.push_back(description);
         identities.push_back(description.Identity);
      }

      std::array<std::string, controller::MAX_PORTS> current = { };
      for (int port=controller::XINPUT_PORTS; port < controller::MAX_PORTS; port++) {
         current[port] = DirectDevices[port].Identity;
      }
      const auto assignment = controller::allocateDirectInputSlots(current, identities);

      for (int port=controller::XINPUT_PORTS; port < controller::MAX_PORTS; port++) {
         if (assignment[port] != DirectDevices[port].Identity) DirectDevices[port].release();
         if (assignment[port].empty() or DirectDevices[port].Device) continue;

         const auto found = std::find_if(accepted.begin(), accepted.end(), [&](const DeviceDescription &Description) {
            return Description.Identity IS assignment[port];
         });
         if (found IS accepted.end()) continue;
         create_direct_device(port, *found);
      }
   }

   void create_direct_device(int Port, const DeviceDescription &Description)
   {
      IDirectInputDevice8 *device = nullptr;
      if (FAILED(DirectInput->CreateDevice(Description.Instance.guidInstance, &device, nullptr))) return;

      if (FAILED(device->SetDataFormat(&c_dfDIJoystick2)) or
          FAILED(device->SetCooperativeLevel(CooperativeWindow, DISCL_NONEXCLUSIVE|DISCL_FOREGROUND))) {
         device->Release();
         return;
      }

      ObjectContext context = { .Device = device, .Configure = true };
      if (FAILED(device->EnumObjects(enumerate_objects, &context, DIDFT_AXIS|DIDFT_BUTTON|DIDFT_POV))) {
         device->Release();
         return;
      }

      auto &record = DirectDevices[Port];
      record.Identity = Description.Identity;
      record.Name = Description.Instance.tszInstanceName;
      record.InstanceGuid = Description.Instance.guidInstance;
      record.Device = device;
      DWORD joystick_flags = 0;
      get_joystick_hardware_flags(controller::directInputProductID(Description.Instance.guidProduct.Data1),
         joystick_flags);
      const bool four_axis_layout = (joystick_flags & JOY_HWS_HASZ) and (joystick_flags & JOY_HWS_HASR) and
         (not (joystick_flags & JOY_HWS_HASU)) and (not (joystick_flags & JOY_HWS_HASV));
      record.Profile = controller::buildDirectInputProfile(context.Present, four_axis_layout);
      device->Acquire();

      std::string mapping;
      for (size_t i=0; i < record.Profile.Targets.size(); i++) {
         if (record.Profile.Targets[i] != controller::AxisTarget::UNUSED) {
            if (not mapping.empty()) mapping += ",";
            mapping += std::to_string(i) + "->" + std::to_string(int(record.Profile.Targets[i]));
         }
      }
      const auto message = std::string("DirectInput controller '") + record.Name + "' assigned to port " +
         std::to_string(Port) + " (instance " + record.Identity + ", mapping " + mapping + ").";
      MsgControllerLog(false, message.c_str());
   }

   bool is_usable_direct_device(const DeviceDescription &Description)
   {
      IDirectInputDevice8 *device = nullptr;
      if (FAILED(DirectInput->CreateDevice(Description.Instance.guidInstance, &device, nullptr))) return false;
      if (FAILED(device->SetDataFormat(&c_dfDIJoystick2)) or
          FAILED(device->SetCooperativeLevel(CooperativeWindow, DISCL_NONEXCLUSIVE|DISCL_FOREGROUND))) {
         device->Release();
         return false;
      }
      ObjectContext context = { .Device = device, .Configure = false };
      const auto result = device->EnumObjects(enumerate_objects, &context, DIDFT_AXIS|DIDFT_BUTTON|DIDFT_POV);
      device->Release();
      return SUCCEEDED(result) and controller::isUsableDirectInputDevice(context.Present);
   }

   ERR read_xinput(int Port, controller::ControllerState &State)
   {
      if (not XInputConnected[Port]) {
         const auto now = Clock::now();
         if ((LastEmptyProbe != Clock::time_point()) and ((now - LastEmptyProbe) < std::chrono::seconds(3))) {
            return ERR::Disconnected;
         }
         LastEmptyProbe = now;
      }

      XINPUT_STATE native_state = { };
      const auto result = XInputGetState(DWORD(Port), &native_state);
      if (result IS ERROR_SUCCESS) {
         XInputConnected[Port] = true;
         State = controller::mapXInputState(native_state.Gamepad.bLeftTrigger, native_state.Gamepad.bRightTrigger,
            native_state.Gamepad.sThumbLX, native_state.Gamepad.sThumbLY, native_state.Gamepad.sThumbRX,
            native_state.Gamepad.sThumbRY, native_state.Gamepad.wButtons);
         return ERR::Okay;
      }
      XInputConnected[Port] = false;
      if (result IS ERROR_DEVICE_NOT_CONNECTED) return ERR::Disconnected;
      if (result IS ERROR_INVALID_PARAMETER) return ERR::Args;
      return ERR::SystemCall;
   }

   ERR read_direct_input(int Port, controller::ControllerState &State)
   {
      auto &record = DirectDevices[Port];
      if (not record.Device) return ERR::Disconnected;

      auto result = record.Device->Poll();
      if (FAILED(result)) {
         if ((result IS DIERR_INPUTLOST) or (result IS DIERR_NOTACQUIRED)) {
            result = record.Device->Acquire();
            if (SUCCEEDED(result)) result = record.Device->Poll();
         }
         if (FAILED(result)) {
            const auto error = direct_input_error(result);
            if (error IS ERR::Disconnected) record.release();
            return error;
         }
      }

      DIJOYSTATE2 native_state = { };
      result = record.Device->GetDeviceState(sizeof(native_state), &native_state);
      if (FAILED(result)) {
         const auto error = direct_input_error(result);
         if (error IS ERR::Disconnected) record.release();
         return error;
      }

      controller::RawDirectInputState raw;
      for (size_t i=0; i < raw.Axes.size(); i++) {
         raw.Axes[i] = direct_axis_value(native_state, controller::RawAxis(i));
      }
      raw.Pov = native_state.rgdwPOV[0];
      std::copy(std::begin(native_state.rgbButtons), std::end(native_state.rgbButtons), raw.Buttons.begin());
      State = controller::mapDirectInputState(raw, record.Profile);
      return ERR::Okay;
   }

   std::mutex Lock;
   std::condition_variable WorkerSignal;
   std::condition_variable RefreshComplete;
   std::thread Worker;
   std::vector<HWND> Windows;
   HWND CooperativeWindow = nullptr;
   IDirectInput8 *DirectInput = nullptr;
   std::array<DirectDevice, controller::MAX_PORTS> DirectDevices;
   std::array<bool, controller::XINPUT_PORTS> XInputConnected = { };
   Clock::time_point LastEmptyProbe;
   int PrimaryPort = -1;
   bool Dirty = true;
   bool RefreshRequested = false;
   bool Stopping = false;
};

ControllerManager glControllerManager;

static void publish_notifications(const Notifications &Changes)
{
   if (not Changes.Changed) return;
   const auto total = controller::totalPorts(Changes.Current);
   for (int port=0; port < controller::MAX_PORTS; port++) {
      if (Changes.Old[port] != Changes.Current[port]) {
         MsgControllerPorts(port, Changes.Current[port], total);
      }
   }
}

} // namespace

ERR winReadController(int Port, double *Values, CON &Buttons)
{
   Notifications changes;
   const auto error = glControllerManager.read(Port, Values, Buttons, changes);
   publish_notifications(changes);
   return error;
}

ERR winGetControllerPorts(int &Value)
{
   return glControllerManager.total(Value);
}

void winControllerSetWindow(HWND Window, bool Enabled)
{
   if (Window) glControllerManager.set_window(Window, Enabled);
}

void winControllerRemoveWindow(HWND Window)
{
   if (Window) glControllerManager.remove_window(Window);
}

void winControllerActivateWindow(HWND Window)
{
   if (Window) glControllerManager.activate_window(Window);
}

void winControllerMarkDirty(HWND Window)
{
   (void)Window;
   glControllerManager.mark_dirty();
}

void winControllerShutdown()
{
   glControllerManager.shutdown();
}

} // namespace display

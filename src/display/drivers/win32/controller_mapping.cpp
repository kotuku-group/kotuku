#include "controller_mapping.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace display::controller {

double normaliseSignedAxis(int32_t Value)
{
   return std::clamp(double(Value) * (1.0 / 32767.0), -1.0, 1.0);
}

double normaliseUnsignedTrigger(uint8_t Value)
{
   return double(Value) * (1.0 / 255.0);
}

void applyStickTolerance(double &X, double &Y)
{
   if ((X < STICK_TOLERANCE) and (X > -STICK_TOLERANCE) and
       (Y < STICK_TOLERANCE) and (Y > -STICK_TOLERANCE)) {
      X = 0;
      Y = 0;
   }
}

uint32_t mapXInputButtons(uint16_t Buttons)
{
   uint32_t result = 0;
   if (Buttons & 0x0001) result |= DPAD_UP;
   if (Buttons & 0x0002) result |= DPAD_DOWN;
   if (Buttons & 0x0004) result |= DPAD_LEFT;
   if (Buttons & 0x0008) result |= DPAD_RIGHT;
   if (Buttons & 0x0010) result |= START;
   if (Buttons & 0x0020) result |= SELECT;
   if (Buttons & 0x0040) result |= LEFT_THUMB;
   if (Buttons & 0x0080) result |= RIGHT_THUMB;
   if (Buttons & 0x0100) result |= LEFT_BUMPER_1;
   if (Buttons & 0x0200) result |= RIGHT_BUMPER_1;
   if (Buttons & 0x1000) result |= GAMEPAD_S;
   if (Buttons & 0x2000) result |= GAMEPAD_E;
   if (Buttons & 0x4000) result |= GAMEPAD_W;
   if (Buttons & 0x8000) result |= GAMEPAD_N;
   return result;
}

ControllerState mapXInputState(uint8_t LeftTrigger, uint8_t RightTrigger, int16_t LeftX, int16_t LeftY,
   int16_t RightX, int16_t RightY, uint16_t Buttons)
{
   ControllerState result;
   result.Axes[0] = normaliseUnsignedTrigger(LeftTrigger);
   result.Axes[1] = normaliseUnsignedTrigger(RightTrigger);
   result.Axes[2] = normaliseSignedAxis(LeftX);
   result.Axes[3] = normaliseSignedAxis(LeftY);
   result.Axes[4] = normaliseSignedAxis(RightX);
   result.Axes[5] = normaliseSignedAxis(RightY);
   applyStickTolerance(result.Axes[2], result.Axes[3]);
   applyStickTolerance(result.Axes[4], result.Axes[5]);
   result.Buttons = mapXInputButtons(Buttons);
   return result;
}

AxisProfile buildDirectInputProfile(const std::array<bool, size_t(RawAxis::END)> &Present, bool LegacyFourAxisLayout)
{
   AxisProfile result;
   result.Targets.fill(AxisTarget::UNUSED);
   if (Present[size_t(RawAxis::X)]) result.Targets[size_t(RawAxis::X)] = AxisTarget::LEFT_X;
   if (Present[size_t(RawAxis::Y)]) result.Targets[size_t(RawAxis::Y)] = AxisTarget::LEFT_Y;

   std::array<bool, size_t(RawAxis::END)> used = { };
   used[size_t(RawAxis::X)] = Present[size_t(RawAxis::X)];
   used[size_t(RawAxis::Y)] = Present[size_t(RawAxis::Y)];

   if (LegacyFourAxisLayout and Present[size_t(RawAxis::Z)] and Present[size_t(RawAxis::RZ)]) {
      result.Targets[size_t(RawAxis::Z)] = AxisTarget::RIGHT_X;
      result.Targets[size_t(RawAxis::RZ)] = AxisTarget::RIGHT_Y;
      used[size_t(RawAxis::Z)] = true;
      used[size_t(RawAxis::RZ)] = true;
   }
   else if (Present[size_t(RawAxis::RX)] and Present[size_t(RawAxis::RY)]) {
      result.Targets[size_t(RawAxis::RX)] = AxisTarget::RIGHT_X;
      result.Targets[size_t(RawAxis::RY)] = AxisTarget::RIGHT_Y;
      used[size_t(RawAxis::RX)] = true;
      used[size_t(RawAxis::RY)] = true;
   }
   else if (Present[size_t(RawAxis::Z)] and Present[size_t(RawAxis::RZ)]) {
      result.Targets[size_t(RawAxis::Z)] = AxisTarget::RIGHT_X;
      result.Targets[size_t(RawAxis::RZ)] = AxisTarget::RIGHT_Y;
      used[size_t(RawAxis::Z)] = true;
      used[size_t(RawAxis::RZ)] = true;
   }

   std::array<RawAxis, size_t(RawAxis::END)> remaining = { };
   size_t count = 0;
   const std::array<RawAxis, 6> trigger_order = {
      RawAxis::SLIDER_0, RawAxis::SLIDER_1, RawAxis::Z, RawAxis::RZ, RawAxis::RX, RawAxis::RY
   };
   for (const auto axis : trigger_order) {
      const auto index = size_t(axis);
      if (Present[index] and (not used[index])) remaining[count++] = axis;
   }

   if (count >= 2) {
      result.Targets[size_t(remaining[0])] = AxisTarget::LEFT_TRIGGER;
      result.Targets[size_t(remaining[1])] = AxisTarget::RIGHT_TRIGGER;
   }
   else if (count IS 1) result.Targets[size_t(remaining[0])] = AxisTarget::SPLIT_TRIGGER;
   return result;
}

bool isUsableDirectInputDevice(const std::array<bool, size_t(RawAxis::END)> &Present)
{
   return Present[size_t(RawAxis::X)] and Present[size_t(RawAxis::Y)];
}

static void map_axis(ControllerState &Result, AxisTarget Target, int32_t Value)
{
   const auto normalised = normaliseSignedAxis(Value);
   switch (Target) {
      case AxisTarget::LEFT_X:        Result.Axes[2] = normalised; break;
      case AxisTarget::LEFT_Y:        Result.Axes[3] = -normalised; break;
      case AxisTarget::RIGHT_X:       Result.Axes[4] = normalised; break;
      case AxisTarget::RIGHT_Y:       Result.Axes[5] = -normalised; break;
      case AxisTarget::LEFT_TRIGGER:  Result.Axes[0] = std::clamp((normalised + 1.0) * 0.5, 0.0, 1.0); break;
      case AxisTarget::RIGHT_TRIGGER: Result.Axes[1] = std::clamp((normalised + 1.0) * 0.5, 0.0, 1.0); break;
      case AxisTarget::SPLIT_TRIGGER:
         Result.Axes[0] = std::clamp(-normalised, 0.0, 1.0);
         Result.Axes[1] = std::clamp(normalised, 0.0, 1.0);
         break;
      case AxisTarget::UNUSED: break;
   }
}

ControllerState mapDirectInputState(const RawDirectInputState &State, const AxisProfile &Profile)
{
   ControllerState result;
   for (size_t i=0; i < State.Axes.size(); i++) map_axis(result, Profile.Targets[i], State.Axes[i]);

   applyStickTolerance(result.Axes[2], result.Axes[3]);
   applyStickTolerance(result.Axes[4], result.Axes[5]);

   if ((State.Pov & 0xffffu) != 0xffffu) {
      const auto angle = State.Pov % 36000u;
      if ((angle >= 31500u) or (angle <= 4500u)) result.Buttons |= DPAD_UP;
      if ((angle >= 4500u) and (angle <= 13500u)) result.Buttons |= DPAD_RIGHT;
      if ((angle >= 13500u) and (angle <= 22500u)) result.Buttons |= DPAD_DOWN;
      if ((angle >= 22500u) and (angle <= 31500u)) result.Buttons |= DPAD_LEFT;
   }

   const std::array<uint32_t, 12> button_map = {
      GAMEPAD_S, GAMEPAD_E, GAMEPAD_W, GAMEPAD_N,
      LEFT_BUMPER_1, RIGHT_BUMPER_1, LEFT_BUMPER_2, RIGHT_BUMPER_2,
      SELECT, START, LEFT_THUMB, RIGHT_THUMB
   };
   for (size_t i=0; i < button_map.size(); i++) {
      if (State.Buttons[i] & 0x80u) result.Buttons |= button_map[i];
   }
   return result;
}

static int hex_value(char Value)
{
   if ((Value >= '0') and (Value <= '9')) return Value - '0';
   Value = char(std::toupper(uint8_t(Value)));
   if ((Value >= 'A') and (Value <= 'F')) return Value - 'A' + 10;
   return -1;
}

static uint32_t parse_hex_word(std::string_view Text, size_t Offset)
{
   if (Offset + 4 > Text.size()) return 0xffffffffu;
   uint32_t result = 0;
   for (size_t i=0; i < 4; i++) {
      const auto digit = hex_value(Text[Offset + i]);
      if (digit < 0) return 0xffffffffu;
      result = (result << 4) | uint32_t(digit);
   }
   return result;
}

uint32_t parseXInputDeviceID(std::string_view DeviceID)
{
   std::string upper(DeviceID);
   std::transform(upper.begin(), upper.end(), upper.begin(), [](char Value) {
      return char(std::toupper(uint8_t(Value)));
   });
   if (upper.find("IG_") IS std::string::npos) return 0xffffffffu;
   const auto vid_pos = upper.find("VID_");
   const auto pid_pos = upper.find("PID_");
   if ((vid_pos IS std::string::npos) or (pid_pos IS std::string::npos)) return 0xffffffffu;
   const auto vid = parse_hex_word(upper, vid_pos + 4);
   const auto pid = parse_hex_word(upper, pid_pos + 4);
   if ((vid > 0xffffu) or (pid > 0xffffu)) return 0xffffffffu;
   return vid | (pid << 16);
}

uint32_t directInputProductID(uint32_t GuidData1)
{
   return GuidData1;
}

std::array<std::string, MAX_PORTS> allocateDirectInputSlots(
   const std::array<std::string, MAX_PORTS> &Current, const std::vector<std::string> &Identities)
{
   std::array<std::string, MAX_PORTS> result = { };
   for (int port=XINPUT_PORTS; port < MAX_PORTS; port++) {
      if (Current[port].empty()) continue;
      if (std::find(Identities.begin(), Identities.end(), Current[port]) != Identities.end()) {
         result[port] = Current[port];
      }
   }

   for (const auto &identity : Identities) {
      bool assigned = false;
      for (int port=XINPUT_PORTS; port < MAX_PORTS; port++) {
         if (result[port] IS identity) {
            assigned = true;
            break;
         }
      }
      if (assigned) continue;
      for (int port=XINPUT_PORTS; port < MAX_PORTS; port++) {
         if (result[port].empty()) {
            result[port] = identity;
            break;
         }
      }
   }
   return result;
}

int totalPorts(const std::array<bool, MAX_PORTS> &Occupied)
{
   for (int port=MAX_PORTS - 1; port >= 0; port--) {
      if (Occupied[port]) return port + 1;
   }
   return 0;
}

int selectPrimary(const std::array<bool, MAX_PORTS> &Occupied, int CachedPort)
{
   if ((CachedPort >= 0) and (CachedPort < MAX_PORTS) and Occupied[CachedPort]) return CachedPort;
   for (int port=0; port < MAX_PORTS; port++) {
      if (Occupied[port]) return port;
   }
   return -1;
}

} // namespace display::controller

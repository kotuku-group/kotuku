#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#ifndef IS
#define IS ==
#endif

namespace display::controller {

constexpr int MAX_PORTS = 32;
constexpr int XINPUT_PORTS = 4;
constexpr double STICK_TOLERANCE = 0.08;

enum Button : uint32_t {
   GAMEPAD_S       = 0x00000001,
   GAMEPAD_E       = 0x00000002,
   GAMEPAD_W       = 0x00000004,
   GAMEPAD_N       = 0x00000008,
   DPAD_UP         = 0x00000010,
   DPAD_DOWN       = 0x00000020,
   DPAD_LEFT       = 0x00000040,
   DPAD_RIGHT      = 0x00000080,
   START            = 0x00000100,
   SELECT           = 0x00000200,
   LEFT_BUMPER_1    = 0x00000400,
   LEFT_BUMPER_2    = 0x00000800,
   RIGHT_BUMPER_1   = 0x00001000,
   RIGHT_BUMPER_2   = 0x00002000,
   LEFT_THUMB       = 0x00004000,
   RIGHT_THUMB      = 0x00008000
};

enum class RawAxis : uint8_t { X, Y, Z, RX, RY, RZ, SLIDER_0, SLIDER_1, END };
enum class AxisTarget : uint8_t {
   UNUSED, LEFT_X, LEFT_Y, RIGHT_X, RIGHT_Y, LEFT_TRIGGER, RIGHT_TRIGGER, SPLIT_TRIGGER
};

struct AxisProfile {
   std::array<AxisTarget, size_t(RawAxis::END)> Targets = { };
};

struct RawDirectInputState {
   std::array<int32_t, size_t(RawAxis::END)> Axes = { };
   uint32_t Pov = 0xffffffffu;
   std::array<uint8_t, 128> Buttons = { };
};

struct ControllerState {
   std::array<double, 6> Axes = { };
   uint32_t Buttons = 0;
};

double normaliseSignedAxis(int32_t Value);
double normaliseUnsignedTrigger(uint8_t Value);
void applyStickTolerance(double &X, double &Y);
uint32_t mapXInputButtons(uint16_t Buttons);
ControllerState mapXInputState(uint8_t LeftTrigger, uint8_t RightTrigger, int16_t LeftX, int16_t LeftY,
   int16_t RightX, int16_t RightY, uint16_t Buttons);
AxisProfile buildDirectInputProfile(const std::array<bool, size_t(RawAxis::END)> &Present,
   bool LegacyFourAxisLayout = false);
bool isUsableDirectInputDevice(const std::array<bool, size_t(RawAxis::END)> &Present);
ControllerState mapDirectInputState(const RawDirectInputState &State, const AxisProfile &Profile);
uint32_t parseXInputDeviceID(std::string_view DeviceID);
uint32_t directInputProductID(uint32_t GuidData1);
std::array<std::string, MAX_PORTS> allocateDirectInputSlots(
   const std::array<std::string, MAX_PORTS> &Current, const std::vector<std::string> &Identities);
int totalPorts(const std::array<bool, MAX_PORTS> &Occupied);
int selectPrimary(const std::array<bool, MAX_PORTS> &Occupied, int CachedPort);

} // namespace display::controller

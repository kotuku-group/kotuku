#include "../win32/controller_mapping.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace display::controller;

static bool near(double A, double B)
{
   return std::abs(A - B) < 0.00001;
}

static void test_xinput_mapping()
{
   const auto state = mapXInputState(0, 255, -32768, 32767, 1000, -1000,
      0x0001|0x0008|0x0010|0x0020|0x0040|0x0080|0x0100|0x0200|0x1000|0x2000|0x4000|0x8000);
   assert(near(state.Axes[0], 0));
   assert(near(state.Axes[1], 1));
   assert(near(state.Axes[2], -1));
   assert(near(state.Axes[3], 1));
   assert(near(state.Axes[4], 0));
   assert(near(state.Axes[5], 0));
   assert(state.Buttons IS (DPAD_UP|DPAD_RIGHT|START|SELECT|LEFT_THUMB|RIGHT_THUMB|LEFT_BUMPER_1|
      RIGHT_BUMPER_1|GAMEPAD_S|GAMEPAD_E|GAMEPAD_W|GAMEPAD_N));

   double x = STICK_TOLERANCE - 0.0001;
   double y = -STICK_TOLERANCE + 0.0001;
   applyStickTolerance(x, y);
   assert(near(x, 0) and near(y, 0));
   x = STICK_TOLERANCE;
   y = 0;
   applyStickTolerance(x, y);
   assert(near(x, STICK_TOLERANCE));
   assert(near(normaliseSignedAxis(40000), 1));
   assert(near(normaliseSignedAxis(-40000), -1));
}

static AxisProfile standard_direct_profile()
{
   std::array<bool, size_t(RawAxis::END)> present = { };
   present[size_t(RawAxis::X)] = true;
   present[size_t(RawAxis::Y)] = true;
   present[size_t(RawAxis::RX)] = true;
   present[size_t(RawAxis::RY)] = true;
   present[size_t(RawAxis::SLIDER_0)] = true;
   present[size_t(RawAxis::SLIDER_1)] = true;
   return buildDirectInputProfile(present);
}

static void test_direct_input_device_filter()
{
   std::array<bool, size_t(RawAxis::END)> present = { };
   assert(not isUsableDirectInputDevice(present));
   present[size_t(RawAxis::X)] = true;
   assert(not isUsableDirectInputDevice(present));
   present[size_t(RawAxis::Y)] = true;
   assert(isUsableDirectInputDevice(present));
}

static void test_legacy_four_axis_profile()
{
   std::array<bool, size_t(RawAxis::END)> present = { };
   present[size_t(RawAxis::X)] = true;
   present[size_t(RawAxis::Y)] = true;
   present[size_t(RawAxis::Z)] = true;
   present[size_t(RawAxis::RX)] = true;
   present[size_t(RawAxis::RY)] = true;
   present[size_t(RawAxis::RZ)] = true;
   present[size_t(RawAxis::SLIDER_0)] = true;
   const auto profile = buildDirectInputProfile(present, true);
   assert(profile.Targets[size_t(RawAxis::Z)] IS AxisTarget::RIGHT_X);
   assert(profile.Targets[size_t(RawAxis::RZ)] IS AxisTarget::RIGHT_Y);
   assert(profile.Targets[size_t(RawAxis::RX)] != AxisTarget::RIGHT_X);
   assert(profile.Targets[size_t(RawAxis::RY)] != AxisTarget::RIGHT_Y);
   RawDirectInputState raw;
   raw.Axes[size_t(RawAxis::Z)] = 32767;
   raw.Axes[size_t(RawAxis::RZ)] = -32768;
   const auto state = mapDirectInputState(raw, profile);
   assert(near(state.Axes[4], 1));
   assert(near(state.Axes[5], 1));
}

static void test_direct_input_axes()
{
   const auto profile = standard_direct_profile();
   RawDirectInputState raw;
   raw.Axes[size_t(RawAxis::X)] = -32768;
   raw.Axes[size_t(RawAxis::Y)] = 32767;
   raw.Axes[size_t(RawAxis::RX)] = 32767;
   raw.Axes[size_t(RawAxis::RY)] = -32768;
   raw.Axes[size_t(RawAxis::SLIDER_0)] = -32768;
   raw.Axes[size_t(RawAxis::SLIDER_1)] = 32767;
   const auto state = mapDirectInputState(raw, profile);
   assert(near(state.Axes[0], 0));
   assert(near(state.Axes[1], 1));
   assert(near(state.Axes[2], -1));
   assert(near(state.Axes[3], -1));
   assert(near(state.Axes[4], 1));
   assert(near(state.Axes[5], 1));

   std::array<bool, size_t(RawAxis::END)> combined_present = { };
   combined_present[size_t(RawAxis::X)] = true;
   combined_present[size_t(RawAxis::Y)] = true;
   combined_present[size_t(RawAxis::Z)] = true;
   raw = { };
   raw.Axes[size_t(RawAxis::Z)] = -16384;
   auto combined = mapDirectInputState(raw, buildDirectInputProfile(combined_present));
   assert(near(combined.Axes[0], 16384.0 / 32767.0));
   assert(near(combined.Axes[1], 0));
   raw.Axes[size_t(RawAxis::Z)] = 16384;
   combined = mapDirectInputState(raw, buildDirectInputProfile(combined_present));
   assert(near(combined.Axes[0], 0));
   assert(near(combined.Axes[1], 16384.0 / 32767.0));
}

static void test_pov_and_buttons()
{
   const auto profile = AxisProfile();
   const std::array<uint32_t, 8> angles = { 0, 4500, 9000, 13500, 18000, 22500, 27000, 31500 };
   const std::array<uint32_t, 8> expected = {
      DPAD_UP, DPAD_UP|DPAD_RIGHT, DPAD_RIGHT, DPAD_RIGHT|DPAD_DOWN,
      DPAD_DOWN, DPAD_DOWN|DPAD_LEFT, DPAD_LEFT, DPAD_LEFT|DPAD_UP
   };
   for (size_t i=0; i < angles.size(); i++) {
      RawDirectInputState raw;
      raw.Pov = angles[i];
      assert(mapDirectInputState(raw, profile).Buttons IS expected[i]);
   }
   RawDirectInputState centred;
   assert(mapDirectInputState(centred, profile).Buttons IS 0);

   const std::array<uint32_t, 12> expected_buttons = {
      GAMEPAD_S, GAMEPAD_E, GAMEPAD_W, GAMEPAD_N, LEFT_BUMPER_1, RIGHT_BUMPER_1,
      LEFT_BUMPER_2, RIGHT_BUMPER_2, SELECT, START, LEFT_THUMB, RIGHT_THUMB
   };
   RawDirectInputState raw;
   raw.Pov = 0xffffffffu;
   for (size_t i=0; i < expected_buttons.size(); i++) raw.Buttons[i] = 0x80;
   raw.Buttons[20] = 0x80;
   uint32_t all_buttons = 0;
   for (const auto button : expected_buttons) all_buttons |= button;
   assert(mapDirectInputState(raw, profile).Buttons IS all_buttons);
}

static void test_duplicate_classification()
{
   assert(parseXInputDeviceID("HID\\VID_045E&PID_028E&IG_00") IS 0x028e045eu);
   assert(parseXInputDeviceID("hid\\vid_1234&pid_abcd&ig_01") IS 0xabcd1234u);
   assert(parseXInputDeviceID("HID\\VID_045E&PID_028E") IS 0xffffffffu);
   assert(parseXInputDeviceID("HID\\VID_BAD&PID_028E&IG_00") IS 0xffffffffu);
   assert(directInputProductID(0x028e045eu) IS 0x028e045eu);
}

static void test_slots_and_primary()
{
   std::array<std::string, MAX_PORTS> slots = { };
   slots[4] = "alpha";
   slots[5] = "bravo";
   auto assigned = allocateDirectInputSlots(slots, { "bravo", "alpha", "charlie" });
   assert(assigned[4] IS "alpha");
   assert(assigned[5] IS "bravo");
   assert(assigned[6] IS "charlie");

   assigned = allocateDirectInputSlots(assigned, { "bravo", "charlie" });
   assert(assigned[4].empty());
   assert(assigned[5] IS "bravo");
   assert(assigned[6] IS "charlie");
   assigned = allocateDirectInputSlots(assigned, { "bravo", "charlie", "delta" });
   assert(assigned[4] IS "delta");

   std::vector<std::string> capacity;
   for (int i=0; i < MAX_PORTS; i++) capacity.push_back("device-" + std::to_string(i));
   assigned = allocateDirectInputSlots({ }, capacity);
   for (int port=XINPUT_PORTS; port < MAX_PORTS; port++) assert(not assigned[port].empty());

   std::array<bool, MAX_PORTS> occupied = { };
   assert(totalPorts(occupied) IS 0);
   occupied[4] = true;
   assert(totalPorts(occupied) IS 5);
   assert(selectPrimary(occupied, -1) IS 4);
   occupied[1] = true;
   assert(selectPrimary(occupied, 4) IS 4);
   assert(selectPrimary(occupied, -1) IS 1);
   occupied[4] = false;
   assert(selectPrimary(occupied, 4) IS 1);
}

int main()
{
   test_xinput_mapping();
   test_direct_input_device_filter();
   test_legacy_four_axis_profile();
   test_direct_input_axes();
   test_pov_and_buttons();
   test_duplicate_classification();
   test_slots_and_primary();
   std::cout << "Windows controller policy tests passed.\n";
   return 0;
}

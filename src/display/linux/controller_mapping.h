#pragma once

#include <array>
#include <cstdint>
#include <linux/input-event-codes.h>

#ifndef IS
#define IS ==
#endif

namespace display::linux_controller {

constexpr double STICK_TOLERANCE = 0.08;

enum class AxisTarget : uint8_t {
   UNUSED, LEFT_X, LEFT_Y, RIGHT_X, RIGHT_Y, LEFT_TRIGGER, RIGHT_TRIGGER
};

using AxisMap = std::array<uint8_t, ABS_CNT>;
using AxisProfile = std::array<AxisTarget, ABS_CNT>;

AxisProfile buildAxisProfile(const AxisMap &Map, uint8_t Axes);
double normaliseAxis(int16_t Value);
double normaliseTrigger(int16_t Value);
void mapAxis(std::array<double, 6> &Values, AxisTarget Target, int16_t Value);
void applyStickTolerance(double &X, double &Y);

} // namespace display::linux_controller

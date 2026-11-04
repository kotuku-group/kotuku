#include "controller_mapping.h"

#include <algorithm>

namespace display::linux_controller {

//********************************************************************************************************************
// Builds a common gamepad profile from the controls reported by the Linux joystick API.

AxisProfile buildAxisProfile(const AxisMap &Map, uint8_t Axes)
{
   AxisProfile result = { };
   result.fill(AxisTarget::UNUSED);

   const auto axis_count = std::min<size_t>(Axes, Map.size());
   const auto has_axis = [&](uint8_t Axis) {
      return std::find(Map.begin(), Map.begin() + axis_count, Axis) != Map.begin() + axis_count;
   };
   const bool standard_sticks = has_axis(ABS_RX) and has_axis(ABS_RY);
   const bool android_triggers = has_axis(ABS_BRAKE) and has_axis(ABS_GAS);

   for (size_t i=0; i < axis_count; i++) {
      switch (Map[i]) {
         case ABS_X: result[i] = AxisTarget::LEFT_X; break;
         case ABS_Y: result[i] = AxisTarget::LEFT_Y; break;
         case ABS_RX:
            if (standard_sticks) result[i] = AxisTarget::RIGHT_X;
            break;
         case ABS_RY:
            if (standard_sticks) result[i] = AxisTarget::RIGHT_Y;
            break;
         case ABS_Z:
            if ((not standard_sticks) and android_triggers) result[i] = AxisTarget::RIGHT_X;
            else if (not android_triggers) result[i] = AxisTarget::LEFT_TRIGGER;
            break;
         case ABS_RZ:
            if ((not standard_sticks) and android_triggers) result[i] = AxisTarget::RIGHT_Y;
            else if (not android_triggers) result[i] = AxisTarget::RIGHT_TRIGGER;
            break;
         case ABS_BRAKE: result[i] = AxisTarget::LEFT_TRIGGER; break;
         case ABS_GAS: result[i] = AxisTarget::RIGHT_TRIGGER; break;
         default: break;
      }
   }

   return result;
}

//********************************************************************************************************************
// Converts a signed Linux joystick axis value to Kōtuku's normalised [-1, 1] axis range.

double normaliseAxis(int16_t Value)
{
   return std::clamp(double(Value) * (1.0 / 32767.0), -1.0, 1.0);
}

// Converts a signed Linux trigger axis value to Kōtuku's normalised [0, 1] trigger range.

double normaliseTrigger(int16_t Value)
{
   return std::clamp((normaliseAxis(Value) + 1.0) * 0.5, 0.0, 1.0);
}

// Maps one Linux joystick axis value to Kōtuku's common controller state.

void mapAxis(std::array<double, 6> &Values, AxisTarget Target, int16_t Value)
{
   switch (Target) {
      case AxisTarget::LEFT_X:        Values[2] = normaliseAxis(Value); break;
      case AxisTarget::LEFT_Y:        Values[3] = -normaliseAxis(Value); break;
      case AxisTarget::RIGHT_X:       Values[4] = normaliseAxis(Value); break;
      case AxisTarget::RIGHT_Y:       Values[5] = -normaliseAxis(Value); break;
      case AxisTarget::LEFT_TRIGGER:  Values[0] = normaliseTrigger(Value); break;
      case AxisTarget::RIGHT_TRIGGER: Values[1] = normaliseTrigger(Value); break;
      case AxisTarget::UNUSED: break;
   }
}

// Applies the shared dead zone to a two-axis thumb stick.

void applyStickTolerance(double &X, double &Y)
{
   if ((X < STICK_TOLERANCE) and (X > -STICK_TOLERANCE) and
       (Y < STICK_TOLERANCE) and (Y > -STICK_TOLERANCE)) {
      X = 0;
      Y = 0;
   }
}

} // namespace display::linux_controller

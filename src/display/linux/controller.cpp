/*********************************************************************************************************************

Linux joystick backend for the Controller class.

*********************************************************************************************************************/

#include "../defs.h"

#ifdef __linux__

#include <algorithm>
#include <cstring>
#include <linux/joystick.h>
#include <string>

namespace {

constexpr int MAX_CONTROLLER_PORTS = 32;
constexpr double STICK_TOLERANCE = 0.08;

struct LinuxController {
   int fd = -1;
   uint8_t axes = 0;
   uint8_t buttons = 0;
   bool axis_map_valid = false;
   bool button_map_valid = false;
   std::array<uint8_t, ABS_CNT> axis_map = { };
   std::array<uint16_t, KEY_MAX - BTN_MISC + 1> button_map = { };
   std::array<double, 6> values = { };
   CON button_state = CON::NIL;

   ~LinuxController()
   {
      if (fd >= 0) close(fd);
   }

   LinuxController() = default;
   LinuxController(const LinuxController &) = delete;
   LinuxController &operator=(const LinuxController &) = delete;
};

std::array<LinuxController, MAX_CONTROLLER_PORTS> glLinuxControllers;
std::mutex glControllerLock;

/*********************************************************************************************************************/

double normalise_axis(int16_t Value)
{
   return std::clamp(double(Value) * (1.0 / 32767.0), -1.0, 1.0);
}

/*********************************************************************************************************************/

double normalise_trigger(int16_t Value)
{
   return std::clamp((normalise_axis(Value) + 1.0) * 0.5, 0.0, 1.0);
}

/*********************************************************************************************************************/

void apply_stick_tolerance(double &X, double &Y)
{
   if ((X < STICK_TOLERANCE) and (X > -STICK_TOLERANCE) and
       (Y < STICK_TOLERANCE) and (Y > -STICK_TOLERANCE)) {
      X = 0;
      Y = 0;
   }
}

/*********************************************************************************************************************/

void set_button(CON &Buttons, CON Flag, bool Pressed)
{
   if (Pressed) Buttons |= Flag;
   else Buttons = CON(uint32_t(Buttons) & ~uint32_t(Flag));
}

/*********************************************************************************************************************/

CON fallback_button(uint8_t Number)
{
   switch (Number) {
      case 0:  return CON::GAMEPAD_S;
      case 1:  return CON::GAMEPAD_E;
      case 2:  return CON::GAMEPAD_W;
      case 3:  return CON::GAMEPAD_N;
      case 4:  return CON::LEFT_BUMPER_1;
      case 5:  return CON::RIGHT_BUMPER_1;
      case 6:  return CON::SELECT;
      case 7:  return CON::START;
      case 9:  return CON::LEFT_THUMB;
      case 10: return CON::RIGHT_THUMB;
      default: return CON::NIL;
   }
}

/*********************************************************************************************************************/

CON map_button(const LinuxController &Controller, uint8_t Number)
{
   if ((Controller.button_map_valid) and (Number < Controller.buttons)) {
      switch (Controller.button_map[Number]) {
         case BTN_SOUTH:  return CON::GAMEPAD_S;
         case BTN_EAST:   return CON::GAMEPAD_E;
         case BTN_WEST:   return CON::GAMEPAD_W;
         case BTN_NORTH:  return CON::GAMEPAD_N;
         case BTN_SELECT: return CON::SELECT;
         case BTN_START:  return CON::START;
         case BTN_TL:     return CON::LEFT_BUMPER_1;
         case BTN_TR:     return CON::RIGHT_BUMPER_1;
         case BTN_TL2:    return CON::LEFT_BUMPER_2;
         case BTN_TR2:    return CON::RIGHT_BUMPER_2;
         case BTN_THUMBL: return CON::LEFT_THUMB;
         case BTN_THUMBR: return CON::RIGHT_THUMB;
         case BTN_DPAD_UP:    return CON::DPAD_UP;
         case BTN_DPAD_DOWN:  return CON::DPAD_DOWN;
         case BTN_DPAD_LEFT:  return CON::DPAD_LEFT;
         case BTN_DPAD_RIGHT: return CON::DPAD_RIGHT;
         default: return CON::NIL;
      }
   }

   return fallback_button(Number);
}

/*********************************************************************************************************************/

void fallback_axis(LinuxController &Controller, uint8_t Number, int16_t Value)
{
   switch (Number) {
      case 0: Controller.values[2] = normalise_axis(Value); break;
      case 1: Controller.values[3] = -normalise_axis(Value); break;
      case 2: Controller.values[0] = normalise_trigger(Value); break;
      case 3: Controller.values[4] = normalise_axis(Value); break;
      case 4: Controller.values[5] = -normalise_axis(Value); break;
      case 5: Controller.values[1] = normalise_trigger(Value); break;
      case 6:
         set_button(Controller.button_state, CON::DPAD_LEFT, Value < 0);
         set_button(Controller.button_state, CON::DPAD_RIGHT, Value > 0);
         break;
      case 7:
         set_button(Controller.button_state, CON::DPAD_UP, Value < 0);
         set_button(Controller.button_state, CON::DPAD_DOWN, Value > 0);
         break;
   }
}

/*********************************************************************************************************************/

void handle_axis(LinuxController &Controller, uint8_t Number, int16_t Value)
{
   if ((not Controller.axis_map_valid) or (Number >= Controller.axes)) {
      fallback_axis(Controller, Number, Value);
      return;
   }

   switch (Controller.axis_map[Number]) {
      case ABS_X:  Controller.values[2] = normalise_axis(Value); break;
      case ABS_Y:  Controller.values[3] = -normalise_axis(Value); break;
      case ABS_Z:  Controller.values[0] = normalise_trigger(Value); break;
      case ABS_RX: Controller.values[4] = normalise_axis(Value); break;
      case ABS_RY: Controller.values[5] = -normalise_axis(Value); break;
      case ABS_RZ: Controller.values[1] = normalise_trigger(Value); break;
      case ABS_HAT0X:
         set_button(Controller.button_state, CON::DPAD_LEFT, Value < 0);
         set_button(Controller.button_state, CON::DPAD_RIGHT, Value > 0);
         break;
      case ABS_HAT0Y:
         set_button(Controller.button_state, CON::DPAD_UP, Value < 0);
         set_button(Controller.button_state, CON::DPAD_DOWN, Value > 0);
         break;
      default:
         break;
   }
}

/*********************************************************************************************************************/

void close_controller(LinuxController &Controller)
{
   if (Controller.fd >= 0) {
      close(Controller.fd);
      Controller.fd = -1;
   }

   Controller.axes = 0;
   Controller.buttons = 0;
   Controller.axis_map_valid = false;
   Controller.button_map_valid = false;
   Controller.values.fill(0);
   Controller.button_state = CON::NIL;
}

/*********************************************************************************************************************/

ERR open_controller(int Port)
{
   if ((Port < 0) or (Port >= MAX_CONTROLLER_PORTS)) return ERR::Search;

   auto &controller = glLinuxControllers[Port];
   if (controller.fd >= 0) return ERR::Okay;

   const auto path = std::string("/dev/input/js") + std::to_string(Port);
   const auto fd = open(path.c_str(), O_RDONLY|O_NONBLOCK|O_CLOEXEC);
   if (fd < 0) {
      if ((errno IS ENOENT) or (errno IS ENODEV)) return ERR::Search;
      if (errno IS EACCES) return ERR::NoPermission;
      return ERR::SystemCall;
   }

   controller.fd = fd;
   controller.axes = 0;
   controller.buttons = 0;
   controller.values.fill(0);
   controller.button_state = CON::NIL;

   ioctl(controller.fd, JSIOCGAXES, &controller.axes);
   ioctl(controller.fd, JSIOCGBUTTONS, &controller.buttons);

   controller.axis_map.fill(0);
   controller.axis_map_valid = ioctl(controller.fd, JSIOCGAXMAP, controller.axis_map.data()) >= 0;

   controller.button_map.fill(0);
   controller.button_map_valid = ioctl(controller.fd, JSIOCGBTNMAP, controller.button_map.data()) >= 0;

   if (Port > glLastPort) glLastPort = Port;
   return ERR::Okay;
}

/*********************************************************************************************************************/

ERR read_controller(LinuxController &Controller)
{
   for (;;) {
      js_event event;
      const auto bytes = read(Controller.fd, &event, sizeof(event));
      if (bytes IS ssize_t(sizeof(event))) {
         const auto type = event.type & ~JS_EVENT_INIT;
         if (type IS JS_EVENT_AXIS) handle_axis(Controller, event.number, event.value);
         else if (type IS JS_EVENT_BUTTON) {
            const auto flag = map_button(Controller, event.number);
            if (flag != CON::NIL) set_button(Controller.button_state, flag, event.value);
         }
      }
      else if (bytes < 0) {
         if ((errno IS EAGAIN) or (errno IS EWOULDBLOCK)) break;
         if (errno IS EINTR) continue;
         if (errno IS ENODEV) {
            close_controller(Controller);
            return ERR::Search;
         }
         return ERR::SystemCall;
      }
      else if (bytes IS 0) {
         break;
      }
      else return ERR::SystemCall;
   }

   apply_stick_tolerance(Controller.values[2], Controller.values[3]);
   apply_stick_tolerance(Controller.values[4], Controller.values[5]);
   return ERR::Okay;
}

} // namespace

/*********************************************************************************************************************/

ERR linuxReadController(int Port, double *Values, CON &Buttons)
{
   std::lock_guard lock(glControllerLock);

   if (auto error = open_controller(Port); error != ERR::Okay) return error;

   auto &controller = glLinuxControllers[Port];
   if (auto error = read_controller(controller); error != ERR::Okay) return error;

   std::memcpy(Values, controller.values.data(), sizeof(double) * controller.values.size());
   Buttons = controller.button_state;
   return ERR::Okay;
}

/*********************************************************************************************************************/

ERR linuxGetControllerPorts(int &Value)
{
   std::lock_guard lock(glControllerLock);

   int last_port = -1;
   for (int port=0; port < MAX_CONTROLLER_PORTS; port++) {
      auto &controller = glLinuxControllers[port];

      if (controller.fd >= 0) {
         if (read_controller(controller) IS ERR::Okay) last_port = port;
      }
      else if (open_controller(port) IS ERR::Okay) last_port = port;
   }

   Value = last_port + 1;
   return ERR::Okay;
}

#endif

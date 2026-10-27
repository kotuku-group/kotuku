#pragma once

#include "../driver/display_driver.h"

enum class DropCase {
   Copy, RejectHover, MoveOnly, SelectedMove, Unsupported, ReadFailure, Text, Legacy, NoAction, Leave, RejectData,
   Shift, Alt, Control, CloseOnHover, ShiftFocusLost, AltFocusLost
};

extern "C" int wayland_drop_test(const DriverCallbacks *Callbacks, DropCase Case);

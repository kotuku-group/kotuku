#pragma once

#include "../driver/display_driver.h"

enum class DropCase {
   Copy, RejectHover, MoveOnly, SelectedMove, Unsupported, ReadFailure, Text, Legacy, NoAction, Leave, RejectData,
   Shift, Alt, Control, CloseOnHover, ShiftFocusLost, AltFocusLost
};

extern "C" int wayland_drop_test(const DriverCallbacks *Callbacks, DropCase Case);

enum class ExportCase {
   ForeignCopy, ForeignRefused, ForeignNoAction, NoTarget, StartRefused, ReturnToOwn, StaleOwnOffer, ForeignKotuku,
   TimeoutServed, TimeoutUnread, Cancel, CancelGranted, ReleaseElsewhere, SourceEnded, Unavailable
};

// Drives the outbound drag export with its own callbacks.  Failures are described on stderr.

extern "C" int wayland_export_test(ExportCase Case);

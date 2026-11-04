#include <kotuku/modules/display.h>
#include "wayland_drop_fixture.h"
#include <cstdio>
#include <cstring>

static DropCase glCase;
static int glDrops = 0;
static int glFailures = 0;
static int glClipboardChanges = 0;
static int glSnapshotsCreated = 0;
static int glSnapshotsReleased = 0;

static ERR register_fd(HOSTHANDLE, RFD, void (*)(HOSTHANDLE, APTR), APTR) { return ERR::Okay; }
static void crossing(OBJECTID, bool, double, double) { }
static void focus_state(OBJECTID, bool) { }
static void leave() { }
static void clipboard(CSTRING, CSTRING, size_t, bool, OBJECTID) { glClipboardChanges++; }

static bool over(OBJECTID SurfaceID, double X, double Y, CSTRING Datatypes, size_t Length)
{
   if ((SurfaceID != 42) or (X != 25.5) or (Y != 40.25) or (Length != 1)) glFailures++;
   if (Datatypes[0] != char(glCase IS DropCase::Text ? DATA::TEXT : DATA::FILE)) glFailures++;
   return glCase != DropCase::RejectHover;
}

static bool start(NativeDrop &Drop)
{
   if ((Drop.SurfaceID != 42) or (Drop.X != 25.5) or (Drop.Y != 40.25) or (not Drop.Session)) glFailures++;
   Drop.TargetID = 43;
   Drop.DisplayID = 44;
   Drop.Snapshot = std::shared_ptr<NativeDropSnapshot>(new NativeDropSnapshot, [](NativeDropSnapshot *Snapshot) {
      glSnapshotsReleased++;
      delete Snapshot;
   });
   Drop.Snapshot->ID = 99;
   glSnapshotsCreated++;
   return glCase != DropCase::RejectHover;
}

static bool data(const NativeDrop &Drop, CSTRING Mime, CSTRING Data, size_t Length)
{
   glDrops++;
   if (not Drop.Snapshot or (Drop.Snapshot->ID != 99)) glFailures++;
   if ((Drop.SurfaceID != 42) or (Drop.TargetID != 43) or (Drop.DisplayID != 44) or
         (Drop.X != 25.5) or (Drop.Y != 40.25)) glFailures++;
   const auto expected = glCase IS DropCase::Text ? "Wayland <&> 三😀" :
      "file:///tmp/two%20words%20%26%20%E4%B8%89.txt\r\n";
   if (std::string_view(Data, Length) != expected) glFailures++;
   if (std::string_view(Mime) != (glCase IS DropCase::Text ? "text/plain;charset=utf-8" : "text/uri-list"))
      glFailures++;
   return glCase != DropCase::RejectData;
}

int main()
{
   struct CoreBase core = {};
   core._RegisterFD = register_fd;
   auto driver = create_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION, &core);
   if (not driver) return 1;
   DriverCallbacks callbacks = {
      .Version = DISPLAY_DRIVER_INTERFACE_VERSION,
      .Crossing = crossing,
      .FocusState = focus_state,
      .ClipboardData = clipboard,
      .DragOver = over,
      .DragStart = start,
      .DragData = data,
      .DragLeave = leave
   };
   for (auto test_case : { DropCase::Copy, DropCase::RejectHover, DropCase::MoveOnly, DropCase::SelectedMove,
         DropCase::Unsupported, DropCase::ReadFailure, DropCase::Text, DropCase::Legacy, DropCase::NoAction,
         DropCase::Leave, DropCase::RejectData, DropCase::Shift, DropCase::Alt, DropCase::Control,
         DropCase::CloseOnHover, DropCase::ShiftFocusLost, DropCase::AltFocusLost }) {
      glCase = test_case;
      glDrops = 0;
      glSnapshotsCreated = glSnapshotsReleased = 0;
      const auto before = glFailures;
      glFailures += wayland_drop_test(&callbacks, test_case);
      const bool expected_drop = (test_case IS DropCase::Copy) or (test_case IS DropCase::Text) or
         (test_case IS DropCase::Legacy) or (test_case IS DropCase::RejectData) or (test_case IS DropCase::Control) or
         (test_case IS DropCase::ShiftFocusLost) or (test_case IS DropCase::AltFocusLost);
      if (glDrops != int(expected_drop)) glFailures++;
      if (glSnapshotsCreated != glSnapshotsReleased) glFailures++;
      if (glFailures != before) std::fprintf(stderr, "Wayland drop case %d failed\n", int(test_case));
   }
   destroy_display_driver(driver);
   if (glClipboardChanges) glFailures++;
   return glFailures ? 1 : 0;
}

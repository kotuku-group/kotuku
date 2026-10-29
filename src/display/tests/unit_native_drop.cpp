// Included by display.cpp so that the test exercises the actual driver callbacks with a headless display.
#include <kotuku/strings.hpp>
#include <kotuku/modules/vector.h>

namespace {

// Keep only the header alive so that a leaked participant subscription is observable after snapshot destruction.
class NativeSnapshotProbe {
   OBJECTPTR snapshot = nullptr;

public:
   NativeSnapshotProbe() = default;

   explicit NativeSnapshotProbe(const NativeDrop &Drop) {
      if (Drop.Snapshot) watch(GetObjectPtr(Drop.Snapshot->ID));
   }

   ~NativeSnapshotProbe() { if (snapshot) snapshot->unpinWeak(); }

   NativeSnapshotProbe(const NativeSnapshotProbe &) = delete;
   NativeSnapshotProbe & operator=(const NativeSnapshotProbe &) = delete;

   void watch(OBJECTPTR Snapshot) {
      if (snapshot) snapshot->unpinWeak();
      snapshot = Snapshot;
      if (snapshot) snapshot->pinWeak();
   }

   bool released() const {
      return snapshot and snapshot->terminating() and (snapshot->RefCount.load() IS Object::WEAK_PINS);
   }
};

static ERR native_prepare_reject(OBJECTPTR, objConfig *Snapshot, double, double, APTR Meta)
{
   ((NativeSnapshotProbe *)Meta)->watch(Snapshot);
   return ERR::Cancelled;
}

struct NativeDropObservation {
   OBJECTPTR Sink = nullptr;
   int Calls = 0;
   double X = 0;
   double Y = 0;
   bool Reject = false;
   OBJECTID MoveSurface = 0;
   const NativeDrop *Reenter = nullptr;
   bool NestedRejected = false;
   std::string Receipt;
};

static ERR native_sink_new(OBJECTPTR Self, APTR)
{
   new (Self) Object(Self->Class, Self->UID);
   return ERR::Okay;
}

static ERR native_sink_init(OBJECTPTR, APTR) { return ERR::Okay; }

static void native_test_movement(OBJECTID SurfaceID, double X, double Y)
{
   int abs_x = 0, abs_y = 0;
   gfx::GetSurfaceCoords(SurfaceID, nullptr, nullptr, &abs_x, &abs_y, nullptr, nullptr);
   display::DriverMovement(SurfaceID, X + abs_x, Y + abs_y, false);
}

static ERR native_sink_feed(OBJECTPTR Self, struct acDataFeed *Args)
{
   if (Args->Datatype != DATA::RECEIPT) return ERR::NoSupport;
   auto state = (NativeDropObservation *)Self->CreatorMeta;
   state->Receipt.assign((const char *)Args->Buffer.data(), Args->Buffer.size());
   return ERR::Okay;
}

static void native_drop_notify(OBJECTPTR, ACTIONID, ERR, struct acDragDrop *Args, APTR Meta)
{
   auto state = (NativeDropObservation *)Meta;
   state->Calls++;
   if (auto pointer = gfx::AccessPointer()) {
      state->X = pointer->OverX;
      state->Y = pointer->OverY;
      if (state->Reject) pointer->DragFeedback = 0;
      ReleaseObject(pointer);
   }
   if (state->Reenter) {
      state->NestedRejected = not display::DriverDragData(*state->Reenter, "text/plain", "nested", 6);
   }
   respond_wayland_drop_request(Args->Source, state->Sink, Args->Item, Args->Datatype[0]);
   if (state->MoveSurface) native_test_movement(state->MoveSurface, 110, 120);
}

static void native_drop_tests(std::string_view Options, int &Passed, int &Total)
{
   kt::Log log("NativeDropTests");
   auto check = [&](bool Condition, CSTRING Message) {
      Total++;
      if (Condition) Passed++;
      else log.warning("%s", Message);
   };
   if (not glHeadless) return; // These tests must not move the user's desktop cursor or change its clipboard.

   // Flute supplies the scene/widget and mutates it on Signal between release and payload arrival.
   OBJECTID fixture_id = 0;
   double fixture_x = 0, fixture_y = 0;
   const std::string options(Options);
   if (std::sscanf(options.c_str(), "native-drop:%d:%lf:%lf", &fixture_id, &fixture_x, &fixture_y) IS 3) {
      kt::ScopedObjectLock fixture(fixture_id);
      if (not fixture.granted()) return;
      const char datatype = char(DATA::FILE);
      NativeDrop drop;
      drop.SurfaceID = fixture_id;
      drop.X = fixture_x;
      drop.Y = fixture_y;
      check(display::DriverDragOver(fixture_id, drop.X, drop.Y, &datatype, 1), "Fixture accepts hover");
      const bool released = display::DriverDragStart(drop);
      check(released IS (not Options.ends_with(":reject")), "Fixture reports the expected release decision");
      const auto snapshot_id = drop.Snapshot ? drop.Snapshot->ID : 0;
      display::DriverDragLeave();
      acSignal(*fixture);
      const char payload[] = "file:///tmp/native-drop.txt\r\n";
      if (Options.ends_with(":fail")) drop.Snapshot.reset();
      else display::DriverDragData(drop, "text/uri-list", payload, sizeof(payload) - 1);
      check(CheckResourceExists(snapshot_id) != ERR::True, "Fixture releases its snapshot on delivery or refusal");
      check(not display::DriverDragData(drop, "text/uri-list", payload, sizeof(payload) - 1),
         "Fixture refuses duplicate delivery");
      return;
   }

   ActionArray actions[] = {
      { AC::New, native_sink_new }, { AC::Init, native_sink_init }, { AC::DataFeed, native_sink_feed },
      { AC::NIL, nullptr }
   };
   FieldArray fields[] = { END_FIELD };
   objMetaClass::create sink_class = {
      fl::Name("NativeDropTestSink"), fl::BaseClassID(CLASSID(strihash("NativeDropTestSink"))),
      fl::Size(sizeof(Object)), fl::Actions(actions), fl::Fields(fields)
   };
   check(sink_class.ok(), "Create receipt sink class");
   if (not sink_class.ok()) return;
   OBJECTPTR sink = nullptr;
   if (NewObject(sink_class->ClassID, &sink) != ERR::Okay) { check(false, "Create receipt sink"); return; }
   auto free_sink = kt::Defer([&]() { FreeResource(sink); });
   if (InitObject(sink) != ERR::Okay) { check(false, "Initialise receipt sink"); return; }
   NativeDropObservation state;
   state.Sink = sink;
   sink->CreatorMeta = &state;

   objSurface::create first = { fl::X(20), fl::Y(30), fl::Width(200), fl::Height(200) };
   objSurface::create second = { fl::X(300), fl::Y(0), fl::Width(200), fl::Height(200) };
   check(first.ok() and second.ok(), "Create native destinations");
   if (not first.ok() or not second.ok()) return;
   acShow(*first);
   acShow(*second);
   const auto saved_modal = gfx::GetModalSurface();
   auto restore_modal = kt::Defer([&]() { gfx::SetModalSurface(saved_modal); display::DriverDragLeave(); });
   gfx::SetModalSurface(0);
   auto callback = C_FUNCTION(native_drop_notify, &state);
   SubscribeAction(*first, AC::DragDrop, callback);
   auto clipboard_snapshot = []() {
      const std::lock_guard lock(glClipboardLock);
      std::string result;
      for (auto &clip : glClips) {
         result += std::to_string(int(clip.Datatype)) + ':' + std::to_string(int(clip.Flags)) + ':';
         for (auto &item : clip.Items) result += item.Path + '\n';
      }
      return result;
   };
   const auto saved_clipboard = clipboard_snapshot();
   auto snapshot_drop = [&](NativeDrop &Drop) {
      const char type = char(DATA::FILE);
      Drop.SurfaceID = first->UID;
      Drop.X = 25.5;
      Drop.Y = 40.25;
      check(display::DriverDragOver(first->UID, Drop.X, Drop.Y, &type, 1), "Accept a fresh offer");
      check(display::DriverDragStart(Drop), "Prepare a fresh single-use destination");
      display::DriverDragLeave();
   };
   const char datatype = char(DATA::FILE);
   check(display::DriverDragOver(first->UID, 25.5, 40.25, &datatype, 1), "Native offer feedback is accepted");
   NativeDrop drop;
   drop.SurfaceID = first->UID;
   drop.X = 25.5;
   drop.Y = 40.25;
   native_test_movement(second->UID, 75, 90);
   check(display::DriverDragStart(drop), "Snapshot an accepted drop");
   display::DriverDragLeave();
   native_test_movement(second->UID, 75, 90);
   const auto payload = "file:///tmp/two%20words%20%26%20%E4%B8%89%22.txt\r\n";
   state.Reenter = &drop;
   check(display::DriverDragData(drop, "text/uri-list", payload, std::strlen(payload)), "Deliver delayed drop");
   state.Reenter = nullptr;
   check(state.NestedRejected, "Nested delivery cannot consume or dispatch the same pending snapshot");
   check((state.Calls IS 1) and (state.X IS 25.5) and (state.Y IS 40.25),
      "Delayed delivery uses the original surface and fractional position");
   check(state.Receipt.find("two words &amp; 三&quot;.txt") != std::string::npos,
      "URI decoding and XML escaping preserve the exact file path");
   check(not display::DriverDragData(drop, "text/uri-list", payload, std::strlen(payload)),
      "A completed snapshot cannot be delivered twice");
   for (int i = 0; i < 8; i++) {
      snapshot_drop(drop);
      NativeSnapshotProbe snapshot_probe(drop);
      check(display::DriverDragData(drop, "text/uri-list", payload, std::strlen(payload)), "Deliver a repeated drop");
      check(snapshot_probe.released(), "Repeated drops release participant subscriptions before window destruction");
   }
   {
      NativeSnapshotProbe rejected_probe;
      auto prepare = C_FUNCTION(native_prepare_reject, &rejected_probe);
      gfx::WindowHook(first->UID, WH::DROP_SNAPSHOT, &prepare);
      auto remove_hook = kt::Defer([&]() { gfx::WindowHook(first->UID, WH::DROP_SNAPSHOT, nullptr); });
      check(display::DriverDragOver(first->UID, 25, 40, &datatype, 1), "Accept an offer before preparation rejection");
      drop.SurfaceID = first->UID;
      drop.X = 25;
      drop.Y = 40;
      check(not display::DriverDragStart(drop), "Reject native destination preparation");
      display::DriverDragLeave();
      check(rejected_probe.released(), "Rejected preparation releases participant subscription pins");
   }
   if (FindClass(CLASSID::VECTORSCENE)) {
      objSurface::create surface = { fl::X(600), fl::Width(100), fl::Height(100) };
      check(surface.ok(), "Create a vector destination surface");
      if (not surface.ok()) return;
      objVectorScene::create scene = { fl::Surface(surface->UID), fl::Flags(VPF::DROP_TARGETS) };
      check(scene.ok(), "Create a native drop scene");
      if (not scene.ok()) return;
      objVectorViewport::create viewport = {
         fl::Owner(scene->UID), fl::Width(100), fl::Height(100)
      };
      check(viewport.ok(), "Create a native drop viewport");
      if (not viewport.ok()) return;
      check(viewport->setDropTarget(1) IS ERR::Okay, "Register the viewport as a drop target");
      acShow(*surface);
      acDraw(*surface);
      auto prepare_drop = [&](NativeDrop &Pending) {
         Pending.SurfaceID = surface->UID;
         Pending.X = Pending.Y = 20;
         check(display::DriverDragOver(surface->UID, 20, 20, &datatype, 1), "Accept a vector offer");
         check(display::DriverDragStart(Pending), "Prepare a vector destination");
         display::DriverDragLeave();
      };
      NativeDrop pending;
      for (int i = 0; i < 8; i++) {
         prepare_drop(pending);
         NativeSnapshotProbe snapshot_probe(pending);
         check(display::DriverDragData(pending, "text/plain", "vector", 6), "Deliver a repeated vector drop");
         check(snapshot_probe.released(), "Repeated vector drops release scene and viewport subscription pins");
      }
      prepare_drop(pending);
      NativeSnapshotProbe failed_probe(pending);
      pending.Snapshot.reset();
      check(failed_probe.released(), "Failed vector transfers release every participant subscription pin");
      prepare_drop(pending);
      NativeSnapshotProbe cancelled_probe(pending);
      if (auto pointer = gfx::AccessPointer()) {
         acDeactivate(pointer);
         ReleaseObject(pointer);
      }
      check(cancelled_probe.released(), "Cancelled vector drops release every participant subscription pin");
      {
         NativeSnapshotProbe rejected_probe;
         viewport->setDropPrepare(C_FUNCTION(native_prepare_reject, &rejected_probe));
         check(display::DriverDragOver(surface->UID, 20, 20, &datatype, 1),
            "Accept an offer before viewport rejection");
         pending.X = pending.Y = 20;
         check(not display::DriverDragStart(pending), "Reject viewport preparation");
         display::DriverDragLeave();
         check(rejected_probe.released(), "Rejected viewport preparation releases every participant subscription pin");
         viewport->setDropPrepare(FUNCTION{});
      }
      prepare_drop(pending);
      NativeSnapshotProbe destroyed_scene_probe(pending);
      FreeResource(scene.detach());
      check(destroyed_scene_probe.released(), "Scene destruction releases every surviving participant subscription");
   }
   snapshot_drop(drop);
   const auto abandoned_id = drop.Snapshot->ID;
   NativeSnapshotProbe abandoned_probe(drop);
   auto transfer_copy = drop;
   drop.Snapshot.reset();
   check(CheckResourceExists(abandoned_id) IS ERR::True, "A queued transfer owns the pending snapshot");
   transfer_copy.Snapshot.reset();
   check(CheckResourceExists(abandoned_id) != ERR::True, "Failed transfers release their last snapshot reference");
   check(abandoned_probe.released(), "Failed transfers release participant subscription pins");
   snapshot_drop(drop);
   const auto cancelled_id = drop.Snapshot->ID;
   NativeSnapshotProbe cancelled_probe(drop);
   if (auto pointer = gfx::AccessPointer()) {
      acDeactivate(pointer);
      ReleaseObject(pointer);
   }
   check(CheckResourceExists(cancelled_id) != ERR::True, "Explicit cancellation releases pending state");
   check(cancelled_probe.released(), "Cancellation releases participant subscription pins");
   check(not display::DriverDragData(drop, "text/uri-list", payload, std::strlen(payload)),
      "Cancelled snapshots refuse late data");
   const auto folder_name = "temp:native_drop_" + std::to_string(getpid()) + '/';
   objFile::create folder = { fl::Path(folder_name), fl::Flags(FL::NEW|FL::FOLDER) };
   std::string folder_path;
   if (folder.ok() and (ResolvePath(folder_name, RSF::NIL, &folder_path) IS ERR::Okay)) {
      auto cleanup = kt::Defer([&]() { DeleteFile(folder_name, nullptr); });
      while (folder_path.ends_with('/')) folder_path.pop_back();
      const auto folder_uri = "file://" + folder_path + "\r\n";
      snapshot_drop(drop);
      native_test_movement(second->UID, 75, 90);
      check(display::DriverDragData(drop, "text/uri-list", folder_uri.data(), folder_uri.size()),
         "Native folder drops are accepted");
      check(state.Receipt.find(folder_path + "/\"") != std::string::npos,
         "Folder sources gain the separator required by CopyFile");
   }
   else check(false, "Create a native folder source");
   kt::ScopedObjectLock source(drop.DisplayID);
   check(source.granted() and (respond_wayland_drop_request(*source, sink, -1, datatype) IS ERR::NoSupport),
      "The native payload expires at the end of synchronous delivery");
   if (auto pointer = gfx::AccessPointer()) {
      check((pointer->OverObjectID IS second->UID) and (pointer->OverX IS 75) and (pointer->OverY IS 90),
         "Delayed delivery restores the live pointer");
      ReleaseObject(pointer);
   }
   snapshot_drop(drop);
   state.MoveSurface = second->UID;
   check(display::DriverDragData(drop, "text/plain", "motion", 6), "Deliver a drop whose callback moves the pointer");
   state.MoveSurface = 0;
   if (auto pointer = gfx::AccessPointer()) {
      check((pointer->OverObjectID IS second->UID) and (pointer->OverX IS 110) and (pointer->OverY IS 120),
         "Movement during delivery remains current after the callback");
      ReleaseObject(pointer);
   }
   const auto delivered = state.Calls;
   snapshot_drop(drop);
   gfx::SetModalSurface(second->UID);
   check(not display::DriverDragData(drop, "text/uri-list", payload, std::strlen(payload)),
      "A modal outside the original destination refuses delayed data");
   check(not display::DriverDragOver(first->UID, 25, 40, &datatype, 1), "Modal scope also rejects hover");
   check(state.Calls IS delivered, "A refused modal drop never reaches the original destination");
   gfx::SetModalSurface(0);
   snapshot_drop(drop);
   state.Reject = true;
   check(not display::DriverDragData(drop, "text/plain", "<&>\"", 4), "Destination rejection is reported to the host");
   check(state.Receipt.find("&lt;&amp;&gt;&quot;") != std::string::npos, "Text receipts are XML escaped");
   snapshot_drop(drop);
   const auto calls = state.Calls;
   check(not display::DriverDragData(drop, "text/uri-list", "file://remote/tmp/a\r\n", 21),
      "A URI list without local files is refused");
   check(state.Calls IS calls, "Malformed imports cannot reuse the preceding payload");
   snapshot_drop(drop);
   NativeSnapshotProbe destroyed_target_probe(drop);
   FreeResource(first.detach());
   check(not display::DriverDragData(drop, "text/plain", "late", 4), "Destroyed destinations refuse delayed data");
   check(destroyed_target_probe.released(), "Destination destruction releases the surviving source subscription");
   check(clipboard_snapshot() IS saved_clipboard, "Native imports preserve the normal clipboard");

   {
      objSurface::create target = { fl::Width(100), fl::Height(100) };
      check(target.ok(), "Create a destination for the isolated source");
      if (target.ok()) {
         acShow(*target);
         NativeDrop pending;
         pending.SurfaceID = target->UID;
         pending.X = pending.Y = 20;
         check(display::DriverDragOver(target->UID, 20, 20, &datatype, 1), "Accept the isolated source offer");
         check(display::DriverDragStart(pending), "Prepare the isolated source drop");
         display::DriverDragLeave();
         const auto pending_id = pending.Snapshot ? pending.Snapshot->ID : 0;
         NativeSnapshotProbe destroyed_source_probe(pending);
         FreeObject(pending.DisplayID);
         check(CheckResourceExists(pending_id) != ERR::True, "Source destruction expires pending state");
         check(destroyed_source_probe.released(), "Source destruction releases the surviving destination subscription");
         check(not display::DriverDragData(pending, "text/plain", "late", 4),
            "Source destruction refuses delayed payload delivery");
      }
   }
}

}

// Included by display.cpp so that the test exercises the actual driver callbacks with a headless display.
#include <kotuku/strings.hpp>

namespace {

struct NativeDropObservation {
   OBJECTPTR Sink = nullptr;
   int Calls = 0;
   double X = 0;
   double Y = 0;
   bool Reject = false;
   OBJECTID MoveSurface = 0;
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
   respond_wayland_drop_request(Args->Source, state->Sink, Args->Item, Args->Datatype[0]);
   if (state->MoveSurface) native_test_movement(state->MoveSurface, 110, 120);
}

static void native_drop_tests(int &Passed, int &Total)
{
   kt::Log log("NativeDropTests");
   auto check = [&](bool Condition, CSTRING Message) {
      Total++;
      if (Condition) Passed++;
      else log.warning("%s", Message);
   };
   if (not glHeadless) return; // These tests must not move the user's desktop cursor or change its clipboard.

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
   check(display::DriverDragData(drop, "text/uri-list", payload, std::strlen(payload)), "Deliver delayed drop");
   check((state.Calls IS 1) and (state.X IS 25.5) and (state.Y IS 40.25),
      "Delayed delivery uses the original surface and fractional position");
   check(state.Receipt.find("two words &amp; 三&quot;.txt") != std::string::npos,
      "URI decoding and XML escaping preserve the exact file path");
   const auto folder_name = "temp:native_drop_" + std::to_string(getpid()) + '/';
   objFile::create folder = { fl::Path(folder_name), fl::Flags(FL::NEW|FL::FOLDER) };
   std::string folder_path;
   if (folder.ok() and (ResolvePath(folder_name, RSF::NIL, &folder_path) IS ERR::Okay)) {
      auto cleanup = kt::Defer([&]() { DeleteFile(folder_name, nullptr); });
      while (folder_path.ends_with('/')) folder_path.pop_back();
      const auto folder_uri = "file://" + folder_path + "\r\n";
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
   state.MoveSurface = second->UID;
   check(display::DriverDragData(drop, "text/plain", "motion", 6), "Deliver a drop whose callback moves the pointer");
   state.MoveSurface = 0;
   if (auto pointer = gfx::AccessPointer()) {
      check((pointer->OverObjectID IS second->UID) and (pointer->OverX IS 110) and (pointer->OverY IS 120),
         "Movement during delivery remains current after the callback");
      ReleaseObject(pointer);
   }
   const auto delivered = state.Calls;
   gfx::SetModalSurface(second->UID);
   check(not display::DriverDragData(drop, "text/uri-list", payload, std::strlen(payload)),
      "A modal outside the original destination refuses delayed data");
   check(not display::DriverDragOver(first->UID, 25, 40, &datatype, 1), "Modal scope also rejects hover");
   check(state.Calls IS delivered, "A refused modal drop never reaches the original destination");
   gfx::SetModalSurface(0);
   state.Reject = true;
   check(not display::DriverDragData(drop, "text/plain", "<&>\"", 4), "Destination rejection is reported to the host");
   check(state.Receipt.find("&lt;&amp;&gt;&quot;") != std::string::npos, "Text receipts are XML escaped");
   const auto calls = state.Calls;
   check(not display::DriverDragData(drop, "text/uri-list", "file://remote/tmp/a\r\n", 21),
      "A URI list without local files is refused");
   check(state.Calls IS calls, "Malformed imports cannot reuse the preceding payload");
   FreeResource(first.detach());
   check(not display::DriverDragData(drop, "text/plain", "late", 4), "Destroyed destinations refuse delayed data");
   check(clipboard_snapshot() IS saved_clipboard, "Native imports preserve the normal clipboard");
}

}

// Exercise the loaded adapter's actual timer, capture and notification interfaces.
#include <kotuku/startup.h>
#include <kotuku/modules/module.h>
#include <kotuku/modules/display.h>
#include <kotuku/modules/font.h>
#include <kotuku/modules/vector.h>

#include <chrono>
#include <cstdio>
#include <thread>

#include "../engine_headers.h"
#include "../scintillakotuku.h"

JUMPTABLE_VECTOR
JUMPTABLE_DISPLAY

CSTRING ProgName = "ScintillaInput";

// Name the protected engine enum without constructing a substitute editor.
struct TimerTypes : ScintillaKTK {
   using ScintillaKTK::TickReason;
};

static void destroy_on_character(extScintilla *Editor, SCAC Event, int, int, CSTRING, APTR)
{
   if (Event IS SCAC::CHAR_ADDED) FreeResource(Editor);
}

static bool run_checks()
{
   int failures = 0;
   auto expect = [&failures](bool Condition, const char *Description) {
      if (not Condition) { std::fprintf(stderr, "%s\n", Description); failures++; }
   };
   auto scene = objVectorScene::create::global(fl::PageWidth(300), fl::PageHeight(120));
   if (not scene) return false;
   const auto release_scene = kt::Defer([scene]() { FreeResource(scene); });
   auto viewport = objVectorViewport::create::global(fl::Owner(scene->UID), fl::Width(300), fl::Height(120));
   if (not viewport) return false;
   auto editor = objScintilla::create::global(fl::Owner(viewport->UID), fl::Viewport(viewport), fl::Flags(SCIF::EDIT),
      fl::Face("Clean"), fl::FontSize(11));
   if (not editor) return false;
   auto self = (extScintilla *)editor;
   auto api = self->API;
   auto send = [api](Scintilla::Message Message, uptr_t WParam = 0, sptr_t LParam = 0) {
      return api->WndProc(Message, WParam, LParam);
   };
   auto list_bitmap = [&]() -> objBitmap * {
      if (Action(sci::Repaint::id, editor, nullptr) != ERR::Okay) return nullptr;
      for (objVector *popup = scene->Viewport; popup; popup = popup->Next) {
         if ((std::string_view(popup->Name) != "scicompletions") or not popup->Child) continue;
         std::string_view fill;
         popup->Child->getFill(fill);
         if (not fill.starts_with("url(#") or not fill.ends_with(')')) return nullptr;
         OBJECTPTR image;
         if (scene->findDef(fill.substr(5, fill.size() - 6), &image) != ERR::Okay) return nullptr;
         return ((objVectorImage *)image)->Bitmap;
      }
      return nullptr;
   };
   const char candidates[] = "WWWWWWWWWWWWWWWWWWWWWWWWW\nxxxxxxxxxxxxxxxxxxxxxxxxx";
   send(Scintilla::Message::AutoCSetSeparator, '\n');
   send(Scintilla::Message::AutoCShow, 0, sptr_t(candidates));
   auto bitmap = list_bitmap();
   expect(bitmap != nullptr, "The completion window owns a painted bitmap");
   const int regular_width = bitmap ? bitmap->Width : 0;
   send(Scintilla::Message::AutoCCancel);
   send(Scintilla::Message::StyleSetFont, STYLE_CALLTIP, sptr_t("Noto Sans"));
   send(Scintilla::Message::StyleSetSizeFractional, STYLE_CALLTIP, 1800); // 24 device pixels
   send(Scintilla::Message::AutoCSetStyle, STYLE_CALLTIP);
   send(Scintilla::Message::SetElementColour, uptr_t(Scintilla::Element::ListBack), 0xffff0000);
   send(Scintilla::Message::SetElementColour, uptr_t(Scintilla::Element::ListSelectedBack), 0xff0000ff);
   send(Scintilla::Message::AutoCShow, 0, sptr_t(candidates));
   bitmap = list_bitmap();
   expect(bitmap and bitmap->Width > regular_width * 1.5,
      "Completion geometry uses the supplied style font rather than the editor's regular handle");
   if (bitmap) {
      RGB8 *pixel;
      gfx::ReadRGBPixel(bitmap, 2, 2, &pixel);
      expect(pixel->Red IS 255 and pixel->Green IS 0 and pixel->Blue IS 0,
         "ListOptions supplies the selected row background");
      gfx::ReadRGBPixel(bitmap, 2, bitmap->Height - 2, &pixel);
      expect(pixel->Red IS 0 and pixel->Green IS 0 and pixel->Blue IS 255,
         "ListOptions supplies the ordinary row background");
   }
   send(Scintilla::Message::AutoCCancel);

   using Reason = TimerTypes::TickReason;
   const auto stop_all = [&]() {
      for (auto reason : { Reason::caret, Reason::scroll, Reason::widen, Reason::dwell, Reason::platform }) {
         api->FineTickerCancel(reason);
      }
   };
   stop_all();
   api->FineTickerStart(Reason::dwell, 500, 0);
   api->FineTickerStart(Reason::platform, 1, 0);
   std::this_thread::sleep_for(std::chrono::milliseconds(10));
   expect(api->panTick() and api->FineTickerRunning(Reason::dwell),
      "A short platform ticker must not trigger the independent dwell deadline");
   api->FineTickerCancel(Reason::platform);
   api->FineTickerStart(Reason::dwell, 1, 0);
   api->FineTickerStart(Reason::platform, 500, 0);
   std::this_thread::sleep_for(std::chrono::milliseconds(10));
   expect(api->panTick() and not api->FineTickerRunning(Reason::dwell) and
      api->FineTickerRunning(Reason::platform), "A one-shot dwell tick cancels only its own reason");
   api->FineTickerCancel(Reason::platform);
   expect(not api->panTick(), "The subscription stops when its last reason is cancelled");

   api->FineTickerStart(Reason::dwell, 1, 0);
   api->FineTickerStart(Reason::dwell, 500, 0);
   std::this_thread::sleep_for(std::chrono::milliseconds(10));
   expect(api->panTick() and api->FineTickerRunning(Reason::dwell), "Restarting a reason replaces its deadline");
   stop_all();
   api->panSetFocus(true);
   expect(api->FineTickerRunning(Reason::caret), "Focus starts the caret ticker");
   api->SetMouseCapture(true);
   api->FineTickerStart(Reason::scroll, 100, 0);
   api->FineTickerStart(Reason::dwell, 500, 0);
   api->panSetFocus(false);
   expect(not api->HaveMouseCapture() and not api->FineTickerRunning(Reason::scroll) and
      not api->FineTickerRunning(Reason::caret) and not api->FineTickerRunning(Reason::dwell),
      "Focus loss cancels capture, autoscroll, caret and dwell work immediately");

   for (int i = 0; i < 10; i++) {
      api->panSetFocus(true);
      api->panSetFocus(false);
   }
   expect(not api->panTick(), "Repeated focus changes leave no active fine ticker");
   api->SetIdle(true);
   api->panSetFocus(true);
   api->Finalise();
   api->FineTickerStart(Reason::platform, 1, 0);
   api->SetIdle(true);
   expect(not api->panTick() and not api->panIdle() and not self->TickTimer and not self->IdleTimer,
      "Finalisation cancels timers and rejects later scheduling");
   api->Finalise();

   auto direct_editor = objScintilla::create::global(fl::Owner(viewport->UID), fl::Viewport(viewport),
      fl::Flags(SCIF::EDIT));
   if (not direct_editor) return false;
   const auto direct_id = direct_editor->UID;
   direct_editor->setCompletionCallback(C_FUNCTION(destroy_on_character));
   auto direct_api = ((extScintilla *)direct_editor)->API;
   const auto context = direct_api->WndProc(Scintilla::Message::GetDirectPointer, 0, 0);
   const auto routine = (SciFnDirect)direct_api->WndProc(Scintilla::Message::GetDirectFunction, 0, 0);
   routine(context, SCI_NEWLINE, 0, 0);
   expect(CheckResourceExists(direct_id) != ERR::True,
      "A direct-function call retains the engine until a destroying character callback returns");
   return failures IS 0;
}

int main(int Argc, CSTRING *Argv)
{
   if (auto error = init_kotuku(Argc, Argv)) { std::fprintf(stderr, "%s\n", error); return 1; }
   OBJECTPTR module;
   const bool passed = (objModule::load("vector", &module, &VectorBase) IS ERR::Okay) and
      (objModule::load("display", &module, &DisplayBase) IS ERR::Okay) and
      (objModule::load("scintilla", &module, nullptr) IS ERR::Okay) and run_checks();
   close_kotuku();
   return passed ? 0 : 1;
}

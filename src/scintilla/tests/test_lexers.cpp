// Check the loaded adapter and its document-owned Lexilla instances through the real message interface.
#include <kotuku/startup.h>
#include <kotuku/modules/module.h>
#include <kotuku/modules/vector.h>

#include <cstdio>

#include "../engine_headers.h"
#include "../scintillakotuku.h"
#include "../lexer_profiles.h"
#include "SciLexer.h"

JUMPTABLE_VECTOR
CSTRING ProgName = "ScintillaLexers";

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
   auto editor = objScintilla::create::global(fl::Owner(viewport->UID), fl::Viewport(viewport), fl::Flags(SCIF::EDIT));
   if (not editor) return false;
   auto self = (extScintilla *)editor;
   auto api = self->API;
   auto send = [api](Scintilla::Message Message, uptr_t WParam = 0, sptr_t LParam = 0) {
      return api->WndProc(Message, WParam, LParam);
   };
   const char text[] = "function example()\n   return 123\nend\n";
   send(Scintilla::Message::SetText, 0, sptr_t(text));
   send(Scintilla::Message::EmptyUndoBuffer);
   for (int iteration=0; iteration < 10; iteration++) {
      for (const auto &profile : glLexerProfiles) {
         expect(api->SetLexer(profile.ID) IS ERR::Okay, "Every public profile creates a lexer");
         char name[32] = {};
         send(Scintilla::Message::GetLexerLanguage, sizeof(name), sptr_t(name));
         expect(std::string_view(name) IS profile.Factory, "The active factory matches the public profile");
         expect(self->Lexer IS profile.ID, "The public lexer field matches the installed instance");
         expect(send(Scintilla::Message::GetLength) IS sizeof(text) - 1, "Switching preserves document text");
         expect(send(Scintilla::Message::CanUndo) IS 0, "Switching does not create undo records");
      }
   }
   expect(api->SetLexer(SCLEX::TIRI) IS ERR::Okay, "Tiri can be selected after repeated switching");
   expect(send(Scintilla::Message::GetStyleAt, 0) IS 5, "Generated Tiri keywords reach the Lua lexer");
   const auto styled = send(Scintilla::Message::GetEndStyled);
   expect(api->SetLexer(SCLEX(99999)) IS ERR::NoSupport, "Unknown public lexers fail before changing the document");
   expect(self->Lexer IS SCLEX::TIRI and send(Scintilla::Message::GetStyleAt, 0) IS 5 and
      send(Scintilla::Message::GetEndStyled) IS styled, "A rejected switch preserves the active lexer and styling");

   // Audit Lua-compatible Tiri tokens beyond reserved words.  Prefixes and interpolation remain the existing
   // Lua profile's limitation; no unverified Tiri-specific vendor changes are introduced.
   struct TokenSample {
      SCLEX Lexer;
      const char *Text;
      const char *Token;
      int Style;
   };
   const TokenSample samples[] = {
      { SCLEX::TIRI, "-- comment\nlocal value = 0x20", "comment", SCE_LUA_COMMENTLINE },
      { SCLEX::TIRI, "local value = 0x20", "0x20", SCE_LUA_NUMBER },
      { SCLEX::TIRI, "--[=[comment]=]", "comment", SCE_LUA_COMMENT },
      { SCLEX::TIRI, "local value = [=[long string]=]", "long string", SCE_LUA_LITERALSTRING },
      { SCLEX::TIRI, "local value = \"text\"", "text", SCE_LUA_STRING },
      { SCLEX::TIRI, "value += 2", "+", SCE_LUA_OPERATOR },
      { SCLEX::PYTHON, "value = f\"text{1}\"", "text", SCE_P_FSTRING },
      { SCLEX::HTML, "<script>const value = `text`;</script>", "text", SCE_HJ_TEMPLATELITERAL }
   };
   for (const auto &sample : samples) {
      send(Scintilla::Message::SetText, 0, sptr_t(sample.Text));
      expect(api->SetLexer(sample.Lexer) IS ERR::Okay, "Token sample creates its lexer");
      const auto position = std::string_view(sample.Text).find(sample.Token);
      expect(send(Scintilla::Message::GetStyleAt, position) IS sample.Style,
         "Non-keyword tokens retain their semantic style in the pinned lexer");
   }
   expect(api->SetLexer(SCLEX::TIRI) IS ERR::Okay, "Restore Tiri for document ownership checks");

   // Documents retain their own lexer when temporarily detached from the editor.
   const auto original = send(Scintilla::Message::GetDocPointer);
   send(Scintilla::Message::AddRefDocument, 0, original);
   const auto temporary = send(Scintilla::Message::CreateDocument);
   send(Scintilla::Message::SetDocPointer, 0, temporary);
   send(Scintilla::Message::ReleaseDocument, 0, temporary);
   expect(api->SetLexer(SCLEX::CPP) IS ERR::Okay, "A replacement document accepts a fresh lexer");
   send(Scintilla::Message::SetDocPointer, 0, original);
   send(Scintilla::Message::ReleaseDocument, 0, original);
   expect(send(Scintilla::Message::GetLexer) IS 15, "The original document retains its Lua lexer");
   expect(api->SetLexer(SCLEX::NIL) IS ERR::Okay, "Plain text replaces the retained lexer");
   expect(send(Scintilla::Message::GetLexer) IS 1, "Plain text uses a null lexer rather than container styling");
   expect(send(Scintilla::Message::GetStyleAt, 0) IS 0, "Plain text clears the old semantic styles");
   return failures IS 0;
}

int main(int Argc, CSTRING *Argv)
{
   if (auto error = init_kotuku(Argc, Argv)) { std::fprintf(stderr, "%s\n", error); return 1; }
   OBJECTPTR module;
   const bool passed = (objModule::load("vector", &module, &VectorBase) IS ERR::Okay) and
      (objModule::load("scintilla", &module, nullptr) IS ERR::Okay) and run_checks();
   close_kotuku();
   return passed ? 0 : 1;
}

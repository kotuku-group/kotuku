#include <cstdlib>
#include <cstdio>
#include <stdexcept>

#include "../engine_headers.h"
#include "UndoHistory.h"

using namespace Scintilla::Internal;

void Platform::Assert(const char *Condition, const char *File, int Line) noexcept
{
   std::fprintf(stderr, "%s:%d: %s\n", File, Line, Condition);
   std::abort();
}

int main()
{
   int failures = 0;
   auto expect = [&failures](bool Condition, const char *Description) {
      if (!Condition) {
         std::fprintf(stderr, "%s\n", Description);
         ++failures;
      }
   };

   Selection empty_selection("");
   empty_selection.Truncate(0); // The editor truncates restored selections to the document length.
   expect(std::cmp_equal(empty_selection.Count(), 1), "Empty serialized input restores one range");
   expect(std::cmp_equal(empty_selection.MainCaret(), 0), "Empty serialized input restores the caret at zero");

   for (const auto text : {"bad", "1v", "1-", "1,", "#bad,1", "999999999999999999999999", "Rbad"}) {
      Selection selection(text);
      expect(std::cmp_equal(selection.Count(), 1), "Malformed selection resets to one range");
      expect(std::cmp_equal(selection.RangeMain().caret.Position(), 0), "Malformed selection resets the caret");
   }
   Selection selection("#1,2v3-4v5,8");
   expect(std::cmp_equal(selection.Count(), 2), "Restore multiple selections");
   expect(std::cmp_equal(selection.Main(), 1), "Restore the main selection");
   expect(!selection.ToString().compare("#1,2v3-4v5,8"), "Preserve positions and virtual space");
   Selection rectangle("R2-4");
   expect(rectangle.IsRectangular(), "Restore rectangular selection mode");
   expect(std::cmp_equal(rectangle.RangeMain().caret.Position(), 0), "Initialise rectangular rows at zero");

   UndoHistory history;
   expect(!history.SetCurrent(-1, 0), "Reject negative undo indices");
   expect(!history.SetCurrent(1, 0), "Reject undo indices beyond the history");
   expect(history.SetCurrent(0, 0), "Restore an empty undo history after failure");
   bool start_sequence = false;
   history.AppendAction(ActionType::insert, 0, "abc", 3, start_sequence);
   expect(history.SetCurrent(1, 3), "Restore a valid undo history");
   expect(!history.SetCurrent(1, 0), "Reject history inconsistent with the document");
   expect(std::cmp_equal(history.Actions(), 0), "Discard invalid restored history");

   wchar_t utf16[2] = {0, 123};
   expect(std::cmp_equal(UTF16FromUTF8("ab", utf16, 1), 1), "Return the completed UTF-16 length");
   expect(std::cmp_equal(int(utf16[1]), 123), "Preserve the UTF-16 buffer guard");
   expect(std::cmp_equal(UTF16FromUTF8("\xf0\x9f\x98\x80", utf16, 1), 0),
      "Do not split a UTF-16 surrogate pair");
   unsigned int utf32[2] = {0, 123};
   expect(std::cmp_equal(UTF32FromUTF8("ab", utf32, 1), 1), "Return the completed UTF-32 length");
   expect(std::cmp_equal(utf32[1], 123), "Preserve the UTF-32 buffer guard");
   expect(std::cmp_equal(UTF32FromUTF8("a", nullptr, 0), 0), "Accept zero output capacity");
   return failures ? 1 : 0;
}

/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
ScintillaSearch: Provides search functionality for use on Scintilla objects.

A ScintillaSearch object scans the document of a @Scintilla object for the #Text string.  Call #Find() to locate the
first match, then the #Next() and #Prev() actions to step through the matches that follow or precede it.  The #Flags determine
the direction of the search, whether it is case sensitive or a regular expression, whether it wraps at the end of the
document and whether it is limited to the user's selection.

Each match is selected in the Scintilla object, unless `STF::MOVE_CURSOR` is set, in which case the cursor is moved
to the start of the match.  The #Start and #End fields record the position of the most recent match.

-END-

*********************************************************************************************************************/

#define PRV_SCINTILLA

#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

#include "Platform.h"
#include "ILexer.h"
#include "Scintilla.h"
#include "PropSetSimple.h"
#include "SplitVector.h"
#include "Partitioning.h"
#include "RunStyles.h"
#include "ContractionState.h"
#include "CellBuffer.h"
#include "CallTip.h"
#include "KeyMap.h"
#include "Indicator.h"
#include "XPM.h"
#include "LineMarker.h"
#include "Style.h"
#include "ViewStyle.h"
#include "AutoComplete.h"
#include "CharClassify.h"
#include "Decoration.h"
#include "Document.h"
#include "Selection.h"
#include "PositionCache.h"
#include "Editor.h"
#include "ScintillaBase.h"

#include <kotuku/main.h>
#include <kotuku/modules/display.h>
#include <kotuku/strings.hpp>

#include "scintillakotuku.h"
#include <kotuku/modules/scintilla.h>

#define SCICALL     ((extScintilla *)(Self->Scintilla))->API->SendScintilla

class extScintillaSearch : public objScintillaSearch {
   public:
   int ScanStart; // The user's selection, recorded for STF::SCAN_SELECTION searches
   int ScanEnd;

   extScintillaSearch(objMetaClass *ClassPtr, OBJECTID ObjectID) : objScintillaSearch(ClassPtr, ObjectID) {
      Start     = -1;
      End       = -1;
      ScanStart = -1;
      ScanEnd   = -1;
   }
};

//********************************************************************************************************************
// Record the user's selection as the range for STF::SCAN_SELECTION searches.

static void record_selection(extScintillaSearch *Self)
{
   Self->ScanStart = SCICALL(SCI_GETSELECTIONSTART);
   Self->ScanEnd   = SCICALL(SCI_GETSELECTIONEND);
}

//********************************************************************************************************************
// Search for the Text between two positions.  If End is less than Start, the search is backwards.  Returns the
// position of the match, or -1 if there is no match.  On success the Scintilla target is set to the match.

static int search_range(extScintillaSearch *Self, int Start, int End)
{
   SCICALL(SCI_SETTARGETSTART, Start);
   SCICALL(SCI_SETTARGETEND, End);
   return SCICALL(SCI_SEARCHINTARGET, (unsigned long)Self->Text.size(), Self->Text.c_str());
}

//********************************************************************************************************************
// Common search routine for Find(), Next() and Prev().  The search begins at From and proceeds in the direction
// indicated by Backwards.  If nothing is found and STF::WRAP is set, the search is repeated over the entire range.

static ERR search(extScintillaSearch *Self, int From, bool Backwards)
{
   kt::Log log(__FUNCTION__);

   if (Self->Text.empty()) return log.warning(ERR::FieldNotSet);

   SCICALL(SCI_SETSEARCHFLAGS, (((Self->Flags & STF::CASE) != STF::NIL) ? SCFIND_MATCHCASE : 0) |
      (((Self->Flags & STF::EXPRESSION) != STF::NIL) ? SCFIND_REGEXP : 0));

   // The searchable range is either the entire document or the recorded selection.

   int range_start, range_end;
   if ((Self->Flags & STF::SCAN_SELECTION) != STF::NIL) {
      range_start = Self->ScanStart;
      range_end   = Self->ScanEnd;
   }
   else {
      range_start = 0;
      range_end   = SCICALL(SCI_GETLENGTH);
   }

   From = std::clamp(From, range_start, range_end);

   log.trace("Search from %d %s within %d - %d", From, Backwards ? "backwards" : "forwards", range_start, range_end);

   int pos = search_range(Self, From, Backwards ? range_start : range_end);

   if ((pos IS -1) and ((Self->Flags & STF::WRAP) != STF::NIL)) {
      log.trace("Wrap-around");
      if (Backwards) pos = search_range(Self, range_end, range_start);
      else pos = search_range(Self, range_start, range_end);
   }

   if (pos IS -1) return ERR::Search;

   Self->Start = SCICALL(SCI_GETTARGETSTART);
   Self->End   = SCICALL(SCI_GETTARGETEND);

   // Expand any folded lines that contain the match

   int last_line = SCICALL(SCI_LINEFROMPOSITION, Self->End);
   for (int line=SCICALL(SCI_LINEFROMPOSITION, Self->Start); line <= last_line; line++) {
      SCICALL(SCI_ENSUREVISIBLEENFORCEPOLICY, line);
   }

   // Select the match with the cursor on the edge that faces the search direction, or move the cursor to it.

   if ((Self->Flags & STF::MOVE_CURSOR) != STF::NIL) SCICALL(SCI_GOTOPOS, Self->Start);
   else if (Backwards) SCICALL(SCI_SETSEL, Self->End, Self->Start);
   else SCICALL(SCI_SETSEL, Self->Start, Self->End);

   return ERR::Okay;
}

//********************************************************************************************************************
// Continue a search from the most recent match.  While the selection (or cursor, for STF::MOVE_CURSOR) still reflects
// that match, the search resumes from the edge of the match that faces the search direction, so reversing direction
// does not find the same match again.  If the user has moved the cursor since, the search resumes from the cursor.

static ERR continue_search(extScintillaSearch *Self, bool Backwards)
{
   if (Self->Text.empty()) return kt::Log(__FUNCTION__).warning(ERR::FieldNotSet);

   if (((Self->Flags & STF::SCAN_SELECTION) != STF::NIL) and (Self->ScanStart < 0)) record_selection(Self);

   int from = SCICALL(SCI_GETCURRENTPOS);
   if (Self->Start >= 0) {
      int sel_start = SCICALL(SCI_GETSELECTIONSTART);
      int sel_end   = SCICALL(SCI_GETSELECTIONEND);
      if ((sel_start IS Self->Start) and ((sel_end IS Self->End) or (sel_end IS Self->Start))) {
         // An empty match (possible with expressions) is stepped over so that it is not found again.
         from = Backwards ? Self->Start : Self->End;
         if (Self->Start IS Self->End) {
            bool selection = (Self->Flags & STF::SCAN_SELECTION) != STF::NIL;
            int range_start = selection ? Self->ScanStart : 0;
            int range_end = selection ? Self->ScanEnd : SCICALL(SCI_GETLENGTH);

            // Preserve exhaustion at the boundary instead of clamping back onto the same empty match.
            if ((Backwards and (from <= range_start)) or ((!Backwards) and (from >= range_end))) {
               if ((Self->Flags & STF::WRAP) IS STF::NIL) return ERR::Search;
               from = Backwards ? range_end : range_start;
            }
            else from += Backwards ? -1 : 1;
         }
      }
   }

   return search(Self, from, Backwards);
}

/*********************************************************************************************************************

-METHOD-
Find: Searches for the first match of the Text string.

Call Find() to start a new search for #Text within the targeted #Scintilla object.  The search begins at the `From`
position and proceeds forwards, or backwards if `STF::BACKWARDS` is set in #Flags.  If no match is found before the
end of the document is reached, the search fails unless `STF::WRAP` is set, in which case it continues from the
opposite end.

If `STF::SCAN_SELECTION` is set, the user's current selection is recorded and the search is limited to it, including
any subsequent calls to #Next() and #Prev().  The `From` position is restricted to the selection.

The match is selected in the Scintilla object, or the cursor is moved to it if `STF::MOVE_CURSOR` is set.  The
#Start and #End fields are updated with the position of the match.

To find subsequent matches, use the #Next() and #Prev() actions.

-INPUT-
int From: The byte position to start searching from.  Set to -1 to start from the cursor, or from the edge of the selection that faces the search direction if `STF::SCAN_SELECTION` is set.

-RESULT-
Okay: A match was found.
NullArgs:
FieldNotSet: The #Text field is empty.
Search: No match was found.

*********************************************************************************************************************/

static ERR SCINTILLASEARCH_Find(extScintillaSearch *Self, struct ss::Find *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.traceBranch("Text: '%.10s', From: %d, Flags: $%.8x", Self->Text.c_str(), Args->From, int(Self->Flags));

   bool backwards = (Self->Flags & STF::BACKWARDS) != STF::NIL;

   Self->Start = -1;
   Self->End   = -1;

   int from = Args->From;
   if ((Self->Flags & STF::SCAN_SELECTION) != STF::NIL) {
      record_selection(Self);
      if (from < 0) from = backwards ? Self->ScanEnd : Self->ScanStart;
   }
   else if (from < 0) from = SCICALL(SCI_GETCURRENTPOS);

   return search(Self, from, backwards);
}

//********************************************************************************************************************

static ERR SCINTILLASEARCH_Init(extScintillaSearch *Self)
{
   kt::Log log;

   if (!Self->Scintilla) { // Find our parent
      auto obj = Self->Owner;
      while ((obj) and (obj->classID() != CLASSID::SCINTILLA)) {
         obj = obj->Owner;
      }
      if (obj) Self->Scintilla = (objScintilla *)obj;
      else return log.warning(ERR::UnsupportedOwner);
   }

   if (!Self->Scintilla->initialised()) return log.warning(ERR::NotInitialised);
   if (Self->Text.empty()) return log.warning(ERR::FieldNotSet);

   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Next: Continues a text search.

Use Next() to continue a search after calling #Find().  The search proceeds in the direction indicated by #Flags,
starting from the most recent match.  If the user has moved the cursor since the most recent match, the search
starts from the cursor instead.

If a match is discovered, the #Start and #End fields are updated with its position.  If there are no further
matches, `ERR::Search` is returned.  The search wraps at the end of the document if `STF::WRAP` is set.

-ERRORS-
Okay: A match was found.
FieldNotSet: The #Text field is empty.
Search: No match was found.
-END-

*********************************************************************************************************************/

static ERR SCINTILLASEARCH_Next(extScintillaSearch *Self)
{
   kt::Log log;

   log.traceBranch("Text: '%.10s', Flags: $%.8x", Self->Text.c_str(), int(Self->Flags));

   return continue_search(Self, (Self->Flags & STF::BACKWARDS) != STF::NIL);
}

/*********************************************************************************************************************

-ACTION-
Prev: Continues a text search in reverse.

The Prev() action operates in the same way as #Next(), except that the search proceeds in the opposite direction to
the one indicated by #Flags.

-ERRORS-
Okay: A match was found.
FieldNotSet: The #Text field is empty.
Search: No match was found.
-END-

*********************************************************************************************************************/

static ERR SCINTILLASEARCH_Prev(extScintillaSearch *Self)
{
   kt::Log log;

   log.traceBranch("Text: '%.10s', Flags: $%.8x", Self->Text.c_str(), int(Self->Flags));

   return continue_search(Self, (Self->Flags & STF::BACKWARDS) IS STF::NIL);
}

/*********************************************************************************************************************

-FIELD-
End: The end of the most recent match.

The End field records the byte position immediately after the most recent match, or -1 if there is no match.  It is
reset by #Find().

-FIELD-
Flags: Optional flags.

Optional flags that affect the search process are specified here.  They apply to #Find(), #Next() and #Prev(), and
can be changed between calls.

-FIELD-
Scintilla: Targets a Scintilla object for searching.

A Scintilla object must be targeted in this field in order to perform the search process.  If it is not set prior to
initialisation, the nearest Scintilla object in the ownership hierarchy is used.

-FIELD-
Start: The start of the most recent match.

The Start field records the byte position of the most recent match, or -1 if there is no match.  It is reset by
#Find().

-FIELD-
Text: The string sequence to search for.

This field defines the string sequence that will be searched for when calling either #Find(), #Next() or #Prev().  If
`STF::EXPRESSION` is set in #Flags, the Text is a regular expression.
-END-

*********************************************************************************************************************/

#include "class_scintilla_search_def.cxx"

static const FieldArray clFields[] = {
   { "Scintilla", FDF_OBJECT|FDF_RI, nullptr, nullptr, CLASSID::SCINTILLA },
   { "Text",      FDF_CPPSTRING|FDF_RW },
   { "Flags",     FDF_INTFLAGS|FDF_RW, nullptr, nullptr, &clScintillaSearchFlags },
   { "Start",     FDF_INT|FDF_R },
   { "End",       FDF_INT|FDF_R },
   END_FIELD
};

//********************************************************************************************************************

OBJECTPTR clScintillaSearch = nullptr;

ERR init_search(void)
{
   clScintillaSearch = objMetaClass::create::global(
      fl::ClassVersion(1.0),
      fl::Name("ScintillaSearch"),
      fl::Category(CCF::TOOL),
      fl::Actions(clScintillaSearchActions),
      fl::Methods(clScintillaSearchMethods),
      fl::Fields(clFields),
      fl::Size(sizeof(extScintillaSearch)),
      fl::Path("modules:scintilla"));

   return clScintillaSearch ? ERR::Okay : ERR::AddClass;
}

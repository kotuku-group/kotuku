/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
ScintillaSearch: Provides search functionality for use on Scintilla objects.

-END-

*********************************************************************************************************************/

#define PRV_SCINTILLA

#include <string.h>
#include <string>
#include <vector>
#include <map>

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

/*********************************************************************************************************************

-METHOD-
Find: Searches for a specific text string.

Call Find to initiate a string search within the targeted #Scintilla object.  The method will scan for the
first instance of the #Text string sequence and return its position in Pos.  The Flags parameter defines special
options that affect the search process.

To find subsequent string matches, call one of either the #Next() or #Prev() methods.

-INPUT-
&int Pos: The position to start searching from.  Set to -1 to start from the cursor position.  This parameter is updated with the byte position of the discovered string sequence.
int(STF) Flags: Optional flags.

-RESULT-
Okay: A successful search was made.
NullArgs:
Search: The string sequence was not found.
-END-

*********************************************************************************************************************/

static ERR SEARCH_Find(objScintillaSearch *Self, struct ss::Find *Args)
{
   pf::Log log;
   int start, end, pos, startLine, endLine, i, targstart, targend;

   if (!Self->Text) return log.warning(ERR::FieldNotSet);

   log.msg("Text: '%.10s'... From: %d, Flags: $%.8x", Self->Text, Args->Pos, int(Self->Flags));

   auto flags = (((Args->Flags & STF::CASE) != STF::NIL) ? SCFIND_MATCHCASE : 0) |
                 (((Args->Flags & STF::EXPRESSION) != STF::NIL) ? SCFIND_REGEXP : 0);

   SCICALL(SCI_SETSEARCHFLAGS, flags);

   if ((Self->Flags & STF::SCAN_SELECTION) != STF::NIL) {
      Self->Start = SCICALL(SCI_GETSELECTIONSTART);
      Self->End   = SCICALL(SCI_GETSELECTIONEND);
      if ((Self->Flags & STF::BACKWARDS) != STF::NIL) {
         start = Self->End;
         end = Self->Start;
      }
      else {
         start = Self->Start;
         end = Self->End;
      }
   }
   else {
      if (Args->Pos < 0) start = SCICALL(SCI_GETCURRENTPOS);
      else start = Args->Pos;

      if ((Self->Flags & STF::BACKWARDS) IS STF::NIL) end = SCICALL(SCI_GETLENGTH);
      else end = 0;

      if (start IS end) {
         if ((Self->Flags & STF::WRAP) != STF::NIL) start = 0;
         else return ERR::Search;
      }
   }

   SCICALL(SCI_SETTARGETSTART, start);
   SCICALL(SCI_SETTARGETEND, end);
   pos = SCICALL(SCI_SEARCHINTARGET, strlen(Self->Text), (char *)Self->Text);

   // If not found and wraparound is wanted, try again

   if ((pos IS -1) and ((Self->Flags & STF::WRAP) != STF::NIL) and ((Self->Flags & STF::SCAN_SELECTION) IS STF::NIL)) {
      if ((Self->Flags & STF::BACKWARDS) IS STF::NIL) {
         start = 0;
         end = SCICALL(SCI_GETLENGTH);
      }
      else {
         start = SCICALL(SCI_GETLENGTH);
         end = 0;
      }

      SCICALL(SCI_SETTARGETSTART, start);
      SCICALL(SCI_SETTARGETEND, end);
      pos = SCICALL(SCI_SEARCHINTARGET, strlen((STRING)Self->Text), (char *)Self->Text);
   }

   if (pos IS -1) return ERR::Search;

   targstart = SCICALL(SCI_GETTARGETSTART);
   targend   = SCICALL(SCI_GETTARGETEND);
   startLine = SCICALL(SCI_LINEFROMPOSITION, targstart);
   endLine   = SCICALL(SCI_LINEFROMPOSITION, targend);

   for (i=startLine; i <= endLine; ++i) {
      SCICALL(SCI_ENSUREVISIBLEENFORCEPOLICY,i);
   }

   if ((Self->Flags & STF::MOVE_CURSOR) != STF::NIL) {
      // Move cursor to the discovered text
   }
   else {
      // Set the selection
      if ((Self->Flags & STF::BACKWARDS) != STF::NIL) SCICALL(SCI_SETSEL, targend, targstart);
      else SCICALL(SCI_SETSEL, targstart, targend);
   }

   Args->Pos = pos; // Return the position to the user
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR SEARCH_Free(objScintillaSearch *Self)
{
   if (Self->Text) { FreeResource(Self->Text); Self->Text = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR SEARCH_Init(objScintillaSearch *Self)
{
   pf::Log log;

   if (!Self->Scintilla) { // Find our parent
      auto obj = Self->Owner;
      while ((obj) and (obj->classID() != CLASSID::SCINTILLA)) {
         obj = obj->Owner;
      }
      if (obj) Self->Scintilla = (objScintilla *)obj;
      else return log.warning(ERR::UnsupportedOwner);
   }

   if ((!Self->Text) or (!Self->Scintilla)) return log.warning(ERR::FieldNotSet);

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Next: Continues a text search.

Use Next to continue a search after calling the #Find() method.  If a string sequence matching that of #Text is
discovered, its byte position will be returned in the `Pos` parameter.  If a new match is not discovered then
`ERR::Search` is returned to indicate an end to the search.

-INPUT-
&int Pos: The byte-position of the discovered string sequence is returned here.

-RESULT-
Okay: A successful search was made.
NullArgs
Search: The string could not be found.

-END-

*********************************************************************************************************************/

static ERR SEARCH_Next(objScintillaSearch *Self, struct ss::Next *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.branch("Text: '%.10s', Flags: $%.8x, Section %d to %d", Self->Text, int(Self->Flags), Self->Start, Self->End);

   int flags = (((Self->Flags & STF::CASE) != STF::NIL) ? SCFIND_MATCHCASE : 0) |
                (((Self->Flags & STF::EXPRESSION) != STF::NIL) ? SCFIND_REGEXP : 0);

   SCICALL(SCI_SETSEARCHFLAGS, flags);

   int start, end, i;
   if ((Self->Flags & STF::SCAN_SELECTION) != STF::NIL) {
      if ((Self->Flags & STF::BACKWARDS) != STF::NIL) {
         start = SCICALL(SCI_GETCURRENTPOS);
         end = Self->Start;
      }
      else {
         start = SCICALL(SCI_GETCURRENTPOS);
         end   = Self->End;
      }
   }
   else {
      start = SCICALL(SCI_GETCURRENTPOS);
      if ((Self->Flags & STF::BACKWARDS) IS STF::NIL) end = SCICALL(SCI_GETLENGTH);
      else end = 0;

      if (start IS end) {
         if ((Self->Flags & STF::WRAP) != STF::NIL) start = 0;
         else return ERR::Search;
      }
   }

   log.trace("Search from %d to %d", start, end);

   SCICALL(SCI_SETTARGETSTART, start);
   SCICALL(SCI_SETTARGETEND, end);
   int pos = SCICALL(SCI_SEARCHINTARGET, strlen(Self->Text), (char *)Self->Text);

   // If not found and wraparound is wanted, try again

   if ((pos IS -1) and ((Self->Flags & STF::WRAP) != STF::NIL)) {
      log.trace("Wrap-around");
      if ((Self->Flags & STF::SCAN_SELECTION) != STF::NIL) {
         start = Self->Start;
         end   = Self->End;
      }
      else {
         start = 0;
         end = SCICALL(SCI_GETLENGTH);
      }

      if ((Self->Flags & STF::BACKWARDS) != STF::NIL) {
         auto tmp = start;
         start = end;
         end   = tmp;
      }

      SCICALL(SCI_SETTARGETSTART, start);
      SCICALL(SCI_SETTARGETEND, end);
      pos = SCICALL(SCI_SEARCHINTARGET, strlen((STRING)Self->Text), (char *)Self->Text);
   }

   if (pos IS -1) return ERR::Search;

   int targstart = SCICALL(SCI_GETTARGETSTART);
   int targend   = SCICALL(SCI_GETTARGETEND);
   int startLine = SCICALL(SCI_LINEFROMPOSITION,targstart);
   int endLine   = SCICALL(SCI_LINEFROMPOSITION,targend);

   for (i=startLine; i <= endLine; ++i) {
      SCICALL(SCI_ENSUREVISIBLEENFORCEPOLICY,i);
   }

   if ((Self->Flags & STF::MOVE_CURSOR) != STF::NIL) { // Move cursor to the discovered text
   }
   else { // Set the selection
      if ((Self->Flags & STF::BACKWARDS) != STF::NIL) SCICALL(SCI_SETSEL, targend, targstart);
      else SCICALL(SCI_SETSEL, targstart, targend);
   }

   Args->Pos = pos;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Prev: Continues a text search in reverse.

The Prev() method operates under the same circumstances as #Next(), except that the search will be in reverse.  Please
refer to #Next() for further information.

-INPUT-
&int Pos: The byte-position of the discovered string is returned here.

-RESULT-
Okay
NullArgs
Search: The string could not be found.
-END-

*********************************************************************************************************************/

static ERR SEARCH_Prev(objScintillaSearch *Self, struct ss::Prev *Args)
{
   if (!Args) return ERR::NullArgs;

   // Temporarily set the STF::BACKWARDS flag

   auto flags = Self->Flags;
   if ((Self->Flags & STF::BACKWARDS) != STF::NIL) Self->Flags &= ~STF::BACKWARDS;
   else Self->Flags |= STF::BACKWARDS;

   SEARCH_Next(Self, (struct ss::Next *)Args);

   Self->Flags = flags; // Restore the original flags
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Flags: Optional flags.

Optional flags that affect the search process are specified here.

-FIELD-
Scintilla: Targets a Scintilla object for searching.

A Scintilla object must be targeted in this field in order to perform the search process.

-FIELD-
Text: The string sequence to search for.

This field defines the string sequence that will be searched for when calling either #Find(), #Next() or #Prev().
-END-

*********************************************************************************************************************/

static ERR SET_Text(objScintillaSearch *Self, CSTRING Value)
{
   if (Self->Text) { FreeResource(Self->Text); Self->Text = nullptr; }
   if (Value) {
      if (!(Self->Text = pf::strclone(Value))) return ERR::AllocMemory;
   }
   return ERR::Okay;
}

//********************************************************************************************************************

static const ActionArray clActions[] = {
   { AC::Free, SEARCH_Free },
   { AC::Init, SEARCH_Init },
   { AC::NIL, nullptr }
};

//********************************************************************************************************************

static const FunctionField argsNext[] = { { "Pos", FD_INT|FD_RESULT }, { nullptr, 0 } };
static const FunctionField argsPrev[] = { { "Pos", FD_INT|FD_RESULT }, { nullptr, 0 } };
static const FunctionField argsFind[] = { { "Pos", FD_INT|FD_RESULT }, { nullptr, 0 } };

static const MethodEntry clMethods[] = {
   { ss::Next::id, (APTR)SEARCH_Next, "Next", argsNext, sizeof(struct ss::Next) },
   { ss::Prev::id, (APTR)SEARCH_Prev, "Prev", argsPrev, sizeof(struct ss::Prev) },
   { ss::Find::id, (APTR)SEARCH_Find, "Find", argsFind, sizeof(struct ss::Find) },
   { AC::NIL, nullptr, nullptr, nullptr, 0 }
};

//********************************************************************************************************************

static const FieldDef clFlags[] = {
   { "Case",          STF::CASE },
   { "MoveCursor",    STF::MOVE_CURSOR },
   { "ScanSelection", STF::SCAN_SELECTION },
   { "Backwards",     STF::BACKWARDS },
   { "Expression",    STF::EXPRESSION },
   { "Wrap",          STF::WRAP },
   { nullptr, 0 }
};

static const FieldArray clFields[] = {
   { "Scintilla", FDF_OBJECT|FDF_RI, nullptr, nullptr, CLASSID::SCINTILLA },
   { "Text",      FDF_STRING|FDF_RW, nullptr, SET_Text },
   { "Flags",     FDF_INTFLAGS|FDF_RW, nullptr, nullptr, &clFlags },
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
      fl::Actions(clActions),
      fl::Methods(clMethods),
      fl::Fields(clFields),
      fl::Size(sizeof(objScintillaSearch)),
      fl::Path("modules:scintilla"));

   return clScintillaSearch ? ERR::Okay : ERR::AddClass;
}

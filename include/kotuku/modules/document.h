#pragma once

// Name:      document.h
// Copyright: Paul Manias © 2005-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_DOCUMENT (1)

#include <kotuku/modules/display.h>
#include <kotuku/modules/xml.h>
#include <kotuku/modules/font.h>
#include <kotuku/modules/vector.h>

class objDocument;

// Official version number (date format).  Any changes to the handling of document content require that this number be updated.

#define RIPL_VERSION "20240126"

enum class TT : int8_t {
   NIL = 0,
   VECTOR = 1,
   LINK = 2,
   EDIT = 3,
};

// Event flags for selectively receiving events from the Document object.

enum class DEF : uint32_t {
   NIL = 0,
   PATH = 0x00000001,
   ON_CLICK = 0x00000002,
   ON_MOTION = 0x00000004,
   ON_CROSSING_IN = 0x00000008,
   ON_CROSSING_OUT = 0x00000010,
   ON_CROSSING = 0x00000018,
   LINK_ACTIVATED = 0x00000020,
   WIDGET_STATE = 0x00000040,
};

DEFINE_ENUM_FLAG_OPERATORS(DEF)

// Internal trigger codes

enum class DRT : int {
   NIL = 0,
   BEFORE_LAYOUT = 0,
   AFTER_LAYOUT = 1,
   USER_CLICK = 2,
   USER_CLICK_RELEASE = 3,
   USER_MOVEMENT = 4,
   REFRESH = 5,
   GOT_FOCUS = 6,
   LOST_FOCUS = 7,
   LEAVING_PAGE = 8,
   PAGE_PROCESSED = 9,
   END = 10,
};

// Document flags

enum class DCF : uint32_t {
   NIL = 0,
   EDIT = 0x00000001,
   OVERWRITE = 0x00000002,
   NO_SYS_KEYS = 0x00000004,
   DISABLED = 0x00000008,
   NO_LAYOUT_MSG = 0x00000010,
   UNRESTRICTED = 0x00000020,
};

DEFINE_ENUM_FLAG_OPERATORS(DCF)

// These are document style flags, as used in the DocStyle structure

enum class FSO : uint32_t {
   NIL = 0,
   UNDERLINE = 0x00000001,
   STYLES = 0x00000001,
   PREFORMAT = 0x00000002,
   ALIGN_RIGHT = 0x00000004,
   ALIGN_CENTER = 0x00000008,
   NO_WRAP = 0x00000010,
};

DEFINE_ENUM_FLAG_OPERATORS(FSO)

// Document class definition

#define VER_DOCUMENT (1.000000)

// Document methods

namespace doc {
struct FeedParser { CSTRING String; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SelectLink { int Index; CSTRING Name; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct FindIndex { CSTRING Name; int Start; int End; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct InsertXML { CSTRING XML; int Index; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RemoveContent { int Start; int End; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct InsertText { CSTRING Text; int Index; int Char; int Preformat; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct CallFunction { CSTRING Function; struct ScriptArg * Args; int TotalArgs; static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct AddListener { DRT Trigger; FUNCTION * Function; static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RemoveListener { int Trigger; FUNCTION * Function; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ShowIndex { CSTRING Name; static const AC id = AC(-11); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct HideIndex { CSTRING Name; static const AC id = AC(-12); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Edit { CSTRING Name; int Flags; static const AC id = AC(-13); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ReadContent { DATA Format; int Start; int End; STRING Result; static const AC id = AC(-14); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objDocument : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::DOCUMENT;
   static constexpr CSTRING CLASS_NAME = "Document";

   using create = pf::Create<objDocument>;

   STRING   Description;            // A description of the document, provided by its author.
   STRING   Title;                  // The title of the document.
   STRING   Author;                 // The author(s) of the document.
   STRING   Copyright;              // Copyright information for the document.
   STRING   Keywords;               // Includes keywords declared by the source document.
   objVectorViewport * Viewport;    // A client-specific viewport that will host the document graphics.
   objVectorViewport * Focus;       // Refers to the object that will be monitored for user focusing.
   objVectorViewport * View;        // The viewing area of the document.
   objVectorViewport * Page;        // The Page contains the document content and is hosted by the View
   OBJECTID TabFocusID;             // Allows the user to hit the tab key to focus on other GUI objects.
   DEF      EventMask;              // Specifies events that need to be reported from the Document object.
   DCF      Flags;                  // Optional flags that affect object behaviour.
   int      PageHeight;             // Measures the page height of the document, in pixels.
   ERR      Error;                  // The most recently generated error code.

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR clear() noexcept { return Action(AC::Clear, this, nullptr); }
   inline ERR clipboard(CLIPMODE Mode) noexcept {
      struct acClipboard args = { Mode };
      return Action(AC::Clipboard, this, &args);
   }
   inline ERR dataFeed(OBJECTPTR Object, DATA Datatype, const void *Buffer, int Size) noexcept {
      struct acDataFeed args = { Object, Datatype, Buffer, Size };
      return Action(AC::DataFeed, this, &args);
   }
   inline ERR disable() noexcept { return Action(AC::Disable, this, nullptr); }
   inline ERR draw() noexcept { return Action(AC::Draw, this, nullptr); }
   inline ERR drawArea(int X, int Y, int Width, int Height) noexcept {
      struct acDraw args = { X, Y, Width, Height };
      return Action(AC::Draw, this, &args);
   }
   inline ERR enable() noexcept { return Action(AC::Enable, this, nullptr); }
   inline ERR focus() noexcept { return Action(AC::Focus, this, nullptr); }
   inline ERR getKey(CSTRING Key, STRING Value, int Size) noexcept {
      struct acGetKey args = { Key, Value, Size };
      auto error = Action(AC::GetKey, this, &args);
      if ((error != ERR::Okay) and (Value)) Value[0] = 0;
      return error;
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR refresh() noexcept { return Action(AC::Refresh, this, nullptr); }
   inline ERR saveToObject(OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) noexcept {
      struct acSaveToObject args = { Dest, { ClassID } };
      return Action(AC::SaveToObject, this, &args);
   }
   inline ERR acSetKey(CSTRING FieldName, CSTRING Value) noexcept {
      struct acSetKey args = { FieldName, Value };
      return Action(AC::SetKey, this, &args);
   }
   inline ERR feedParser(CSTRING String) noexcept {
      struct doc::FeedParser args = { String };
      return(Action(AC(-1), this, &args));
   }
   inline ERR selectLink(int Index, CSTRING Name) noexcept {
      struct doc::SelectLink args = { Index, Name };
      return(Action(AC(-2), this, &args));
   }
   inline ERR findIndex(CSTRING Name, int * Start, int * End) noexcept {
      struct doc::FindIndex args = { Name, (int)0, (int)0 };
      ERR error = Action(AC(-4), this, &args);
      if (Start) *Start = args.Start;
      if (End) *End = args.End;
      return(error);
   }
   inline ERR insertXML(CSTRING XML, int Index) noexcept {
      struct doc::InsertXML args = { XML, Index };
      return(Action(AC(-5), this, &args));
   }
   inline ERR removeContent(int Start, int End) noexcept {
      struct doc::RemoveContent args = { Start, End };
      return(Action(AC(-6), this, &args));
   }
   inline ERR insertText(CSTRING Text, int Index, int Char, int Preformat) noexcept {
      struct doc::InsertText args = { Text, Index, Char, Preformat };
      return(Action(AC(-7), this, &args));
   }
   inline ERR callFunction(CSTRING Function, struct ScriptArg * Args, int TotalArgs) noexcept {
      struct doc::CallFunction args = { Function, Args, TotalArgs };
      return(Action(AC(-8), this, &args));
   }
   inline ERR addListener(DRT Trigger, FUNCTION Function) noexcept {
      struct doc::AddListener args = { Trigger, &Function };
      return(Action(AC(-9), this, &args));
   }
   inline ERR removeListener(int Trigger, FUNCTION Function) noexcept {
      struct doc::RemoveListener args = { Trigger, &Function };
      return(Action(AC(-10), this, &args));
   }
   inline ERR showIndex(CSTRING Name) noexcept {
      struct doc::ShowIndex args = { Name };
      return(Action(AC(-11), this, &args));
   }
   inline ERR hideIndex(CSTRING Name) noexcept {
      struct doc::HideIndex args = { Name };
      return(Action(AC(-12), this, &args));
   }
   inline ERR edit(CSTRING Name, int Flags) noexcept {
      struct doc::Edit args = { Name, Flags };
      return(Action(AC(-13), this, &args));
   }
   inline ERR readContent(DATA Format, int Start, int End, STRING * Result) noexcept {
      struct doc::ReadContent args = { Format, Start, End, (STRING)0 };
      ERR error = Action(AC(-14), this, &args);
      if (Result) *Result = args.Result;
      return(error);
   }

   // Customised field setting

   inline ERR setViewport(objVectorViewport * Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[25];
      return field->WriteValue(target, field, 0x08000301, Value, 1);
   }

   inline ERR setFocus(objVectorViewport * Value) noexcept {
      if (this->initialised()) return ERR::NoFieldAccess;
      this->Focus = Value;
      return ERR::Okay;
   }

   inline ERR setTabFocus(OBJECTID Value) noexcept {
      this->TabFocusID = Value;
      return ERR::Okay;
   }

   inline ERR setEventMask(const DEF Value) noexcept {
      this->EventMask = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const DCF Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setClientScript(OBJECTPTR Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[12];
      return field->WriteValue(target, field, 0x08000401, Value, 1);
   }

   inline ERR setEventCallback(FUNCTION Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(target, field, FD_FUNCTION, &Value, 1);
   }

   template <class T> inline ERR setPath(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setOrigin(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   inline ERR setPageWidth(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[13];
      Unit var(Value);
      return field->WriteValue(target, field, FD_UNIT, &var, 1);
   }

   template <class T> inline ERR setPretext(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[22];
      return field->WriteValue(target, field, 0x08800200, to_cstring(Value), 1);
   }

};

namespace fl {
   using namespace pf;

constexpr FieldValue EventCallback(const FUNCTION &Value) { return FieldValue(FID_EventCallback, &Value); }
constexpr FieldValue EventCallback(APTR Value) { return FieldValue(FID_EventCallback, Value); }
constexpr FieldValue EventMask(DEF Value) { return FieldValue(FID_EventMask, int(Value)); }
constexpr FieldValue Flags(DCF Value) { return FieldValue(FID_Flags, int(Value)); }

}


#pragma once

// Name:      svg.h
// Copyright: Paul Manias © 2010-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_SVG (1)

#ifdef __cplusplus
#include <string_view>
#endif

class objSVG;

// SVG flags.

enum class SVF : uint32_t {
   NIL = 0,
   AUTOSCALE = 0x00000001,
   ALPHA = 0x00000002,
   ENFORCE_TRACKING = 0x00000004,
};

DEFINE_ENUM_FLAG_OPERATORS(SVF)

// SVG class definition

#define VER_SVG (1.000000)

// SVG methods

namespace svg {
struct Render { objBitmap *Bitmap; int X; int Y; int Width; int Height; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ParseSymbol { std::string_view ID; objVectorViewport *Viewport; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objSVG : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::SVG;
   static constexpr CSTRING CLASS_NAME = "SVG";

   using create = kt::Create<objSVG>;
   objSVG(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   OBJECTPTR Target;                // Destination container for the generated SVG scene graph elements.
   std::string Path;                // File system path to the source SVG document.
   std::string Title;               // The title of the SVG document.
   std::string Statement;           // String containing complete SVG document markup.
   std::string Colour;              // Defines the default fill to use for currentColor references.
   objVectorViewport * Viewport;    // Reference to the primary VectorViewport containing the SVG document content.
   objVectorScene * Scene;          // Reference to the VectorScene object containing the SVG scene graph.
   int       Frame;                 // Constrains rendering to a specific frame number for frame-based display systems.
   SVF       Flags;                 // Configuration flags that modify SVG processing behaviour.
   int       FrameRate;             // Controls the maximum frame rate for SVG animation playback.

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR dataFeed(OBJECTPTR Object, DATA Datatype, const void *Buffer, int Size) noexcept {
      struct acDataFeed args = { Object, Datatype, Buffer, Size };
      return Action(AC::DataFeed, this, &args);
   }
   inline ERR deactivate() noexcept { return Action(AC::Deactivate, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR saveImage(OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) noexcept {
      struct acSaveImage args = { Dest, { ClassID } };
      return Action(AC::SaveImage, this, &args);
   }
   inline ERR saveToObject(OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) noexcept {
      struct acSaveToObject args = { Dest, { ClassID } };
      return Action(AC::SaveToObject, this, &args);
   }
   inline ERR render(objBitmap * Bitmap, int X, int Y, int Width, int Height) noexcept {
      struct svg::Render args = { Bitmap, X, Y, Width, Height };
      return Action(AC(-1), this, &args);
   }
   inline ERR parseSymbol(const std::string_view &ID, objVectorViewport * Viewport) noexcept {
      struct svg::ParseSymbol args = { ID, Viewport };
      return Action(AC(-2), this, &args);
   }

   // Customised field getting

   inline ERR getTarget(OBJECTPTR &Value) noexcept {
      Value = this->Target;
      return ERR::Okay;
   }

   inline ERR getPath(std::string_view &Value) noexcept {
      Value = this->Path;
      return ERR::Okay;
   }

   inline ERR getTitle(std::string_view &Value) noexcept {
      Value = this->Title;
      return ERR::Okay;
   }

   inline ERR getStatement(std::string_view &Value) noexcept {
      Value = this->Statement;
      return ERR::Okay;
   }

   inline ERR getColour(std::string_view &Value) noexcept {
      Value = this->Colour;
      return ERR::Okay;
   }

   inline ERR getViewport(objVectorViewport * &Value) noexcept {
      Value = this->Viewport;
      return ERR::Okay;
   }

   inline ERR getScene(objVectorScene * &Value) noexcept {
      Value = this->Scene;
      return ERR::Okay;
   }

   inline ERR getFrame(int &Value) noexcept {
      Value = this->Frame;
      return ERR::Okay;
   }

   inline ERR getFlags(SVF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getFrameRate(int &Value) noexcept {
      Value = this->FrameRate;
      return ERR::Okay;
   }

   inline ERR getFrameCallback(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      return get_field(this, Value);
   }


   // Customised field setting

   inline ERR setTarget(OBJECTPTR Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(this, field, 0x08000501, Value);
   }

   inline ERR setPath(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setTitle(const std::string_view &Value) noexcept {
      this->Title = Value;
      return ERR::Okay;
   }

   inline ERR setStatement(const std::string_view &Value) noexcept {
      this->Statement = Value;
      return ERR::Okay;
   }

   inline ERR setColour(const std::string_view &Value) noexcept {
      this->Colour = Value;
      return ERR::Okay;
   }

   inline ERR setFrame(const int Value) noexcept {
      this->Frame = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const SVF Value) noexcept {
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setFrameRate(const int Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setFrameCallback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

};
#pragma once

//********************************************************************************************************************
// These field name and type declarations help to ensure that fields are paired with the correct type during create().
//
// This header depends on declarations from main.h and must not be included directly.

#ifndef KOTUKU_MAIN_H
#error "Include <kotuku/main.h> instead of <kotuku/field_values.hpp>"
#endif

class objBitmap;
class objNetClient;

namespace fl {
   using namespace kt;

// Declares a string_view overload and a null-safe CSTRING overload for a string field helper.

#define KT_FIELD_STRING(FieldName) \
[[nodiscard]] inline FieldValue FieldName(std::string_view Value) { return FieldValue(FID_##FieldName, Value); } \
[[nodiscard]] inline FieldValue FieldName(CSTRING Value) { return FieldValue(FID_##FieldName, Value ? std::string_view(Value) : std::string_view()); }

KT_FIELD_STRING(Path)
KT_FIELD_STRING(Location)
KT_FIELD_STRING(Args)
KT_FIELD_STRING(Fill)
KT_FIELD_STRING(Statement)
KT_FIELD_STRING(Stroke)
KT_FIELD_STRING(String)
KT_FIELD_STRING(Name)
KT_FIELD_STRING(Allow)
KT_FIELD_STRING(Style)
KT_FIELD_STRING(Face)
KT_FIELD_STRING(FileExtension)
KT_FIELD_STRING(FileDescription)
KT_FIELD_STRING(FileHeader)
KT_FIELD_STRING(ArchiveName)
KT_FIELD_STRING(Volume)
KT_FIELD_STRING(DPMS)
KT_FIELD_STRING(Icon)
KT_FIELD_STRING(Procedure)
KT_FIELD_STRING(ButtonOrder)
KT_FIELD_STRING(Points)
KT_FIELD_STRING(Pretext)
KT_FIELD_STRING(Point)

#undef KT_FIELD_STRING

[[nodiscard]] constexpr FieldValue FontSize(double Value) { return FieldValue(FID_FontSize, Value); }
[[nodiscard]] constexpr FieldValue FontSize(int Value) { return FieldValue(FID_FontSize, Value); }
inline FieldValue FontSize(std::string_view Value) { return FieldValue(FID_FontSize, Value); }

[[nodiscard]] constexpr FieldValue ReadOnly(int Value) { return FieldValue(FID_ReadOnly, Value); }
[[nodiscard]] constexpr FieldValue ReadOnly(bool Value) { return FieldValue(FID_ReadOnly, (Value ? 1 : 0)); }

[[nodiscard]] constexpr FieldValue Point(double Value) { return FieldValue(FID_Point, Value); }
[[nodiscard]] constexpr FieldValue Point(int Value) { return FieldValue(FID_Point, Value); }
[[nodiscard]] constexpr FieldValue Acceleration(double Value) { return FieldValue(FID_Acceleration, Value); }
[[nodiscard]] constexpr FieldValue Actions(CPTR Value) { return FieldValue(FID_Actions, Value); }
[[nodiscard]] constexpr FieldValue AmtColours(int Value) { return FieldValue(FID_AmtColours, Value); }
[[nodiscard]] constexpr FieldValue BaseClassID(CLASSID Value) { return FieldValue(FID_BaseClassID, int(Value)); }
[[nodiscard]] constexpr FieldValue Bitmap(objBitmap *Value) { return FieldValue(FID_Bitmap, Value); }
[[nodiscard]] constexpr FieldValue BitsPerPixel(int Value) { return FieldValue(FID_BitsPerPixel, Value); }
[[nodiscard]] constexpr FieldValue BytesPerPixel(int Value) { return FieldValue(FID_BytesPerPixel, Value); }
[[nodiscard]] constexpr FieldValue Category(CCF Value) { return FieldValue(FID_Category, int(Value)); }
[[nodiscard]] constexpr FieldValue ClassID(CLASSID Value) { return FieldValue(FID_ClassID, int(Value)); }
[[nodiscard]] constexpr FieldValue ClassVersion(double Value) { return FieldValue(FID_ClassVersion, Value); }
[[nodiscard]] constexpr FieldValue Client(struct NetClient *Value) { return FieldValue(FID_Client, Value); }
[[nodiscard]] constexpr FieldValue Closed(bool Value) { return FieldValue(FID_Closed, (Value ? 1 : 0)); }
[[nodiscard]] constexpr FieldValue Cursor(PTC Value) { return FieldValue(FID_Cursor, int(Value)); }
[[nodiscard]] constexpr FieldValue DataFlags(MEM Value) { return FieldValue(FID_DataFlags, int(Value)); }
[[nodiscard]] constexpr FieldValue DoubleClick(double Value) { return FieldValue(FID_DoubleClick, Value); }
[[nodiscard]] inline    FieldValue Feedback(const FUNCTION &Value) { return FieldValue(FID_Feedback, Value); }
[[nodiscard]] constexpr FieldValue Feedback(CPTR Value) { return FieldValue(FID_Feedback, Value); }
[[nodiscard]] constexpr FieldValue Fields(const FieldArray *Value) { return FieldValue(FID_Fields, Value, FD_ARRAY); }
[[nodiscard]] constexpr FieldValue Flags(int Value) { return FieldValue(FID_Flags, Value); }
[[nodiscard]] constexpr FieldValue Font(OBJECTPTR Value) { return FieldValue(FID_Font, Value); }
[[nodiscard]] constexpr FieldValue Handle(int Value) { return FieldValue(FID_Handle, Value); }
[[nodiscard]] constexpr FieldValue Handle(APTR Value) { return FieldValue(FID_Handle, Value); }
[[nodiscard]] constexpr FieldValue HostScene(OBJECTPTR Value) { return FieldValue(FID_HostScene, Value); }
[[nodiscard]] inline    FieldValue Incoming(const FUNCTION &Value) { return FieldValue(FID_Incoming, Value); }
[[nodiscard]] constexpr FieldValue Incoming(CPTR Value) { return FieldValue(FID_Incoming, Value); }
[[nodiscard]] constexpr FieldValue Input(CPTR Value) { return FieldValue(FID_Input, Value); }
[[nodiscard]] constexpr FieldValue LineLimit(int Value) { return FieldValue(FID_LineLimit, Value); }
[[nodiscard]] constexpr FieldValue Listener(int Value) { return FieldValue(FID_Listener, Value); }
[[nodiscard]] constexpr FieldValue MatrixColumns(int Value) { return FieldValue(FID_MatrixColumns, Value); }
[[nodiscard]] constexpr FieldValue MatrixRows(int Value) { return FieldValue(FID_MatrixRows, Value); }
[[nodiscard]] constexpr FieldValue MaxHeight(int Value) { return FieldValue(FID_MaxHeight, Value); }
[[nodiscard]] constexpr FieldValue MaxSpeed(double Value) { return FieldValue(FID_MaxSpeed, Value); }
[[nodiscard]] constexpr FieldValue MaxWidth(int Value) { return FieldValue(FID_MaxWidth, Value); }
[[nodiscard]] constexpr FieldValue Methods(const MethodEntry *Value) { return FieldValue(FID_Methods, Value, FD_ARRAY); }
[[nodiscard]] constexpr FieldValue Opacity(double Value) { return FieldValue(FID_Opacity, Value); }
[[nodiscard]] constexpr FieldValue Owner(OBJECTID Value) { return FieldValue(FID_Owner, Value); }
[[nodiscard]] constexpr FieldValue Parent(OBJECTID Value) { return FieldValue(FID_Parent, Value); }
[[nodiscard]] constexpr FieldValue Permissions(PERMIT Value) { return FieldValue(FID_Permissions, int(Value)); }
[[nodiscard]] constexpr FieldValue Image(OBJECTPTR Value) { return FieldValue(FID_Image, Value); }
[[nodiscard]] constexpr FieldValue PopOver(OBJECTID Value) { return FieldValue(FID_PopOver, Value); }
[[nodiscard]] constexpr FieldValue Port(int Value) { return FieldValue(FID_Port, Value); }
[[nodiscard]] constexpr FieldValue RefreshRate(double Value) { return FieldValue(FID_RefreshRate, Value); }
[[nodiscard]] constexpr FieldValue Routine(CPTR Value) { return FieldValue(FID_Routine, Value); }
[[nodiscard]] constexpr FieldValue Size(int Value) { return FieldValue(FID_Size, Value); }
[[nodiscard]] constexpr FieldValue Speed(double Value) { return FieldValue(FID_Speed, Value); }
[[nodiscard]] constexpr FieldValue StrokeWidth(double Value) { return FieldValue(FID_StrokeWidth, Value); }
[[nodiscard]] constexpr FieldValue Surface(OBJECTID Value) { return FieldValue(FID_Surface, Value); }
[[nodiscard]] constexpr FieldValue Target(OBJECTID Value) { return FieldValue(FID_Target, Value); }
[[nodiscard]] constexpr FieldValue Target(OBJECTPTR Value) { return FieldValue(FID_Target, Value); }
[[nodiscard]] constexpr FieldValue ClientData(CPTR Value) { return FieldValue(FID_ClientData, Value); }
[[nodiscard]] constexpr FieldValue Version(double Value) { return FieldValue(FID_Version, Value); }
[[nodiscard]] constexpr FieldValue Viewport(OBJECTID Value) { return FieldValue(FID_Viewport, Value); }
[[nodiscard]] constexpr FieldValue Viewport(OBJECTPTR Value) { return FieldValue(FID_Viewport, Value); }
[[nodiscard]] constexpr FieldValue Weight(int Value) { return FieldValue(FID_Weight, Value); }
[[nodiscard]] constexpr FieldValue WheelSpeed(double Value) { return FieldValue(FID_WheelSpeed, Value); }
[[nodiscard]] constexpr FieldValue WindowHandle(APTR Value) { return FieldValue(FID_WindowHandle, Value); }
[[nodiscard]] constexpr FieldValue WindowHandle(int Value) { return FieldValue(FID_WindowHandle, Value); }

// Template-based Flags are required for strongly typed enums

template <NumericOrEnum T> [[nodiscard]] FieldValue Type(T Value) {
   return FieldValue(FID_Type, int(Value));
}

template <NumericOrEnum T> [[nodiscard]] FieldValue AspectRatio(T Value) {
   return FieldValue(FID_AspectRatio, int(Value));
}

template <NumericOrEnum T> [[nodiscard]] FieldValue BlendMode(T Value) {
   return FieldValue(FID_BlendMode, int(Value));
}

template <NumericOrEnum T> [[nodiscard]] FieldValue ColourSpace(T Value) {
   return FieldValue(FID_ColourSpace, int(Value));
}

template <NumericOrEnum T> [[nodiscard]] FieldValue Flags(T Value) {
   return FieldValue(FID_Flags, int(Value));
}

template <NumericOrEnum T> [[nodiscard]] FieldValue Units(T Value) {
   return FieldValue(FID_Units, int(Value));
}

template <NumericOrEnum T> [[nodiscard]] FieldValue SpreadMethod(T Value) {
   return FieldValue(FID_SpreadMethod, int(Value));
}

template <NumericOrEnum T> [[nodiscard]] FieldValue Visibility(T Value) {
   return FieldValue(FID_Visibility, int(Value));
}

// Dimension fields that accept any arithmetic type but not SCALE values.

template <Numeric T> [[nodiscard]] FieldValue PageWidth(T Value) {
   return FieldValue(FID_PageWidth, Value);
}

template <Numeric T> [[nodiscard]] FieldValue PageHeight(T Value) {
   return FieldValue(FID_PageHeight, Value);
}

template <Numeric T> [[nodiscard]] FieldValue ResX(T Value) {
   return FieldValue(FID_ResX, Value);
}

template <Numeric T> [[nodiscard]] FieldValue ResY(T Value) {
   return FieldValue(FID_ResY, Value);
}

template <Numeric T> [[nodiscard]] FieldValue ViewX(T Value) {
   return FieldValue(FID_ViewX, Value);
}

template <Numeric T> [[nodiscard]] FieldValue ViewY(T Value) {
   return FieldValue(FID_ViewY, Value);
}

template <Numeric T> [[nodiscard]] FieldValue ViewWidth(T Value) {
   return FieldValue(FID_ViewWidth, Value);
}

template <Numeric T> [[nodiscard]] FieldValue ViewHeight(T Value) {
   return FieldValue(FID_ViewHeight, Value);
}

template <NumericOrScale T> [[nodiscard]] FieldValue Radius(T Value) {
   return FieldValue(FID_Radius, Value);
}

template <NumericOrScale T> [[nodiscard]] FieldValue CenterX(T Value) {
   return FieldValue(FID_CenterX, Value);
}

template <NumericOrScale T> [[nodiscard]] FieldValue CenterY(T Value) {
   return FieldValue(FID_CenterY, Value);
}

template <NumericOrScale T> [[nodiscard]] FieldValue FX(T Value) {
   return FieldValue(FID_FX, Value);
}

template <NumericOrScale T> [[nodiscard]] FieldValue FY(T Value) {
   return FieldValue(FID_FY, Value);
}

template <NumericOrScale T> [[nodiscard]] FieldValue Width(T Value) {
   return FieldValue(FID_Width, Value);
}

template <NumericOrScale T> [[nodiscard]] FieldValue Height(T Value) {
   return FieldValue(FID_Height, Value);
}

template <NumericOrScale T> [[nodiscard]] FieldValue X(T Value) {
   return FieldValue(FID_X, Value);
}

template <NumericOrScale T> [[nodiscard]] FieldValue XOffset(T Value) {
   return FieldValue(FID_XOffset, Value);
}

template <NumericOrScale T> [[nodiscard]] FieldValue Y(T Value) {
   return FieldValue(FID_Y, Value);
}

template <NumericOrScale T> [[nodiscard]] FieldValue YOffset(T Value) {
   return FieldValue(FID_YOffset, Value);
}

template <NumericOrScale T> [[nodiscard]] FieldValue X1(T Value) {
   return FieldValue(FID_X1, Value);
}

template <NumericOrScale T> [[nodiscard]] FieldValue Y1(T Value) {
   return FieldValue(FID_Y1, Value);
}

template <NumericOrScale T> [[nodiscard]] FieldValue X2(T Value) {
   return FieldValue(FID_X2, Value);
}

template <NumericOrScale T> [[nodiscard]] FieldValue Y2(T Value) {
   return FieldValue(FID_Y2, Value);
}

}

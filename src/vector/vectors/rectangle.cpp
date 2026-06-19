/*********************************************************************************************************************

-CLASS-
VectorRectangle: Extends the Vector class with support for generating rectangles.

VectorRectangle extends the @Vector class with the ability to generate rectangular paths.

-END-

*********************************************************************************************************************/

static constexpr DMF RECTANGLE_RADIUS_FLAGS = DMF::FIXED_RADIUS_X|DMF::FIXED_RADIUS_Y|
   DMF::SCALED_RADIUS_X|DMF::SCALED_RADIUS_Y;

inline double rect_fixed_width(extVectorRectangle *Vector, const Unit &Value) {
   return Value.scaled() ? double(Value) * get_parent_width(Vector) : double(Value);
}

inline double rect_fixed_height(extVectorRectangle *Vector, const Unit &Value) {
   return Value.scaled() ? double(Value) * get_parent_height(Vector) : double(Value);
}

struct RectangleGeometry {
   double x, y, width, height;
};

static RectangleGeometry get_rectangle_geometry(extVectorRectangle *Vector)
{
   double x, y, width, height;
   const double parent_width  = get_parent_width(Vector);
   const double parent_height = get_parent_height(Vector);

   if (Vector->rX.defined()) x = rect_fixed_width(Vector, Vector->rX);
   else if ((Vector->rWidth.defined()) and (Vector->rXOffset.defined())) {
      width = rect_fixed_width(Vector, Vector->rWidth);
      x = parent_width - width - rect_fixed_width(Vector, Vector->rXOffset);
   }
   else x = 0;

   if (Vector->rY.defined()) y = rect_fixed_height(Vector, Vector->rY);
   else if ((Vector->rHeight.defined()) and (Vector->rYOffset.defined())) {
      height = rect_fixed_height(Vector, Vector->rHeight);
      y = parent_height - height - rect_fixed_height(Vector, Vector->rYOffset);
   }
   else y = 0;

   if (Vector->rWidth.defined()) width = rect_fixed_width(Vector, Vector->rWidth);
   else if (Vector->rXOffset.defined()) {
      if (not Vector->rX.defined()) x = 0;
      width = parent_width - rect_fixed_width(Vector, Vector->rXOffset) - x;
   }
   else width = parent_width;

   if (Vector->rHeight.defined()) height = rect_fixed_height(Vector, Vector->rHeight);
   else if (Vector->rYOffset.defined()) {
      if (not Vector->rY.defined()) y = 0;
      height = parent_height - rect_fixed_height(Vector, Vector->rYOffset) - y;
   }
   else height = parent_height;

   return { x, y, width, height };
}

static void generate_rectangle(extVectorRectangle *Vector, agg::path_storage &Path)
{
   auto geometry = get_rectangle_geometry(Vector);
   double x      = geometry.x;
   double y      = geometry.y;
   double width  = geometry.width;
   double height = geometry.height;

   if (Vector->rFullControl) {
      // Full control of rounded corners has been requested by the client (four X,Y coordinate pairs).
      // Coordinates are either ALL scaled or ALL fixed, not a mix of both.
      // This feature is not SVG compliant.

      const double diagonal = sqrt((width * width) + (height * height)) * INV_SQRT2;

      double rx[4], ry[4];
      for (unsigned i=0; i < 4; i++) {
         const auto &round_x = Vector->rRound[i].x;
         const auto &round_y = Vector->rRound[i].y;

         rx[i] = round_x.scaled() ? double(round_x) * diagonal : double(round_x);
         ry[i] = round_y.scaled() ? double(round_y) * diagonal : double(round_y);
         if (rx[i] > width * 0.5) rx[i] = width * 0.5;
         if (ry[i] > height * 0.5) ry[i] = height * 0.5;
      }

      // Top left -> Top right
      Path.move_to(x+rx[0], y);
      Path.line_to(x+width-rx[1], y);
      Path.arc_to(rx[1], ry[1], 0 /* angle */, 0 /* large */, 1 /* sweep */, x+width, y+ry[1]);

      // Top right -> Bottom right
      Path.line_to(x+width, y+height-ry[1]);
      Path.arc_to(rx[2], ry[2], 0, 0, 1, x+width-rx[2], y+height);

      // Bottom right -> Bottom left
      Path.line_to(x+rx[2], y+height);
      Path.arc_to(rx[3], ry[3], 0, 0, 1, x, y+height-ry[3]);

      // Bottom left -> Top left
      Path.line_to(x, y+ry[3]);
      Path.arc_to(rx[0], ry[0], 0, 0, 1, x+rx[0], y);

      Path.close_polygon();
   }
   else if (Vector->rRound[0].x.defined() or Vector->rRound[0].y.defined()) {
      // Simple rounded rectangle (all four corners identical)
      //
      // SVG rules that RX will also apply to RY unless RY != 0.
      // If RX is greater than width/2, set RX to width/2.  Same for RY on the vertical axis.
      // If RX or RY is explicitly defined as zero by the client, the corners are square.

      Unit rx = Vector->rRound[0].x, ry = Vector->rRound[0].y;
      bool x_defined = Vector->rRound[0].x.defined();
      bool y_defined = Vector->rRound[0].y.defined();

      if (rx.scaled()) rx = rx * sqrt((width * width) + (height * height)) * INV_SQRT2;
      if (ry.scaled()) ry = ry * sqrt((width * width) + (height * height)) * INV_SQRT2;
      if (not x_defined) { rx = ry; x_defined = true; }
      if (not y_defined) { ry = rx; y_defined = true; }

      rx = std::clamp<double>(rx, 0.0, width * 0.5);
      ry = std::clamp<double>(ry, 0.0, height * 0.5);

      if ((rx IS 0) or (ry IS 0)) {
         Path.move_to(x, y);
         Path.line_to(x+width, y);
         Path.line_to(x+width, y+height);
         Path.line_to(x, y+height);
         Path.close_polygon();
      }
      else {
         // Top left -> Top right
         Path.move_to(x+rx, y);
         Path.line_to(x+width-rx, y);
         Path.arc_to(rx, ry, 0 /* angle */, 0 /* large */, 1 /* sweep */, x+width, y+ry);

         // Top right -> Bottom right
         Path.line_to(x+width, y+height-ry);
         Path.arc_to(rx, ry, 0, 0, 1, x+width-rx, y+height);

         // Bottom right -> Bottom left
         Path.line_to(x+rx, y+height);
         Path.arc_to(rx, ry, 0, 0, 1, x, y+height-ry);

         // Bottom left -> Top left
         Path.line_to(x, y+ry);
         Path.arc_to(rx, ry, 0, 0, 1, x+rx, y);
      }
      Path.close_polygon();
   }
   else {
      Path.move_to(x, y);
      Path.line_to(x+width, y);
      Path.line_to(x+width, y+height);
      Path.line_to(x, y+height);
      Path.close_polygon();
   }

   Vector->Bounds = { x, y, x + width, y + height };

   // SVG rules stipulate that a rectangle missing a dimension won't be drawn, but it does maintain a bounding box.

   if ((not width) or (not height)) Vector->ValidState = false;
   else Vector->ValidState = true;
}

/*********************************************************************************************************************
-ACTION-
Move: Moves the vector to a new position.

*********************************************************************************************************************/

static ERR VECTORRECTANGLE_Move(extVectorRectangle *Self, struct acMove *Args)
{
   if (not Args) return ERR::NullArgs;

   const auto geometry = get_rectangle_geometry(Self);

   if (Self->rX.scaled()) Self->rX.set(rect_fixed_width(Self, Self->rX));
   else if (not Self->rX.defined()) Self->rX = geometry.x;

   if (Self->rY.scaled()) Self->rY.set(rect_fixed_height(Self, Self->rY));
   else if (not Self->rY.defined()) Self->rY = geometry.y;

   Self->rX = Unit(Self->rX + Args->DeltaX);
   Self->rY = Unit(Self->rY + Args->DeltaY);
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
MoveToPoint: Moves the vector to a new fixed position.

*********************************************************************************************************************/

static ERR VECTORRECTANGLE_MoveToPoint(extVectorRectangle *Self, struct acMoveToPoint *Args)
{
   if (not Args) return ERR::NullArgs;

   if ((Args->Flags & MTF::RELATIVE) != MTF::NIL) {
      if ((Args->Flags & MTF::X) != MTF::NIL) Self->rX = Unit(Args->X, FD_SCALED);
      if ((Args->Flags & MTF::Y) != MTF::NIL) Self->rY = Unit(Args->Y, FD_SCALED);
   }
   else {
      if ((Args->Flags & MTF::X) != MTF::NIL) Self->rX = Unit(Args->X);
      if ((Args->Flags & MTF::Y) != MTF::NIL) Self->rY = Unit(Args->Y);
   }
   reset_path(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORRECTANGLE_NewObject(extVectorRectangle *Self)
{
   Self->GeneratePath = (void (*)(extVector *, agg::path_storage &))&generate_rectangle;
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Resize: Changes the rectangle dimensions.

*********************************************************************************************************************/

static ERR VECTORRECTANGLE_Resize(extVectorRectangle *Self, struct acResize *Args)
{
   if (not Args) return ERR::NullArgs;

   Self->rWidth = Unit(Args->Width);
   Self->rHeight = Unit(Args->Height);
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Height: The height of the rectangle.  Can be expressed as a fixed or scaled coordinate.

The height of the rectangle is defined here as either a fixed or scaled value.  Negative values are permitted (this
will flip the rectangle on the vertical axis).

*********************************************************************************************************************/

static ERR VECTORRECTANGLE_GET_Height(extVectorRectangle *Self, Unit &Value)
{
   if (Self->rHeight.defined()) Value= Self->rHeight;
   else Value = Unit(get_rectangle_geometry(Self).height);
   return ERR::Okay;
}

static ERR VECTORRECTANGLE_SET_Height(extVectorRectangle *Self, Unit &Value)
{
   Self->rHeight = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Rounding: Precisely controls rounded corner positioning.

Set the Rounding field if all four corners of the rectangle need to be precisely controlled.  Four X,Y sizing
pairs must be provided in sequence, with the first describing the top-left corner and proceeding in clockwise fashion.
Each pair of values is equivalent to a #RoundX,#RoundY definition for that corner only.

*********************************************************************************************************************/

static ERR VECTORRECTANGLE_GET_Rounding(extVectorRectangle *Self, std::span<Unit> &Array)
{
   Array = std::span<Unit>((Unit *)Self->rRound.data(), 8);
   return ERR::Okay;
}

static ERR VECTORRECTANGLE_SET_Rounding(extVectorRectangle *Self, std::span<const Unit> &Array)
{
   if (Array.size() IS 8) {
      for (unsigned i=0; i < 4; i++) {
         Self->rRound[i].x = Array[i * 2];
         Self->rRound[i].y = Array[(i * 2) + 1];
      }

      Self->rFullControl = true;
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************

-FIELD-
RoundX: Specifies the size of rounded corners on the horizontal axis.

The corners of a rectangle can be rounded by setting the RoundX and RoundY values.  Each value is interpreted as a
radius along the relevant axis.  A value of zero (the default) turns off this feature.

*********************************************************************************************************************/

static ERR VECTORRECTANGLE_GET_RoundX(extVectorRectangle *Self, Unit &Value)
{
   Value = Self->rRound[0].x;
   return ERR::Okay;
}

static ERR VECTORRECTANGLE_SET_RoundX(extVectorRectangle *Self, Unit &Value)
{
   if ((Value < 0) or (Value > 1000)) return ERR::OutOfRange;

   Self->rRound[0].x = Self->rRound[1].x = Self->rRound[2].x = Self->rRound[3].x = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
RoundY: Specifies the size of rounded corners on the vertical axis.

The corners of a rectangle can be rounded by setting the RoundX and RoundY values.  Each value is interpreted as a
radius along the relevant axis.  A value of zero (the default) turns off this feature.

*********************************************************************************************************************/

static ERR VECTORRECTANGLE_GET_RoundY(extVectorRectangle *Self, Unit &Value)
{
   Value = Self->rRound[0].y;
   return ERR::Okay;
}

static ERR VECTORRECTANGLE_SET_RoundY(extVectorRectangle *Self, Unit &Value)
{
   if ((Value < 0) or (Value > 1000)) return ERR::OutOfRange;
   Self->rRound[0].y = Self->rRound[1].y = Self->rRound[2].y = Self->rRound[3].y = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
X: The left-side of the rectangle.  Can be expressed as a fixed or scaled coordinate.
-END-

*********************************************************************************************************************/

static ERR VECTORRECTANGLE_GET_X(extVectorRectangle *Self, Unit &Value)
{
   if (Self->rX.defined()) Value = Self->rX;
   else Value = Unit(get_rectangle_geometry(Self).x);
   return ERR::Okay;
}

static ERR VECTORRECTANGLE_SET_X(extVectorRectangle *Self, Unit &Value)
{
   Self->rX = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XOffset: The right-side of the rectangle, expressed as a fixed or scaled offset value.
-END-

*********************************************************************************************************************/

static ERR VECTORRECTANGLE_GET_XOffset(extVectorRectangle *Self, Unit &Value)
{
   double value = 0;

   if (Self->rXOffset.defined()) value = rect_fixed_width(Self, Self->rXOffset);
   else if ((Self->rX.defined()) and (Self->rWidth.defined())) {
      const double width = rect_fixed_width(Self, Self->rWidth);
      value = get_parent_width(Self) - (rect_fixed_width(Self, Self->rX) + width);
   }
   else value = 0;

   if ((Value.scaled()) and (get_parent_width(Self) != 0)) Value = Unit(value / get_parent_width(Self), FD_SCALED);
   else Value = Unit(value);

   return ERR::Okay;
}

static ERR VECTORRECTANGLE_SET_XOffset(extVectorRectangle *Self, Unit &Value)
{
   Self->rXOffset = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Width: The width of the rectangle.  Can be expressed as a fixed or scaled coordinate.

The width of the rectangle is defined here as either a fixed or scaled value.  Negative values are permitted (this
will flip the rectangle on the horizontal axis).

*********************************************************************************************************************/

static ERR VECTORRECTANGLE_GET_Width(extVectorRectangle *Self, Unit &Value)
{
   if (Self->rWidth.defined()) Value = Self->rWidth;
   else Value = Unit(get_rectangle_geometry(Self).width);
   return ERR::Okay;
}

static ERR VECTORRECTANGLE_SET_Width(extVectorRectangle *Self, Unit &Value)
{
   Self->rWidth = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Y: The top of the rectangle.  Can be expressed as a fixed or scaled coordinate.
-END-

*********************************************************************************************************************/

static ERR VECTORRECTANGLE_GET_Y(extVectorRectangle *Self, Unit &Value)
{
   if (Self->rY.defined()) Value = Unit(Self->rY);
   else Value = Unit(get_rectangle_geometry(Self).y);
   return ERR::Okay;
}

static ERR VECTORRECTANGLE_SET_Y(extVectorRectangle *Self, Unit &Value)
{
   Self->rY = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
YOffset: The bottom of the rectangle, expressed as a fixed or scaled offset value.
-END-

*********************************************************************************************************************/

static ERR VECTORRECTANGLE_GET_YOffset(extVectorRectangle *Self, Unit &Value)
{
   double value = 0;

   if (Self->rYOffset.defined()) value = rect_fixed_height(Self, Self->rYOffset);
   else if ((Self->rY.defined()) and (Self->rHeight.defined())) {
      const double height = rect_fixed_height(Self, Self->rHeight);
      value = get_parent_height(Self) - (rect_fixed_height(Self, Self->rY) + height);
   }
   else value = 0;

   if ((Value.scaled()) and (get_parent_height(Self) != 0)) Value = Unit(value / get_parent_height(Self), FD_SCALED);
   else Value = Unit(value);
   return ERR::Okay;
}

static ERR VECTORRECTANGLE_SET_YOffset(extVectorRectangle *Self, Unit &Value)
{
   Self->rYOffset = Value;
   reset_path(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

#include "rectangle_def.cpp"

static const FieldArray clRectangleFields[] = {
   { "Rounding",   FDF_VIRTUAL|FDF_UNIT|FDF_ARRAY|FDF_RW|FDF_PURE, VECTORRECTANGLE_GET_Rounding, VECTORRECTANGLE_SET_Rounding },
   { "RoundX",     FDF_VIRTUAL|FD_UNIT|FDF_SCALED|FDF_RW|FDF_PURE, VECTORRECTANGLE_GET_RoundX, VECTORRECTANGLE_SET_RoundX },
   { "RoundY",     FDF_VIRTUAL|FD_UNIT|FDF_SCALED|FDF_RW|FDF_PURE, VECTORRECTANGLE_GET_RoundY, VECTORRECTANGLE_SET_RoundY },
   { "X",          FDF_VIRTUAL|FD_UNIT|FDF_SCALED|FDF_RW|FDF_PURE, VECTORRECTANGLE_GET_X, VECTORRECTANGLE_SET_X },
   { "Y",          FDF_VIRTUAL|FD_UNIT|FDF_SCALED|FDF_RW|FDF_PURE, VECTORRECTANGLE_GET_Y, VECTORRECTANGLE_SET_Y },
   { "XOffset",    FDF_VIRTUAL|FD_UNIT|FDF_SCALED|FDF_RW, VECTORRECTANGLE_GET_XOffset, VECTORRECTANGLE_SET_XOffset },
   { "YOffset",    FDF_VIRTUAL|FD_UNIT|FDF_SCALED|FDF_RW, VECTORRECTANGLE_GET_YOffset, VECTORRECTANGLE_SET_YOffset },
   { "Width",      FDF_VIRTUAL|FD_UNIT|FDF_SCALED|FDF_RW|FDF_PURE, VECTORRECTANGLE_GET_Width, VECTORRECTANGLE_SET_Width },
   { "Height",     FDF_VIRTUAL|FD_UNIT|FDF_SCALED|FDF_RW|FDF_PURE, VECTORRECTANGLE_GET_Height, VECTORRECTANGLE_SET_Height },
   END_FIELD
};

static ERR init_rectangle(void)
{
   clVectorRectangle = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTOR),
      fl::ClassID(CLASSID::VECTORRECTANGLE),
      fl::Name("VectorRectangle"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clVectorRectangleActions),
      fl::Fields(clRectangleFields),
      fl::Size(sizeof(extVectorRectangle)),
      fl::Path(MOD_PATH));

   return clVectorRectangle ? ERR::Okay : ERR::AddClass;
}

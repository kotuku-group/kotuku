/*********************************************************************************************************************

-CLASS-
VectorRectangle: Extends the Vector class with support for generating rectangles.

VectorRectangle extends the @Vector class with the ability to generate rectangular paths.

-END-

*********************************************************************************************************************/

static void generate_rectangle(extVectorRectangle *Vector, agg::path_storage &Path)
{
   double x, y, width, height;

   if (dmf::hasX(Vector->rDimensions)) x = Vector->rX;
   else if (dmf::hasScaledX(Vector->rDimensions)) x = Vector->rX * get_parent_width(Vector);
   else if ((dmf::hasAnyWidth(Vector->rDimensions)) and (dmf::hasAnyXOffset(Vector->rDimensions))) {
      if (dmf::hasWidth(Vector->rDimensions)) width = Vector->rWidth;
      else width = get_parent_width(Vector) * Vector->rWidth;

      if (dmf::hasXOffset(Vector->rDimensions)) x = get_parent_width(Vector) - width - Vector->rXOffset;
      else x = get_parent_width(Vector) - width - (get_parent_width(Vector) * Vector->rXOffset);
   }
   else x = 0;

   if (dmf::hasY(Vector->rDimensions)) y = Vector->rY;
   else if (dmf::hasScaledY(Vector->rDimensions)) y = Vector->rY * get_parent_height(Vector);
   else if ((dmf::hasAnyHeight(Vector->rDimensions)) and (dmf::hasAnyYOffset(Vector->rDimensions))) {
      if (dmf::hasHeight(Vector->rDimensions)) height = Vector->rHeight;
      else height = get_parent_height(Vector) * Vector->rHeight;

      if (dmf::hasYOffset(Vector->rDimensions)) y = get_parent_height(Vector) - height - Vector->rYOffset;
      else y = get_parent_height(Vector) - height - (get_parent_height(Vector) * Vector->rYOffset);
   }
   else y = 0;

   if (dmf::hasWidth(Vector->rDimensions)) width = Vector->rWidth;
   else if (dmf::hasScaledWidth(Vector->rDimensions)) width = Vector->rWidth * get_parent_width(Vector);
   else if (dmf::hasXOffset(Vector->rDimensions)) {
      if (dmf::hasScaledX(Vector->rDimensions)) x = Vector->rX * get_parent_width(Vector);
      else x = Vector->rX;

      if (dmf::hasXOffset(Vector->rDimensions)) width = get_parent_width(Vector) - Vector->rXOffset - x;
      else width = get_parent_width(Vector) - (Vector->rXOffset * get_parent_width(Vector)) - x;
   }
   else width = get_parent_width(Vector);

   if (dmf::hasHeight(Vector->rDimensions)) height = Vector->rHeight;
   else if (dmf::hasScaledHeight(Vector->rDimensions)) height = Vector->rHeight * get_parent_height(Vector);
   else if (dmf::hasYOffset(Vector->rDimensions)) {
      if (dmf::hasScaledY(Vector->rDimensions)) y = Vector->rY * get_parent_height(Vector);
      else y = Vector->rY;

      if (dmf::hasYOffset(Vector->rDimensions)) height = get_parent_height(Vector) - Vector->rYOffset - y;
      else height = get_parent_height(Vector) - (Vector->rYOffset * get_parent_height(Vector)) - y;
   }
   else height = get_parent_height(Vector);

   if (Vector->rFullControl) {
      // Full control of rounded corners has been requested by the client (four X,Y coordinate pairs).
      // Coordinates are either ALL scaled or ALL fixed, not a mix of both.
      // This feature is not SVG compliant.

      double scale_x = 1.0, scale_y = 1.0;

      if (dmf::hasScaledRadiusX(Vector->rDimensions)) {
         scale_x = sqrt((width * width) + (height * height)) * INV_SQRT2;
      }

      if (dmf::hasScaledRadiusY(Vector->rDimensions)) {
         if (scale_x != 1.0) scale_x = scale_x;
         else scale_y = sqrt((width * width) + (height * height)) * INV_SQRT2;
      }

      double rx[4], ry[4];
      for (unsigned i=0; i < 4; i++) {
         rx[i] = Vector->rRound[i].x * scale_x;
         ry[i] = Vector->rRound[i].y * scale_y;
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
   else if ((Vector->rDimensions & (DMF::FIXED_RADIUS_X|DMF::FIXED_RADIUS_Y|DMF::SCALED_RADIUS_X|DMF::SCALED_RADIUS_Y)) != DMF::NIL) {
      // SVG rules that RX will also apply to RY unless RY != 0.
      // If RX is greater than width/2, set RX to width/2.  Same for RY on the vertical axis.
      // If RX or RY is explicitly defined as zero by the client, the corners are square.

      double rx = Vector->rRound[0].x, ry = Vector->rRound[0].y;
      bool x_defined = dmf::hasScaledRadiusX(Vector->rDimensions) or dmf::hasRadiusX(Vector->rDimensions);
      bool y_defined = dmf::hasScaledRadiusY(Vector->rDimensions) or dmf::hasRadiusY(Vector->rDimensions);

      if (dmf::hasScaledRadiusX(Vector->rDimensions)) {
         rx *= sqrt((width * width) + (height * height)) * INV_SQRT2;
      }

      if (dmf::hasScaledRadiusY(Vector->rDimensions)) {
         ry *= sqrt((width * width) + (height * height)) * INV_SQRT2;
      }

      if (!x_defined) { rx = ry; x_defined = true; }
      if (!y_defined) { ry = rx; y_defined = true; }

      rx = std::clamp(rx, 0.0, width * 0.5);
      ry = std::clamp(ry, 0.0, height * 0.5);

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

   if ((!width) or (!height)) Vector->ValidState = false;
   else Vector->ValidState = true;
}

/*********************************************************************************************************************
-ACTION-
Move: Moves the vector to a new position.

*********************************************************************************************************************/

static ERR RECTANGLE_Move(extVectorRectangle *Self, struct acMove *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   Self->rX += Args->DeltaX;
   Self->rY += Args->DeltaY;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
MoveToPoint: Moves the vector to a new fixed position.

*********************************************************************************************************************/

static ERR RECTANGLE_MoveToPoint(extVectorRectangle *Self, struct acMoveToPoint *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   if ((Args->Flags & MTF::X) != MTF::NIL) Self->rX = Args->X;
   if ((Args->Flags & MTF::Y) != MTF::NIL) Self->rY = Args->Y;
   if ((Args->Flags & MTF::RELATIVE) != MTF::NIL) Self->rDimensions = (Self->rDimensions | DMF::SCALED_X | DMF::SCALED_Y) & ~(DMF::FIXED_X | DMF::FIXED_Y);
   else Self->rDimensions = (Self->rDimensions | DMF::FIXED_X | DMF::FIXED_Y) & ~(DMF::SCALED_X | DMF::SCALED_Y);
   reset_path(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR RECTANGLE_NewObject(extVectorRectangle *Self)
{
   Self->GeneratePath = (void (*)(extVector *, agg::path_storage &))&generate_rectangle;
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Resize: Changes the rectangle dimensions.

*********************************************************************************************************************/

static ERR RECTANGLE_Resize(extVectorRectangle *Self, struct acResize *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   Self->rWidth = Args->Width;
   Self->rHeight = Args->Height;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Dimensions: Dimension flags define whether individual dimension fields contain fixed or scaled values.

The following dimension flags are supported:

<types lookup="DMF">
<type name="FIXED_HEIGHT">The #Height value is a fixed coordinate.</>
<type name="FIXED_WIDTH">The #Width value is a fixed coordinate.</>
<type name="FIXED_X">The #X value is a fixed coordinate.</>
<type name="FIXED_Y">The #Y value is a fixed coordinate.</>
<type name="FIXED_RADIUS_X">The #RoundX value is a fixed coordinate.</>
<type name="FIXED_RADIUS_Y">The #RoundY value is a fixed coordinate.</>
<type name="SCALED_HEIGHT">The #Height value is a scaled coordinate.</>
<type name="SCALED_WIDTH">The #Width value is a scaled coordinate.</>
<type name="SCALED_X">The #X value is a scaled coordinate.</>
<type name="SCALED_Y">The #Y value is a scaled coordinate.</>
<type name="SCALED_RADIUS_X">The #RoundX value is a scaled coordinate.</>
<type name="SCALED_RADIUS_Y">The #RoundY value is a scaled coordinate.</>
</types>

*********************************************************************************************************************/

static ERR RECTANGLE_GET_Dimensions(extVectorRectangle *Self, DMF *Value)
{
   *Value = Self->rDimensions;
   return ERR::Okay;
}

static ERR RECTANGLE_SET_Dimensions(extVectorRectangle *Self, DMF Value)
{
   Self->rDimensions = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Height: The height of the rectangle.  Can be expressed as a fixed or scaled coordinate.

The height of the rectangle is defined here as either a fixed or scaled value.  Negative values are permitted (this
will flip the rectangle on the vertical axis).

*********************************************************************************************************************/

static ERR RECTANGLE_GET_Height(extVectorRectangle *Self, Unit *Value)
{
   Value->set(Self->rHeight);
   return ERR::Okay;
}

static ERR RECTANGLE_SET_Height(extVectorRectangle *Self, Unit &Value)
{
   if (Value.scaled()) Self->rDimensions = (Self->rDimensions | DMF::SCALED_HEIGHT) & (~DMF::FIXED_HEIGHT);
   else Self->rDimensions = (Self->rDimensions | DMF::FIXED_HEIGHT) & (~DMF::SCALED_HEIGHT);
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

By default, values will be treated as fixed pixel units.  They can be changed to scaled values by defining the
`DMF::SCALED_RADIUS_X` and/or `DMF::SCALED_RADIUS_Y` flags in the #Dimensions field.  The scale is calculated
against the rectangle's diagonal.

*********************************************************************************************************************/

static ERR RECTANGLE_GET_Rounding(extVectorRectangle *Self, double **Value, int *Elements)
{
   *Value = (double *)Self->rRound.data();
   *Elements = 8;
   return ERR::Okay;
}

static ERR RECTANGLE_SET_Rounding(extVectorRectangle *Self, double *Value, int Elements)
{
   if (Elements >= 8) {
      copymem(Value, Self->rRound.data(), sizeof(double) * 8);
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

static ERR RECTANGLE_GET_RoundX(extVectorRectangle *Self, Unit *Value)
{
   Value->set(Self->rRound[0].x);
   return ERR::Okay;
}

static ERR RECTANGLE_SET_RoundX(extVectorRectangle *Self, Unit &Value)
{
   if ((Value < 0) or (Value > 1000)) return ERR::OutOfRange;

   if (Value.scaled()) Self->rDimensions = (Self->rDimensions | DMF::SCALED_RADIUS_X) & (~DMF::FIXED_RADIUS_X);
   else Self->rDimensions = (Self->rDimensions | DMF::FIXED_RADIUS_X) & (~DMF::SCALED_RADIUS_X);

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

static ERR RECTANGLE_GET_RoundY(extVectorRectangle *Self, Unit *Value)
{
   Value->set(Self->rRound[0].y);
   return ERR::Okay;
}

static ERR RECTANGLE_SET_RoundY(extVectorRectangle *Self, Unit &Value)
{
   if ((Value < 0) or (Value > 1000)) return ERR::OutOfRange;
   if (Value.scaled()) Self->rDimensions = (Self->rDimensions | DMF::SCALED_RADIUS_Y) & (~DMF::FIXED_RADIUS_Y);
   else Self->rDimensions = (Self->rDimensions | DMF::FIXED_RADIUS_Y) & (~DMF::SCALED_RADIUS_Y);
   Self->rRound[0].y = Self->rRound[1].y = Self->rRound[2].y = Self->rRound[3].y = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
X: The left-side of the rectangle.  Can be expressed as a fixed or scaled coordinate.
-END-

*********************************************************************************************************************/

static ERR RECTANGLE_GET_X(extVectorRectangle *Self, Unit *Value)
{
   Value->set(Self->rX);
   return ERR::Okay;
}

static ERR RECTANGLE_SET_X(extVectorRectangle *Self, Unit &Value)
{
   if (Value.scaled()) Self->rDimensions = (Self->rDimensions | DMF::SCALED_X) & (~DMF::FIXED_X);
   else Self->rDimensions = (Self->rDimensions | DMF::FIXED_X) & (~DMF::SCALED_X);
   Self->rX = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XOffset: The right-side of the rectangle, expressed as a fixed or scaled offset value.
-END-

*********************************************************************************************************************/

static ERR RECTANGLE_GET_XOffset(extVectorRectangle *Self, Unit *Value)
{
   double value = 0;

   if (dmf::hasXOffset(Self->rDimensions)) value = Self->rXOffset;
   else if (dmf::hasScaledXOffset(Self->rDimensions)) {
      value = Self->rXOffset * get_parent_width(Self);
   }
   else if ((dmf::hasAnyX(Self->rDimensions)) and (dmf::hasAnyWidth(Self->rDimensions))) {
      double width;
      if (dmf::hasWidth(Self->rDimensions)) width = Self->rWidth;
      else width = get_parent_width(Self) * Self->rWidth;

      if (dmf::hasX(Self->rDimensions)) value = get_parent_width(Self) - (Self->rX + width);
      else value = get_parent_width(Self) - ((Self->rX * get_parent_width(Self)) + width);
   }
   else value = 0;

   if (Value->scaled()) value = value / get_parent_width(Self);

   Value->set(value);

   return ERR::Okay;
}

static ERR RECTANGLE_SET_XOffset(extVectorRectangle *Self, Unit &Value)
{
   Self->rXOffset = Value;
   if (Value.scaled()) Self->rDimensions = (Self->rDimensions | DMF::SCALED_X_OFFSET) & (~DMF::FIXED_X_OFFSET);
   else Self->rDimensions = (Self->rDimensions | DMF::FIXED_X_OFFSET) & (~DMF::SCALED_X_OFFSET);
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Width: The width of the rectangle.  Can be expressed as a fixed or scaled coordinate.

The width of the rectangle is defined here as either a fixed or scaled value.  Negative values are permitted (this
will flip the rectangle on the horizontal axis).

*********************************************************************************************************************/

static ERR RECTANGLE_GET_Width(extVectorRectangle *Self, Unit *Value)
{
   Value->set(Self->rWidth);
   return ERR::Okay;
}

static ERR RECTANGLE_SET_Width(extVectorRectangle *Self, Unit &Value)
{
   Self->rWidth = Value;
   if (Value.scaled()) Self->rDimensions = (Self->rDimensions | DMF::SCALED_WIDTH) & (~DMF::FIXED_WIDTH);
   else Self->rDimensions = (Self->rDimensions | DMF::FIXED_WIDTH) & (~DMF::SCALED_WIDTH);
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Y: The top of the rectangle.  Can be expressed as a fixed or scaled coordinate.
-END-

*********************************************************************************************************************/

static ERR RECTANGLE_GET_Y(extVectorRectangle *Self, Unit *Value)
{
   Value->set(Self->rY);
   return ERR::Okay;
}

static ERR RECTANGLE_SET_Y(extVectorRectangle *Self, Unit &Value)
{
   if (Value.scaled()) Self->rDimensions = (Self->rDimensions | DMF::SCALED_Y) & (~DMF::FIXED_Y);
   else Self->rDimensions = (Self->rDimensions | DMF::FIXED_Y) & (~DMF::SCALED_Y);
   Self->rY = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
YOffset: The bottom of the rectangle, expressed as a fixed or scaled offset value.
-END-

*********************************************************************************************************************/

static ERR RECTANGLE_GET_YOffset(extVectorRectangle *Self, Unit *Value)
{
   double value = 0;

   if (dmf::hasYOffset(Self->rDimensions)) value = Self->rYOffset;
   else if (dmf::hasScaledYOffset(Self->rDimensions)) {
      value = Self->rYOffset * get_parent_height(Self);
   }
   else if ((dmf::hasAnyY(Self->rDimensions)) and (dmf::hasAnyHeight(Self->rDimensions))) {
      double height;
      if (dmf::hasHeight(Self->rDimensions)) height = Self->rHeight;
      else height = get_parent_height(Self) * Self->rHeight;

      if (dmf::hasY(Self->rDimensions)) value = get_parent_height(Self) - (Self->rY + height);
      else value = get_parent_height(Self) - ((Self->rY * get_parent_height(Self)) + height);
   }
   else value = 0;

   if (Value->scaled()) Value->set(value / get_parent_height(Self));
   else Value->set(value);
   return ERR::Okay;
}

static ERR RECTANGLE_SET_YOffset(extVectorRectangle *Self, Unit &Value)
{
   Self->rYOffset = Value;
   if (Value.scaled()) Self->rDimensions = (Self->rDimensions | DMF::SCALED_Y_OFFSET) & (~DMF::FIXED_Y_OFFSET);
   else Self->rDimensions = (Self->rDimensions | DMF::FIXED_Y_OFFSET) & (~DMF::SCALED_Y_OFFSET);
   reset_path(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static const FieldDef clRectDimensions[] = {
   { "FixedHeight",   DMF::FIXED_HEIGHT },
   { "FixedWidth",    DMF::FIXED_WIDTH },
   { "FixedX",        DMF::FIXED_X },
   { "FixedY",        DMF::FIXED_Y },
   { "FixedXOffset",  DMF::FIXED_X_OFFSET },
   { "FixedYOffset",  DMF::FIXED_Y_OFFSET },
   { "ScaledHeight",  DMF::SCALED_HEIGHT },
   { "ScaledWidth",   DMF::SCALED_WIDTH },
   { "ScaledX",       DMF::SCALED_X },
   { "ScaledY",       DMF::SCALED_Y },
   { "ScaledXOffset", DMF::SCALED_X_OFFSET },
   { "ScaledYOffset", DMF::SCALED_Y_OFFSET },
   { nullptr, 0 }
};

static const FieldArray clRectangleFields[] = {
   { "Rounding",   FDF_VIRTUAL|FDF_DOUBLE|FDF_ARRAY|FDF_RW|FDF_PURE, RECTANGLE_GET_Rounding, RECTANGLE_SET_Rounding },
   { "RoundX",     FDF_VIRTUAL|FD_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, RECTANGLE_GET_RoundX, RECTANGLE_SET_RoundX },
   { "RoundY",     FDF_VIRTUAL|FD_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, RECTANGLE_GET_RoundY, RECTANGLE_SET_RoundY },
   { "X",          FDF_VIRTUAL|FD_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, RECTANGLE_GET_X, RECTANGLE_SET_X },
   { "Y",          FDF_VIRTUAL|FD_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, RECTANGLE_GET_Y, RECTANGLE_SET_Y },
   { "XOffset",    FDF_VIRTUAL|FD_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, RECTANGLE_GET_XOffset, RECTANGLE_SET_XOffset },
   { "YOffset",    FDF_VIRTUAL|FD_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, RECTANGLE_GET_YOffset, RECTANGLE_SET_YOffset },
   { "Width",      FDF_VIRTUAL|FD_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, RECTANGLE_GET_Width, RECTANGLE_SET_Width },
   { "Height",     FDF_VIRTUAL|FD_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, RECTANGLE_GET_Height, RECTANGLE_SET_Height },
   { "Dimensions", FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW|FDF_PURE, RECTANGLE_GET_Dimensions, RECTANGLE_SET_Dimensions, &clRectDimensions },
   END_FIELD
};

static const ActionArray clRectangleActions[] = {
   { AC::Move,          RECTANGLE_Move },
   { AC::MoveToPoint,   RECTANGLE_MoveToPoint },
   { AC::NewObject,     RECTANGLE_NewObject },
   //{ AC::Redimension, RECTANGLE_Redimension },
   { AC::Resize,      RECTANGLE_Resize },
   { AC::NIL, nullptr }
};

static ERR init_rectangle(void)
{
   clVectorRectangle = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTOR),
      fl::ClassID(CLASSID::VECTORRECTANGLE),
      fl::Name("VectorRectangle"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clRectangleActions),
      fl::Fields(clRectangleFields),
      fl::Size(sizeof(extVectorRectangle)),
      fl::Path(MOD_PATH));

   return clVectorRectangle ? ERR::Okay : ERR::AddClass;
}

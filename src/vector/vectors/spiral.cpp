/*********************************************************************************************************************

-CLASS-
VectorSpiral: Extends the Vector class with support for spiral path generation.

The VectorSpiral class generates spiral paths that extend from a central point.
-END-

*********************************************************************************************************************/

#define MAX_SPIRAL_VERTICES 65536

class extVectorSpiral : public extVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORSPIRAL;
   static constexpr CSTRING CLASS_NAME = "VectorSpiral";
   using create = kt::Create<extVectorSpiral>;

   double Spacing;
   double Offset;
   double Radius;
   double CX, CY;
   double Step;
   double LoopLimit;
   DMF Dimensions;
};

//********************************************************************************************************************

static void generate_spiral(extVectorSpiral *Vector, agg::path_storage &Path)
{
   const double cx = dmf::hasScaledCenterX(Vector->Dimensions) ? Vector->CX * get_parent_width(Vector) : Vector->CX;
   const double cy = dmf::hasScaledCenterY(Vector->Dimensions) ? Vector->CY * get_parent_height(Vector) : Vector->CY;

   double min_x = DBL_MAX, max_x = -DBL_MAX, min_y = DBL_MAX, max_y = -DBL_MAX;
   double angle  = 0;
   double radius = Vector->Offset;
   double limit  = Vector->LoopLimit * 360.0;
   double max_radius = Vector->Radius ? Vector->Radius : DBL_MAX;
   double lx = -DBL_MAX, ly = -DBL_MAX;
   double step = std::clamp(Vector->Step, 0.1, 180.0);

   if ((max_radius IS DBL_MAX) and (limit <= 0.01)) limit = 360;
   else if (limit < 0.001) limit = DBL_MAX; // Ignore the loop limit in favour of radius limit

   for (int v=0; (v < MAX_SPIRAL_VERTICES) and (angle < limit) and (radius < max_radius); v++) {
      double x = radius * cos(angle * DEG2RAD);
      double y = radius * sin(angle * DEG2RAD);

      x += cx;
      y += cy;
      if ((std::abs(x - lx) >= 1.0) or (std::abs(y - ly) >= 1.0)) { // Only record a vertex if its position has significantly changed from the last
         if (!v) Path.move_to(x, y); // First vertex
         else Path.line_to(x, y);
         lx = x;
         ly = y;
      }

      // Boundary management

      if (x < min_x) min_x = x;
      if (y < min_y) min_y = y;
      if (x > max_x) max_x = x;
      if (y > max_y) max_y = y;

      // These computations control the radius, effectively changing the rate at which the spiral expands.

      if (Vector->Spacing) radius = Vector->Offset + (Vector->Spacing * (angle / 360.0));
      else radius += step * 0.1;

      // Increment the angle by the step.  A high step value results in a jagged spiral.

      angle += step;
   }

   Vector->Bounds = { min_x, min_y, max_x, max_y };
}

//********************************************************************************************************************

static ERR SPIRAL_NewObject(extVectorSpiral *Self)
{
   Self->Step   = 1.0;
   Self->GeneratePath = (void (*)(extVector *, agg::path_storage &))&generate_spiral;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
CenterX: The horizontal center of the spiral.  Expressed as a fixed or scaled coordinate.

The horizontal center of the spiral is defined here as either a fixed or scaled value.
-END-
*********************************************************************************************************************/

static ERR SPIRAL_GET_CenterX(extVectorSpiral *Self, Unit *Value)
{
   Value->set(Self->CX);
   return ERR::Okay;
}

static ERR SPIRAL_SET_CenterX(extVectorSpiral *Self, Unit &Value)
{
   if (Value.scaled()) Self->Dimensions = (Self->Dimensions | DMF::SCALED_CENTER_X) & (~DMF::FIXED_CENTER_X);
   else Self->Dimensions = (Self->Dimensions | DMF::FIXED_CENTER_X) & (~DMF::SCALED_CENTER_X);
   Self->CX = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
CenterY: The vertical center of the spiral.  Expressed as a fixed or scaled coordinate.

The vertical center of the spiral is defined here as either a fixed or scaled value.

*********************************************************************************************************************/

static ERR SPIRAL_GET_CenterY(extVectorSpiral *Self, Unit *Value)
{
   Value->set(Self->CY);
   return ERR::Okay;
}

static ERR SPIRAL_SET_CenterY(extVectorSpiral *Self, Unit &Value)
{
   if (Value.scaled()) Self->Dimensions = (Self->Dimensions | DMF::SCALED_CENTER_Y) & (~DMF::FIXED_CENTER_Y);
   else Self->Dimensions = (Self->Dimensions | DMF::FIXED_CENTER_Y) & (~DMF::SCALED_CENTER_Y);
   Self->CY = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
LoopLimit: Used to limit the number of loops produced by the spiral path generator.

The LoopLimit can be used to impose a limit on the total number of loops that are performed by the spiral path
generator.  It can be used as an alternative to, or conjunction with the #Radius value to limit the final spiral size.

If the LoopLimit is not set, the #Radius will take precedence.

*********************************************************************************************************************/

static ERR SPIRAL_GET_LoopLimit(extVectorSpiral *Self, double *Value)
{
   *Value = Self->LoopLimit;
   return ERR::Okay;
}

static ERR SPIRAL_SET_LoopLimit(extVectorSpiral *Self, double Value)
{
   if (Value >= 0) {
      Self->LoopLimit = Value;
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
Spacing: Declares the amount of empty space between each loop of the spiral.

Spacing tightly controls the computation of the spiral path, ensuring that a specific amount of empty space is left
between each loop.  The space is declared in pixel units.

If Spacing is undeclared, the spiral expands at an incremental rate of `Step * 0.1`.

*********************************************************************************************************************/

static ERR SPIRAL_GET_Spacing(extVectorSpiral *Self, double *Value)
{
   *Value = Self->Spacing;
   return ERR::Okay;
}

static ERR SPIRAL_SET_Spacing(extVectorSpiral *Self, double Value)
{
   if (Value >= 0.0) {
      Self->Spacing = Value;
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
Height: The height (vertical diameter) of the spiral.

The height of the spiral is expressed as `Radius * 2.0`.

*********************************************************************************************************************/

static ERR SPIRAL_GET_Height(extVectorSpiral *Self, Unit *Value)
{
   Value->set(Self->Radius * 2.0);
   return ERR::Okay;
}

static ERR SPIRAL_SET_Height(extVectorSpiral *Self, Unit &Value)
{
   Self->Radius = Value * 0.5;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Offset: Offset the starting coordinate of the spiral by this value.

The generation of a spiral's path can be offset by specifying a positive value in the Offset field.  By default the
Offset is set to zero.

*********************************************************************************************************************/

static ERR SPIRAL_GET_Offset(extVectorSpiral *Self, double *Value)
{
   *Value = Self->Offset;
   return ERR::Okay;
}

static ERR SPIRAL_SET_Offset(extVectorSpiral *Self, double Value)
{
   if (Value >= 0.0) {
      Self->Offset = Value;
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
PathLength: Calibrates the user agent's distance-along-a-path calculations with that of the author.

The author's computation of the total length of the path, in user units. This value is used to calibrate the user
agent's own distance-along-a-path calculations with that of the author. The user agent will scale all
distance-along-a-path computations by the ratio of PathLength to the user agent's own computed value for total path
length.

*********************************************************************************************************************/

static ERR SPIRAL_GET_PathLength(extVectorSpiral *Self, int *Value)
{
   *Value = Self->PathLength;
   return ERR::Okay;
}

static ERR SPIRAL_SET_PathLength(extVectorSpiral *Self, int Value)
{
   if (Value >= 0) {
      Self->PathLength = Value;
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
Radius: The radius of the spiral.  Expressed as a fixed or scaled coordinate.

The radius of the spiral is defined here as either a fixed or scaled value.  If zero, preference is given to
#LoopLimit.

*********************************************************************************************************************/

static ERR SPIRAL_GET_Radius(extVectorSpiral *Self, Unit *Value)
{
   Value->set(Self->Radius);
   return ERR::Okay;
}

static ERR SPIRAL_SET_Radius(extVectorSpiral *Self, Unit &Value)
{
   if (Value < 0) return ERR::InvalidDimension;
   if (Value.scaled()) Self->Dimensions = (Self->Dimensions|DMF::SCALED_RADIUS_X|DMF::SCALED_RADIUS_Y) & (~(DMF::FIXED_RADIUS_X|DMF::FIXED_RADIUS_Y));
   else Self->Dimensions = (Self->Dimensions | (DMF::FIXED_RADIUS_X|DMF::FIXED_RADIUS_Y)) & (~(DMF::SCALED_RADIUS_X|DMF::SCALED_RADIUS_Y));
   Self->Radius = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Step: Determines the distance between each vertex in the spiral's path.

The Step value affects the distance between each vertex in the spiral path during its generation.  The default value
is `1.0`.  Using larger values will create a spiral with jagged corners due to the reduction in vertices.

*********************************************************************************************************************/

static ERR SPIRAL_GET_Step(extVectorSpiral *Self, double *Value)
{
   *Value = Self->Step;
   return ERR::Okay;
}

static ERR SPIRAL_SET_Step(extVectorSpiral *Self, double Value)
{
   if (Value != 0.0) {
      Self->Step = Value;
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************

-FIELD-
Width: The width (horizontal diameter) of the spiral.

The width of the spiral is expressed as `Radius * 2.0`.
-END-

*********************************************************************************************************************/

static ERR SPIRAL_GET_Width(extVectorSpiral *Self, Unit *Value)
{
   Value->set(Self->Radius * 2.0);
   return ERR::Okay;
}

static ERR SPIRAL_SET_Width(extVectorSpiral *Self, Unit &Value)
{
   Self->Radius = Value * 0.5;
   reset_path(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static const ActionArray clVectorSpiralActions[] = {
   { AC::NewObject, SPIRAL_NewObject },
   { AC::NIL, nullptr }
};

static const FieldArray clVectorSpiralFields[] = {
   { "PathLength", FDF_VIRTUAL|FDF_INT|FDF_RW|FDF_PURE, SPIRAL_GET_PathLength, SPIRAL_SET_PathLength },
   { "Width",      FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, SPIRAL_GET_Width,   SPIRAL_SET_Width },
   { "Height",     FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, SPIRAL_GET_Height,  SPIRAL_SET_Height },
   { "CenterX",    FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, SPIRAL_GET_CenterX, SPIRAL_SET_CenterX },
   { "CenterY",    FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, SPIRAL_GET_CenterY, SPIRAL_SET_CenterY },
   { "Radius",     FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, SPIRAL_GET_Radius,  SPIRAL_SET_Radius },
   { "Offset",     FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, SPIRAL_GET_Offset, SPIRAL_SET_Offset },
   { "Step",       FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, SPIRAL_GET_Step, SPIRAL_SET_Step },
   { "Spacing",    FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, SPIRAL_GET_Spacing, SPIRAL_SET_Spacing },
   { "LoopLimit",  FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, SPIRAL_GET_LoopLimit, SPIRAL_SET_LoopLimit },
   // Synonyms
   { "CX", FDF_SYNONYM|FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, SPIRAL_GET_CenterX, SPIRAL_SET_CenterX },
   { "CY", FDF_SYNONYM|FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, SPIRAL_GET_CenterY, SPIRAL_SET_CenterY },
   { "R",  FDF_SYNONYM|FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, SPIRAL_GET_Radius,  SPIRAL_SET_Radius },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_spiral(void)
{
   clVectorSpiral = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTOR),
      fl::ClassID(CLASSID::VECTORSPIRAL),
      fl::Name("VectorSpiral"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clVectorSpiralActions),
      fl::Fields(clVectorSpiralFields),
      fl::Size(sizeof(extVectorSpiral)),
      fl::Path(MOD_PATH));

   return clVectorSpiral ? ERR::Okay : ERR::AddClass;
}


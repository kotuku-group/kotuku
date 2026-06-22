/*********************************************************************************************************************

-CLASS-
VectorSpiral: Extends the Vector class with support for spiral path generation.

The VectorSpiral class generates spiral paths that extend from a central point.
-END-

*********************************************************************************************************************/

const int MAX_SPIRAL_VERTICES = 0xffff;

static void generate_spiral(class extVectorSpiral *Vector, agg::path_storage &Path);

class extVectorSpiral : public extVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORSPIRAL;
   static constexpr CSTRING CLASS_NAME = "VectorSpiral";
   using create = kt::Create<extVectorSpiral>;

   double Spacing;
   double Offset;
   double Step;
   double LoopLimit;
   Unit Radius;
   Unit CX, CY;

   extVectorSpiral() {
      Spacing = 0;
      Offset = 0;
      Radius = 0;
      CX = 0;
      CY = 0;
      Step = 1.0;
      LoopLimit = 0;
      GeneratePath = (void (*)(extVector *, agg::path_storage &))&generate_spiral;
   }
};

//********************************************************************************************************************

static void generate_spiral(extVectorSpiral *Vector, agg::path_storage &Path)
{
   const double cx = Vector->CX.scaled() ? Vector->CX * get_parent_width(Vector) : double(Vector->CX);
   const double cy = Vector->CY.scaled() ? Vector->CY * get_parent_height(Vector) : double(Vector->CY);

   double min_x = DBL_MAX, max_x = -DBL_MAX, min_y = DBL_MAX, max_y = -DBL_MAX;
   double angle  = 0;
   double radius = Vector->Offset;
   double limit  = Vector->LoopLimit * 360.0;
   const bool has_radius_limit = Vector->Radius.defined() and (double(Vector->Radius) != 0);
   double max_radius = has_radius_limit ? double(Vector->Radius) : DBL_MAX;
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

/*********************************************************************************************************************
-FIELD-
CX: The horizontal center of the spiral.  Expressed as a fixed or scaled coordinate.

The horizontal center of the spiral is defined here as either a fixed or scaled value.
-END-
*********************************************************************************************************************/

static ERR VECTORSPIRAL_SET_CX(extVectorSpiral *Self, Unit &Value)
{
   Self->CX = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
CY: The vertical center of the spiral.  Expressed as a fixed or scaled coordinate.

The vertical center of the spiral is defined here as either a fixed or scaled value.

*********************************************************************************************************************/

static ERR VECTORSPIRAL_SET_CY(extVectorSpiral *Self, Unit &Value)
{
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

static ERR VECTORSPIRAL_SET_LoopLimit(extVectorSpiral *Self, double Value)
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

static ERR VECTORSPIRAL_SET_Spacing(extVectorSpiral *Self, double Value)
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

static ERR VECTORSPIRAL_GET_Height(extVectorSpiral *Self, Unit &Value)
{
   Value = Unit(Self->Radius * 2.0, Self->Radius.Type);
   return ERR::Okay;
}

static ERR VECTORSPIRAL_SET_Height(extVectorSpiral *Self, Unit &Value)
{
   Self->Radius = Unit(Value * 0.5, Value.Type);
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Offset: Offset the starting coordinate of the spiral by this value.

The generation of a spiral's path can be offset by specifying a positive value in the Offset field.  By default the
Offset is set to zero.

*********************************************************************************************************************/

static ERR VECTORSPIRAL_SET_Offset(extVectorSpiral *Self, double Value)
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

static ERR VECTORSPIRAL_GET_PathLength(extVectorSpiral *Self, int &Value)
{
   Value = Self->PathLength;
   return ERR::Okay;
}

static ERR VECTORSPIRAL_SET_PathLength(extVectorSpiral *Self, int Value)
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

static ERR VECTORSPIRAL_SET_Radius(extVectorSpiral *Self, Unit &Value)
{
   if (Value < 0) return ERR::InvalidDimension;
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

static ERR VECTORSPIRAL_SET_Step(extVectorSpiral *Self, double Value)
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

static ERR VECTORSPIRAL_GET_Width(extVectorSpiral *Self, Unit &Value)
{
   Value = Unit(Self->Radius * 2.0, Self->Radius.Type);
   return ERR::Okay;
}

static ERR VECTORSPIRAL_SET_Width(extVectorSpiral *Self, Unit &Value)
{
   Self->Radius = Unit(Value * 0.5, Value.Type);
   reset_path(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

#include "spiral_def.cpp"

static const FieldArray clVectorSpiralFields[] = {
   { "Spacing",    FDF_DOUBLE|FDF_RW, nullptr, VECTORSPIRAL_SET_Spacing },
   { "Offset",     FDF_DOUBLE|FDF_RW, nullptr, VECTORSPIRAL_SET_Offset },
   { "Step",       FDF_DOUBLE|FDF_RW, nullptr, VECTORSPIRAL_SET_Step },
   { "LoopLimit",  FDF_DOUBLE|FDF_RW, nullptr, VECTORSPIRAL_SET_LoopLimit },
   { "Radius",     FDF_UNIT|FDF_RW, nullptr, VECTORSPIRAL_SET_Radius },
   { "R",          FDF_SYNONYM },
   { "CX",         FDF_UNIT|FDF_RW, nullptr, VECTORSPIRAL_SET_CX },
   { "CY",         FDF_UNIT|FDF_RW, nullptr, VECTORSPIRAL_SET_CY },
   { "PathLength", FDF_VIRTUAL|FDF_INT|FDF_RW|FDF_PURE, VECTORSPIRAL_GET_PathLength, VECTORSPIRAL_SET_PathLength },
   { "Width",      FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, VECTORSPIRAL_GET_Width,   VECTORSPIRAL_SET_Width },
   { "Height",     FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, VECTORSPIRAL_GET_Height,  VECTORSPIRAL_SET_Height },
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

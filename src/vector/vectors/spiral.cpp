/*********************************************************************************************************************

-CLASS-
VectorSpiral: Extends the Vector class with support for spiral path generation.

The VectorSpiral class generates spiral paths that extend from a central point.
-END-

*********************************************************************************************************************/

const int MAX_SPIRAL_VERTICES = 0xffff;
constexpr double MIN_SPIRAL_SPACING_GAIN = 0.01;

static void generate_spiral(class extVectorSpiral *Vector, agg::path_storage &Path);

class extVectorSpiral : public extVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORSPIRAL;
   static constexpr CSTRING CLASS_NAME = "VectorSpiral";
   using create = kt::Create<extVectorSpiral>;

   double Spacing;
   double Decay;
   double Offset;
   double Step;
   double LoopLimit;
   Unit Radius;
   Unit CX, CY;

   extVectorSpiral(objMetaClass *ClassPtr, OBJECTID ObjectID) : extVector(ClassPtr, ObjectID) {
      Spacing   = 0;
      Decay     = 1.0;
      Offset    = 0;
      Radius    = 0;
      CX        = 0;
      CY        = 0;
      Step      = 1.0;
      LoopLimit = 0;
      GeneratePath = (void (*)(extVector *, agg::path_storage &))&generate_spiral;
   }
};

//********************************************************************************************************************

static void generate_spiral(extVectorSpiral *Vector, agg::path_storage &Path)
{
   const double cx = Vector->CX.scaled() ? Vector->CX * get_parent_width(Vector) : double(Vector->CX);
   const double cy = Vector->CY.scaled() ? Vector->CY * get_parent_height(Vector) : double(Vector->CY);

   double limit  = Vector->LoopLimit * 360.0;
   const bool has_radius_limit = Vector->Radius.defined() and (double(Vector->Radius) != 0);
   const double max_radius = has_radius_limit ?
      (Vector->Radius.scaled() ? double(Vector->Radius) * svg_diag(get_parent_width(Vector),
         get_parent_height(Vector)) : double(Vector->Radius)) : DBL_MAX;
   const double step              = Vector->Step;
   const double spacing           = Vector->Spacing > 0 ? Vector->Spacing : 36.0;
   const double log_decay         = Vector->Decay IS 1.0 ? 0.0 : std::log(Vector->Decay);
   const double decay_denominator = Vector->Decay IS 1.0 ? 1.0 : std::expm1(log_decay);
   bool recorded = false;

   if ((max_radius IS DBL_MAX) and (limit <= 0.01)) limit = 360;
   else if (limit < 0.001) limit = DBL_MAX; // Ignore the loop limit in favour of radius limit

   const bool stop_at_decay = (limit IS DBL_MAX) and (Vector->Decay < 1.0) and
      (max_radius >= Vector->Offset + (spacing / (1.0 - Vector->Decay)));

   double min_x = 0, max_x = 0, min_y = 0, max_y = 0, angle = 0, lx = 0, ly = 0;

   for (int v=0; (v < MAX_SPIRAL_VERTICES) and (angle < limit); v++) {
      const double turns       = angle / 360.0;
      const double decay_phase = turns * log_decay;
      double radius            = Vector->Offset + spacing * (Vector->Decay IS 1.0 ?
         turns : std::expm1(decay_phase) / decay_denominator);

      // If the radius limit is beyond the asymptote, finish once another turn adds less than 0.01 units.

      if (stop_at_decay and (turns >= 1.0) and (spacing * std::exp(decay_phase) < MIN_SPIRAL_SPACING_GAIN)) break;
      const bool at_radius_limit = radius >= max_radius;
      if (at_radius_limit) {
         if (not recorded) break;
         // Solve the same growth equation for the angle at the boundary.
         const double radial_turns = (max_radius - Vector->Offset) / spacing;
         angle = 360.0 * (Vector->Decay IS 1.0 ? radial_turns :
            std::log1p(radial_turns * decay_denominator) / log_decay);
         radius = max_radius;
      }

      double x = radius * cos(angle * DEG2RAD);
      double y = radius * sin(angle * DEG2RAD);

      x += cx;
      y += cy;
      if ((not recorded) or at_radius_limit or (std::abs(x - lx) >= 1.0) or (std::abs(y - ly) >= 1.0)) {
         if (not recorded) {
            Path.move_to(x, y);
            min_x = max_x = x;
            min_y = max_y = y;
            recorded = true;
         }
         else {
            Path.line_to(x, y);
            if (x < min_x) min_x = x;
            if (y < min_y) min_y = y;
            if (x > max_x) max_x = x;
            if (y > max_y) max_y = y;
         }
         lx = x;
         ly = y;
      }

      if (at_radius_limit) break;

      // Increment the angle by the step.  A high step value results in a jagged spiral.

      angle += step;
   }

   Vector->Bounds = { min_x, min_y, max_x, max_y };
}

/*********************************************************************************************************************

-FIELD-
CX: The horizontal center of the spiral.  Expressed as a fixed or scaled coordinate.

The horizontal center of the spiral is defined here as either a fixed or scaled value.

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
Decay: Reduces the radial spacing after each revolution.

The default value of `1.0` leaves the spacing unchanged.  Values between `0.0` and `1.0` multiply the spacing between
successive turns, so `0.5` halves it each revolution.  Values must be greater than zero and no greater than one.
The radius approaches `Offset + Spacing / (1 - Decay)` when Spacing is set.  If Spacing is zero, a base spacing
of 36 units per revolution is used.

*********************************************************************************************************************/

static ERR VECTORSPIRAL_SET_Decay(extVectorSpiral *Self, double Value)
{
   if (std::isfinite(Value) and (Value > 0.0) and (Value <= 1.0)) {
      Self->Decay = Value;
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************

-FIELD-
LoopLimit: Used to limit the number of loops produced by the spiral path generator.

The LoopLimit can be used to impose a limit on the total number of loops that are performed by the spiral path
generator.  It can be used as an alternative to, or conjunction with the #Radius value to limit the final spiral size.

If the LoopLimit is not set, the #Radius will take precedence.
When #Decay makes the Radius unreachable, generation stops once radial growth is less than `0.01` units per turn.

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

If Spacing is undeclared, the base spacing is 36 units per revolution, equivalent to an incremental rate of
`Step * 0.1` when #Decay is `1.0`.

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
Radius: Clamps the radius of the spiral.  Expressed as a fixed or scaled coordinate.

The maximum radius of the spiral is defined here as either a fixed value or a percentage of the viewport's normalised
diagonal.  If zero, preference is given to #LoopLimit.

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
Step: Determines the angle between sampled points in the spiral's path.

The default Step is `1.0` degree.  Values are clamped to the range `0.1` to `180.0` degrees.  Using larger values will
create a spiral with jagged corners due to the reduction in vertices.

*********************************************************************************************************************/

static ERR VECTORSPIRAL_SET_Step(extVectorSpiral *Self, double Value)
{
   if (std::isfinite(Value) and (Value > 0.0)) {
      Self->Step = std::clamp(Value, 0.1, 180.0);
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
   { "Decay",      FDF_DOUBLE|FDF_RW, nullptr, VECTORSPIRAL_SET_Decay },
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

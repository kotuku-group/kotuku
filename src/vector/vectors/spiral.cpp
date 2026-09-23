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
   double StartTurn = 0;
   double EndTurn = 0;
   double Tolerance = 0;
   Unit Radius;
   Unit CX, CY;
   int Clockwise = TRUE;
   int Reverse = FALSE;
   Unit Thickness = Unit(0);

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

   double thickness = Vector->Thickness.scaled() ?
      get_parent_diagonal(Vector) * INV_SQRT2 * Vector->Thickness : double(Vector->Thickness);
   const unsigned first_vertex    = Path.total_vertices();
   const double spacing           = Vector->Spacing > 0 ? Vector->Spacing : 36.0;
   const double log_decay         = std::log(Vector->Decay);
   const double decay_denominator = Vector->Decay - 1.0;
   const double tau               = 360.0 * DEG2RAD;
   const bool has_radius_limit    = Vector->Radius.defined() and (double(Vector->Radius) != 0);
   const double max_radius        = has_radius_limit ?
      (Vector->Radius.scaled() ? double(Vector->Radius) * svg_diag(get_parent_width(Vector),
         get_parent_height(Vector)) : double(Vector->Radius)) : DBL_MAX;

   auto radius_at = [&](double Turn) {
      return Vector->Offset + spacing * (Vector->Decay IS 1.0 ? Turn :
         std::expm1(Turn * log_decay) / decay_denominator);
   };

   double limit = DBL_MAX;
   if (Vector->LoopLimit > 0) limit = Vector->LoopLimit;
   if (Vector->EndTurn > 0) limit = std::min(limit, Vector->EndTurn);
   if (has_radius_limit) {
      const double radial_turns = (max_radius - Vector->Offset) / spacing;
      if (radial_turns < 0) limit = -1;
      else if (Vector->Decay IS 1.0) limit = std::min(limit, radial_turns);
      else if ((Vector->Decay > 1.0) or (radial_turns * decay_denominator > -1.0)) {
         const double product = radial_turns * decay_denominator;
         const double boundary = std::isfinite(product) ? std::log1p(product) / log_decay :
            (std::log(radial_turns) + std::log(decay_denominator)) / log_decay;
         limit = std::min(limit, boundary);
      }
   }
   if (limit IS DBL_MAX) {
      if (has_radius_limit and (Vector->Decay < 1.0)) {
         limit = std::max(1.0, (std::log(MIN_SPIRAL_SPACING_GAIN) - std::log(spacing)) / log_decay);
      }
      else limit = Vector->StartTurn + 1.0;
   }

   double min_x = 0, max_x = 0, min_y = 0, max_y = 0, lx = 0, ly = 0;
   double turn = Vector->StartTurn;
   bool recorded = false;

   for (int v=0; (v < MAX_SPIRAL_VERTICES) and (turn <= limit); v++) {
      const double radius = radius_at(turn);
      const double angle = std::fmod(turn, 1.0) * tau * (Vector->Clockwise ? 1.0 : -1.0);
      const double x = cx + radius * std::cos(angle);
      const double y = cy + radius * std::sin(angle);
      if ((not std::isfinite(x)) or (not std::isfinite(y))) break;
      if ((not recorded) or (Vector->Tolerance > 0) or (turn IS limit) or
          (std::abs(x - lx) >= 1.0) or (std::abs(y - ly) >= 1.0)) {
         if (not recorded) {
            Path.move_to(x, y);
            min_x = max_x = x;
            min_y = max_y = y;
            recorded = true;
         }
         else {
            Path.line_to(x, y);
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
         }
         lx = x;
         ly = y;
      }
      if (turn IS limit) break;

      double increment = std::min(limit - turn, Vector->Tolerance > 0 ? 0.25 : Vector->Step / 360.0);
      if (Vector->Tolerance > 0) {
         // Linear interpolation error is at most h*h*max(|P''|)/8.  Bound the radial and
         // angular derivatives over the entire interval, in turn coordinates.

         while (turn + increment > turn) {
            const double next_radius = radius_at(turn + increment);
            const double derivative_turn = Vector->Decay > 1.0 ? turn + increment : turn;
            const double radial_derivative = Vector->Decay IS 1.0 ? spacing :
               spacing * (log_decay / decay_denominator) * std::exp(derivative_turn * log_decay);
            const double bound = tau * tau * next_radius +
               (2.0 * tau + std::abs(log_decay)) * radial_derivative;
            if (std::isfinite(bound) and (increment * increment * bound / 8.0 <= Vector->Tolerance)) break;
            increment *= 0.5;
         }
      }
      const double next_turn = std::min(limit, turn + increment);
      if (next_turn <= turn) break;
      turn = next_turn;
   }

   if (thickness > spacing - 1) thickness = spacing - 1; // Sanity check to avoid self-intersection of the ribbon.

   if (thickness > 0) {
      agg::path_storage centreline;
      for (unsigned i=first_vertex; i < Path.total_vertices(); i++) {
         double x, y;
         const unsigned command = Path.vertex(i, &x, &y);
         if (agg::is_move_to(command)) centreline.move_to(x, y);
         else centreline.line_to(x, y);
      }

      agg::conv_stroke<agg::path_storage> ribbon(centreline);
      configure_stroke(*Vector, ribbon);

      ribbon.width(thickness);
      if (Vector->Tolerance > 0) {
         ribbon.approximation_scale(std::clamp(0.25 / Vector->Tolerance, 1.0, 1000000.0));
      }

      agg::path_storage outline;
      agg::resolve_stroke_self_intersections(outline, ribbon);
      Path = std::move(outline);
      min_x = min_y = max_x = max_y = 0;
      bool has_bounds = false;
      for (unsigned i=0; i < Path.total_vertices(); i++) {
         double x, y;
         if (not agg::is_vertex(Path.vertex(i, &x, &y))) continue;
         if (not has_bounds) {
            min_x = max_x = x;
            min_y = max_y = y;
            has_bounds = true;
         }
         else {
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
         }
      }
   }

   if (Vector->Reverse) {
      // Reverse each contour's coordinates, retaining move-to, line-to and close commands.
      // This preserves the sampled geometry even when generation stops at the vertex cap.

      unsigned start = thickness > 0 ? 0 : first_vertex;
      while (start < Path.total_vertices()) {
         if (not agg::is_vertex(Path.command(start))) {
            start++;
            continue;
         }
         unsigned end = start + 1;
         while ((end < Path.total_vertices()) and agg::is_line_to(Path.command(end))) end++;
         for (unsigned i=0; i < (end - start) / 2; i++) {
            const unsigned left = start + i;
            const unsigned right = end - 1 - i;
            double left_x, left_y, right_x, right_y;
            Path.vertex(left, &left_x, &left_y);
            Path.vertex(right, &right_x, &right_y);
            Path.modify_vertex(left, right_x, right_y);
            Path.modify_vertex(right, left_x, left_y);
         }
         if ((end < Path.total_vertices()) and agg::is_end_poly(Path.command(end))) {
            const unsigned command = Path.command(end);
            if (agg::is_cw(command)) Path.modify_command(end, agg::set_orientation(command, agg::path_flags_ccw));
            else if (agg::is_ccw(command)) Path.modify_command(end, agg::set_orientation(command, agg::path_flags_cw));
         }
         start = end;
      }
   }

   Vector->Bounds = { min_x, min_y, max_x, max_y };
}

/*********************************************************************************************************************

-FIELD-
Thickness: Expands the spiral centreline into a closed ribbon.

Zero (the default) keeps the original open spiral.  A positive value specifies the full width of a constant-width
ribbon centred on the sampled spiral.  Fixed values use local user units; percentages are resolved against the
normalised diagonal of the parent viewport, matching @VectorWave.Thickness and @Vector.StrokeWidth.  Values must be
finite and non-negative.

The ribbon uses @Vector.LineCap, @Vector.LineJoin, @Vector.InnerJoin and the miter limits to construct its outline.
Fill paints its interior and Stroke paints its boundary.  Simple self-intersection loops at tight turns are resolved
by the outline generator.  Wider ribbons can overlap neighbouring turns.

#Radius, #StartTurn and #EndTurn clip the centreline, so the ribbon's boundary can extend beyond Radius.  #Tolerance
controls centreline sampling and the precision of rounded joins, with join refinement capped for extreme values.
An empty or single-point centreline produces no ribbon.  #Reverse reverses each closed contour without changing its
geometry.  Ribbon outlines can contain more vertices than the centreline's 65535-vertex limit.

*********************************************************************************************************************/

static ERR VECTORSPIRAL_SET_Thickness(extVectorSpiral *Self, Unit &Value)
{
   if ((not std::isfinite(double(Value))) or (Value < 0)) return ERR::InvalidValue;
   Self->Thickness = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Clockwise: Controls the winding direction as the spiral grows outwards.

The default is TRUE, producing clockwise winding in the usual screen coordinate system where Y increases downwards.
FALSE produces anticlockwise winding by reflecting the spiral about the horizontal line through #CY.  The start
remains on the positive X axis at turn zero.  This setting is independent of #Reverse; reversing traversal does not
change the shape's winding as measured outwards.  Transforms can change its apparent winding on screen.

*********************************************************************************************************************/

static ERR VECTORSPIRAL_SET_Clockwise(extVectorSpiral *Self, int Value)
{
   Self->Clockwise = Value ? TRUE : FALSE;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Reverse: Reverses traversal along the generated spiral.

The default is FALSE, traversing from #StartTurn towards the final clipped turn.  TRUE traverses the same sampled
path from its last point to its first point.  Geometry, bounds, sampling and clipping remain unchanged.  This applies
to the generated portion even if the vertex cap truncates the path.  Use #Clockwise to change the outward winding
independently of traversal.

*********************************************************************************************************************/

static ERR VECTORSPIRAL_SET_Reverse(extVectorSpiral *Self, int Value)
{
   Self->Reverse = Value ? TRUE : FALSE;
   reset_path(Self);
   return ERR::Okay;
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
Decay: Multiplies the radial spacing after each revolution.

The default value of `1.0` leaves the spacing unchanged.  Values between `0.0` and `1.0` multiply the spacing between
successive turns, so `0.5` halves it each revolution.  Values above `1.0` expand the spacing; `2.0` doubles it each
revolution.  Values must be finite and greater than zero.  For values below `1.0`, the radius approaches
`Offset + Spacing / (1 - Decay)` when Spacing is set.  If Spacing is zero, a base spacing of 36 units per revolution
is used.

*********************************************************************************************************************/

static ERR VECTORSPIRAL_SET_Decay(extVectorSpiral *Self, double Value)
{
   if (std::isfinite(Value) and (Value > 0.0)) {
      Self->Decay = Value;
      reset_path(Self);
      return ERR::Okay;
   }
   else return kt::Log().warning(ERR::InvalidValue);
}

/*********************************************************************************************************************

-FIELD-
LoopLimit: Used to limit the number of loops produced by the spiral path generator.

The LoopLimit can be used to impose a limit on the total number of loops that are performed by the spiral path
generator.  It can be used as an alternative to, or conjunction with the #Radius value to limit the final spiral size.

LoopLimit is an absolute turn limit measured from turn zero, including when #StartTurn is non-zero.
If the LoopLimit is not set, #EndTurn or #Radius takes precedence.
When #Decay makes the Radius unreachable, generation stops once radial growth is less than `0.01` units per turn.

*********************************************************************************************************************/

static ERR VECTORSPIRAL_SET_LoopLimit(extVectorSpiral *Self, double Value)
{
   if (std::isfinite(Value) and (Value >= 0)) {
      Self->LoopLimit = Value;
      reset_path(Self);
      return ERR::Okay;
   }
   else return kt::Log().warning(ERR::InvalidValue);
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
   if (std::isfinite(Value) and (Value >= 0.0)) {
      Self->Spacing = Value;
      reset_path(Self);
      return ERR::Okay;
   }
   else return kt::Log().warning(ERR::InvalidValue);
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
   if ((not std::isfinite(double(Value))) or (Value < 0)) return ERR::InvalidDimension;
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
   if (std::isfinite(Value) and (Value >= 0.0)) {
      Self->Offset = Value;
      reset_path(Self);
      return ERR::Okay;
   }
   else return kt::Log().warning(ERR::InvalidValue);
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
   else return kt::Log().warning(ERR::InvalidValue);
}

/*********************************************************************************************************************

-FIELD-
Radius: Clamps the radius of the spiral.  Expressed as a fixed or scaled coordinate.

The maximum radius of the spiral is defined here as either a fixed value or a percentage of the viewport's normalised
diagonal.  If zero, preference is given to #LoopLimit.

*********************************************************************************************************************/

static ERR VECTORSPIRAL_SET_Radius(extVectorSpiral *Self, Unit &Value)
{
   if ((not std::isfinite(double(Value))) or (Value < 0)) return ERR::InvalidDimension;
   Self->Radius = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Step: Determines the angle between sampled points in the spiral's path.

The default Step is `1.0` degree.  Values are clamped to the range `0.1` to `180.0` degrees.  Using larger values will
create a spiral with jagged corners due to the reduction in vertices.  A positive #Tolerance overrides Step.

*********************************************************************************************************************/

static ERR VECTORSPIRAL_SET_Step(extVectorSpiral *Self, double Value)
{
   if (std::isfinite(Value) and (Value > 0.0)) {
      Self->Step = std::clamp(Value, 0.1, 180.0);
      reset_path(Self);
      return ERR::Okay;
   }
   else return kt::Log().warning(ERR::InvalidValue);
}

/*********************************************************************************************************************

-FIELD-
StartTurn: The absolute turn at which the spiral begins.

Defaults to zero.  Fractional turns are supported; radial growth and orientation remain relative to turn zero.
The value must be finite and non-negative.  An interval whose end precedes StartTurn produces an empty path.
Without an end or radius limit, one turn is generated starting here.

*********************************************************************************************************************/

static ERR VECTORSPIRAL_SET_StartTurn(extVectorSpiral *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0)) return ERR::InvalidValue;
   Self->StartTurn = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
EndTurn: The absolute turn at which the spiral ends.

A positive value clips the original spiral without restarting radial growth.  Zero (the default) disables this
limit.  Values must be finite and non-negative.  The earliest of EndTurn, #LoopLimit and #Radius determines the end.
The final point is included, including for fractional turns.

*********************************************************************************************************************/

static ERR VECTORSPIRAL_SET_EndTurn(extVectorSpiral *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0)) return kt::Log().warning(ERR::InvalidValue);
   Self->EndTurn = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Tolerance: Enables adaptive sampling with a maximum local-coordinate error.

Zero (the default) uses fixed angular sampling controlled by #Step.  A positive value replaces Step with adaptive
sampling, bounding the distance between the ideal spiral and each line segment in local user units, before transforms.
Smaller values produce more vertices.  Values must be finite and non-negative.  Adaptive sampling retains sub-unit
segments.  Generation stops at 65535 vertices or when numerical precision prevents further progress, so extreme
settings can truncate the path before its requested endpoint.

*********************************************************************************************************************/

static ERR VECTORSPIRAL_SET_Tolerance(extVectorSpiral *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0)) return kt::Log().warning(ERR::InvalidValue);
   Self->Tolerance = Value;
   reset_path(Self);
   return ERR::Okay;
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
   if ((not std::isfinite(double(Value))) or (Value < 0)) return kt::Log().warning(ERR::InvalidDimension);
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
   { "StartTurn",  FDF_DOUBLE|FDF_RW, nullptr, VECTORSPIRAL_SET_StartTurn },
   { "EndTurn",    FDF_DOUBLE|FDF_RW, nullptr, VECTORSPIRAL_SET_EndTurn },
   { "Tolerance",  FDF_DOUBLE|FDF_RW, nullptr, VECTORSPIRAL_SET_Tolerance },
   { "Radius",     FDF_UNIT|FDF_RW, nullptr, VECTORSPIRAL_SET_Radius },
   { "R",          FDF_SYNONYM },
   { "CX",         FDF_UNIT|FDF_RW, nullptr, VECTORSPIRAL_SET_CX },
   { "CY",         FDF_UNIT|FDF_RW, nullptr, VECTORSPIRAL_SET_CY },
   { "Clockwise",  FDF_INT|FDF_RW, nullptr, VECTORSPIRAL_SET_Clockwise },
   { "Reverse",    FDF_INT|FDF_RW, nullptr, VECTORSPIRAL_SET_Reverse },
   { "Thickness",  FDF_UNIT|FDF_RW, nullptr, VECTORSPIRAL_SET_Thickness },
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

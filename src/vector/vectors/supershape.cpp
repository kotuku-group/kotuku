/*********************************************************************************************************************

-CLASS-
VectorShape: Extends the Vector class with support for the Superformula algorithm.

The VectorShape class extends the Vector class with support for generating paths with the Superformula algorithm by
Johan Gielis.  Formula parameters must be finite; A, B and N1 must also be non-zero.  Parameter combinations that
produce singular or overflowing geometry generate an empty path.  Degenerate zero-extent shapes collapse to the centre.

This feature is not part of the SVG standard and therefore should not be used in cases where SVG
compliance is a strict requirement.

The Superformula is documented in detail at Wikipedia: http://en.wikipedia.org/wiki/Superformula

-END-

TODO:
* Thickness field - see VectorWave / VectorSpiral for implementation
* Reverse field - Reverses the vertices if TRUE
* Repeat (total repeats), RepeatScale (multiplier), RepeatRotation (degrees)

*********************************************************************************************************************/

constexpr int DEFAULT_VERTICES = 360 * 4;

static void generate_supershape(class extVectorShape *Vector, agg::path_storage &Path);

class extVectorShape : public extVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORSHAPE;
   static constexpr CSTRING CLASS_NAME = "VectorShape";
   using create = kt::Create<extVectorShape>;

   // Concrete (direct-access) fields first, in the same order as the field array.
   double M, N1, N2, N3, A, B, Phi;
   double Tolerance = 0.25;
   double StartAngle = 0;
   double EndAngle = 0;
   double Offset = 0;
   int Vertices;
   int Spiral;
   int Repeat;
   int Close;
   int Mod;
   int Normalise = TRUE;

   Unit Radius; // Radius/CX/CY remain virtual (their getters apply a defined() guard)
   Unit CX, CY;

   extVectorShape(objMetaClass *ClassPtr, OBJECTID ObjectID) : extVector(ClassPtr, ObjectID) {
      Radius = 100;
      CX = 0;
      CY = 0;
      N1 = 0.1;
      N2 = 1.7;
      N3 = 1.7;
      M = 5;
      A = 1;
      B = 1;
      Phi = 2;
      Vertices = DEFAULT_VERTICES;
      Spiral = 0;
      Repeat = 0;
      Close = true;
      Mod = 0;
      GeneratePath = (void (*)(extVector *, agg::path_storage &))&generate_supershape;
   }
};

//********************************************************************************************************************

static double shape_fixed_x(extVectorShape *Vector, const Unit &Value)
{
   if (!Value.defined()) return 0;
   return Value.scaled() ? double(Value) * get_parent_width(Vector) : double(Value);
}

static double shape_fixed_y(extVectorShape *Vector, const Unit &Value)
{
   if (!Value.defined()) return 0;
   return Value.scaled() ? double(Value) * get_parent_height(Vector) : double(Value);
}

static double shape_fixed_radius(extVectorShape *Vector, const Unit &Value)
{
   if (!Value.defined()) return 0;
   return Value.scaled() ? double(Value) * svg_diag(get_parent_width(Vector), get_parent_height(Vector)) : double(Value);
}

//********************************************************************************************************************

static void generate_supershape(extVectorShape *Vector, agg::path_storage &Path)
{
   double cx = shape_fixed_x(Vector, Vector->CX);
   double cy = shape_fixed_y(Vector, Vector->CY);

   agg::path_storage path_buffer, *target;
   if (Path.empty()) target = &Path;
   else target = &path_buffer;

   const double scale = shape_fixed_radius(Vector, Vector->Radius);
   Vector->Bounds = { 0, 0, 0, 0 };
   if ((not std::isfinite(cx)) or (not std::isfinite(cy)) or (not std::isfinite(scale))) return;
   double rescale = 0;
   int vertices = Vector->Vertices;
   if ((vertices IS DEFAULT_VERTICES) and (Vector->Spiral > 1)) vertices *= 2;

   const double sweep = agg::pi * Vector->Phi * (Vector->Spiral > 1 ? double(Vector->Spiral) : 1.0);
   if (not std::isfinite(sweep)) return;
   if (Vector->Tolerance > 0) {
      // Choose the normalisation scan from the formula frequency, independently of manual vertex counts.
      const double intervals = std::ceil(sweep * std::max(8.0, std::abs(Vector->M) * 2.0) / agg::pi);
      if ((not std::isfinite(intervals)) or (intervals > 65534)) return;
      vertices = int(intervals);
   }
   const bool custom_growth = (Vector->Spiral > 1) and (Vector->Offset > 0);
   const double turns = sweep / (2.0 * agg::pi);
   const double spacing = scale / turns;
   const double output_scale = custom_growth ? 1.0 : scale;

   const double start = Vector->StartAngle * DEG2RAD;
   const double end = Vector->EndAngle > 0 ? std::min(sweep, Vector->EndAngle * DEG2RAD) : sweep;
   if ((not std::isfinite(start)) or (start > end)) return;
   if (not Vector->Normalise) rescale = 1.0;

   struct sample { double angle, x, y; };
   unsigned evaluations = 0;
   auto evaluate = [&](double Angle, sample &Result) {
      if (++evaluations > 262144) return false;

      const double phi = Angle;
      const double progress = phi / sweep;
      const double phase = (Vector->M * 0.25) * phi;
      if (not std::isfinite(phase)) return false;

      const double t1 = std::pow(std::abs(std::cos(phase) / Vector->A), Vector->N2);
      const double t2 = std::pow(std::abs(std::sin(phase) / Vector->B), Vector->N3);
      const double sum = t1 + t2;
      double r = std::pow(sum, -1.0 / Vector->N1);

      if ((not std::isfinite(sum)) or (sum <= 0) or (not std::isfinite(r))) return false;

      // These additional transforms can help in building a greater library of shapes.

      switch(Vector->Mod) {
         case 1: r = std::exp(r); break;
         case 2: r = std::log(r); break;
         case 3: r = std::atan(r); break;
         case 4: r = std::exp(1.0 / r); break;
         case 5: r = 1 + fastPow(std::cos(r), 2); break;
         case 6: r = fastPow(std::sin(r), 2); break;
         case 7: r = 1 + fastPow(std::sin(r), 2); break;
         case 8: r = fastPow(std::cos(r), 2); break;
      }

      if (not std::isfinite(r)) return false;

      double x = r * std::cos(phi);
      double y = r * std::sin(phi);

      if (Vector->Normalise) rescale = std::max(rescale, std::max(std::abs(x), std::abs(y)));

      if (Vector->Spiral > 1) {
         double growth = progress;
         if (custom_growth) {
            const double turn = phi / (2.0 * agg::pi);
            growth = Vector->Offset + spacing * turn;
            if (not std::isfinite(growth)) return false;
         }

         x *= growth;
         y *= growth;
         if ((not std::isfinite(x)) or (not std::isfinite(y))) return false;
      }

      Result = { phi, x, y };
      return true;
   };

   // Fit against the complete unspiralled sweep so clipping does not enlarge a selected lobe.

   std::vector<sample> samples;
   for (int i=0; i <= vertices; i++) {
      sample point;
      if (not evaluate(sweep * (double(i) / vertices), point)) return;
      samples.push_back(point);
   }

   if (Vector->Tolerance > 0) {
      std::vector<sample> refined;
      refined.push_back(samples.front());
      auto subdivide = [&](auto &Subdivide, const sample &Left, const sample &Right, int Depth) -> bool {
         sample probes[3];
         bool flat = true;
         for (int i=0; i < 3; i++) {
            const double fraction = (i + 1) * 0.25;
            if (not evaluate(Left.angle + (Right.angle - Left.angle) * fraction, probes[i])) return false;
         }

         for (int i=0; i < 3; i++) {
            const double fraction = (i + 1) * 0.25;
            const double divisor = rescale > 0 ? rescale : 1.0;
            const double dx = ((probes[i].x / divisor) -
               ((Left.x / divisor) * (1 - fraction) + (Right.x / divisor) * fraction)) * output_scale;
            const double dy = ((probes[i].y / divisor) -
               ((Left.y / divisor) * (1 - fraction) + (Right.y / divisor) * fraction)) * output_scale;
            if ((not std::isfinite(dx)) or (not std::isfinite(dy))) return false;
            if (std::hypot(dx, dy) > Vector->Tolerance) flat = false;
         }

         if (flat) {
            if (refined.size() >= 65535) return false;
            refined.push_back(Right);
            return true;
         }

         if ((Depth >= 20) or (probes[1].angle <= Left.angle) or (probes[1].angle >= Right.angle)) return false;
         return Subdivide(Subdivide, Left, probes[1], Depth + 1) and
            Subdivide(Subdivide, probes[1], Right, Depth + 1);
      };

      // Seed independently of Vertices, limiting both polar and formula-phase increments to avoid aliasing lobes.

      std::vector<double> angles { 0.0, start, end, sweep };
      for (int i=1; i < vertices; i++) angles.push_back(samples[i].angle);
      std::sort(angles.begin(), angles.end());
      angles.erase(std::unique(angles.begin(), angles.end()), angles.end());
      sample left = samples.front();
      for (unsigned i=1; i < angles.size(); i++) {
         sample right;
         if ((not evaluate(angles[i], right)) or (not subdivide(subdivide, left, right, 0))) return;
         left = right;
      }

      samples = std::move(refined);
   }

   sample first, last;
   if ((not evaluate(start, first)) or (not evaluate(end, last))) return;

   target->move_to(first.x, first.y);
   for (const auto &point : samples) {
      if ((point.angle > start) and (point.angle < end)) target->line_to(point.x, point.y);
   }

   if (end > start) target->line_to(last.x, last.y);

   if (Vector->Spiral <= 1) { // Spiral disabled
      if (Vector->Repeat > 1) { // Repeat the path n times, scaling each repeat to fit within the original path.
         target->close_polygon(); // Repeated paths are always closed.

         agg::path_storage clone(*target);

         for (int i=0; i < Vector->Repeat-1; i++) {
            agg::trans_affine transform;
            transform.scale(double(i+1) / double(Vector->Repeat));
            agg::conv_transform<agg::path_storage, agg::trans_affine> scaled_path(clone, transform);
            target->concat_path(scaled_path);
         }
      }
      else if (Vector->Close) target->close_polygon();
   }

   for (unsigned i=0; i < target->total_vertices(); i++) {
      double x, y;
      if (not agg::is_vertex(target->vertex(i, &x, &y))) continue;
      // Divide before multiplying to avoid overflow for extremely small or large formula radii.
      x = cx + (rescale > 0 ? (x / rescale) * output_scale : 0.0);
      y = cy + (rescale > 0 ? (y / rescale) * output_scale : 0.0);
      if ((not std::isfinite(x)) or (not std::isfinite(y))) {
         target->remove_all();
         return;
      }
      target->modify_vertex(i, x, y);
   }

   if (&Path != target) Path.concat_path(*target);

   Vector->Bounds = get_bounds(*target);
}

/*********************************************************************************************************************
-FIELD-
Offset: Sets the starting radial envelope of a superspiral.

Defaults to zero.  Values must be finite and non-negative, in local user units.  Only applies when #Spiral is greater
than one.  The offset is multiplied by the formula profile along with the growth envelope.  It does not change the
starting angle.  Use #StartAngle to select a later section without restarting growth.

*********************************************************************************************************************/

static ERR VECTORSHAPE_SET_Offset(extVectorShape *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0)) return ERR::InvalidValue;
   Self->Offset = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Tolerance: Controls adaptive sampling error in local user units.

The default of 0.25 automatically selects samples without requiring a vertex count.  Smaller positive values produce
finer paths.  Zero enables manual uniform sampling with #Vertices.  Positive values enable recursive refinement using
deviations at quarter, midpoint and three-quarter positions of each segment.  This is a sampled error criterion, not a
rigorous bound for arbitrary singular or sharply varying parameters.  Values must be finite and non-negative.  Limits of
65535 samples, 262144 formula evaluations and 20 subdivision levels prevent excessive work; failure to satisfy the
criterion within these limits produces an empty path.  Closing edges are not refined.

*********************************************************************************************************************/

static ERR VECTORSHAPE_SET_Tolerance(extVectorShape *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0)) return ERR::InvalidValue;
   Self->Tolerance = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
StartAngle: Selects the beginning of the shape in degrees.

Defaults to zero.  Values must be finite and non-negative.  Angles are measured from the positive X axis, increasing
clockwise in screen coordinates.  Radial spiral growth remains relative to the complete sweep.  A start beyond the
end produces an empty path.  Use Close=FALSE for an open section; repeated shapes remain closed.

*********************************************************************************************************************/

static ERR VECTORSHAPE_SET_StartAngle(extVectorShape *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0)) return ERR::InvalidValue;
   Self->StartAngle = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
EndAngle: Selects the end of the shape in degrees.

Zero (the default) uses the complete sweep specified by #Phi and #Spiral.  A positive value clips that sweep; both
selected endpoints are included.  Values must be finite and non-negative.  Equal endpoints produce a single point.

*********************************************************************************************************************/

static ERR VECTORSHAPE_SET_EndAngle(extVectorShape *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0)) return ERR::InvalidValue;
   Self->EndAngle = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Normalise: Fits the sampled formula to Radius when enabled.

TRUE (the default) divides by the largest absolute X or Y coordinate of the complete sampled unspiralled sweep.
Partial sections retain this common scale.  Adaptive probes contribute to the fitted extent, so its precision depends
on sampling.  FALSE preserves the natural formula radius, using Radius or the configured superspiral growth envelope
as a multiplier; coordinates may then exceed Radius.  Disabling normalisation is useful when animating formula
parameters without automatic size compensation.

*********************************************************************************************************************/

static ERR VECTORSHAPE_SET_Normalise(extVectorShape *Self, int Value)
{
   Self->Normalise = Value ? TRUE : FALSE;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
A: A parameter for the Superformula.

This field sets the Superformula's 'A' parameter value.

*********************************************************************************************************************/

static ERR VECTORSHAPE_SET_A(extVectorShape *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value IS 0)) return ERR::InvalidValue;
   Self->A = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
B: A parameter for the Superformula.

This field sets the Superformula's 'B' parameter value.

*********************************************************************************************************************/

static ERR VECTORSHAPE_SET_B(extVectorShape *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value IS 0)) return ERR::InvalidValue;
   Self->B = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
CX: The center of the shape on the x-axis.  Expressed as a fixed or scaled coordinate.

The horizontal center of the shape is defined here as either a fixed or scaled value.

*********************************************************************************************************************/

static ERR VECTORSHAPE_GET_CX(extVectorShape *Self, Unit *Value)
{
   *Value = Self->CX.defined() ? Self->CX : Unit(0);
   return ERR::Okay;
}

static ERR VECTORSHAPE_SET_CX(extVectorShape *Self, Unit &Value)
{
   if ((not std::isfinite(double(Value)))) return ERR::InvalidValue;
   Self->CX = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
CY: The center of the shape on the y-axis.  Expressed as a fixed or scaled coordinate.

The vertical center of the shape is defined here as either a fixed or scaled value.

*********************************************************************************************************************/

static ERR VECTORSHAPE_GET_CY(extVectorShape *Self, Unit *Value)
{
   *Value = Self->CY.defined() ? Self->CY : Unit(0);
   return ERR::Okay;
}

static ERR VECTORSHAPE_SET_CY(extVectorShape *Self, Unit &Value)
{
   if ((not std::isfinite(double(Value)))) return ERR::InvalidValue;
   Self->CY = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Close: A parameter for the super shape algorithm.

If TRUE, the shape path will be closed between the beginning and end points.

*********************************************************************************************************************/

static ERR VECTORSHAPE_SET_Close(extVectorShape *Self, int Value)
{
   Self->Close = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
M: A parameter for the Superformula.

This field sets the Superformula's 'M' parameter value.

*********************************************************************************************************************/

static ERR VECTORSHAPE_SET_M(extVectorShape *Self, double Value)
{
   if ((not std::isfinite(Value))) return ERR::InvalidValue;
   Self->M = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Mod: A special modification parameter that alters the super shape algorithm.

The Mod field alters the super shape algorithm, sometimes in radical ways that allow entirely new shapes to be
discovered in the super shape library.  The value that is specified will result in a formula being applied to the
generated 'r' value.  Possible values and their effects are:

<types>
<type name="0">Default</>
<type name="1">exp(r)</>
<type name="2">log(r)</>
<type name="3">atan(r)</>
<type name="4">exp(1.0/r)</>
<type name="5">1+cos(r)^2</>
<type name="6">sin(r)^2</>
<type name="7">1+sin(r)^2</>
<type name="8">cos(r)^2</>
</types>

*********************************************************************************************************************/

static ERR VECTORSHAPE_SET_Mod(extVectorShape *Self, int Value)
{
   if ((Value < 0) or (Value > 8)) return ERR::InvalidValue;
   Self->Mod = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
N1: A parameter for the super shape algorithm.

This field sets the Superformula's 'N1' parameter value.

*********************************************************************************************************************/

static ERR VECTORSHAPE_SET_N1(extVectorShape *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value IS 0)) return ERR::InvalidValue;
   Self->N1 = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
N2: A parameter for the super shape algorithm.

This field sets the Superformula's 'N2' parameter value.

*********************************************************************************************************************/

static ERR VECTORSHAPE_SET_N2(extVectorShape *Self, double Value)
{
   if ((not std::isfinite(Value))) return ERR::InvalidValue;
   Self->N2 = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
N3: A parameter for the super shape algorithm.

This field sets the Superformula's 'N3' parameter value.

*********************************************************************************************************************/

static ERR VECTORSHAPE_SET_N3(extVectorShape *Self, double Value)
{
   if ((not std::isfinite(Value))) return ERR::InvalidValue;
   Self->N3 = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Phi: A parameter for the super shape algorithm.

The Phi value has an impact on the length of the generated path.  If the super shape parameters form a circular path
(whereby the last vertex meets the first) then the Phi value should not be modified.  If the path does not meet
itself then the Phi value should be increased until it does.  The minimum (and default) value is 2.  It is recommended
that the Phi value is increased in increments of 2 until the desired effect is achieved.

*********************************************************************************************************************/

static ERR VECTORSHAPE_SET_Phi(extVectorShape *Self, double Value)
{
   if (std::isfinite(Value) and (Value >= 2.0)) {
      Self->Phi = Value;
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
Radius: The radius of the generated shape.  Expressed as a fixed or scaled coordinate.

The Radius defines the final size of the generated shape.  It can be expressed in fixed or scaled terms and must be
finite and non-negative.  With #Normalise enabled, the unspiralled formula is fitted by its largest sampled absolute
X or Y extent.  With Normalise disabled, Radius multiplies the natural formula coordinates.

*********************************************************************************************************************/

static ERR VECTORSHAPE_GET_Radius(extVectorShape *Self, Unit *Value)
{
   *Value = Self->Radius.defined() ? Self->Radius : Unit(0);
   return ERR::Okay;
}

static ERR VECTORSHAPE_SET_Radius(extVectorShape *Self, Unit &Value)
{
   if ((not std::isfinite(double(Value))) or (Value < 0)) return ERR::InvalidValue;
   Self->Radius = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Repeat: Repeat the generated shape multiple times.

If set to a value greater than one, the Repeat field will cause the generated shape to be replicated multiple times
at consistent intervals leading to the center point.

The Repeat value cannot be set in conjunction with #Spiral.

*********************************************************************************************************************/

static ERR VECTORSHAPE_SET_Repeat(extVectorShape *Self, int Value)
{
   if ((Value >= 0) and (Value < 512)) {
      Self->Repeat = Value;
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
Spiral: Alters the generated super shape so that it forms a spiral.

Setting the Spiral field to a value greater than one will cause the path generator to form spirals, up to the value
specified.  For instance, a value of 5 with the default Phi generates five revolutions.

*********************************************************************************************************************/

static ERR VECTORSHAPE_SET_Spiral(extVectorShape *Self, int Value)
{
   if (Value >= 0) {
      Self->Spiral = Value;
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
Vertices: Controls the number of angular sampling intervals for the super shape.

Setting the Vertices field disables the #Tolerance behaviour by setting it to zero, and enables manual mode.
Vertices specifies uniform angular sampling intervals across the sweep.
Low values can deliberately produce polygonal shapes.  Values must be between 3 and 16KiB.
-END-
*********************************************************************************************************************/

static ERR VECTORSHAPE_SET_Vertices(extVectorShape *Self, int Value)
{
   if ((Value >= 3) and (Value < 16 * 1024)) {
      Self->Vertices = Value;
      Self->Tolerance = 0;
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

//********************************************************************************************************************

#include "supershape_def.cpp"

static const FieldArray clVectorShapeFields[] = {
   { "M",          FDF_DOUBLE|FDF_RW, nullptr, VECTORSHAPE_SET_M },
   { "N1",         FDF_DOUBLE|FDF_RW, nullptr, VECTORSHAPE_SET_N1 },
   { "N2",         FDF_DOUBLE|FDF_RW, nullptr, VECTORSHAPE_SET_N2 },
   { "N3",         FDF_DOUBLE|FDF_RW, nullptr, VECTORSHAPE_SET_N3 },
   { "A",          FDF_DOUBLE|FDF_RW, nullptr, VECTORSHAPE_SET_A },
   { "B",          FDF_DOUBLE|FDF_RW, nullptr, VECTORSHAPE_SET_B },
   { "Phi",        FDF_DOUBLE|FDF_RW, nullptr, VECTORSHAPE_SET_Phi },
   { "Tolerance",  FDF_DOUBLE|FDF_RW, nullptr, VECTORSHAPE_SET_Tolerance },
   { "StartAngle", FDF_DOUBLE|FDF_RW, nullptr, VECTORSHAPE_SET_StartAngle },
   { "EndAngle",   FDF_DOUBLE|FDF_RW, nullptr, VECTORSHAPE_SET_EndAngle },
   { "Offset",     FDF_DOUBLE|FDF_RW, nullptr, VECTORSHAPE_SET_Offset },
   { "Vertices",   FDF_INT|FDF_RW, nullptr, VECTORSHAPE_SET_Vertices },
   { "Spiral",     FDF_INT|FDF_RW, nullptr, VECTORSHAPE_SET_Spiral },
   { "Repeat",     FDF_INT|FDF_RW, nullptr, VECTORSHAPE_SET_Repeat },
   { "Close",      FDF_INT|FDF_RW, nullptr, VECTORSHAPE_SET_Close },
   { "Mod",        FDF_INT|FDF_RW, nullptr, VECTORSHAPE_SET_Mod },
   { "Normalise",  FDF_INT|FDF_RW, nullptr, VECTORSHAPE_SET_Normalise },
   { "CX",         FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, VECTORSHAPE_GET_CX, VECTORSHAPE_SET_CX },
   { "CY",         FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, VECTORSHAPE_GET_CY, VECTORSHAPE_SET_CY },
   { "Radius",     FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, VECTORSHAPE_GET_Radius,  VECTORSHAPE_SET_Radius },
   { "R",          FDF_SYNONYM },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_supershape(void)
{
   clVectorShape = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTOR),
      fl::ClassID(CLASSID::VECTORSHAPE),
      fl::Name("VectorShape"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clVectorShapeActions),
      fl::Fields(clVectorShapeFields),
      fl::Size(sizeof(extVectorShape)),
      fl::Path(MOD_PATH));

   return clVectorShape ? ERR::Okay : ERR::AddClass;
}

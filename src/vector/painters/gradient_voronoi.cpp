/*********************************************************************************************************************

-CLASS-
GradientVoronoi: Seeded Worley/Voronoi field colour gradient paint server.

GradientVoronoi scatters deterministic feature points inside the target path bounds and maps the selected Worley
field through the inherited colour ramp.  `Seed` controls repeatability, `PointCount` controls feature density,
`WorleyMode` selects the field interpretation, and `WorleyMetric` selects the distance metric.

This gradient operates exclusively in `BOUNDING_BOX` space.  If @Gradient.Units is set to `USERSPACE` it will be
reset to `BOUNDING_BOX` during initialisation.

-END-

*********************************************************************************************************************/

static ERR GRADIENTVORONOI_Init(extGradientVoronoi *Self)
{
   kt::Log log;

   if (Self->Units IS VUNIT::USERSPACE) {
      log.warning("Voronoi gradients are not compatible with Units.USERSPACE.");
      Self->Units = VUNIT::BOUNDING_BOX;
   }

   if (Self->Floor >= Self->Multiplier) return log.warning(ERR::OutOfRange);

   if ((Self->PointCount < 1) or (Self->PointCount > 4096)) return log.warning(ERR::OutOfRange);

   if (Self->Points.size() > 4096) return log.warning(ERR::OutOfRange);

   for (auto &point : Self->Points) {
      if ((not std::isfinite(point.X)) or (not std::isfinite(point.Y)) or (not std::isfinite(point.Height))) {
         return log.warning(ERR::InvalidValue);
      }

      if ((point.X < 0.0) or (point.X > 1.0) or (point.Y < 0.0) or (point.Y > 1.0)) {
         return log.warning(ERR::OutOfRange);
      }
   }

   if ((Self->Jitter < 0.0) or (Self->Jitter > 1.0)) return log.warning(ERR::OutOfRange);

   if ((int(Self->WorleyMode) < int(WLF::F1)) or (int(Self->WorleyMode) > int(WLF::PEAKS))) {
      return log.warning(ERR::OutOfRange);
   }

   if ((int(Self->WorleyMetric) < int(WLM::EUCLIDEAN)) or (int(Self->WorleyMetric) > int(WLM::CHEBYSHEV))) {
      return log.warning(ERR::OutOfRange);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Floor: Colour ramp floor for Voronoi gradients.

The Floor value is used as the floor for Voronoi gradient colour values.  It has a valid range of `0 <= Floor <
Multiplier`; the constraint against #Multiplier is enforced at initialisation.

*********************************************************************************************************************/

static ERR GRADIENTVORONOI_GET_Floor(extGradientVoronoi *Self, Unit *Value)
{
   *Value = Self->Floor;
   return ERR::Okay;
}

static ERR GRADIENTVORONOI_SET_Floor(extGradientVoronoi *Self, Unit &Value)
{
   if (Value < 0) return ERR::OutOfRange;
   Self->Floor = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
HeightMax: Maximum seeded feature-point height.

HeightMax defines the upper bound for per-point heights used by the `WEIGHTED` and `PEAKS` Worley modes.

*********************************************************************************************************************/

static ERR GRADIENTVORONOI_GET_HeightMax(extGradientVoronoi *Self, double *Value)
{
   *Value = Self->HeightMax;
   return ERR::Okay;
}

static ERR GRADIENTVORONOI_SET_HeightMax(extGradientVoronoi *Self, double Value)
{
   Self->HeightMax = Value;
   Self->WorleyHash = 0;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
HeightMin: Minimum seeded feature-point height.

HeightMin defines the lower bound for per-point heights used by the `WEIGHTED` and `PEAKS` Worley modes.  When
HeightMin and #HeightMax match, every feature point has the same height.

*********************************************************************************************************************/

static ERR GRADIENTVORONOI_GET_HeightMin(extGradientVoronoi *Self, double *Value)
{
   *Value = Self->HeightMin;
   return ERR::Okay;
}

static ERR GRADIENTVORONOI_SET_HeightMin(extGradientVoronoi *Self, double Value)
{
   Self->HeightMin = Value;
   Self->WorleyHash = 0;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Jitter: Normalised jitter for grid-distributed feature points.

Jitter ranges from `0` to `1`.  A value of zero uses uniform random placement; values above zero place points on a
jittered grid, with larger values allowing greater movement within each grid cell.

*********************************************************************************************************************/

static ERR GRADIENTVORONOI_GET_Jitter(extGradientVoronoi *Self, double *Value)
{
   *Value = Self->Jitter;
   return ERR::Okay;
}

static ERR GRADIENTVORONOI_SET_Jitter(extGradientVoronoi *Self, double Value)
{
   if ((Value < 0.0) or (Value > 1.0)) return ERR::OutOfRange;
   Self->Jitter = Value;
   Self->WorleyHash = 0;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Multiplier: Colour ramp multiplier for Voronoi gradients.

The Multiplier value acts as a multiplier for Voronoi gradient colour values.  It has a valid range of `.01 <
Multiplier < 10`.

*********************************************************************************************************************/

static ERR GRADIENTVORONOI_GET_Multiplier(extGradientVoronoi *Self, Unit *Value)
{
   *Value = Self->Multiplier;
   return ERR::Okay;
}

static ERR GRADIENTVORONOI_SET_Multiplier(extGradientVoronoi *Self, Unit &Value)
{
   if ((Value < 0.01) or (Value > 10)) return ERR::OutOfRange;
   Self->Multiplier = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
PointCount: Number of seeded Voronoi feature points.

PointCount controls how many feature points are generated inside the target path bounds.  The accepted range is
`1` to `4096` and the default is `16`.  This field is ignored when explicit #Points are supplied.

*********************************************************************************************************************/

static ERR GRADIENTVORONOI_GET_PointCount(extGradientVoronoi *Self, int *Value)
{
   *Value = Self->PointCount;
   return ERR::Okay;
}

static ERR GRADIENTVORONOI_SET_PointCount(extGradientVoronoi *Self, int Value)
{
   if ((Value < 1) or (Value > 4096)) return ERR::OutOfRange;
   Self->PointCount = Value;
   Self->WorleyHash = 0;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Points: User-defined Voronoi feature points.

The Points field accepts a vector of !VoronoiPoint structures that define the feature-point coordinates used by the
Voronoi field.  When at least one point is supplied, the procedural #Seed, #PointCount, #HeightMin, #HeightMax and
#Jitter fields are ignored and the field is generated from the supplied points instead.

Point coordinates are normalised to the target path bounds, with `(0, 0)` at the top-left of the bounds and `(1, 1)`
at the bottom-right.  The Height value is used by the `WEIGHTED` and `PEAKS` Worley modes and ignored by the distance
only modes.

*********************************************************************************************************************/

static ERR GRADIENTVORONOI_GET_Points(extGradientVoronoi *Self, std::span<VoronoiPoint> *Value)
{
   *Value = std::span<VoronoiPoint>(Self->Points);
   return ERR::Okay;
}

static ERR GRADIENTVORONOI_SET_Points(extGradientVoronoi *Self, std::span<const VoronoiPoint> *Value)
{
   Self->Points.clear();
   if ((Value) and (Value->size() > 0)) {
      if (Value->size() > 4096) return ERR::OutOfRange;

      Self->Points.reserve(Value->size());
      for (int i=0; i < Value->size(); i++) {
         const VoronoiPoint &point = Value[0][i];
         if ((not std::isfinite(point.X)) or (not std::isfinite(point.Y)) or (not std::isfinite(point.Height))) {
            return ERR::InvalidValue;
         }

         if ((point.X < 0.0) or (point.X > 1.0) or (point.Y < 0.0) or (point.Y > 1.0)) return ERR::OutOfRange;
         Self->Points.push_back(point);
      }
   }

   Self->WorleyHash = 0;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Seed: Deterministic seed for feature-point generation.

Seed controls the generated feature points and their heights.  A value of zero derives a stable seed from the target
path fingerprint.

*********************************************************************************************************************/

static ERR GRADIENTVORONOI_GET_Seed(extGradientVoronoi *Self, int64_t *Value)
{
   *Value = Self->Seed;
   return ERR::Okay;
}

static ERR GRADIENTVORONOI_SET_Seed(extGradientVoronoi *Self, int64_t Value)
{
   Self->Seed = Value;
   Self->WorleyHash = 0;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
WorleyMetric: Distance metric for the Voronoi field.
Lookup: WLM

WorleyMetric selects the distance function used when measuring each pixel against the seeded feature points.

*********************************************************************************************************************/

static ERR GRADIENTVORONOI_GET_WorleyMetric(extGradientVoronoi *Self, WLM *Value)
{
   *Value = Self->WorleyMetric;
   return ERR::Okay;
}

static ERR GRADIENTVORONOI_SET_WorleyMetric(extGradientVoronoi *Self, WLM Value)
{
   if ((int(Value) < int(WLM::EUCLIDEAN)) or (int(Value) > int(WLM::CHEBYSHEV))) return ERR::OutOfRange;
   Self->WorleyMetric = Value;
   Self->WorleyHash = 0;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
WorleyMode: Field mode for the Voronoi gradient.
Lookup: WLF

WorleyMode selects how distances to feature points are converted into the scalar field that feeds the colour ramp.

-END-
*********************************************************************************************************************/

static ERR GRADIENTVORONOI_GET_WorleyMode(extGradientVoronoi *Self, WLF *Value)
{
   *Value = Self->WorleyMode;
   return ERR::Okay;
}

static ERR GRADIENTVORONOI_SET_WorleyMode(extGradientVoronoi *Self, WLF Value)
{
   if ((int(Value) < int(WLF::F1)) or (int(Value) > int(WLF::PEAKS))) return ERR::OutOfRange;
   Self->WorleyMode = Value;
   Self->WorleyHash = 0;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "gradient_voronoi_def.cpp"

static const FieldArray clGradientVoronoiFields[] = {
   { "Floor",         FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, GRADIENTVORONOI_GET_Floor, GRADIENTVORONOI_SET_Floor },
   { "Multiplier",    FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, GRADIENTVORONOI_GET_Multiplier, GRADIENTVORONOI_SET_Multiplier },
   { "Seed",          FDF_VIRTUAL|FDF_INT64|FDF_RW|FDF_PURE, GRADIENTVORONOI_GET_Seed, GRADIENTVORONOI_SET_Seed },
   { "PointCount",    FDF_VIRTUAL|FDF_INT|FDF_RW|FDF_PURE, GRADIENTVORONOI_GET_PointCount, GRADIENTVORONOI_SET_PointCount },
   { "WorleyMode",    FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW|FDF_PURE, GRADIENTVORONOI_GET_WorleyMode, GRADIENTVORONOI_SET_WorleyMode, &clGradientVoronoiWLF },
   { "WorleyMetric",  FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW|FDF_PURE, GRADIENTVORONOI_GET_WorleyMetric, GRADIENTVORONOI_SET_WorleyMetric, &clGradientVoronoiWLM },
   { "HeightMin",     FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, GRADIENTVORONOI_GET_HeightMin, GRADIENTVORONOI_SET_HeightMin },
   { "HeightMax",     FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, GRADIENTVORONOI_GET_HeightMax, GRADIENTVORONOI_SET_HeightMax },
   { "Jitter",        FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, GRADIENTVORONOI_GET_Jitter, GRADIENTVORONOI_SET_Jitter },
   { "Points",        FDF_VIRTUAL|FDF_ARRAY|FDF_STRUCT|FDF_RW|FDF_PURE, GRADIENTVORONOI_GET_Points, GRADIENTVORONOI_SET_Points, "VoronoiPoint" },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_gradient_voronoi(void)
{
   clGradientVoronoi = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::GRADIENT),
      fl::ClassID(CLASSID::GRADIENTVORONOI),
      fl::Name("GradientVoronoi"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clGradientVoronoiActions),
      fl::Fields(clGradientVoronoiFields),
      fl::Size(sizeof(extGradientVoronoi)),
      fl::Path(MOD_PATH));

   return clGradientVoronoi ? ERR::Okay : ERR::AddClass;
}

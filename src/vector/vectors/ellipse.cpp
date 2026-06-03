/*********************************************************************************************************************

-CLASS-
VectorEllipse: Extends the Vector class with support for elliptical path generation.

The VectorEllipse class provides the necessary functionality for elliptical path generation.

-END-

*********************************************************************************************************************/

class extVectorEllipse : public extVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORELLIPSE;
   static constexpr CSTRING CLASS_NAME = "VectorEllipse";
   using create = kt::Create<extVectorEllipse>;

   double eCX, eCY;
   double eRadiusX, eRadiusY;
   DMF eDimensions;
   int eVertices;
};

//********************************************************************************************************************

static void generate_ellipse(extVectorEllipse *Vector, agg::path_storage &Path)
{
   double cx = Vector->eCX, cy = Vector->eCY;
   double rx = Vector->eRadiusX, ry = Vector->eRadiusY;

   if (dmf::has(Vector->eDimensions, DMF::SCALED_CENTER_X|DMF::SCALED_CENTER_Y|DMF::SCALED_RADIUS_X|DMF::SCALED_RADIUS_Y)) {
      auto view_width = get_parent_width(Vector);
      auto view_height = get_parent_height(Vector);
      double diag = 0;

      if (dmf::hasScaledCenterX(Vector->eDimensions)) cx *= view_width;
      if (dmf::hasScaledCenterY(Vector->eDimensions)) cy *= view_height;

      if (dmf::hasScaledRadiusX(Vector->eDimensions)) {
         if (diag IS 0) diag = svg_diag(view_width, view_height);
         rx *= diag;
      }

      if (dmf::hasScaledRadiusY(Vector->eDimensions)) {
         if (diag IS 0) diag = svg_diag(view_width, view_height);
         ry *= diag;
      }
   }

#if 0
   // Create an ellipse using bezier arcs.  Unfortunately the precision of the existing arc code
   // is not good enough to make this viable at the current time.
   // Top -> right -> bottom -> left -> top

   Path.move_to(cx, cy-ry);
   Path.arc_to(rx, ry, 0 /* angle */, 0 /* large */, 1 /* sweep */, cx+rx, cy);
   Path.arc_to(rx, ry, 0, 0, 1, cx, cy+ry);
   Path.arc_to(rx, ry, 0, 0, 1, cx-rx, cy);
   Path.arc_to(rx, ry, 0, 0, 1, cx, cy-ry);
   Path.close_polygon();
#else
   uint32_t vertices;
   if (Vector->eVertices >= 3) vertices = Vector->eVertices;
   else {
      // Calculate the number of vertices needed for a smooth result, based on the final scale of the ellipse
      // when parent views are taken into consideration.
      auto scale = Vector->Transform.scale();
      double ra = (fabs(rx * scale) + fabs(ry * scale)) * 0.5;
      double da = acos(ra / (ra + 0.125 / scale)) * 2.0;
      vertices = agg::uround(2.0 * agg::pi / da);
      if (vertices < 3) vertices = 3; // Because you need at least 3 vertices to create a shape.
   }

   // TODO: Using co/sine lookup tables would speed up this loop.

   for (uint32_t v=0; v < vertices; v++) {
      double angle = double(v) / double(vertices) * 2.0 * agg::pi;
      //if (m_cw) angle = 2.0 * agg::pi - angle;
      double x = cx + cos(angle) * rx;
      double y = cy + sin(angle) * ry;
      if (v == 0) Path.move_to(x, y);
      else Path.line_to(x, y);
   }
   Path.close_polygon();
#endif

   Vector->Bounds = { cx - rx, cy - ry, cx + rx, cy + ry };

   if ((rx <= 0) or (ry <= 0)) Vector->ValidState = false;
   else Vector->ValidState = true;
}

/*********************************************************************************************************************
-ACTION-
Move: Moves the center of the ellipse by a relative distance.

*********************************************************************************************************************/

static ERR ELLIPSE_Move(extVectorEllipse *Self, struct acMove *Args)
{
   if (!Args) return ERR::NullArgs;

   Self->eCX += Args->DeltaX;
   Self->eCY += Args->DeltaY;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
MoveToPoint: Moves the center of the ellipse to a new position.

*********************************************************************************************************************/

static ERR ELLIPSE_MoveToPoint(extVectorEllipse *Self, struct acMoveToPoint *Args)
{
   if (!Args) return ERR::NullArgs;

   if ((Args->Flags & MTF::X) != MTF::NIL) Self->eCX = Args->X;
   if ((Args->Flags & MTF::Y) != MTF::NIL) Self->eCY = Args->Y;
   if ((Args->Flags & MTF::RELATIVE) != MTF::NIL) Self->eDimensions = (Self->eDimensions | DMF::SCALED_CENTER_X | DMF::SCALED_CENTER_Y) & ~(DMF::FIXED_CENTER_X | DMF::FIXED_CENTER_Y);
   else Self->eDimensions = (Self->eDimensions | DMF::FIXED_CENTER_X | DMF::FIXED_CENTER_Y) & ~(DMF::SCALED_CENTER_X | DMF::SCALED_CENTER_Y);
   reset_path(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR ELLIPSE_NewObject(extVectorEllipse *Self)
{
   Self->GeneratePath = (void (*)(extVector *, agg::path_storage &))&generate_ellipse;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Dimensions: Dimension flags define whether individual dimension fields contain fixed or relative values.

The following dimension flags are supported:

<types lookup="DMF">
<type name="FIXED_RADIUS_X">The #RadiusX value is a fixed coordinate.</>
<type name="FIXED_RADIUS_Y">The #RadiusY value is a fixed coordinate.</>
<type name="FIXED_CENTER_X">The #CenterX value is a fixed coordinate.</>
<type name="FIXED_CENTER_Y">The #CenterY value is a fixed coordinate.</>
<type name="SCALED_RADIUS_X">The #RadiusX value is a scaled coordinate.</>
<type name="SCALED_RADIUS_Y">The #RadiusY value is a scaled coordinate.</>
<type name="SCALED_CENTER_X">The #CenterX value is a scaled coordinate.</>
<type name="SCALED_CENTER_Y">The #CenterY value is a scaled coordinate.</>
</types>

*********************************************************************************************************************/

static ERR ELLIPSE_GET_Dimensions(extVectorEllipse *Self, DMF *Value)
{
   *Value = Self->eDimensions;
   return ERR::Okay;
}

static ERR ELLIPSE_SET_Dimensions(extVectorEllipse *Self, DMF Value)
{
   Self->eDimensions = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Height: The height (vertical diameter) of the ellipse.

The height of the ellipse is defined here as the equivalent of `RadiusY * 2.0`.

*********************************************************************************************************************/

static ERR ELLIPSE_GET_Height(extVectorEllipse *Self, Unit *Value)
{
   Value->set(Self->eRadiusY * 2.0);
   return ERR::Okay;
}

static ERR ELLIPSE_SET_Height(extVectorEllipse *Self, Unit &Value)
{
   Self->eRadiusY = Value * 0.5;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
CenterX: The horizontal center of the ellipse.  Expressed as a fixed or scaled coordinate.

The horizontal center of the ellipse is defined here as either a fixed or scaled value.

*********************************************************************************************************************/

static ERR ELLIPSE_GET_CenterX(extVectorEllipse *Self, Unit *Value)
{
   Value->set(Self->eCX);
   return ERR::Okay;
}

static ERR ELLIPSE_SET_CenterX(extVectorEllipse *Self, Unit &Value)
{
   if (Value.scaled()) Self->eDimensions = (Self->eDimensions | DMF::SCALED_CENTER_X) & (~DMF::FIXED_CENTER_X);
   else Self->eDimensions = (Self->eDimensions | DMF::FIXED_CENTER_X) & (~DMF::SCALED_CENTER_X);
   Self->eCX = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
CenterY: The vertical center of the ellipse.  Expressed as a fixed or scaled coordinate.

The vertical center of the ellipse is defined here as either a fixed or scaled value.

*********************************************************************************************************************/

static ERR ELLIPSE_GET_CenterY(extVectorEllipse *Self, Unit *Value)
{
   Value->set(Self->eCY);
   return ERR::Okay;
}

static ERR ELLIPSE_SET_CenterY(extVectorEllipse *Self, Unit &Value)
{
   if (Value.scaled()) Self->eDimensions = (Self->eDimensions | DMF::SCALED_CENTER_Y) & (~DMF::FIXED_CENTER_Y);
   else Self->eDimensions = (Self->eDimensions | DMF::FIXED_CENTER_Y) & (~DMF::SCALED_CENTER_Y);
   Self->eCY = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Radius: The radius of the ellipse.  Expressed as a fixed or scaled coordinate.

The radius of the ellipse is defined here as either a fixed or scaled value.  Updating the radius will set both the
#RadiusX and #RadiusY values simultaneously.

*********************************************************************************************************************/

static ERR ELLIPSE_GET_Radius(extVectorEllipse *Self, Unit *Value)
{
   Value->set((Self->eRadiusX + Self->eRadiusY) * 0.5);
   return ERR::Okay;
}

static ERR ELLIPSE_SET_Radius(extVectorEllipse *Self, Unit &Value)
{
   if (Value.scaled()) Self->eDimensions = (Self->eDimensions|DMF::SCALED_RADIUS_X|DMF::SCALED_RADIUS_Y) & (~(DMF::FIXED_RADIUS_X|DMF::FIXED_RADIUS_Y));
   else Self->eDimensions = (Self->eDimensions|DMF::FIXED_RADIUS_X|DMF::FIXED_RADIUS_Y) & (~(DMF::SCALED_RADIUS_X|DMF::SCALED_RADIUS_Y));
   Self->eRadiusX = Self->eRadiusY = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
RadiusX: The horizontal radius of the ellipse.

The horizontal radius of the ellipse is defined here as either a fixed or scaled value.

*********************************************************************************************************************/

static ERR ELLIPSE_GET_RadiusX(extVectorEllipse *Self, Unit *Value)
{
   Value->set(Self->eRadiusX);
   return ERR::Okay;
}

static ERR ELLIPSE_SET_RadiusX(extVectorEllipse *Self, Unit &Value)
{
   if (Value.scaled()) Self->eDimensions = (Self->eDimensions | DMF::SCALED_RADIUS_X) & (~DMF::FIXED_RADIUS_X);
   else Self->eDimensions = (Self->eDimensions | DMF::FIXED_RADIUS_X) & (~DMF::SCALED_RADIUS_X);
   Self->eRadiusX = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
RadiusY: The vertical radius of the ellipse.

The vertical radius of the ellipse is defined here as either a fixed or scaled value.

*********************************************************************************************************************/

static ERR ELLIPSE_GET_RadiusY(extVectorEllipse *Self, Unit *Value)
{
   Value->set(Self->eRadiusY);
   return ERR::Okay;
}

static ERR ELLIPSE_SET_RadiusY(extVectorEllipse *Self, Unit &Value)
{
   if (Value.scaled()) Self->eDimensions = (Self->eDimensions | DMF::SCALED_RADIUS_Y) & (~DMF::FIXED_RADIUS_Y);
   else Self->eDimensions = (Self->eDimensions | DMF::FIXED_RADIUS_Y) & (~DMF::SCALED_RADIUS_Y);
   Self->eRadiusY = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Vertices: Limits the total number of vertices generated for the ellipse.

Setting a value in Vertices will limit the total number of vertices that are generated for the ellipse.  This feature
is useful for generating common convex geometrical shapes such as triangles, polygons, hexagons and so forth; because
their vertices will always touch the sides of an elliptical area.

Please note that this feature is not part of the SVG standard.

*********************************************************************************************************************/

static ERR ELLIPSE_GET_Vertices(extVectorEllipse *Self, int *Value)
{
   *Value = Self->eVertices;
   return ERR::Okay;
}

static ERR ELLIPSE_SET_Vertices(extVectorEllipse *Self, int Value)
{
   if (((Value >= 3) and (Value < 4096)) or (!Value)) {
      Self->eVertices = Value;
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
Width: The width (horizontal diameter) of the ellipse.

The width of the ellipse is defined here as the equivalent of `RadiusX * 2.0`.
-END-
*********************************************************************************************************************/

static ERR ELLIPSE_GET_Width(extVectorEllipse *Self, Unit *Value)
{
   Value->set(Self->eRadiusX * 2.0);
   return ERR::Okay;
}

static ERR ELLIPSE_SET_Width(extVectorEllipse *Self, Unit &Value)
{
   Self->eRadiusX = Value * 0.5;
   reset_path(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static const FieldDef clEllipseDimensions[] = {
   { "FixedRadiusX",  DMF::FIXED_RADIUS_X },
   { "FixedRadiusY",  DMF::FIXED_RADIUS_Y },
   { "FixedCenterX",  DMF::FIXED_CENTER_X },
   { "FixedCenterY",  DMF::FIXED_CENTER_Y },
   { "ScaledRadiusX", DMF::SCALED_RADIUS_X },
   { "ScaledRadiusY", DMF::SCALED_RADIUS_Y },
   { "ScaledCenterX", DMF::SCALED_CENTER_X },
   { "ScaledCenterY", DMF::SCALED_CENTER_Y },
   { nullptr, 0 }
};

static const FieldArray clEllipseFields[] = {
   { "Width",      FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, ELLIPSE_GET_Width,   ELLIPSE_SET_Width },
   { "Height",     FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, ELLIPSE_GET_Height,  ELLIPSE_SET_Height },
   { "CenterX",    FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, ELLIPSE_GET_CenterX, ELLIPSE_SET_CenterX },
   { "CenterY",    FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, ELLIPSE_GET_CenterY, ELLIPSE_SET_CenterY },
   { "Radius",     FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, ELLIPSE_GET_Radius,  ELLIPSE_SET_Radius },
   { "RadiusX",    FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, ELLIPSE_GET_RadiusX, ELLIPSE_SET_RadiusX },
   { "RadiusY",    FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, ELLIPSE_GET_RadiusY, ELLIPSE_SET_RadiusY },
   { "Dimensions", FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW|FDF_PURE, ELLIPSE_GET_Dimensions, ELLIPSE_SET_Dimensions, &clEllipseDimensions },
   { "Vertices",   FDF_VIRTUAL|FDF_INT|FDF_RW|FDF_PURE, ELLIPSE_GET_Vertices, ELLIPSE_SET_Vertices },
   // Synonyms
   { "CX", FDF_SYNONYM|FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, ELLIPSE_GET_CenterX, ELLIPSE_SET_CenterX },
   { "CY", FDF_SYNONYM|FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, ELLIPSE_GET_CenterY, ELLIPSE_SET_CenterY },
   { "R",  FDF_SYNONYM|FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, ELLIPSE_GET_Radius,  ELLIPSE_SET_Radius },
   { "RX", FDF_SYNONYM|FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, ELLIPSE_GET_RadiusX, ELLIPSE_SET_RadiusX },
   { "RY", FDF_SYNONYM|FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, ELLIPSE_GET_RadiusY, ELLIPSE_SET_RadiusY },
   END_FIELD
};

static const ActionArray clEllipseActions[] = {
   { AC::NewObject,     ELLIPSE_NewObject },
   { AC::Move,          ELLIPSE_Move },
   { AC::MoveToPoint,   ELLIPSE_MoveToPoint },
   //{ AC::Redimension, ELLIPSE_Redimension },
   //{ AC::Resize,      ELLIPSE_Resize },
   { AC::NIL, nullptr }
};

static ERR init_ellipse(void)
{
   clVectorEllipse = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTOR),
      fl::ClassID(CLASSID::VECTORELLIPSE),
      fl::Name("VectorEllipse"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clEllipseActions),
      fl::Fields(clEllipseFields),
      fl::Size(sizeof(extVectorEllipse)),
      fl::Path(MOD_PATH));

   return clVectorEllipse ? ERR::Okay : ERR::AddClass;
}

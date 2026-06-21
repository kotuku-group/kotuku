/*********************************************************************************************************************

-CLASS-
VectorEllipse: Extends the Vector class with support for elliptical path generation.

The VectorEllipse class provides the necessary functionality for elliptical path generation.

-END-

*********************************************************************************************************************/

static void generate_ellipse(class extVectorEllipse *Vector, agg::path_storage &Path);

class extVectorEllipse : public extVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORELLIPSE;
   static constexpr CSTRING CLASS_NAME = "VectorEllipse";
   using create = kt::Create<extVectorEllipse>;

   Unit eCX = Unit(0), eCY = Unit(0);
   Unit eRadiusX = Unit(0), eRadiusY = Unit(0);
   int eVertices = 0;

   extVectorEllipse() {
      GeneratePath = (void (*)(extVector *, agg::path_storage &))&generate_ellipse;
   }
};

//********************************************************************************************************************

static void generate_ellipse(extVectorEllipse *Vector, agg::path_storage &Path)
{
   Unit cx = Vector->eCX;
   Unit cy = Vector->eCY;
   Unit rx = Vector->eRadiusX;
   Unit ry = Vector->eRadiusY;

   if (Vector->eCX.scaled() or Vector->eCY.scaled() or Vector->eRadiusX.scaled() or Vector->eRadiusY.scaled()) {
      auto view_width = get_parent_width(Vector);
      auto view_height = get_parent_height(Vector);
      double diag = 0;

      if (Vector->eCX.scaled()) cx = Unit(cx * view_width);
      if (Vector->eCY.scaled()) cy = Unit(cy * view_height);

      if (Vector->eRadiusX.scaled()) {
         if (diag IS 0) diag = svg_diag(view_width, view_height);
         rx = Unit(rx * diag);
      }

      if (Vector->eRadiusY.scaled()) {
         if (diag IS 0) diag = svg_diag(view_width, view_height);
         ry = Unit(ry * diag);
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

static ERR VECTORELLIPSE_Move(extVectorEllipse *Self, struct acMove *Args)
{
   if (not Args) return ERR::NullArgs;

   if (Self->eCX.scaled()) Self->eCX = Unit((Self->eCX * get_parent_width(Self)) + Args->DeltaX);
   else Self->eCX = Unit(Self->eCX + Args->DeltaX);

   if (Self->eCY.scaled()) Self->eCY = Unit((Self->eCY * get_parent_height(Self)) + Args->DeltaY);
   else Self->eCY = Unit(Self->eCY + Args->DeltaY);

   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
MoveToPoint: Moves the center of the ellipse to a new position.

*********************************************************************************************************************/

static ERR VECTORELLIPSE_MoveToPoint(extVectorEllipse *Self, struct acMoveToPoint *Args)
{
   if (not Args) return ERR::NullArgs;

   if ((Args->Flags & MTF::RELATIVE) != MTF::NIL) {
      if ((Args->Flags & MTF::X) != MTF::NIL) Self->eCX = Unit(Args->X, FD_SCALED);
      if ((Args->Flags & MTF::Y) != MTF::NIL) Self->eCY = Unit(Args->Y, FD_SCALED);
   }
   else {
      if ((Args->Flags & MTF::X) != MTF::NIL) Self->eCX = Unit(Args->X);
      if ((Args->Flags & MTF::Y) != MTF::NIL) Self->eCY = Unit(Args->Y);
   }
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Height: The height (vertical diameter) of the ellipse.

The height of the ellipse is defined here as the equivalent of `RadiusY * 2.0`.

*********************************************************************************************************************/

static ERR VECTORELLIPSE_GET_Height(extVectorEllipse *Self, Unit &Value)
{
   Value = Unit(Self->eRadiusY * 2.0, Self->eRadiusY.Type);
   return ERR::Okay;
}

static ERR VECTORELLIPSE_SET_Height(extVectorEllipse *Self, Unit &Value)
{
   Self->eRadiusY = Unit(Value * 0.5, Value.Type);
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
CX: The horizontal center of the ellipse.  Expressed as a fixed or scaled coordinate.

The horizontal center of the ellipse is defined here as either a fixed or scaled value.

*********************************************************************************************************************/

static ERR VECTORELLIPSE_SET_CX(extVectorEllipse *Self, Unit &Value)
{
   Self->eCX = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
CY: The vertical center of the ellipse.  Expressed as a fixed or scaled coordinate.

The vertical center of the ellipse is defined here as either a fixed or scaled value.

*********************************************************************************************************************/

static ERR VECTORELLIPSE_SET_CY(extVectorEllipse *Self, Unit &Value)
{
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

static ERR VECTORELLIPSE_GET_Radius(extVectorEllipse *Self, Unit &Value)
{
   if (Self->eRadiusX.Type IS Self->eRadiusY.Type) {
      Value = Unit((Self->eRadiusX + Self->eRadiusY) * 0.5, Self->eRadiusX.Type);
      return ERR::Okay;
   }
   else if (Self->initialised()) {
      auto view_width = get_parent_width(Self);
      auto view_height = get_parent_height(Self);
      double diag = svg_diag(view_width, view_height);
      double rx = Self->eRadiusX.scaled() ? double(Self->eRadiusX) * diag : double(Self->eRadiusX);
      double ry = Self->eRadiusY.scaled() ? double(Self->eRadiusY) * diag : double(Self->eRadiusY);
      Value = Unit((rx + ry) * 0.5);
      return ERR::Okay;
   }
   else return ERR::NotInitialised;
}

static ERR VECTORELLIPSE_SET_Radius(extVectorEllipse *Self, Unit &Value)
{
   Self->eRadiusX = Self->eRadiusY = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
RadiusX: The horizontal radius of the ellipse.

The horizontal radius of the ellipse is defined here as either a fixed or scaled value.

*********************************************************************************************************************/

static ERR VECTORELLIPSE_GET_RadiusX(extVectorEllipse *Self, Unit &Value)
{
   Value = Self->eRadiusX;
   return ERR::Okay;
}

static ERR VECTORELLIPSE_SET_RadiusX(extVectorEllipse *Self, Unit &Value)
{
   Self->eRadiusX = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
RadiusY: The vertical radius of the ellipse.

The vertical radius of the ellipse is defined here as either a fixed or scaled value.

*********************************************************************************************************************/

static ERR VECTORELLIPSE_GET_RadiusY(extVectorEllipse *Self, Unit &Value)
{
   Value = Self->eRadiusY;
   return ERR::Okay;
}

static ERR VECTORELLIPSE_SET_RadiusY(extVectorEllipse *Self, Unit &Value)
{
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

static ERR VECTORELLIPSE_SET_Vertices(extVectorEllipse *Self, int Value)
{
   if (((Value >= 3) and (Value < 4096)) or (not Value)) {
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

static ERR VECTORELLIPSE_GET_Width(extVectorEllipse *Self, Unit &Value)
{
   Value = Unit(Self->eRadiusX * 2.0, Self->eRadiusX.Type);
   return ERR::Okay;
}

static ERR VECTORELLIPSE_SET_Width(extVectorEllipse *Self, Unit &Value)
{
   Self->eRadiusX = Unit(Value * 0.5, Value.Type);
   reset_path(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

#include "ellipse_def.cpp"

static const FieldArray clEllipseFields[] = {
   { "CX",         FDF_UNIT|FDF_RW, nullptr, VECTORELLIPSE_SET_CX },
   { "CY",         FDF_UNIT|FDF_RW, nullptr, VECTORELLIPSE_SET_CY },
   { "RadiusX",    FDF_UNIT|FDF_RW, nullptr, VECTORELLIPSE_SET_RadiusX },
   { "RadiusY",    FDF_UNIT|FDF_RW, nullptr, VECTORELLIPSE_SET_RadiusY },
   { "Vertices",   FDF_INT|FDF_RW, nullptr, VECTORELLIPSE_SET_Vertices },
   { "Width",      FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, VECTORELLIPSE_GET_Width,   VECTORELLIPSE_SET_Width },
   { "Height",     FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, VECTORELLIPSE_GET_Height,  VECTORELLIPSE_SET_Height },
   { "Radius",     FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, VECTORELLIPSE_GET_Radius,  VECTORELLIPSE_SET_Radius },
   // Synonyms
   { "R",  FDF_SYNONYM|FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, VECTORELLIPSE_GET_Radius,  VECTORELLIPSE_SET_Radius },
   { "RX", FDF_SYNONYM|FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, VECTORELLIPSE_GET_RadiusX, VECTORELLIPSE_SET_RadiusX },
   { "RY", FDF_SYNONYM|FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, VECTORELLIPSE_GET_RadiusY, VECTORELLIPSE_SET_RadiusY },
   END_FIELD
};

static ERR init_ellipse(void)
{
   clVectorEllipse = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTOR),
      fl::ClassID(CLASSID::VECTORELLIPSE),
      fl::Name("VectorEllipse"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clVectorEllipseActions),
      fl::Fields(clEllipseFields),
      fl::Size(sizeof(extVectorEllipse)),
      fl::Path(MOD_PATH));

   return clVectorEllipse ? ERR::Okay : ERR::AddClass;
}

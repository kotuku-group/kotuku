/*********************************************************************************************************************

-CLASS-
VectorPolygon: Extends the Vector class with support for generating polygons.

The VectorPolygon class provides support for three different types of vector:

<list type="ordered">
<li>Closed-point polygons consisting of at least 3 points.</li>
<li>Open polygons consisting of at least 3 points (a 'polyline' in SVG).</li>
<li>Single lines consisting of two points only (a 'line' in SVG).</li>
</list>

To create a polyline, set the #Closed field to `false`.

To create a line, set the #Closed field to `false` and set only two points (#X1,#Y1) and (#X2,#Y2)

-END-

TODO: Add a SetPoint(DOUBLE X, DOUBLE Y) method for modifying existing points.

*********************************************************************************************************************/

constexpr int MAX_POINTS = 1024 * 16; // Maximum of 16k points per polygon object.

static void generate_polygon(extVectorPolygon *Vector, agg::path_storage &Path)
{
   auto view_width = get_parent_width(Vector);
   auto view_height = get_parent_height(Vector);

   if (Vector->Points.size() >= 2) {
      kt::POINT<double> p = { Vector->Points[0].X, Vector->Points[0].Y };
      if (Vector->Points[0].XScaled) p.x *= view_width;
      if (Vector->Points[0].YScaled) p.y *= view_height;
      Path.move_to(p.x, p.y);

      auto min = p; // Record min and max for the boundary.
      auto max = p;
      auto last = p;

      for (unsigned i=1; i < Vector->Points.size(); i++) {
         p.x = Vector->Points[i].X;
         p.y = Vector->Points[i].Y;
         if (Vector->Points[i].XScaled) p.x *= view_width;
         if (Vector->Points[i].YScaled) p.y *= view_height;

         if (p.x < min.x) min.x = p.x;
         if (p.y < min.y) min.y = p.y;
         if (p.x > max.x) max.x = p.x;
         if (p.y > max.y) max.y = p.y;

         // AGG won't draw a line if the start and end points are equal.  The SVG take on zero-length lines
         // complicates things: A zero length subpath with 'stroke-linecap' set to 'square' or 'round' is stroked,
         // but not stroked when 'stroke-linecap' is set to 'butt'.
         //
         // A ham-fisted way of controlling whether or not the line is stroked is to make a micro-adjustment
         // to the coordinate so that they remain unequal.

         if ((Vector->LineCap != agg::line_cap_e::butt_cap) and (p IS last)) p.x += 1.0e-10;

         Path.line_to(p.x, p.y);
         last = p;
      }

      if ((Vector->Points.size() > 2) and (Vector->Closed)) Path.close_polygon();

      Vector->Bounds = { min.x, min.y, max.x, max.y };
   }
   else Vector->Bounds = { 0, 0, 0, 0 };
}

//********************************************************************************************************************
// Converts a string of paired coordinates into a VectorPoint array.

static ERR read_points(extVectorPolygon *Self, std::string_view Value)
{
   Self->Points.clear();

   double x = 0, y = 0;
   bool expect_x = true;
   while (!Value.empty()) {
      if (std::isdigit(Value.front()) or (Value.front() == '-')) {
         auto [ptr, error] = std::from_chars(Value.data(), Value.data() + Value.size(), expect_x ? x : y);
         if (error != std::errc()) break;
         Value.remove_prefix(ptr - Value.data());

         if (!expect_x) Self->Points.emplace_back(VectorPoint{x, y});

         expect_x = !expect_x;
      }
      else Value.remove_prefix(1);
   }

   if (Self->Points.size() < 2) {
      kt::Log log(__FUNCTION__);
      log.traceWarning("List of points requires a minimum of 2 number pairs.");
      Self->Points.clear();
      return log.warning(ERR::InvalidValue);
   }
   return ERR::Okay;
}

//********************************************************************************************************************

/*********************************************************************************************************************
-ACTION-
Move: Moves a polygon to a new position.
-END-
*********************************************************************************************************************/

static ERR VECTORPOLYGON_Move(extVectorPolygon *Self, struct acMove *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   // If any of the polygon's points are relative then we have to cancel the move.
   for (unsigned i=0; i < Self->Points.size(); i++) {
      if ((Self->Points[i].XScaled) or (Self->Points[i].YScaled)) return ERR::InvalidValue;
   }

   for (unsigned i=0; i < Self->Points.size(); i++) {
      Self->Points[i].X += Args->DeltaX;
      Self->Points[i].Y += Args->DeltaY;
   }

   Self->Bounds.left   += Args->DeltaX;
   Self->Bounds.top    += Args->DeltaY;
   Self->Bounds.right  += Args->DeltaX;
   Self->Bounds.bottom += Args->DeltaY;

   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
MoveToPoint: Moves a polygon to a new central position.

This action will permanently modify the coordinates of a polygon so that they offset by the provided coordinate values.

The operation will abort if any of the points in the polygon are discovered to be relative coordinates.
-END-
*********************************************************************************************************************/

static ERR VECTORPOLYGON_MoveToPoint(extVectorPolygon *Self, struct acMoveToPoint *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   // Check if any of the polygon's points are relative, in which case we have to cancel the move.
   for (unsigned i=0; i < Self->Points.size(); i++) {
      if ((Self->Points[i].XScaled) or (Self->Points[i].YScaled)) return ERR::InvalidValue;
   }

   // The provided (X,Y) coordinates will be treated as the polygon's new central position.

   if ((Args->Flags & MTF::X) != MTF::NIL) {
      double center_x = Self->Bounds.width() * 0.5;
      double x_change = Args->X - center_x;
      for (unsigned i=0; i < Self->Points.size(); i++) {
         Self->Points[i].X += x_change;
         Self->Points[i].XScaled = ((Args->Flags & MTF::RELATIVE) != MTF::NIL);
      }
      Self->Bounds.left += x_change;
      Self->Bounds.right += x_change;
   }

   if ((Args->Flags & MTF::Y) != MTF::NIL) {
      double center_y = Self->Bounds.height() * 0.5;
      double y_change = Args->Y - center_y;
      for (unsigned i=0; i < Self->Points.size(); i++) Self->Points[i].Y += y_change;
      Self->Bounds.top += y_change;
      Self->Bounds.bottom += y_change;
   }

   reset_path(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORPOLYGON_NewObject(extVectorPolygon *Self)
{
   Self->GeneratePath = (void (*)(extVector *, agg::path_storage &))&generate_polygon;
   Self->Closed       = true;
   Self->Points.push_back({ 0, 0 }); // Two blank points are needed on construction in order to satisfy polyline requirements.
   Self->Points.push_back({ 0, 0 });
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Resize: Resize the polygon by its width and height.

This action will resize a polygon by adjusting all of its existing points.  The points are rescaled to guarantee that
they are within the provided dimensions.

If a Width and/or Height value of zero is passed, no scaling on the associated axis will occur.

*********************************************************************************************************************/

static ERR VECTORPOLYGON_Resize(extVectorPolygon *Self, struct acResize *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   double current_width = Self->Bounds.width();
   double current_height = Self->Bounds.height();
   double xratio = (Args->Width > 0) ? (current_width / Args->Width) : current_width;
   double yratio = (Args->Height > 0) ? (current_height / Args->Height) : current_height;

   for (unsigned i=0; i < Self->Points.size(); i++) {
      Self->Points[i].X = Self->Points[i].X * xratio;
      Self->Points[i].Y = Self->Points[i].Y * yratio;
   }

   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Closed: If `true`, the polygon will be closed between the start and end points.

Set the Closed field to `true` to ensure that the polygon is closed between the start and end points.  This behaviour is
the default.  If `false`, the polygon will not be closed, which results in the equivalent of the SVG polyline type.

*********************************************************************************************************************/

static ERR POLY_GET_Closed(extVectorPolygon *Self, int *Value)
{
   *Value = Self->Closed;
   return ERR::Okay;
}

static ERR POLY_SET_Closed(extVectorPolygon *Self, int Value)
{
   if (Value) Self->Closed = true;
   else Self->Closed = false;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
PathLength: Calibrates the user agent's distance-along-a-path calculations with that of the author.

The author's computation of the total length of the path, in user units. This value is used to calibrate the user
agent's own distance-along-a-path calculations with that of the author. The user agent will scale all
distance-along-a-path computations by the ratio of PathLength to the user agent's own computed value for total path
length.  This feature potentially affects calculations for text on a path, motion animation and various stroke
operations.

*********************************************************************************************************************/

static ERR POLY_GET_PathLength(extVectorPolygon *Self, int *Value)
{
   *Value = Self->PathLength;
   return ERR::Okay;
}

static ERR POLY_SET_PathLength(extVectorPolygon *Self, int Value)
{
   if (Value >= 0) {
      Self->PathLength = Value;
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
PointsArray: A series of numbered pairs that define the polygon.

The PointsArray field can be set with a !VectorPoint array that defines the shape of a polygon.  A minimum of two
points is required for the shape to be valid.  The !VectorPoint structure consists of the following fields:

!VectorPoint

*********************************************************************************************************************/

static ERR POLY_GET_PointsArray(extVectorPolygon *Self, std::span<VectorPoint> &Value)
{
   Value = std::span<VectorPoint>(Self->Points.data(), Self->Points.size());
   return ERR::Okay;
}

static ERR POLY_SET_PointsArray(extVectorPolygon *Self, std::span<const VectorPoint> &Value)
{
   if (Value.size() >= 2) {
      Self->Points.assign(Value.begin(), Value.end());
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
Points: A series of (X,Y) coordinates that define the polygon.

The Points field can be set with a series of `(X, Y)` coordinates that will define the polygon's shape.  A minimum of
two numbered pairs will be required to define a valid polygon.  Each point must be separated with either white-space or
a comma.

*********************************************************************************************************************/

static ERR POLY_SET_Points(extVectorPolygon *Self, const std::string_view &Value)
{
   if (auto error = read_points(Self, Value); !error) {
      reset_path(Self);
      return ERR::Okay;
   }
   else return error;
}

/*********************************************************************************************************************
-FIELD-
TotalPoints: The total number of coordinates defined in the Points field.

TotalPoints is a read-only field value that reflects the total number of coordinates that have been set in the
#Points array.  The minimum value is 2.

*********************************************************************************************************************/

static ERR POLY_GET_TotalPoints(extVectorPolygon *Self, int *Value)
{
   *Value = Self->Points.size();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
X1: Defines the X coordinate of the first point.

This field defines the X coordinate of the first point of the polygon.  It is recommended that this field is only used
when creating a VectorPolygon that will be used to draw a single line.

By default the value will be treated as a fixed coordinate.  Scaled values are supported if the value is a defined as
a percentage.

*********************************************************************************************************************/

static ERR POLY_GET_X1(extVectorPolygon *Self, Unit *Value)
{
   Value->set(Self->Points[0].X);
   return ERR::Okay;
}

static ERR POLY_SET_X1(extVectorPolygon *Self, Unit &Value)
{
   Self->Points[0].XScaled = Value.scaled();
   Self->Points[0].X = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
X2: Defines the X coordinate of the second point.

This field defines the X coordinate of the second point of the polygon.  It is recommended that this field is only used
when creating a VectorPolygon that will be used to draw a single line.

By default the value will be treated as a fixed coordinate.  Scaled values are supported if the value is a defined as
a percentage.

*********************************************************************************************************************/

static ERR POLY_GET_X2(extVectorPolygon *Self, Unit *Value)
{
   Value->set(Self->Points[1].X);
   return ERR::Okay;
}

static ERR POLY_SET_X2(extVectorPolygon *Self, Unit &Value)
{
   Self->Points[1].XScaled = Value.scaled();
   Self->Points[1].X = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Y1: Defines the Y coordinate of the first point.

This field defines the Y coordinate of the first point of the polygon.  It is recommended that this field is only used
when creating a VectorPolygon that will be used to draw a single line.

By default the value will be treated as a fixed coordinate.  Scaled values are supported if the value is a defined as
a percentage.

*********************************************************************************************************************/

static ERR POLY_GET_Y1(extVectorPolygon *Self, Unit *Value)
{
   Value->set(Self->Points[0].Y);
   return ERR::Okay;
}

static ERR POLY_SET_Y1(extVectorPolygon *Self, Unit &Value)
{
   Self->Points[0].YScaled = Value.scaled();
   Self->Points[0].Y = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Y2: Defines the Y coordinate of the second point.

This field defines the Y coordinate of the second point of the polygon.  It is recommended that this field is only used
when creating a VectorPolygon that will be used to draw a single line.

By default the value will be treated as a fixed coordinate.  Scaled values are supported if the value is a defined as
a percentage.
-END-
*********************************************************************************************************************/

static ERR POLY_GET_Y2(extVectorPolygon *Self, Unit *Value)
{
   Value->set(Self->Points[1].Y);
   return ERR::Okay;
}

static ERR POLY_SET_Y2(extVectorPolygon *Self, Unit &Value)
{
   Self->Points[1].YScaled = Value.scaled();
   Self->Points[1].Y = Value;
   reset_path(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

#include "polygon_def.cpp"

static const FieldArray clPolygonFields[] = {
   { "Closed",      FDF_VIRTUAL|FDF_INT|FD_RW|FDF_PURE,               POLY_GET_Closed, POLY_SET_Closed },
   { "PathLength",  FDF_VIRTUAL|FDF_INT|FDF_RW|FDF_PURE,              POLY_GET_PathLength, POLY_SET_PathLength },
   { "PointsArray", FDF_VIRTUAL|FDF_ARRAY|FDF_STRUCT|FDF_RW|FDF_PURE, POLY_GET_PointsArray, POLY_SET_PointsArray, "VectorPoint" },
   { "Points",      FDF_VIRTUAL|FDF_CPPSTRING|FDF_W,                  nullptr, POLY_SET_Points },
   { "TotalPoints", FDF_VIRTUAL|FDF_INT|FDF_R|FDF_PURE,               POLY_GET_TotalPoints },
   { "X1",          FDF_VIRTUAL|FDF_UNIT|FDF_SCALED|FDF_RW|FDF_PURE, POLY_GET_X1, POLY_SET_X1 },
   { "Y1",          FDF_VIRTUAL|FDF_UNIT|FDF_SCALED|FDF_RW|FDF_PURE, POLY_GET_Y1, POLY_SET_Y1 },
   { "X2",          FDF_VIRTUAL|FDF_UNIT|FDF_SCALED|FDF_RW|FDF_PURE, POLY_GET_X2, POLY_SET_X2 },
   { "Y2",          FDF_VIRTUAL|FDF_UNIT|FDF_SCALED|FDF_RW|FDF_PURE, POLY_GET_Y2, POLY_SET_Y2 },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_polygon(void)
{
   clVectorPolygon = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTOR),
      fl::ClassID(CLASSID::VECTORPOLYGON),
      fl::Name("VectorPolygon"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clVectorPolygonActions),
      fl::Fields(clPolygonFields),
      fl::Size(sizeof(extVectorPolygon)),
      fl::Path(MOD_PATH));

   return clVectorPolygon ? ERR::Okay : ERR::AddClass;
}

/*********************************************************************************************************************

-CLASS-
VectorPath: Extends the Vector class with support for generating custom paths.

VectorPath provides support for parsing SVG styled path strings.

The generated path can be placed with the #X, #Y, #XOffset and #YOffset fields and sized with the #Width and #Height
fields.  These are applied as a post-transform, so the authored path commands are never modified.

A path whose #Sequence uses `~` coordinates on an axis treats a defined box on that axis as its frame.  The box is
defined by #Width, or by #X and #XOffset together.  Refer to #Sequence for details.

-END-

*********************************************************************************************************************/

// Include the exact extrema of one cubic Bezier axis, independent of the renderer's tessellation tolerance.

static void include_curve_bounds(double P0, double P1, double P2, double P3, double &Minimum, double &Maximum)
{
   Minimum = std::min(Minimum, P3);
   Maximum = std::max(Maximum, P3);

   auto include_root = [&](double T) {
      if ((T <= 0) or (T >= 1)) return;
      const double p01 = std::lerp(P0, P1, T);
      const double p12 = std::lerp(P1, P2, T);
      const double p23 = std::lerp(P2, P3, T);
      const double value = std::lerp(std::lerp(p01, p12, T), std::lerp(p12, p23, T), T);
      Minimum = std::min(Minimum, value);
      Maximum = std::max(Maximum, value);
   };

   // The derivative divided by three is a*t*t + b*t + c.  Use a stable quadratic solution to avoid cancellation.

   const double a = (P3 - P0) + 3 * (P1 - P2);
   const double b = 2 * ((P0 - P1) + (P2 - P1));
   const double c = P1 - P0;
   if (a IS 0) {
      if (b != 0) include_root(-c / b);
   }
   else {
      const double discriminant = b * b - 4 * a * c;
      if (discriminant < 0) return;
      const double q = -0.5 * (b + std::copysign(std::sqrt(discriminant), b));
      if (q IS 0) include_root(-b / (2 * a));
      else {
         include_root(q / a);
         include_root(c / q);
      }
   }
}

//********************************************************************************************************************

static TClipRectangle<double> get_authored_path_bounds(const agg::path_storage &Path)
{
   TClipRectangle<double> bounds(1, 1, 0, 0); // Preserve the empty-path bounds used by get_bounds().
   bool first = true;
   double last_x = 0, last_y = 0;

   for (unsigned i=0; i < Path.total_vertices(); i++) {
      double x, y;
      const auto command = Path.vertex(i, &x, &y);
      if (not agg::is_vertex(command)) continue;

      if (first) {
         bounds = { x, y, x, y };
         first = false;
      }

      if (agg::is_curve3(command) and (i + 1 < Path.total_vertices())) {
         double end_x, end_y;
         Path.vertex(++i, &end_x, &end_y);

         // Elevate a quadratic to a cubic so both kinds share the same extrema calculation.

         include_curve_bounds(last_x, std::lerp(last_x, x, 2.0/3.0), std::lerp(end_x, x, 2.0/3.0),
            end_x, bounds.left, bounds.right);
         include_curve_bounds(last_y, std::lerp(last_y, y, 2.0/3.0), std::lerp(end_y, y, 2.0/3.0),
            end_y, bounds.top, bounds.bottom);
         last_x = end_x;
         last_y = end_y;
      }
      else if (agg::is_curve4(command) and (i + 2 < Path.total_vertices())) {
         double control_x, control_y, end_x, end_y;
         Path.vertex(++i, &control_x, &control_y);
         Path.vertex(++i, &end_x, &end_y);
         include_curve_bounds(last_x, x, control_x, end_x, bounds.left, bounds.right);
         include_curve_bounds(last_y, y, control_y, end_y, bounds.top, bounds.bottom);
         last_x = end_x;
         last_y = end_y;
      }
      else {
         bounds.left = std::min(bounds.left, x);
         bounds.right = std::max(bounds.right, x);
         bounds.top = std::min(bounds.top, y);
         bounds.bottom = std::max(bounds.bottom, y);
         last_x = x;
         last_y = y;
      }
   }

   return bounds;
}

//********************************************************************************************************************

inline double path_fixed_width(extVectorPath *Vector, const Unit &Value) {
   return Value.scaled() ? double(Value) * get_parent_width(Vector) : double(Value);
}

inline double path_fixed_height(extVectorPath *Vector, const Unit &Value) {
   return Value.scaled() ? double(Value) * get_parent_height(Vector) : double(Value);
}

// True if an axis of the path has '~' coordinates.  CoordinateDependencies must be current, which holds after
// refresh_unplaced_path().

inline bool path_uses_tilde(extVectorPath *Vector, bool Horizontal) {
   const auto mask = Horizontal ? (PCO::X_OFFSET | PCO::X2_OFFSET | PCO::X3_OFFSET) :
      (PCO::Y_OFFSET | PCO::Y2_OFFSET | PCO::Y3_OFFSET);
   return (Vector->CoordinateDependencies & mask) != PCO::NIL;
}

// The box of one axis, following the VectorRectangle conventions.  An axis has a box if it is sized by Width, or
// stretched between X and XOffset.  On an axis with '~' coordinates the box is the reference frame of the path: '~'
// is measured from the box size, the path is not scaled, and its coordinates are relative to the box start.  A
// negative size is treated as zero because the frame cannot be flipped.

struct PathBox {
   bool defined;
   double start, size;
};

static PathBox resolve_path_box(extVectorPath *Vector, bool Horizontal)
{
   const Unit &pos    = Horizontal ? Vector->pX : Vector->pY;
   const Unit &offset = Horizontal ? Vector->pXOffset : Vector->pYOffset;
   const Unit &size   = Horizontal ? Vector->pWidth : Vector->pHeight;

   auto fixed = [Vector, Horizontal](const Unit &Value) {
      return Horizontal ? path_fixed_width(Vector, Value) : path_fixed_height(Vector, Value);
   };

   auto parent_size = [Vector, Horizontal]() {
      return Horizontal ? get_parent_width(Vector) : get_parent_height(Vector);
   };

   if (size.defined()) {
      const double box_size = std::max(0.0, fixed(size));
      double start = 0;
      if (pos.defined()) start = fixed(pos);
      else if (offset.defined()) start = parent_size() - box_size - fixed(offset);
      return { true, start, box_size };
   }
   else if (pos.defined() and offset.defined()) {
      const double start = fixed(pos);
      return { true, start, std::max(0.0, parent_size() - fixed(offset) - start) };
   }
   else return { false, 0, 0 };
}

// The size that '~' coordinates are measured from on one axis: the box if the axis has one, otherwise the parent.

static double path_reference_size(extVectorPath *Vector, bool Horizontal)
{
   const auto box = resolve_path_box(Vector, Horizontal);
   if (box.defined) return box.size;
   return Horizontal ? get_parent_width(Vector) : get_parent_height(Vector);
}

//********************************************************************************************************************
// Refreshes the cached AGG conversion of the command list if it has been invalidated.  Placement-only changes
// (e.g. the X/Y/XOffset/YOffset/Width/Height fields, Move and Resize actions) leave the cache intact because
// generate_path() applies sizing and placement as a post-transform.  The exception is an axis with '~' coordinates,
// which are resolved against the size of the box when there is one.

static void refresh_unplaced_path(extVectorPath *Vector)
{
   if (Vector->CommandsChanged) {
      Vector->CoordinateDependencies = PCO::NIL;
      for (const auto &command : Vector->Commands) Vector->CoordinateDependencies |= command.Options;
   }

   const auto modes = Vector->CoordinateDependencies;
   const bool uses_width = (modes & (PCO::X_OFFSET | PCO::X2_OFFSET | PCO::X3_OFFSET)) != PCO::NIL;
   const bool uses_height = (modes & (PCO::Y_OFFSET | PCO::Y2_OFFSET | PCO::Y3_OFFSET)) != PCO::NIL;

   // Natural-size getters can reach here before a draw has refreshed the parent viewport.

   if ((uses_width or uses_height) and Vector->ParentView) gen_vector_tree(Vector->ParentView);

   const double width = uses_width ? path_reference_size(Vector, true) : 0;
   const double height = uses_height ? path_reference_size(Vector, false) : 0;
   if ((not Vector->CommandsChanged) and (width IS Vector->ReferenceWidth) and
       (height IS Vector->ReferenceHeight)) return;

   Vector->ReferenceWidth = width;
   Vector->ReferenceHeight = height;
   Vector->UnplacedPath.free_all();

   convert_to_aggpath(Vector, Vector->Commands, Vector->UnplacedPath);

   Vector->UnplacedBounds = get_authored_path_bounds(Vector->UnplacedPath);
   Vector->CommandsChanged = false;
}

//********************************************************************************************************************
// Axes with a natural extent below this threshold are never scaled.  This covers straight vertical/horizontal lines
// and the 1.0e-10 nudge that check_point() applies to zero-length segments.

static constexpr double MIN_SCALABLE_EXTENT = 1.0e-6;

// Resolved placement of one axis: the path is scaled by Scale about its natural near edge, which is then placed at
// Position.  Size is the resulting (signed) extent of the path on that axis.  If Boxed is set, Position and Size
// describe the box and the path's coordinates are translated by Position without scaling.

struct PathAxis {
   double scale, position, size;
   bool boxed;
};

// Resolves one axis following the VectorRectangle conventions for X/Width/XOffset.  The natural extent of the path
// stands in for an undefined Width, so an XOffset on its own anchors the far edge without resizing the path.  If both
// X and XOffset are defined and Width is not, the path is stretched to fill the space between them.

static PathAxis resolve_path_axis(extVectorPath *Vector, double Near, double Far, const Unit &Pos,
   const Unit &Offset, const Unit &Size, bool Horizontal)
{
   auto fixed = [Vector, Horizontal](const Unit &Value) {
      return Horizontal ? path_fixed_width(Vector, Value) : path_fixed_height(Vector, Value);
   };

   auto parent_size = [Vector, Horizontal]() {
      return Horizontal ? get_parent_width(Vector) : get_parent_height(Vector);
   };

   if (path_uses_tilde(Vector, Horizontal)) {
      const auto box = resolve_path_box(Vector, Horizontal);
      if (box.defined) return { 1.0, box.start, box.size, true };
   }

   const double extent = Far - Near;
   double target = extent;
   if (Size.defined()) target = fixed(Size);
   else if (Pos.defined() and Offset.defined()) target = parent_size() - fixed(Offset) - fixed(Pos);

   const double scale = (extent >= MIN_SCALABLE_EXTENT) ? target / extent : 1.0;
   const double size = extent * scale;

   double position;
   if (Pos.defined()) position = fixed(Pos);
   else if (Offset.defined()) position = parent_size() - std::max(0.0, size) - fixed(Offset);
   else position = Near;

   return { scale, position, size, false };
}

static PathAxis resolve_path_x(extVectorPath *Vector, const TClipRectangle<double> &Bounds)
{
   return resolve_path_axis(Vector, Bounds.left, Bounds.right, Vector->pX, Vector->pXOffset, Vector->pWidth, true);
}

static PathAxis resolve_path_y(extVectorPath *Vector, const TClipRectangle<double> &Bounds)
{
   return resolve_path_axis(Vector, Bounds.top, Bounds.bottom, Vector->pY, Vector->pYOffset, Vector->pHeight, false);
}

// Field getters can be called before a draw has refreshed the parent viewport, so its dimensions are brought up to date
// before resolving any placement that depends on them.  Not for use within generate_path().

static void sync_parent_view(extVectorPath *Vector)
{
   if (Vector->ParentView) gen_vector_tree(Vector->ParentView);
}

//********************************************************************************************************************

static void generate_path(extVectorPath *Vector, agg::path_storage &Path)
{
   refresh_unplaced_path(Vector);
   Path.copy_path(Vector->UnplacedPath);

   auto &bounds = Vector->UnplacedBounds;
   Vector->Bounds = bounds;

   // Width and Height scale the path about its left/top bound, then X and Y place that bound.  Both are combined
   // into a single transform: x' = (x - left) * sx + dest_left.  A boxed axis is translated to the box start.

   const auto axis_x = resolve_path_x(Vector, bounds);
   const auto axis_y = resolve_path_y(Vector, bounds);
   const double sx = axis_x.scale, sy = axis_y.scale;
   const double tx = axis_x.boxed ? axis_x.position : axis_x.position - (bounds.left * sx);
   const double ty = axis_y.boxed ? axis_y.position : axis_y.position - (bounds.top * sy);

   if ((sx IS 1.0) and (sy IS 1.0) and (tx IS 0.0) and (ty IS 0.0)) return;

   Path.transform(agg::trans_affine(sx, 0, 0, sy, tx, ty));

   // A negative scale flips the path, so the transformed bounds are normalised.

   Vector->Bounds.left   = bounds.left * sx + tx;
   Vector->Bounds.right  = bounds.right * sx + tx;
   Vector->Bounds.top    = bounds.top * sy + ty;
   Vector->Bounds.bottom = bounds.bottom * sy + ty;
   if (Vector->Bounds.left > Vector->Bounds.right) std::swap(Vector->Bounds.left, Vector->Bounds.right);
   if (Vector->Bounds.top > Vector->Bounds.bottom) std::swap(Vector->Bounds.top, Vector->Bounds.bottom);
}

//********************************************************************************************************************

static TClipRectangle<double> get_unplaced_path_bounds(extVectorPath *Vector)
{
   refresh_unplaced_path(Vector);
   return Vector->UnplacedBounds;
}

//********************************************************************************************************************

ERR convert_to_aggpath(extVectorPath *Vector, std::vector<PathCommand> &Paths, agg::path_storage &BasePath)
{
   const double width = Vector ? Vector->ReferenceWidth : 0;
   const double height = Vector ? Vector->ReferenceHeight : 0;
   size_t estimate = 0; // Reserve the vertex storage up-front to avoid repeated reallocation.

   for (auto &path : Paths) {
      switch (path.Type) {
         case PE::Curve: case PE::CurveRel: case PE::Smooth: case PE::SmoothRel:
            estimate += 3; break;
         case PE::QuadCurve: case PE::QuadCurveRel: case PE::QuadSmooth: case PE::QuadSmoothRel:
            estimate += 2; break;
         case PE::Arc: case PE::ArcRel:
            estimate += 13; break; // Arcs are expanded to a maximum of four bezier curves
         default:
            estimate += 1; break;
      }
   }

   BasePath.reserve(BasePath.total_vertices() + estimate);

   bool lp_curved = false;
   bool poly_started = false;
   agg::point_d lp = { 0, 0 }; // Previous point in the path
   agg::point_d start = { 0, 0 }; // Starting point of the current polygon

   // check_point() Checks for equality between lines and adjusts according to SVG rules.  A zero length subpath with
   // 'stroke-linecap' set to 'square' or 'round' is stroked, but not stroked when 'stroke-linecap' is set to 'butt'.

   auto check_point = [&lp, &Vector](PathCommand &Cmd) {
      if ((Cmd.AbsX IS lp.x) and (Cmd.AbsY IS lp.y) and Vector and (Vector->LineCap != VLC::BUTT)) {
         Cmd.AbsX += 1.0e-10;
      }
   };

   for (size_t i=0; i < Paths.size(); i++) {
      auto path = Paths[i]; // Resolve a temporary; authored magnitudes and modes must survive rendering.

      const auto mode = path.Options;
      if ((mode & PCO::X_OFFSET) != PCO::NIL) path.X = width - path.X;
      if ((mode & PCO::Y_OFFSET) != PCO::NIL) path.Y = height - path.Y;
      if ((mode & PCO::X2_OFFSET) != PCO::NIL) path.X2 = width - path.X2;
      if ((mode & PCO::Y2_OFFSET) != PCO::NIL) path.Y2 = height - path.Y2;
      if ((mode & PCO::X3_OFFSET) != PCO::NIL) path.X3 = width - path.X3;
      if ((mode & PCO::Y3_OFFSET) != PCO::NIL) path.Y3 = height - path.Y3;

      switch (path.Type) {
         case PE::Move:
            path.AbsX = path.X;
            path.AbsY = path.Y;
            BasePath.move_to(path.AbsX, path.AbsY);
            lp_curved = false;
            break;

         case PE::MoveRel:
            path.AbsX = path.X + lp.x;
            path.AbsY = path.Y + lp.y;
            BasePath.move_to(path.AbsX, path.AbsY);
            lp_curved = false;
            break;

         case PE::Line:
            if (not poly_started) { poly_started = true; start = lp; };
            path.AbsX = path.X;
            path.AbsY = path.Y;
            check_point(path);
            BasePath.line_to(path.AbsX, path.AbsY);
            lp_curved = false;
            break;

         case PE::LineRel:
            if (not poly_started) { poly_started = true; start = lp; };
            path.AbsX = path.X + lp.x;
            path.AbsY = path.Y + lp.y;
            check_point(path);
            BasePath.line_to(path.AbsX, path.AbsY);
            lp_curved = false;
            break;

         case PE::HLine:
            if (not poly_started) { poly_started = true; start = lp; };
            path.AbsX = path.X;
            path.AbsY = lp.y;
            check_point(path);
            BasePath.line_to(path.AbsX, path.AbsY);
            lp_curved = false;
            break;

         case PE::HLineRel:
            if (not poly_started) { poly_started = true; start = lp; };
            path.AbsX = path.X + lp.x;
            path.AbsY = lp.y;
            check_point(path);
            BasePath.line_to(path.AbsX, path.AbsY);
            lp_curved = false;
            break;

         case PE::VLine:
            if (not poly_started) { poly_started = true; start = lp; };
            path.AbsX = lp.x;
            path.AbsY = path.Y;
            check_point(path);
            BasePath.line_to(path.AbsX, path.AbsY);
            lp_curved = false;
            break;

         case PE::VLineRel:
            if (not poly_started) { poly_started = true; start = lp; };
            path.AbsX = lp.x;
            path.AbsY = path.Y + lp.y;
            check_point(path);
            BasePath.line_to(path.AbsX, path.AbsY);
            lp_curved = false;
            break;

         case PE::Curve: // curve4()
            if (not poly_started) { poly_started = true; start = lp; };
            path.AbsX = path.X;
            path.AbsY = path.Y;
            check_point(path);
            BasePath.curve4(path.X2, path.Y2, path.X3, path.Y3, path.AbsX, path.AbsY);
            lp_curved = true;
            break;

         case PE::CurveRel:
            if (not poly_started) { poly_started = true; start = lp; };
            path.AbsX = lp.x + path.X;
            path.AbsY = lp.y + path.Y;
            check_point(path);
            BasePath.curve4(path.X2+lp.x, path.Y2+lp.y, path.X3+lp.x, path.Y3+lp.y, path.AbsX, path.AbsY);
            lp_curved = true;
            break;

         case PE::Smooth:
            // Simplified curve3/4 with one control inherited from the previous vertex
            if (not poly_started) { poly_started = true; start = lp; };
            path.AbsX = path.X;
            path.AbsY = path.Y;
            check_point(path);
            if (not lp_curved) BasePath.curve3(path.X2, path.Y2, path.AbsX, path.AbsY);
            else BasePath.curve4(path.X2, path.Y2, path.AbsX, path.AbsY);
            lp_curved = true;
            break;

         case PE::SmoothRel:
            if (not poly_started) { poly_started = true; start = lp; };
            path.AbsX = lp.x + path.X;
            path.AbsY = lp.y + path.Y;
            check_point(path);
            if (not lp_curved) BasePath.curve3(path.X2+lp.x, path.Y2+lp.y, path.AbsX, path.AbsY);
            else BasePath.curve4(path.X2+lp.x, path.Y2+lp.y, path.AbsX, path.AbsY);
            lp_curved = true;
            break;

         case PE::QuadCurve:
            if (not poly_started) { poly_started = true; start = lp; };
            path.AbsX = path.X;
            path.AbsY = path.Y;
            check_point(path);
            BasePath.curve3(path.X2, path.Y2, path.AbsX, path.AbsY);
            lp_curved = true;
            break;

         case PE::QuadCurveRel:
            if (not poly_started) { poly_started = true; start = lp; };
            path.AbsX = lp.x + path.X;
            path.AbsY = lp.y + path.Y;
            check_point(path);
            BasePath.curve3(path.X2+lp.x, path.Y2+lp.y, path.AbsX, path.AbsY);
            lp_curved = true;
            break;

         case PE::QuadSmooth: // Inherits a control from previous vertex 'T'
            if (not poly_started) { poly_started = true; start = lp; };
            path.AbsX = path.X;
            path.AbsY = path.Y;
            check_point(path);
            BasePath.curve3(path.AbsX, path.AbsY);
            lp_curved = true;
            break;

         case PE::QuadSmoothRel: // Inherits a control from previous vertex 't'
            if (not poly_started) { poly_started = true; start = lp; };
            path.AbsX = lp.x + path.X;
            path.AbsY = lp.y + path.Y;
            check_point(path);
            BasePath.curve3(path.X+lp.x, path.Y+lp.y);
            lp_curved = true;
            break;

         case PE::Arc:
            if (not poly_started) { poly_started = true; start = lp; };
            path.AbsX = path.X;
            path.AbsY = path.Y;
            check_point(path);
            BasePath.arc_to(path.X2, path.Y2, path.Angle * DEG2RAD, (path.Options & PCO::LARGE_ARC) != PCO::NIL, (path.Options & PCO::SWEEP) != PCO::NIL, path.AbsX, path.AbsY);
            lp_curved = true;
            break;

         case PE::ArcRel:
            if (not poly_started) { poly_started = true; start = lp; };
            path.AbsX = lp.x + path.X;
            path.AbsY = lp.y + path.Y;
            check_point(path);
            BasePath.arc_to(path.X2, path.Y2, path.Angle * DEG2RAD, (path.Options & PCO::LARGE_ARC) != PCO::NIL, (path.Options & PCO::SWEEP) != PCO::NIL, path.AbsX, path.AbsY);
            lp_curved = true;
            break;

         case PE::ClosePath: {
            path.AbsX = start.x;
            path.AbsY = start.y;
            BasePath.close_polygon();
            poly_started = false;
            break;
         }

         default:
            break;
      }

      Paths[i].AbsX = path.AbsX;
      Paths[i].AbsY = path.AbsY;
      lp = { path.AbsX, path.AbsY };
   }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORPATH_Clear(extVectorPath *Self)
{
   Self->Commands.clear();
   Self->CommandsChanged = true;
   reset_path(Self);
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Flush: Validates writable command edits and invalidates the generated path.

After editing the native writable #Commands span, call Flush to refresh parent-size dependencies.
Tiri reads Commands as a copied typed array; assign edited entries to that array and assign the array back to
Commands before flushing.

-END-
*********************************************************************************************************************/

static ERR VECTORPATH_Flush(extVectorPath *Self)
{
   Self->CommandsChanged = true; // Commands may have been edited in-place via the Commands field pointer
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Move: Moves the path to a new position.
-END-
*********************************************************************************************************************/

static ERR VECTORPATH_Move(extVectorPath *Self, struct acMove *Args)
{
   if (not Args) return ERR::NullArgs;

   if (Self->pX.scaled() or Self->pY.scaled() or Self->pXOffset.scaled() or Self->pYOffset.scaled()) {
      sync_parent_view(Self);
   }
   auto bounds = get_unplaced_path_bounds(Self);

   // Each defined placement edge is shifted, so that right/bottom anchored and stretched paths keep their size.
   // Placement is fixed by the resolved position (the X/Y bounds, or the box start) if neither edge is defined.

   if ((not Self->pX.defined()) and (not Self->pXOffset.defined())) {
      Self->pX = resolve_path_x(Self, bounds).position;
   }
   if (Self->pX.defined()) Self->pX = Unit(path_fixed_width(Self, Self->pX) + Args->DeltaX);
   if (Self->pXOffset.defined()) Self->pXOffset = Unit(path_fixed_width(Self, Self->pXOffset) - Args->DeltaX);

   if ((not Self->pY.defined()) and (not Self->pYOffset.defined())) {
      Self->pY = resolve_path_y(Self, bounds).position;
   }
   if (Self->pY.defined()) Self->pY = Unit(path_fixed_height(Self, Self->pY) + Args->DeltaY);
   if (Self->pYOffset.defined()) Self->pYOffset = Unit(path_fixed_height(Self, Self->pYOffset) - Args->DeltaY);

   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
MoveToPoint: Moves the path to a new fixed position.

This action updates the #X and #Y placement fields.  The path commands remain unchanged.
-END-
*********************************************************************************************************************/

static ERR VECTORPATH_MoveToPoint(extVectorPath *Self, struct acMoveToPoint *Args)
{
   if (not Args) return ERR::NullArgs;

   if ((Args->Flags & MTF::RELATIVE) != MTF::NIL) {
      if ((Args->Flags & MTF::X) != MTF::NIL) Self->pX = Unit(Args->X, FD_SCALED);
      if ((Args->Flags & MTF::Y) != MTF::NIL) Self->pY = Unit(Args->Y, FD_SCALED);
   }
   else {
      if ((Args->Flags & MTF::X) != MTF::NIL) Self->pX = Unit(Args->X);
      if ((Args->Flags & MTF::Y) != MTF::NIL) Self->pY = Unit(Args->Y);
   }
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Resize: Scales the path to fixed dimensions.

This action sets the #Width and #Height fields to fixed values.  The path commands remain unchanged.
-END-
*********************************************************************************************************************/

static ERR VECTORPATH_Resize(extVectorPath *Self, struct acResize *Args)
{
   if (not Args) return ERR::NullArgs;

   Self->pWidth = Unit(Args->Width);
   Self->pHeight = Unit(Args->Height);
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
AddCommand: Add one or more commands to the end of the path sequence.

This method will add a series of commands to the end of a Vector's existing path sequence.  The commands must be
provided as a sequential array.  Option masks are checked against the supported absolute operands.

Calling this method will also result in the path being recomputed for the next redraw.

-INPUT-
array(struct(PathCommand)) Commands: Array of commands to add to the path.

-ERRORS-
Okay
NullArgs

-TAGS-
mutates-object, copies-input

*********************************************************************************************************************/

static ERR VECTORPATH_AddCommand(extVectorPath *Self, struct vp::AddCommand *Args)
{
   kt::Log log;

   if ((not Args) or Args->Commands.empty()) return log.warning(ERR::NullArgs);

   const size_t total_cmds = Args->Commands.size();

   if (total_cmds > 1000000) return log.warning(ERR::Args);

   auto list = Args->Commands.data();
   for (size_t i=0; i < total_cmds; i++) {
      Self->Commands.push_back(list[i]);
   }

   Self->CommandsChanged = true;
   reset_path(Self);
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetCommand: Retrieve a specific command from the path sequence.

Calling GetCommand() will return a direct pointer to the command identified at `Index`.  The pointer will remain valid
for as long as the @VectorPath is not modified.  Returned coordinates retain authored offset magnitudes and their
Option mask.  In Tiri the returned structure is read-only; clone it and submit edits through #SetCommand().

-INPUT-
int Index: The index of the command to retrieve.
&struct(*PathCommand) Command: The requested command will be returned in this parameter.

-RESULT-
Okay
NullArgs
OutOfRange

-TAGS-
pure-query, object-owns-result

*********************************************************************************************************************/

static ERR VECTORPATH_GetCommand(extVectorPath *Self, struct vp::GetCommand *Args)
{
   if (not Args) return ERR::NullArgs;
   if ((Args->Index < 0) or ((size_t)Args->Index >= Self->Commands.size())) return kt::Log().warning(ERR::OutOfRange);

   Args->Command = &Self->Commands[Args->Index];
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
RemoveCommand: Remove at least one command from the path sequence.

This method will remove a series of commands from the current path, starting at the given `Index`.  The total number
of commands to remove is indicated by the `Total` parameter.

-INPUT-
int Index: The index of the command to remove.
int Total: The total number of commands to remove, starting from the given Index.

-RESULT-
Okay
NullArgs
OutOfRange
NothingDone

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR VECTORPATH_RemoveCommand(extVectorPath *Self, struct vp::RemoveCommand *Args)
{
   if (not Args) return ERR::NullArgs;
   if (Self->Commands.empty()) return ERR::NothingDone;
   if ((Args->Index < 0) or (size_t(Args->Index) >= Self->Commands.size()) or (Args->Total < 1) or
       (size_t(Args->Total) > Self->Commands.size() - size_t(Args->Index))) return ERR::OutOfRange;

   auto first = Self->Commands.begin() + Args->Index;
   auto last = first + Args->Total;
   Self->Commands.erase(first, last);

   Self->CommandsChanged = true;
   reset_path(Self);
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SetCommand: Copies one or more commands into an existing path.

Use SetCommand() to copy one or more commands into an existing path.

-INPUT-
int Index: The index of the command that is to be set.
array(struct(PathCommand)) Command: An array of commands to set in the path.

-ERRORS-
Okay
NullArgs
OutOfRange
BufferOverflow

-TAGS-
mutates-object, copies-input

*********************************************************************************************************************/

static ERR VECTORPATH_SetCommand(extVectorPath *Self, struct vp::SetCommand *Args)
{
   if ((not Args) or Args->Command.empty()) return ERR::NullArgs;
   if (Args->Index < 0) return ERR::OutOfRange;

   const size_t total_cmds = Args->Command.size();
   if (total_cmds > 1000000) return ERR::Args;

   const size_t index = size_t(Args->Index);
   if (index > 1000000 - total_cmds) return ERR::BufferOverflow;
   if (index > Self->Commands.max_size() - total_cmds) return ERR::BufferOverflow;
   const size_t required_size = index + total_cmds;
   if (required_size > Self->Commands.size()) Self->Commands.resize(required_size);

   std::copy(Args->Command.begin(), Args->Command.end(), Self->Commands.begin() + index);

   Self->CommandsChanged = true;
   reset_path(Self);
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SetCommandList: The fastest available mechanism for setting a series of path instructions.

Use SetCommandList() to copy a series of path commands to a @VectorPath object.  All existing commands will be
cleared as a result of this process.

-INPUT-
array(struct(PathCommand)) Commands: An array of !PathCommand structures.

-ERRORS-
Okay
NullArgs
NotInitialised
Args

-TAGS-
mutates-object, copies-input

*********************************************************************************************************************/

static ERR VECTORPATH_SetCommandList(extVectorPath *Self, struct vp::SetCommandList *Args)
{
   kt::Log log;

   if ((not Args) or Args->Commands.empty()) return log.warning(ERR::NullArgs);
   if (not Self->initialised()) return log.warning(ERR::NotInitialised);

   const size_t total_cmds = Args->Commands.size();
   if (total_cmds > 1000000) return log.warning(ERR::Args);

   Self->Commands.assign(Args->Commands.begin(), Args->Commands.end());

   Self->CommandsChanged = true;
   reset_path(Self);
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Commands: Direct pointer to the PathCommand array.

In C++, read the Commands field to obtain a direct pointer to the !PathCommand array.  This allows the path control
points to be modified directly, but it is not possible to resize the path.  After making changes to the path, call
#Flush() to register the changes for the next redraw.

This field can also be written at any time with a new array of !PathCommand structures.  Doing so will clear the
existing path, if any.

`Options` preserves authored offset magnitudes using the !PCO bits.  Tiri returns a copied typed array; edit
its entries and assign the array back to Commands to replace the authored commands.

*********************************************************************************************************************/

static ERR VECTORPATH_GET_Commands(extVectorPath *Self, std::span<PathCommand> &Value)
{
   Value = std::span<PathCommand>(Self->Commands.data(), Self->Commands.size());
   return ERR::Okay;
}

static ERR VECTORPATH_SET_Commands(extVectorPath *Self, std::span<const PathCommand> &Value)
{
   if (not Value.data()) return ERR::NullArgs;
   if (Value.size() > 1000000) return ERR::Args;

   Self->Commands.assign(Value.begin(), Value.end());

   Self->CommandsChanged = true;
   if (Self->initialised()) {
      reset_path(Self);
      Self->modified();
   }
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

PathLength is always relative to the authored path.  It is not adjusted when the path is sized by the #Width and
#Height fields.

*********************************************************************************************************************/

static ERR VECTORPATH_GET_PathLength(extVectorPath *Self, int *Value)
{
   *Value = Self->PathLength;
   return ERR::Okay;
}

static ERR VECTORPATH_SET_PathLength(extVectorPath *Self, int Value)
{
   if (Value >= 0) {
      Self->PathLength = Value;
      Self->modified();
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
X: The left-side of the path.  Can be expressed as a fixed or scaled coordinate.

Setting X moves the computed path so that its left-most boundary aligns with the supplied coordinate.  The path
commands remain unchanged.

If the path is sized by #Width, X places the left edge of the sized path.  A negative #Width extends the path to the
left of X.

If the path has a horizontal box and `~` coordinates on that axis, X is the left edge of the box and the path's
coordinates are relative to it, as described in #Sequence.
-END-
*********************************************************************************************************************/

static ERR VECTORPATH_GET_X(extVectorPath *Self, Unit &Value)
{
   // The placement is resolved on demand because Self->Bounds is only refreshed during path generation.
   if (Self->pX.defined()) Value = Self->pX;
   else {
      sync_parent_view(Self);
      Value = Unit(resolve_path_x(Self, get_unplaced_path_bounds(Self)).position);
   }
   return ERR::Okay;
}

static ERR VECTORPATH_SET_X(extVectorPath *Self, Unit &Value)
{
   Self->pX = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Y: The top of the path.  Can be expressed as a fixed or scaled coordinate.

Setting Y moves the computed path so that its top-most boundary aligns with the supplied coordinate.  The path
commands remain unchanged.

If the path is sized by #Height, Y places the top edge of the sized path.  A negative #Height extends the path above
Y.
-END-
*********************************************************************************************************************/

static ERR VECTORPATH_GET_Y(extVectorPath *Self, Unit &Value)
{
   if (Self->pY.defined()) Value = Self->pY;
   else {
      sync_parent_view(Self);
      Value = Unit(resolve_path_y(Self, get_unplaced_path_bounds(Self)).position);
   }
   return ERR::Okay;
}

static ERR VECTORPATH_SET_Y(extVectorPath *Self, Unit &Value)
{
   Self->pY = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
XOffset: The right-side of the path, expressed as a fixed or scaled offset value.

XOffset places the path relative to the right edge of the parent viewport.  The value is measured inward from that
edge, so an XOffset of zero aligns the right-most boundary of the path with the right edge of the parent.  Scaled
values are relative to the width of the parent viewport.  The path commands remain unchanged.

XOffset follows the @VectorRectangle conventions and is combined with #X and #Width as follows:

<list type="bullet">
<li>If only XOffset is defined, the path keeps its width and its right edge is anchored to the offset.</li>
<li>If #Width and XOffset are defined, the path is sized by #Width and its right edge is anchored to the offset.</li>
<li>If #X and XOffset are defined, the path is stretched to fill the space between them.</li>
<li>If #X and #Width are both defined, XOffset is ignored.</li>
</list>

When XOffset is set, reading it returns the stored value.  Otherwise it returns the distance between the right edge of
the placed path, or of its box (see #Sequence), and the right edge of the parent viewport.  To remove the offset, set 
an undefined value (NaN).
-END-
*********************************************************************************************************************/

static ERR VECTORPATH_GET_XOffset(extVectorPath *Self, Unit &Value)
{
   if (Self->pXOffset.defined()) {
      Value = Self->pXOffset;
      return ERR::Okay;
   }

   sync_parent_view(Self);
   const auto axis = resolve_path_x(Self, get_unplaced_path_bounds(Self));
   const double parent_size = get_parent_width(Self);
   const double value = parent_size - (axis.position + std::max(0.0, axis.size));

   if ((Value.scaled()) and (parent_size != 0)) Value = Unit(value / parent_size, FD_SCALED);
   else Value = Unit(value);
   return ERR::Okay;
}

static ERR VECTORPATH_SET_XOffset(extVectorPath *Self, Unit &Value)
{
   Self->pXOffset = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
YOffset: The bottom of the path, expressed as a fixed or scaled offset value.

YOffset places the path relative to the bottom edge of the parent viewport.  The value is measured inward from that
edge, so a YOffset of zero aligns the bottom-most boundary of the path with the bottom edge of the parent.  Scaled
values are relative to the height of the parent viewport.

YOffset behaves in the same way as #XOffset on the vertical axis, in combination with #Y and #Height.  Refer to
#XOffset for further details.
-END-
*********************************************************************************************************************/

static ERR VECTORPATH_GET_YOffset(extVectorPath *Self, Unit &Value)
{
   if (Self->pYOffset.defined()) {
      Value = Self->pYOffset;
      return ERR::Okay;
   }

   sync_parent_view(Self);
   const auto axis = resolve_path_y(Self, get_unplaced_path_bounds(Self));
   const double parent_size = get_parent_height(Self);
   const double value = parent_size - (axis.position + std::max(0.0, axis.size));

   if ((Value.scaled()) and (parent_size != 0)) Value = Unit(value / parent_size, FD_SCALED);
   else Value = Unit(value);
   return ERR::Okay;
}

static ERR VECTORPATH_SET_YOffset(extVectorPath *Self, Unit &Value)
{
   Self->pYOffset = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Width: Scales the path to a fixed or scaled width.

Setting Width scales the computed path on the horizontal axis so that its bounding box matches the supplied width.
Scaled values are relative to the width of the parent viewport.  The path is scaled about its left edge, after which
#X placement is applied.  If Width is not set, the path keeps its natural width unless it is stretched between #X and
#XOffset.

Width and #Height are independent: if only one is set, the other axis keeps its natural size and the aspect ratio
is not preserved.  A width of zero collapses the path to a vertical line.  Negative values are permitted and will
flip the path on the horizontal axis, so that it spans from `X + Width` to #X.

If the natural width of the path is close to zero (for example, a vertical line) then the path cannot be scaled
horizontally and Width has no effect.

If the #Sequence has `~` coordinates on the horizontal axis, Width does not scale the path.  It instead sets the size
of the box that the `~` coordinates are measured from, so that fixed insets and corner radii are preserved.  A
negative Width is treated as zero in this case.

Only the path geometry is scaled.  The stroke width, dash pattern and markers keep their sizes, and the fit is
applied to the geometric bounds rather than the stroked bounds.  Transforms applied to the vector take effect after
the Width is applied.

The path commands remain unchanged, so reading #Commands or calling #GetCommand() returns the authored path.  The
#Sequence field returns the scaled geometry.

When Width is not set, reading it returns the resolved width of the path, or of its box if it is stretched between
#X and #XOffset with `~` coordinates.  To return the path to its natural width,
set an undefined value (NaN).
-END-
*********************************************************************************************************************/

static ERR VECTORPATH_GET_Width(extVectorPath *Self, Unit &Value)
{
   if (Self->pWidth.defined()) Value = Self->pWidth;
   else {
      sync_parent_view(Self);
      Value = Unit(resolve_path_x(Self, get_unplaced_path_bounds(Self)).size);
   }
   return ERR::Okay;
}

static ERR VECTORPATH_SET_Width(extVectorPath *Self, Unit &Value)
{
   Self->pWidth = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Height: Scales the path to a fixed or scaled height.

Setting Height scales the computed path on the vertical axis so that its bounding box matches the supplied height.
Scaled values are relative to the height of the parent viewport.  The path is scaled about its top edge, after which
#Y placement is applied.  If Height is not set, the path keeps its natural height.

Height behaves in the same way as #Width on the vertical axis.  Negative values flip the path, so that it spans from
`Y + Height` to #Y.  Refer to #Width for further details.
-END-
*********************************************************************************************************************/

static ERR VECTORPATH_GET_Height(extVectorPath *Self, Unit &Value)
{
   if (Self->pHeight.defined()) Value = Self->pHeight;
   else {
      sync_parent_view(Self);
      Value = Unit(resolve_path_y(Self, get_unplaced_path_bounds(Self)).size);
   }
   return ERR::Okay;
}

static ERR VECTORPATH_SET_Height(extVectorPath *Self, Unit &Value)
{
   Self->pHeight = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Sequence: A sequence of points and instructions that will define the path.

The Sequence is a string of points and instructions that define the path.  It is based on the SVG standard for the path
element `d` attribute, with additional Kōtuku features.  Commands are case sensitive: uppercase commands use
absolute coordinates and lowercase commands use relative coordinates.

The following commands are supported:

<pre>
M: Move To
L: Line To
V: Vertical Line To
H: Horizontal Line To
Q: Quadratic Curve To
T: Quadratic Smooth Curve To
C: Curve To
S: Smooth Curve To
A: Arc
Z: Close Path
</pre>

The use of lower case characters will indicate that the provided coordinates are relative (based on the coordinate
of the previous command).

To terminate a path without joining it to the first coordinate, omit the `Z` from the end of the sequence.

In uppercase M, L, H, V, Q, T, C and S commands, prefix any explicit coordinate with `~` to measure inward from
its parent's right edge (X) or bottom edge (Y).  Arc A endpoints also support this syntax; radii, angle and flags do
not.  For example, `M 10 0 H ~10 L ~0 ~0 Z` follows the parent size.  Signed finite decimal and exponent values are
accepted, including `~0`, `~-10` and `~1e2`; whitespace between the marker and number is invalid.  Percentages,
expressions and offsets in lowercase commands are not supported.  Values are not clamped.

`~` coordinates on an axis are measured from the path's box on that axis if it has one, otherwise from the parent.
The box follows @VectorRectangle conventions: it is defined by #Width (or #Height), placed by #X or #XOffset, or
stretched between #X and #XOffset when no size is given.  An axis with a box and `~` coordinates is not scaled; its
coordinates are relative to the start of the box, as if the path were hosted by a viewport of that size.  For
example, `M 4 0 H ~4 A 4 4 0 0 1 ~0 4 V ~4 ...` with an #X of 10 and a #Width of 200 draws a shape that spans
x = 10 to 210 with 4 unit corners at any width.  Axes without `~` coordinates, or without a box, are placed and
scaled as described in #X and #Width.

The parent's reference dimensions follow @VectorRectangle conventions: the parent viewport's view-box dimensions when
defined, otherwise its dimensions, with scene page dimensions as fallback.  View-box origins are not added.  Resolution
occurs before bounds, placement and sizing.  Resizing the parent or the box resolves the authored values again without
modifying #Commands.

Reading Sequence returns resolved, post-sizing numeric geometry.  Assigning that result creates a fixed snapshot
without parent dependencies.  SVG export likewise uses standard numeric coordinates.  Authored sequences containing
`~` are a Kōtuku extension and cannot be passed directly to standard SVG readers.

*********************************************************************************************************************/

static ERR VECTORPATH_SET_Sequence(extVectorPath *Self, const std::string_view &Value)
{
   Self->Commands.clear();

   ERR error = ERR::Okay;
   if (not Value.empty()) error = read_path(Self->Commands, Value, true);
   Self->CommandsChanged = true;
   reset_path(Self);
   Self->modified();
   return error;
}

/*********************************************************************************************************************
-FIELD-
TotalCommands: The total number of points defined in the path sequence.

The total number of points defined in the path #Sequence is reflected in this field.  Modifying the total directly is
permitted.  Expansion creates zero-initialised entries with no coordinate modes.  The maximum is one million entries.
-END-
*********************************************************************************************************************/

static ERR VECTORPATH_GET_TotalCommands(extVectorPath *Self, int *Value)
{
   *Value = Self->Commands.size();
   return ERR::Okay;
}

static ERR VECTORPATH_SET_TotalCommands(extVectorPath *Self, int Value)
{
   if (Value < 0) return ERR::OutOfRange;
   if (Value > 1000000) return ERR::OutOfRange;
   Self->Commands.resize(Value);
   Self->CommandsChanged = true;
   reset_path(Self);
   Self->modified();
   return ERR::Okay;
}

//********************************************************************************************************************

extVectorPath::extVectorPath(objMetaClass *ClassPtr, OBJECTID ObjectID) : extVector(ClassPtr, ObjectID) {
   GeneratePath = (void (*)(extVector *, agg::path_storage &))&generate_path;
}

//********************************************************************************************************************

static const FieldArray clPathFields[] = {
   { "Sequence",      FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_RW, VECTOR_GET_Sequence, VECTORPATH_SET_Sequence },
   { "X",             FDF_VIRTUAL|FD_UNIT|FDF_SCALED|FDF_RW|FDF_PURE, VECTORPATH_GET_X, VECTORPATH_SET_X },
   { "Y",             FDF_VIRTUAL|FD_UNIT|FDF_SCALED|FDF_RW|FDF_PURE, VECTORPATH_GET_Y, VECTORPATH_SET_Y },
   { "XOffset",       FDF_VIRTUAL|FD_UNIT|FDF_SCALED|FDF_RW, VECTORPATH_GET_XOffset, VECTORPATH_SET_XOffset },
   { "YOffset",       FDF_VIRTUAL|FD_UNIT|FDF_SCALED|FDF_RW, VECTORPATH_GET_YOffset, VECTORPATH_SET_YOffset },
   { "Width",         FDF_VIRTUAL|FD_UNIT|FDF_SCALED|FDF_RW|FDF_PURE, VECTORPATH_GET_Width, VECTORPATH_SET_Width },
   { "Height",        FDF_VIRTUAL|FD_UNIT|FDF_SCALED|FDF_RW|FDF_PURE, VECTORPATH_GET_Height, VECTORPATH_SET_Height },
   { "TotalCommands", FDF_VIRTUAL|FDF_INT|FDF_RW|FDF_PURE, VECTORPATH_GET_TotalCommands, VECTORPATH_SET_TotalCommands },
   { "PathLength",    FDF_VIRTUAL|FDF_INT|FDF_RW|FDF_PURE, VECTORPATH_GET_PathLength, VECTORPATH_SET_PathLength },
   { "Commands",      FDF_VIRTUAL|FDF_ARRAY|FDF_STRUCT|FDF_RW|FDF_PURE, VECTORPATH_GET_Commands, VECTORPATH_SET_Commands, "PathCommand" },
   END_FIELD
};

#include "path_def.c"

//********************************************************************************************************************

static ERR init_path(void)
{
   clVectorPath = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTOR),
      fl::ClassID(CLASSID::VECTORPATH),
      fl::Name("VectorPath"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clVectorPathActions),
      fl::Methods(clVectorPathMethods),
      fl::Fields(clPathFields),
      fl::Size(sizeof(extVectorPath)),
      fl::Path(MOD_PATH));

   return clVectorPath ? ERR::Okay : ERR::AddClass;
}

/*********************************************************************************************************************

-CLASS-
VectorPath: Extends the Vector class with support for generating custom paths.

VectorPath provides support for parsing SVG styled path strings.

-END-

*********************************************************************************************************************/

// Refreshes the cached AGG conversion of the command list if it has been invalidated.  Placement-only changes
// (e.g. the X/Y fields, Move actions) leave the cache intact because generate_path() applies placement as a
// post-transform.

static void refresh_unplaced_path(extVectorPath *Vector)
{
   if (not Vector->CommandsChanged) return;
   Vector->UnplacedPath.free_all();
   convert_to_aggpath(Vector, Vector->Commands, Vector->UnplacedPath);
   Vector->UnplacedBounds = get_bounds(Vector->UnplacedPath);
   Vector->CommandsChanged = false;
}

static void generate_path(extVectorPath *Vector, agg::path_storage &Path)
{
   refresh_unplaced_path(Vector);
   Path.copy_path(Vector->UnplacedPath);
   Vector->Bounds = Vector->UnplacedBounds;

   double tx = 0, ty = 0;
   if (Vector->pX.defined()) {
      if (Vector->pX.scaled()) tx = (Vector->pX * get_parent_width(Vector)) - Vector->Bounds.left;
      else tx = Vector->pX - Vector->Bounds.left;
   }

   if (Vector->pY.scaled()) ty = (Vector->pY * get_parent_height(Vector)) - Vector->Bounds.top;
   else if (Vector->pY.defined()) ty = Vector->pY - Vector->Bounds.top;

   if ((tx != 0) or (ty != 0)) {
      Path.transform(agg::trans_affine_translation(tx, ty));
      Vector->Bounds.left   += tx;
      Vector->Bounds.right  += tx;
      Vector->Bounds.top    += ty;
      Vector->Bounds.bottom += ty;
   }
}

//********************************************************************************************************************

static TClipRectangle<double> get_unplaced_path_bounds(extVectorPath *Vector)
{
   refresh_unplaced_path(Vector);
   return Vector->UnplacedBounds;
}

//********************************************************************************************************************

void convert_to_aggpath(extVectorPath *Vector, std::vector<PathCommand> &Paths, agg::path_storage &BasePath)
{
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
      auto &path = Paths[i];
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
            BasePath.arc_to(path.X2, path.Y2, path.Angle * DEG2RAD, path.LargeArc, path.Sweep, path.AbsX, path.AbsY);
            lp_curved = true;
            break;

         case PE::ArcRel:
            if (not poly_started) { poly_started = true; start = lp; };
            path.AbsX = lp.x + path.X;
            path.AbsY = lp.y + path.Y;
            check_point(path);
            BasePath.arc_to(path.X2, path.Y2, path.Angle * DEG2RAD, path.LargeArc, path.Sweep, path.AbsX, path.AbsY);
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

      lp = { path.AbsX, path.AbsY };
   }
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

//********************************************************************************************************************

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

   auto bounds = get_unplaced_path_bounds(Self);

   if (Self->pX.scaled()) Self->pX.set(Self->pX * get_parent_width(Self));
   else if (not Self->pX.defined()) Self->pX = bounds.left;

   if (Self->pY.scaled()) Self->pY.set(Self->pY * get_parent_height(Self));
   else if (not Self->pY.defined()) Self->pY = bounds.top;

   Self->pX = Unit(Self->pX + Args->DeltaX);
   Self->pY = Unit(Self->pY + Args->DeltaY);
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

-METHOD-
AddCommand: Add one or more commands to the end of the path sequence.

This method will add a series of commands to the end of a Vector's existing path sequence.  The commands must be
provided as a sequential array.  No checks will be performed to confirm the validity of the sequence.

Calling this method will also result in the path being recomputed for the next redraw.

-INPUT-
buf(struct(*PathCommand)) Commands: Array of commands to add to the path.
bufsize Size: The size of the `Commands` buffer, in bytes.

-RESULT-
Okay
NullArgs

-TAGS-
mutates-object, copies-input

*********************************************************************************************************************/

static ERR VECTORPATH_AddCommand(extVectorPath *Self, struct vp::AddCommand *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Commands)) return log.warning(ERR::NullArgs);

   const int total_cmds = Args->Size / sizeof(PathCommand);

   if ((total_cmds <= 0) or (total_cmds > 1000000)) return log.warning(ERR::Args);

   auto list = Args->Commands;
   for (int i=0; i < total_cmds; i++) {
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
for as long as the @VectorPath is not modified.

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
   if ((Args->Index < 0) or ((size_t)Args->Index > Self->Commands.size()-1)) return kt::Log().warning(ERR::OutOfRange);
   if (Self->Commands.empty()) return ERR::NothingDone;

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
buf(struct(*PathCommand)) Command: An array of commands to set in the path.
bufsize Size: The size of the `Command` buffer, in bytes.

-RESULT-
Okay
NullArgs
OutOfRange
BufferOverflow

-TAGS-
mutates-object, copies-input

*********************************************************************************************************************/

static ERR VECTORPATH_SetCommand(extVectorPath *Self, struct vp::SetCommand *Args)
{
   if ((not Args) or (not Args->Command)) return ERR::NullArgs;
   if (Args->Index < 0) return ERR::OutOfRange;

   const int total_cmds = Args->Size / sizeof(PathCommand);
   if ((size_t)Args->Index + total_cmds > Self->Commands.size()) Self->Commands.resize(Args->Index + total_cmds);

   copymem(Args->Command, &Self->Commands[Args->Index], total_cmds * sizeof(PathCommand));

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

NOTE: This method is not compatible with Tiri calls.

-INPUT-
buf(ptr) Commands: An array of !PathCommand structures.
bufsize Size: The byte size of the `Commands` buffer.

-RESULT-
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

   if ((not Args) or (not Args->Size)) return log.warning(ERR::NullArgs);
   if (not Self->initialised()) return log.warning(ERR::NotInitialised);

   const int total_cmds = Args->Size / sizeof(PathCommand);
   if ((total_cmds < 0) or (total_cmds > 1000000)) return log.warning(ERR::Args);

   Self->Commands.clear();

   auto list = (PathCommand *)Args->Commands;
   for (int i=0; i < total_cmds; i++) {
      Self->Commands.push_back(list[i]);
   }

   Self->CommandsChanged = true;
   reset_path(Self);
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Commands: Direct pointer to the PathCommand array.

Read the Commands field to obtain a direct pointer to the !PathCommand array.  This will allow the control points of
the path to be modified directly, but it is not possible to resize the path.  After making changes to the path, call
#Flush() to register the changes for the next redraw.

This field can also be written at any time with a new array of !PathCommand structures.  Doing so will clear the
existing path, if any.

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
-END-
*********************************************************************************************************************/

static ERR VECTORPATH_GET_X(extVectorPath *Self, Unit &Value)
{
   // With no X placement there is no horizontal translation, so the unplaced bounds are authoritative.  They are
   // computed on demand because Self->Bounds is only refreshed during path generation.
   if (Self->pX.defined()) Value = Self->pX;
   else Value = Unit(get_unplaced_path_bounds(Self).left);
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
-END-
*********************************************************************************************************************/

static ERR VECTORPATH_GET_Y(extVectorPath *Self, Unit &Value)
{
   if (Self->pY.defined()) Value = Self->pY;
   else Value = Unit(get_unplaced_path_bounds(Self).top);
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
Sequence: A sequence of points and instructions that will define the path.

The Sequence is a string of points and instructions that define the path.  It is based on the SVG standard for the path
element `d` attribute, but also provides some additional features that are present in the vector engine.  Commands are
case insensitive.

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

*********************************************************************************************************************/

static ERR VECTORPATH_SET_Sequence(extVectorPath *Self, const std::string_view &Value)
{
   Self->Commands.clear();

   ERR error = ERR::Okay;
   if (not Value.empty()) error = read_path(Self->Commands, Value);
   Self->CommandsChanged = true;
   reset_path(Self);
   Self->modified();
   return error;
}

/*********************************************************************************************************************
-FIELD-
TotalCommands: The total number of points defined in the path sequence.

The total number of points defined in the path #Sequence is reflected in this field.  Modifying the total directly is
permitted, although this should be used for shrinking the list because expansion will create uninitialised command
entries.
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
   Self->Commands.resize(Value);
   Self->CommandsChanged = true;
   Self->modified();
   return ERR::Okay;
}

//********************************************************************************************************************

extVectorPath::extVectorPath(objMetaClass *ClassPtr, OBJECTID ObjectID) : extVector(ClassPtr, ObjectID) {
   GeneratePath = (void (*)(extVector *, agg::path_storage &))&generate_path;
}

//********************************************************************************************************************

static const FieldArray clPathFields[] = {
   { "Sequence",      FDF_VIRTUAL|FDF_CPPSTRING|FDF_RW, VECTOR_GET_Sequence, VECTORPATH_SET_Sequence },
   { "X",             FDF_VIRTUAL|FD_UNIT|FDF_SCALED|FDF_RW|FDF_PURE, VECTORPATH_GET_X, VECTORPATH_SET_X },
   { "Y",             FDF_VIRTUAL|FD_UNIT|FDF_SCALED|FDF_RW|FDF_PURE, VECTORPATH_GET_Y, VECTORPATH_SET_Y },
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

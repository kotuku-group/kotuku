/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-MODULE-
Vector: Create, manipulate and draw vector graphics to bitmaps.

The Vector module exports a small number of functions to assist the @Vector class, as well as some primitive
functions for creating paths and rendering them to bitmaps.

-END-

*********************************************************************************************************************/

//#include "vector.h"
//#include "font.h"
#include "colours.cpp"

inline char read_nibble(CSTRING Str)
{
   if (std::isdigit(*Str)) return (*Str - '0');
   else if ((*Str >= 'A') and (*Str <= 'F')) return ((*Str - 'A')+10);
   else if ((*Str >= 'a') and (*Str <= 'f')) return ((*Str - 'a')+10);
   else return char(0xff);
}

// Resource management for the SimpleVector follows.  NB: This is a beta feature in the Core.

static ERR simplevector_free(APTR Address) {
   return ERR::Okay;
}

static ResourceManager glResourceSimpleVector = {
   "SimpleVector",
   &simplevector_free
};

static SimpleVector * new_simplevector(void)
{
   SimpleVector *vector;
   if (AllocMemory(sizeof(SimpleVector), MEM::MANAGED, &vector) != ERR::Okay) return nullptr;
   SetResourceMgr(vector, &glResourceSimpleVector);
   new(vector) SimpleVector;
   return vector;
}

namespace vec {

/*********************************************************************************************************************

-FUNCTION-
ApplyPath: Copy a pre-generated or custom path to a VectorPath object.

Any path originating from ~GeneratePath(), ~GenerateEllipse() or ~GenerateRectangle() can be applied to a VectorPath
object by calling ApplyPath().  The source Path can then be deallocated with ~Core.FreeResource() if it is no longer required.

This method is particularly useful when paths need to be generated or changed in real-time and the alternative of
processing the path as a string is detrimental to performance.

-INPUT-
ptr Path: The source path to be copied.
obj(VectorPath) VectorPath: The target VectorPath object.

-ERRORS-
Okay
NullArgs

*********************************************************************************************************************/

ERR ApplyPath(APTR Vector, objVectorPath *VectorPath)
{
   if ((!Vector) or (!VectorPath)) return ERR::NullArgs;
   if (VectorPath->classID() != CLASSID::VECTORPATH) return ERR::Args;

   VectorPath->set(FID_Sequence, CSTRING(nullptr)); // Clear any pre-existing path information.

   // TODO: Apply mPath to VectorPath

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
ArcTo: Alter a path by setting an arc-to command at the current vertex position.

This function will set an arc-to command at the current vertex.  It then increments the vertex position for the next
path command.

-INPUT-
ptr Path: The vector path to modify.
double RX: The horizontal radius of the arc.
double RY: The vertical radius of the arc.
double Angle: The angle of the arc, expressed in radians.
double X: The horizontal end point for the arc command.
double Y: The vertical end point for the arc command.
int(ARC) Flags: Optional flags.

*********************************************************************************************************************/

void ArcTo(APTR Vector, double RX, double RY, double Angle, double X, double Y, ARC Flags)
{
   ((SimpleVector *)Vector)->mPath.arc_to(RX, RY, Angle, ((Flags & ARC::LARGE) != ARC::NIL) ? 1 : 0, ((Flags & ARC::SWEEP) != ARC::NIL) ? 1 : 0, X, Y);
}

/*********************************************************************************************************************

-FUNCTION-
CharWidth: Returns the width of a character.

This function will return the pixel width of a font character.  The character is specified as a unicode value in the
`Char` parameter. Kerning values can also be returned, which affect the position of the character along the horizontal.
The previous character in the word is set in `KChar` and the kerning value will be returned in the `Kerning` parameter.
If kerning information is not required, set the `KChar` and `Kerning` parameters to zero.

The font's glyph spacing value is not used in calculating the character width.

-INPUT-
ptr FontHandle: The font to use for calculating the character width.
uint Char: A 32-bit unicode character.
uint KChar: A unicode character to use for calculating the font kerning (optional).
&double Kerning: The resulting kerning value (optional).

-RESULT-
double: The pixel width of the character will be returned.

*********************************************************************************************************************/

double CharWidth(APTR Handle, uint32_t Char, uint32_t KChar, double *Kerning)
{
   if (!Handle) return 0;

   if (((common_font *)Handle)->type IS CF_FREETYPE) {
      auto pt = (freetype_font::ft_point *)Handle;
      FT_Activate_Size(pt->ft_size);

      auto &cache = pt->get_glyph(Char);
      if (Kerning) {
         if (KChar) {
            FT_Vector delta;
            FT_Get_Kerning(pt->ft_size->face, FT_Get_Char_Index(pt->font->face, KChar), cache.glyph_index, FT_KERNING_DEFAULT, &delta);
            *Kerning = int26p6_to_dbl(delta.x);
         }
         else *Kerning = 0;
      }
      return cache.adv_x;
   }
   else {
      if (Kerning) *Kerning = 0;
      return fnt::CharWidth(((bmp_font *)Handle)->font, Char);
   }
}

/*********************************************************************************************************************

-FUNCTION-
ClosePath: Close the path by connecting the beginning and end points.

This function will set a close-path command at the current vertex.  It then increments the vertex position
for the next path command.

Note that closing a path does not necessarily terminate the vector.  Further paths can be added to the sequence and
interesting effects can be created by taking advantage of fill rules.

-INPUT-
ptr Path: The vector path to modify.

*********************************************************************************************************************/

void ClosePath(APTR Vector)
{
   ((SimpleVector *)Vector)->mPath.close_polygon();
}

/*********************************************************************************************************************

-FUNCTION-
Curve3: Alter a path by inserting a quadratic bezier curve command at the current vertex position.

This function will set a quadratic bezier curve command at the current vertex.  It then increments the vertex position
for the next path command.

-INPUT-
ptr Path: The vector path to modify.
double CtrlX: Control point horizontal coordinate.
double CtrlY: Control point vertical coordinate.
double X: The horizontal end point for the curve3 command.
double Y: The vertical end point for the curve3 command.

*********************************************************************************************************************/

void Curve3(APTR Vector, double CtrlX, double CtrlY, double X, double Y)
{
   ((SimpleVector *)Vector)->mPath.curve3(CtrlX, CtrlY, X, Y);
}

/*********************************************************************************************************************

-FUNCTION-
Curve4: Alter a path by inserting a curve4 command at the current vertex position.

This function will set a cubic bezier curve command at the current vertex.  It then increments the vertex position
for the next path command.

-INPUT-
ptr Path: The vector path to modify.
double CtrlX1: Control point 1 horizontal coordinate.
double CtrlY1: Control point 1 vertical coordinate.
double CtrlX2: Control point 2 horizontal coordinate.
double CtrlY2: Control point 2 vertical coordinate.
double X: The horizontal end point for the curve4 command.
double Y: The vertical end point for the curve4 command.

*********************************************************************************************************************/

void Curve4(APTR Vector, double CtrlX1, double CtrlY1, double CtrlX2, double CtrlY2, double X, double Y)
{
   ((SimpleVector *)Vector)->mPath.curve4(CtrlX1, CtrlY1, CtrlX2, CtrlY2, X, Y);
}

/*********************************************************************************************************************

-FUNCTION-
DrawPath: Draws a vector path to a target bitmap.

Use DrawPath() to draw a generated path to a @Bitmap, using customised fill and stroke definitions.  This
functionality provides an effective alternative to configuring vector scenes for situations where only
simple vector shapes are required.  However, it is limited in that advanced rendering options and effects are not
available to the client.

A `StrokeStyle` and/or `FillStyle` will be required to render the path.  Valid styles are allocated and configured using
recognised vector style objects, specifically from the classes @VectorImage, @VectorPattern and @VectorGradient.  If a
fill or stroke operation is not required, set the relevant parameter to `NULL`.

-INPUT-
obj(Bitmap) Bitmap: Pointer to a target @Bitmap object.
ptr Path: The vector path to render.
double StrokeWidth: The width of the stroke.  Set to 0 if no stroke is required.
obj StrokeStyle: Pointer to a valid object for stroke definition, or `NULL` if none required.
obj FillStyle: Pointer to a valid object for fill definition, or `NULL` if none required.

-ERRORS-
Okay
NullArgs

*********************************************************************************************************************/

ERR DrawPath(objBitmap *Bitmap, APTR Path, double StrokeWidth, OBJECTPTR StrokeStyle, OBJECTPTR FillStyle)
{
   pf::Log log(__FUNCTION__);

   if ((!Bitmap) or (!Path)) return log.warning(ERR::NullArgs);
   if (StrokeWidth < 0.001) StrokeStyle = nullptr;

   if ((!StrokeStyle) and (!FillStyle)) {
      log.traceWarning("No Stroke or Fill parameter provided.");
      return ERR::Okay;
   }

   ((SimpleVector *)Path)->DrawPath(Bitmap, StrokeWidth, StrokeStyle, FillStyle);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
FlushMatrix: Flushes matrix changes to a vector.

If the matrices values of a vector have been directly modified by the client, the changes will need to be flushed in
order to have those changes reflected on the display.  This needs to be done before the next draw cycle.

Note that if the client uses API functions to modify a !VectorMatrix, a call to FlushMatrix() is unnecessary as the
vector will have already been marked for an update.

-INPUT-
struct(*VectorMatrix) Matrix: The matrix to be flushed.

-ERRORS-
Okay:
NullArgs:

*********************************************************************************************************************/

ERR FlushMatrix(VectorMatrix *Matrix)
{
   if (!Matrix) {
      pf::Log log(__FUNCTION__);
      return log.warning(ERR::NullArgs);
   }

   if (Matrix->Vector) mark_dirty(Matrix->Vector, RC::TRANSFORM);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
GetVertex: Retrieve the coordinates of the current vertex.

The coordinates of the current vertex are returned by this function in the `X` and `Y` parameters.  In addition, the
internal command number for that vertex is the return value.

-INPUT-
ptr Path: The vector path to query.
&double X: Pointer to a double that will receive the X coordinate value.
&double Y: Pointer to a double that will receive the Y coordinate value.

-RESULT-
int: The internal command value for the vertex will be returned.

*********************************************************************************************************************/

int GetVertex(APTR Vector, double *X, double *Y)
{
   return ((SimpleVector *)Vector)->mPath.vertex(X, Y);
}

/*********************************************************************************************************************

-FUNCTION-
GenerateEllipse: Generates an elliptical path.

Use GenerateEllipse() to create an elliptical path suitable for passing to vector functions that receive a Path
parameter.  The path must be manually deallocated with ~Core.FreeResource() once it is no longer required.

-INPUT-
double CX: Horizontal center point of the ellipse.
double CY: Vertical center point of the ellipse.
double RX: Horizontal radius of the ellipse.
double RY: Vertical radius of the ellipse.
int Vertices: Optional.  If `>= 3`, the total number of generated vertices will be limited to the specified value.
&ptr Path: A pointer variable that will receive the resulting path.

-ERRORS-
Okay
NullArgs
AllocMemory

*********************************************************************************************************************/

ERR GenerateEllipse(double CX, double CY, double RX, double RY, int Vertices, APTR *Path)
{
   pf::Log log(__FUNCTION__);

   if (!Path) return log.warning(ERR::NullArgs);

   auto vector = new_simplevector();
   if (!vector) return log.warning(ERR::CreateResource);

#if 0
   // Bezier curves can produce a reasonable approximation of an ellipse, but in practice there is
   // both a noticeable loss of speed and path accuracy vs the point plotting method.

   const double kappa = 0.5522848; // 4 * ((√(2) - 1) / 3)

   const double ox = RX * kappa;  // control point offset horizontal
   const double oy = RY * kappa;  // control point offset vertical
   const double xe = CX + RX;
   const double ye = CY + RY;

   vector->mPath.move_to(CX - RX, CY);
   vector->mPath.curve4(CX - RX, CY - oy, CX - ox, CY - RY, CX, CY - RY);
   vector->mPath.curve4(CX + ox, CY - RY, xe, CY - oy, xe, CY);
   vector->mPath.curve4(xe, CY + oy, CX + ox, ye, CX, ye);
   vector->mPath.curve4(CX - ox, ye, CX - RX, CY + oy, CX - RX, CY);
   vector->mPath.close_polygon();
#else
   uint32_t steps;

   if (Vertices >= 3) steps = Vertices;
   else {
      const double ra = (fabs(RX) + fabs(RY)) * 0.5;
      const double da = acos(ra / (ra + 0.125)) * 2.0;
      steps = agg::uround(2.0 * agg::pi / da);
      if (steps < 3) steps = 3; // Because you need at least 3 vertices to create a shape.
   }

   for (uint32_t step=0; step < steps; step++) {
      const double angle = double(step) / double(steps) * 2.0 * agg::pi;
      const double x = CX + cos(angle) * RX;
      const double y = CY + sin(angle) * RY;
      if (step == 0) vector->mPath.move_to(x, y);
      else vector->mPath.line_to(x, y);
   }
   vector->mPath.close_polygon();
#endif

   *Path = vector;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
GenerateRectangle: Generate a rectangular path at (x,y) with size (width,height).

Use GenerateRectangle() to create a rectangular path suitable for passing to vector functions that receive a Path
parameter.  The path must be manually deallocated with ~Core.FreeResource() once it is no longer required.

-INPUT-
double X: The horizontal position of the rectangle.
double Y: The vertical position of the rectangle.
double Width: The width of the rectangle.
double Height: The height of the rectangle.
&ptr Path: A pointer variable that will receive the resulting path.

-ERRORS-
Okay
NullArgs
AllocMemory

*********************************************************************************************************************/

ERR GenerateRectangle(double X, double Y, double Width, double Height, APTR *Path)
{
   pf::Log log(__FUNCTION__);

   if (!Path) return log.warning(ERR::NullArgs);

   auto vector = new_simplevector();
   if (!vector) return log.warning(ERR::CreateResource);

   vector->mPath.move_to(X, Y);
   vector->mPath.line_to(X+Width, Y);
   vector->mPath.line_to(X+Width, Y+Height);
   vector->mPath.line_to(X, Y+Height);
   vector->mPath.close_polygon();
   *Path = vector;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
GeneratePath: Generates a path from an SVG path command sequence, or an empty path for custom configuration.

This function will generate a vector path from a sequence of fixed point coordinates and curve instructions.  The
resulting path can then be passed to vector functions that receive a Path parameter.  The path must be manually
deallocated with ~Core.FreeResource() once it is no longer required.

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

If the `Sequence` is `NULL` then an empty path resource will be generated.  This path will be suitable for passing
to path modifying functions such as ~MoveTo() and ~LineTo() for custom path generation.

-INPUT-
cstr Sequence: The command sequence to process.  If no sequence is specified then the path will be empty.
&ptr Path: A pointer variable that will receive the resulting path.

-ERRORS-
Okay
NullArgs
AllocMemory

*********************************************************************************************************************/

ERR GeneratePath(CSTRING Sequence, APTR *Path)
{
   if (!Path) return ERR::NullArgs;

   ERR error = ERR::Okay;

   if (!Sequence) {
      auto vector = new_simplevector();
      if (vector) *Path = vector;
      else error = ERR::AllocMemory;
   }
   else {
      std::vector<PathCommand> paths;
      if ((error = read_path(paths, Sequence)) IS ERR::Okay) {
         auto vector = new_simplevector();
         if (vector) {
            convert_to_aggpath(nullptr, paths, vector->mPath);
            *Path = vector;
         }
         else error = ERR::AllocMemory;
      }
   }

   return error;
}

/*********************************************************************************************************************

-FUNCTION-
GetFontHandle: Returns a handle for a given font family.

For a given font family and size, this function will return a `Handle` that can be passed to font querying functions.

The handle is deterministic and permanent, remaining valid for the lifetime of the program.

-INPUT-
cstr Family: The name of the font family to access.
cstr Style: The preferred style to choose from the family.  Use `Regular` or `NULL` for the default.
int Weight: Equivalent to CSS font-weight; a value of 400 or 0 will equate to normal.
int Size: The font-size, measured in pixels @ 72 DPI.
&ptr Handle: The resulting font handle is returned here.

-ERRORS-
Okay:
Args:
NullArgs:
-END-

*********************************************************************************************************************/

ERR GetFontHandle(CSTRING Family, CSTRING Style, int Weight, int Size, APTR *Handle)
{
   pf::Log log(__FUNCTION__);

   if (Size < 1) return log.warning(ERR::Args);

   if (!Style) Style = "Regular";
   common_font *handle;
   if (auto error = get_font(log, Family, Style, Weight, Size, &handle); error IS ERR::Okay) {
      *Handle = handle;
      return ERR::Okay;
   }
   else return error;
}

/*********************************************************************************************************************

-FUNCTION-
GetFontMetrics: Returns a set of display metric values for a font.

Call GetFontMetrics() to retrieve a basic set of display metrics measured in pixels (adjusted to the display's DPI)
for a given font.

-INPUT-
ptr Handle: A font handle obtained from ~GetFontHandle().
struct(*FontMetrics) Info: The font metrics for the `Handle` will be stored here.

-ERRORS-
Okay:
NullArgs:
-END-

*********************************************************************************************************************/

ERR GetFontMetrics(APTR Handle, struct FontMetrics *Metrics)
{
   if ((!Handle) or (!Metrics)) return ERR::NullArgs;

   if (((common_font *)Handle)->type IS CF_FREETYPE) {
      auto pt = (freetype_font::ft_point *)Handle;
      Metrics->Height      = pt->height;
      Metrics->LineSpacing = pt->line_spacing;
      Metrics->Ascent      = pt->ascent;
      Metrics->Descent     = pt->descent;
      return ERR::Okay;
   }
   else {
      auto font = ((bmp_font *)Handle)->font;
      Metrics->Height      = font->Ascent;
      Metrics->LineSpacing = font->LineSpacing;
      Metrics->Ascent      = font->Height;
      Metrics->Descent     = font->Gutter;
      return ERR::Okay;
   }
}

/*********************************************************************************************************************

-FUNCTION-
TracePath: Returns the coordinates for a vector path, using callbacks.

Any vector that generates a path can be traced by calling this method.  Tracing allows the caller to follow the
`Path` from point-to-point if the path were to be rendered with a stroke.  The prototype of the callback  function
is `ERR Function(OBJECTPTR Vector, INT Index, INT Command, double X, double Y, APTR Meta)`.

The `Index` is an incrementing counter that reflects the currently plotted point.  The `X` and `Y` parameters reflect the
coordinate of a point on the path.

If the `Callback` returns `ERR::Terminate`, then no further coordinates will be processed.

-INPUT-
ptr Path:      The vector path to trace.
ptr(func) Callback: A function to call with the path coordinates.
double Scale:  Set to 1.0 (recommended) to trace the path at a scale of 1 to 1.

-ERRORS-
Okay:
NullArgs:

*********************************************************************************************************************/

ERR TracePath(APTR Path, FUNCTION *Callback, double Scale)
{
   pf::Log log;

   if ((!Path) or (!Callback)) return ERR::NullArgs;

   ((SimpleVector *)Path)->mPath.rewind(0);
   ((SimpleVector *)Path)->mPath.approximation_scale(Scale);

   double x, y;
   int cmd = -1;
   int index = 0;

   if (Callback->isC()) {
      auto routine = ((ERR (*)(SimpleVector *, int, int, double, double, APTR))(Callback->Routine));

      pf::SwitchContext context(ParentContext());

      do {
         cmd = ((SimpleVector *)Path)->mPath.vertex(&x, &y);
         if (agg::is_vertex(cmd)) {
            if (routine((SimpleVector *)Path, index++, cmd, x, y, Callback->Meta) IS ERR::Terminate) {
               return ERR::Okay;
            }
         }
      } while (cmd != agg::path_cmd_stop);
   }
   else if (Callback->isScript()) {
      std::array<ScriptArg, 5> args {{
         { "Path",    Path },
         { "Index",   int(0) },
         { "Command", int(0) },
         { "X",       double(0) },
         { "Y",       double(0) }
      }};
      args[0].Address = Path;

      ERR result;
      do {
         cmd = ((SimpleVector *)Path)->mPath.vertex(&x, &y);
         if (agg::is_vertex(cmd)) {
            args[1].Int = index++;
            args[2].Int = cmd;
            args[3].Double = x;
            args[4].Double = y;
            if (sc::Call(*Callback, args, result) != ERR::Okay) return ERR::Function;
            if (result IS ERR::Terminate) return ERR::Okay;
         }
      } while (cmd != agg::path_cmd_stop);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
LineTo: Alter a path by setting a line-to command at the current vertex position.

This function alters a path by setting a line-to command at the current vertex position.  The index is then advanced by
one to the next vertex position.

-INPUT-
ptr Path: The vector path to modify.
double X: The line end point on the horizontal plane.
double Y: The line end point on the vertical plane.

*********************************************************************************************************************/

void LineTo(APTR Vector, double X, double Y)
{
   ((SimpleVector *)Vector)->mPath.line_to(X, Y);
}

/*********************************************************************************************************************

-FUNCTION-
MoveTo: Alter a path by setting a move-to command at the current vertex position.

This function will set a move-to command at the current vertex.  It then increments the vertex position for the next
path command.

The move-to command is used to move the pen to a new coordinate without drawing a line.

-INPUT-
ptr Path: The vector path to modify.
double X: The horizontal end point for the command.
double Y: The vertical end point for the command.

*********************************************************************************************************************/

void MoveTo(APTR Vector, double X, double Y)
{
   ((SimpleVector *)Vector)->mPath.move_to(X, Y);
}

/*********************************************************************************************************************

-FUNCTION-
Multiply: Combines a matrix with a series of matrix values.

This function uses matrix multiplication to combine a set of values with a !VectorMatrix structure.

-INPUT-
struct(*VectorMatrix) Matrix: The target transformation matrix.
double ScaleX: Matrix value A.
double ShearY: Matrix value B.
double ShearX: Matrix value C.
double ScaleY: Matrix value D.
double TranslateX: Matrix value E.
double TranslateY: Matrix value F.

-ERRORS-
Okay:
NullArgs:
-END-

*********************************************************************************************************************/

ERR Multiply(VectorMatrix *Matrix, double ScaleX, double ShearY, double ShearX,
   double ScaleY, double TranslateX, double TranslateY)
{
   if (!Matrix) {
      pf::Log log(__FUNCTION__);
      return log.warning(ERR::NullArgs);
   }

   auto &d = *Matrix;
   double t0    = (d.ScaleX * ScaleX) + (d.ShearY * ShearX);
   double t2    = (d.ShearX * ScaleX) + (d.ScaleY * ShearX);
   double t4    = (d.TranslateX * ScaleX) + (d.TranslateY * ShearX) + TranslateX;
   d.ShearY     = (d.ScaleX * ShearY) + (d.ShearY * ScaleY);
   d.ScaleY     = (d.ShearX * ShearY) + (d.ScaleY * ScaleY);
   d.TranslateY = (d.TranslateX * ShearY) + (d.TranslateY * ScaleY) + TranslateY;
   d.ScaleX     = t0;
   d.ShearX     = t2;
   d.TranslateX = t4;

   if (Matrix->Vector) mark_dirty(Matrix->Vector, RC::TRANSFORM);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
MultiplyMatrix: Combines a source matrix with a target.

This function uses matrix multiplication to combine a `Source` matrix with a `Target`.

-INPUT-
struct(*VectorMatrix) Target: The target transformation matrix.
struct(*VectorMatrix) Source: The source transformation matrix.

-ERRORS-
Okay:
NullArgs:
-END-

*********************************************************************************************************************/

ERR MultiplyMatrix(VectorMatrix *Target, VectorMatrix *Source)
{
   if ((!Target) or (!Source)) {
      pf::Log log(__FUNCTION__);
      return log.warning(ERR::NullArgs);
   }

   auto &d = *Target;
   auto &s = *Source;
   double t0  = (d.ScaleX * s.ScaleX) + (d.ShearY * s.ShearX);
   double t2  = (d.ShearX * s.ScaleX) + (d.ScaleY * s.ShearX);
   double t4  = (d.TranslateX * s.ScaleX) + (d.TranslateY * s.ShearX) + s.TranslateX;
   d.ShearY     = (d.ScaleX * s.ShearY) + (d.ShearY * s.ScaleY);
   d.ScaleY     = (d.ShearX * s.ShearY) + (d.ScaleY * s.ScaleY);
   d.TranslateY = (d.TranslateX * s.ShearY) + (d.TranslateY * s.ScaleY) + s.TranslateY;
   d.ScaleX     = t0;
   d.ShearX     = t2;
   d.TranslateX = t4;

   if (Target->Vector) mark_dirty(Target->Vector, RC::TRANSFORM);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
ParseTransform: Parse an SVG transformation string and apply the values to a matrix.

This function parses a sequence of transform instructions and applies them to a matrix.

The string must be written using SVG guidelines for the transform attribute.  For example,
`skewX(20) rotate(45 50 50)` would be valid.  Transform instructions are applied in reverse, as per the standard.

Note that any existing transforms applied to the matrix will be cancelled as a result of calling this function.
If existing matrix values need to be retained, create a fresh matrix and use ~Multiply() to combine them.

-INPUT-
struct(*VectorMatrix) Matrix: The target transformation matrix.
cstr Transform: The transform to apply, expressed as a string instruction.

-ERRORS-
Okay:
NullArgs:
-END-

*********************************************************************************************************************/

ERR ParseTransform(VectorMatrix *Matrix, CSTRING Commands)
{
   if ((!Matrix) or (!Commands)) {
      pf::Log log(__FUNCTION__);
      return log.warning(ERR::NullArgs);
   }

   enum { M_MUL, M_TRANSLATE, M_ROTATE, M_SCALE, M_SKEW };
   class cmd {
      public:
      int8_t type;
      double sx, sy, shx, shy, tx, ty;
      double angle;
      cmd(int8_t pType) : type(pType), sx(0), sy(0), shx(0), shy(0), tx(0), ty(0), angle(0) {};
   };

   std::vector<cmd> list;

   auto str = Commands;
   while (*str) {
      if ((*str >= 'a') and (*str <= 'z')) {
         if (startswith("matrix", str)) {
            cmd m(M_MUL);
            str += 6;
            read_numseq(str, { &m.sx, &m.shy, &m.shx, &m.sy, &m.tx, &m.ty });
            list.push_back(std::move(m));
         }
         else if (startswith("translate", str)) {
            cmd m(M_TRANSLATE);
            str += 9;
            bool scaled_x, scaled_y;
            next_value(str);
            m.tx = read_unit(str, scaled_x);
            next_value(str);
            m.ty = read_unit(str, scaled_y);
            read_numseq(str, { &m.tx, &m.ty });
            list.push_back(std::move(m));
         }
         else if (startswith("rotate", str)) {
            cmd m(M_ROTATE);
            str += 6;
            read_numseq(str, { &m.angle, &m.tx, &m.ty });
            list.push_back(std::move(m));
         }
         else if (startswith("scale", str)) {
            cmd m(M_SCALE);
            m.tx = 1.0;
            m.ty = DBL_EPSILON;
            str += 5;
            read_numseq(str, { &m.tx, &m.ty });
            if (m.ty IS DBL_EPSILON) m.ty = m.tx;
            list.push_back(std::move(m));
         }
         else if (startswith("skewX", str)) {
            cmd m(M_SKEW);
            m.ty = 0;
            str += 5;
            read_numseq(str, { &m.tx });
            list.push_back(std::move(m));
         }
         else if (startswith("skewY", str)) {
            cmd m(M_SKEW);
            m.tx = 0;
            str += 5;
            read_numseq(str, { &m.ty });
            list.push_back(std::move(m));
         }
         else str++;
      }
      else str++;
   }

   Matrix->ScaleX = 1.0;
   Matrix->ShearY = 0;
   Matrix->ShearX = 0;
   Matrix->ScaleY = 1.0;
   Matrix->TranslateX = 0;
   Matrix->TranslateY = 0;

   std::for_each(list.rbegin(), list.rend(), [&](auto m) {
      switch (m.type) {
         case M_MUL: {
            auto &d = *Matrix;
            auto &s = m;
            double t0    = (d.ScaleX * s.sx) + (d.ShearY * s.shx);
            double t2    = (d.ShearX * s.sx) + (d.ScaleY * s.shx);
            double t4    = (d.TranslateX * s.sx) + (d.TranslateY * s.shx) + s.tx;
            d.ShearY     = (d.ScaleX * s.shy) + (d.ShearY * s.sy);
            d.ScaleY     = (d.ShearX * s.shy) + (d.ScaleY * s.sy);
            d.TranslateY = (d.TranslateX * s.shy) + (d.TranslateY * s.sy) + s.ty;
            d.ScaleX     = t0;
            d.ShearX     = t2;
            d.TranslateX = t4;
            break;
         }

         case M_TRANSLATE:
            Matrix->TranslateX += m.tx;
            Matrix->TranslateY += m.ty;
            break;

         case M_ROTATE: {
            vec::Rotate(Matrix, m.angle, m.tx, m.ty);
            break;
         }

         case M_SCALE:
            Matrix->ScaleX     *= m.tx;
            Matrix->ShearX     *= m.tx;
            Matrix->TranslateX *= m.tx;
            Matrix->ShearY     *= m.ty;
            Matrix->ScaleY     *= m.ty;
            Matrix->TranslateY *= m.ty;
            break;

         case M_SKEW:
            vec::Skew(Matrix, m.tx, m.ty);
            break;
      }
   });

   if (Matrix->Vector) mark_dirty(Matrix->Vector, RC::TRANSFORM);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
ResetMatrix: Resets a transformation matrix to its default state.

Call ResetMatrix() to reset a transformation matrix to its default state, undoing all former transform operations.

-INPUT-
struct(*VectorMatrix) Matrix: The target transformation matrix.

-ERRORS-
Okay:
NullArgs:

*********************************************************************************************************************/

ERR ResetMatrix(VectorMatrix *Matrix)
{
   if (!Matrix) {
      pf::Log log(__FUNCTION__);
      return log.warning(ERR::NullArgs);
   }

   Matrix->ScaleX     = 1.0;
   Matrix->ScaleY     = 1.0;
   Matrix->ShearX     = 0;
   Matrix->ShearY     = 0;
   Matrix->TranslateX = 0;
   Matrix->TranslateY = 0;

   if (Matrix->Vector) mark_dirty(Matrix->Vector, RC::TRANSFORM);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
RewindPath: Resets the vertex seek position to zero.

Rewinding a path will reset the current vertex index to zero.  The next call to a vertex modification function such as
~LineTo() would result in the first vertex being modified.

If the referenced `Path` is empty, this function does nothing.

-INPUT-
ptr Path: The vector path to rewind.

*********************************************************************************************************************/

void RewindPath(APTR Vector)
{
   if (Vector) ((SimpleVector *)Vector)->mPath.rewind(0);
}

/*********************************************************************************************************************

-FUNCTION-
Rotate: Applies a rotation transformation to a matrix.

This function will apply a rotation transformation to a matrix.  By default, rotation will occur around point `(0, 0)`
unless `CenterX` and `CenterY` values are specified.

-INPUT-
struct(*VectorMatrix) Matrix: The target transformation matrix.
double Angle: Angle of rotation, in degrees.
double CenterX: Center of rotation on the horizontal axis.
double CenterY: Center of rotation on the vertical axis.

-ERRORS-
Okay:
NullArgs:

*********************************************************************************************************************/

ERR Rotate(VectorMatrix *Matrix, double Angle, double CenterX, double CenterY)
{
   if (!Matrix) {
      pf::Log log(__FUNCTION__);
      return log.warning(ERR::NullArgs);
   }

   Matrix->TranslateX -= CenterX;
   Matrix->TranslateY -= CenterY;

   double ca = cos(Angle * DEG2RAD);
   double sa = sin(Angle * DEG2RAD);
   double t0 = (Matrix->ScaleX * ca) - (Matrix->ShearY * sa);
   double t2 = (Matrix->ShearX * ca) - (Matrix->ScaleY * sa);
   double t4 = (Matrix->TranslateX  * ca) - (Matrix->TranslateY * sa);
   Matrix->ShearY     = (Matrix->ScaleX * sa) + (Matrix->ShearY * ca);
   Matrix->ScaleY     = (Matrix->ShearX * sa) + (Matrix->ScaleY * ca);
   Matrix->TranslateY = (Matrix->TranslateX * sa) + (Matrix->TranslateY * ca);
   Matrix->ScaleX     = t0;
   Matrix->ShearX     = t2;
   Matrix->TranslateX = t4;

   Matrix->TranslateX += CenterX;
   Matrix->TranslateY += CenterY;

   if (Matrix->Vector) mark_dirty(Matrix->Vector, RC::TRANSFORM);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
Scale: Scale the size of the vector by (x,y)

This function will perform a scale operation on a matrix.  Values of less than `1.0` will shrink the affected vector
path, while values greater than `1.0` will enlarge it.

Scaling is relative to position `(0, 0)`.  If the width and height of the vector path needs to be transformed without
affecting its top-left position, the client must translate the path to `(0, 0)` around its center point.  The path
should then be scaled before being transformed back to its original top-left coordinate.

The scale operation can also be used to flip a vector path if negative values are used.  For instance, a value of
`-1.0` on the x axis would result in a `1:1` flip across the horizontal.

-INPUT-
struct(*VectorMatrix) Matrix: The target transformation matrix.
double X: The scale factor on the x-axis.
double Y: The scale factor on the y-axis.

-ERRORS-
Okay
NullArgs

*********************************************************************************************************************/

ERR Scale(VectorMatrix *Matrix, double X, double Y)
{
   if (!Matrix) {
      pf::Log log(__FUNCTION__);
      return log.warning(ERR::NullArgs);
   }

   Matrix->ScaleX     *= X;
   Matrix->ShearX     *= X;
   Matrix->TranslateX *= X;
   Matrix->ShearY     *= Y;
   Matrix->ScaleY     *= Y;
   Matrix->TranslateY *= Y;

   if (Matrix->Vector) mark_dirty(Matrix->Vector, RC::TRANSFORM);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
Skew: Skews the matrix along the horizontal and/or vertical axis.

The Skew function applies a skew transformation to the horizontal and/or vertical axis of the matrix.
Valid X and Y values are in the range of `-90 < Angle < 90`.

-INPUT-
struct(*VectorMatrix) Matrix: The target transformation matrix.
double X: The angle to skew along the horizontal.
double Y: The angle to skew along the vertical.

-ERRORS-
Okay:
NullArgs:
OutOfRange: At least one of the angles is out of the allowable range.
-END-

*********************************************************************************************************************/

ERR Skew(VectorMatrix *Matrix, double X, double Y)
{
   pf::Log log(__FUNCTION__);

   if (!Matrix) return log.warning(ERR::NullArgs);

   if ((X > -90) and (X < 90)) {
      VectorMatrix skew = {
         .ScaleX = 1.0, .ShearY = 0, .ShearX = tan(X * DEG2RAD),
         .ScaleY = 1.0, .TranslateX = 0, .TranslateY = 0
      };

      vec::MultiplyMatrix(Matrix, &skew);
   }
   else return log.warning(ERR::OutOfRange);

   if ((Y > -90) and (Y < 90)) {
      VectorMatrix skew = {
         .ScaleX = 1.0, .ShearY = tan(Y * DEG2RAD), .ShearX = 0,
         .ScaleY = 1.0, .TranslateX = 0, .TranslateY = 0
      };

      vec::MultiplyMatrix(Matrix, &skew);
   }
   else return log.warning(ERR::OutOfRange);

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
Smooth3: Alter a path by setting a smooth3 command at the current vertex position.

This function will set a quadratic bezier curve command at the current vertex.  It then increments the vertex position
for the next path command.

The control point from the previous curve is used as the control point for the new curve, hence the 'smooth'.

-INPUT-
ptr Path: The vector path to modify.
double X: The horizontal end point for the smooth3 command.
double Y: The vertical end point for the smooth3 command.

*********************************************************************************************************************/

void Smooth3(APTR Vector, double X, double Y)
{
   if (!Vector) return;
   ((SimpleVector *)Vector)->mPath.curve3(X, Y);
}

/*********************************************************************************************************************

-FUNCTION-
Smooth4: Alter a path by setting a smooth4 command at the current vertex position.

This function will set a cubic bezier curve command at the current vertex.  It then increments the vertex position
for the next path command.

The control point from the previous curve will be used in addition to the CtrlX and CtrlY points, hence the
name 'smoothed curve'.

-INPUT-
ptr Path: The vector path to modify.
double CtrlX: Control point horizontal coordinate.
double CtrlY: Control point vertical coordinate.
double X: The horizontal end point for the smooth4 instruction.
double Y: The vertical end point for the smooth4 instruction.

*********************************************************************************************************************/

void Smooth4(APTR Vector, double CtrlX, double CtrlY, double X, double Y)
{
   if (!Vector) return;
   ((SimpleVector *)Vector)->mPath.curve4(CtrlX, CtrlY, X, Y);
}

/*********************************************************************************************************************

-FUNCTION-
StringWidth: Calculate the pixel width of a UTF-8 string, for a given font.

This function calculates the pixel width of a string, in relation to a known font.  The function takes into account
any line-feeds that are encountered, so if the `String` contains multiple lines, then the width of the longest line will
be returned.

The font's kerning specifications will be taken into account when computing the distance between glyphs.

-INPUT-
ptr FontHandle: A font handle obtained from ~GetFontHandle().
cstr String: Pointer to a null-terminated string.
int Chars: The maximum number of unicode characters to process in calculating the string width.  Set to `-1` for all chars.

-RESULT-
double: The pixel width of the string is returned.
-END-

*********************************************************************************************************************/

double StringWidth(APTR Handle, CSTRING String, int Chars)
{
   pf::Log log(__FUNCTION__);

   if ((!Handle) or (!String)) { log.warning(ERR::NullArgs); return 0; }

   const std::lock_guard lock(glFontMutex);

   if (((common_font *)Handle)->type IS CF_FREETYPE) {
      auto pt = (freetype_font::ft_point *)Handle;
      FT_Activate_Size(pt->ft_size);

      if (Chars IS -1) Chars = 0x7fffffff;

      int len        = 0;
      int widest     = 0;
      int prev_glyph = 0;
      int i = 0;
      while ((i < Chars) and (String[i])) {
         if (String[i] IS '\n') {
            if (widest < len) widest = len;
            len = 0;
            i++;
         }
         else {
            uint32_t unicode;
            auto charlen = get_utf8(String, unicode, i);
            auto &glyph  = pt->get_glyph(unicode);
            len += glyph.adv_x;
            if (prev_glyph) {;
               FT_Vector delta;
               FT_Get_Kerning(pt->ft_size->face, prev_glyph, glyph.glyph_index, FT_KERNING_DEFAULT, &delta);
               len += int26p6_to_dbl(delta.x);
            }
            prev_glyph = glyph.glyph_index;
            i += charlen;
         }
      }

      if (widest > len) return widest;
      else return len;
   }
   else return fnt::StringWidth(((bmp_font *)Handle)->font, String, Chars);
}

/*********************************************************************************************************************

-FUNCTION-
Translate: Translates the vector by (X,Y).

This function will translate the matrix in the direction of the provided (X,Y) values.

-INPUT-
struct(*VectorMatrix) Matrix: The target transformation matrix.
double X: Translation along the x-axis.
double Y: Translation along the y-axis.

-ERRORS-
Okay:
NullArgs:
-END-

*********************************************************************************************************************/

ERR Translate(VectorMatrix *Matrix, double X, double Y)
{
   if (!Matrix) {
      pf::Log log(__FUNCTION__);
      return log.warning(ERR::NullArgs);
   }

   Matrix->TranslateX += X;
   Matrix->TranslateY += Y;

   if (Matrix->Vector) mark_dirty(Matrix->Vector, RC::TRANSFORM);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
TranslatePath: Translates a path by (x,y)

This function will translate all vertices of a path by (X,Y).

-INPUT-
ptr Path: Pointer to a generated path.
double X: Translate the path horizontally by the given value.
double Y: Translate the path vertically by the given value.

-END-

*********************************************************************************************************************/

void TranslatePath(APTR Vector, double X, double Y)
{
   if (!Vector) return;
   ((SimpleVector *)Vector)->mPath.translate_all_paths(X, Y);
}

#include "vector_readpainter.cpp"

} // namespace

//********************************************************************************************************************

#include "module_def.c"

//********************************************************************************************************************

ERR MODOpen(OBJECTPTR Module)
{
   ((objModule *)Module)->setFunctionList(glFunctions);
   return ERR::Okay;
}

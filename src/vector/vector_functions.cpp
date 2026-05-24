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

#include <charconv>

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

//********************************************************************************************************************

inline void mark_matrix_dirty(VectorMatrix *Matrix)
{
   if (Matrix->Vector) mark_dirty(Matrix->Vector, RC::TRANSFORM);
}

static void multiply_values(VectorMatrix &Matrix, double ScaleX, double ShearY, double ShearX, double ScaleY,
   double TranslateX, double TranslateY)
{
   double t0       = (Matrix.ScaleX * ScaleX) + (Matrix.ShearY * ShearX);
   double t2       = (Matrix.ShearX * ScaleX) + (Matrix.ScaleY * ShearX);
   double t4       = (Matrix.TranslateX * ScaleX) + (Matrix.TranslateY * ShearX) + TranslateX;
   Matrix.ShearY     = (Matrix.ScaleX * ShearY) + (Matrix.ShearY * ScaleY);
   Matrix.ScaleY     = (Matrix.ShearX * ShearY) + (Matrix.ScaleY * ScaleY);
   Matrix.TranslateY = (Matrix.TranslateX * ShearY) + (Matrix.TranslateY * ScaleY) + TranslateY;
   Matrix.ScaleX     = t0;
   Matrix.ShearX     = t2;
   Matrix.TranslateX = t4;
}

inline void multiply_matrix(VectorMatrix &Target, const VectorMatrix &Source)
{
   multiply_values(Target, Source.ScaleX, Source.ShearY, Source.ShearX, Source.ScaleY,
      Source.TranslateX, Source.TranslateY);
}

inline void scale_matrix(VectorMatrix &Matrix, double X, double Y)
{
   Matrix.ScaleX     *= X;
   Matrix.ShearX     *= X;
   Matrix.TranslateX *= X;
   Matrix.ShearY     *= Y;
   Matrix.ScaleY     *= Y;
   Matrix.TranslateY *= Y;
}

static void rotate_matrix(VectorMatrix &Matrix, double Angle, double CenterX, double CenterY)
{
   Matrix.TranslateX -= CenterX;
   Matrix.TranslateY -= CenterY;

   double ca = cos(Angle * DEG2RAD);
   double sa = sin(Angle * DEG2RAD);
   double t0 = (Matrix.ScaleX * ca) - (Matrix.ShearY * sa);
   double t2 = (Matrix.ShearX * ca) - (Matrix.ScaleY * sa);
   double t4 = (Matrix.TranslateX * ca) - (Matrix.TranslateY * sa);
   Matrix.ShearY     = (Matrix.ScaleX * sa) + (Matrix.ShearY * ca);
   Matrix.ScaleY     = (Matrix.ShearX * sa) + (Matrix.ScaleY * ca);
   Matrix.TranslateY = (Matrix.TranslateX * sa) + (Matrix.TranslateY * ca);
   Matrix.ScaleX     = t0;
   Matrix.ShearX     = t2;
   Matrix.TranslateX = t4;

   Matrix.TranslateX += CenterX;
   Matrix.TranslateY += CenterY;
}

static ERR skew_matrix(VectorMatrix &Matrix, double X, double Y)
{
   if ((X > -90) and (X < 90)) {
      VectorMatrix skew = {
         .ScaleX = 1.0, .ShearY = 0, .ShearX = tan(X * DEG2RAD),
         .ScaleY = 1.0, .TranslateX = 0, .TranslateY = 0
      };

      multiply_matrix(Matrix, skew);
   }
   else return ERR::OutOfRange;

   if ((Y > -90) and (Y < 90)) {
      VectorMatrix skew = {
         .ScaleX = 1.0, .ShearY = tan(Y * DEG2RAD), .ShearX = 0,
         .ScaleY = 1.0, .TranslateX = 0, .TranslateY = 0
      };

      multiply_matrix(Matrix, skew);
   }
   else return ERR::OutOfRange;

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
ApplyPath: Applies a generated path resource to a VectorPath object.

Any path originating from ~GeneratePath(), ~GenerateEllipse() or ~GenerateRectangle() can be applied to a VectorPath
object by calling ApplyPath().  The source `Path` can then be deallocated with ~Core.FreeResource() if it is no longer
required.

This function is intended for paths that are generated or changed at run-time, where converting the path back to a
string would add unnecessary processing overhead.

-INPUT-
ptr Path: The source path to be copied.
obj(VectorPath) VectorPath: The target VectorPath object.

-ERRORS-
Okay
NullArgs
Args

*********************************************************************************************************************/

ERR ApplyPath(APTR Vector, objVectorPath *VectorPath)
{
   if ((not Vector) or (not VectorPath)) return ERR::NullArgs;
   if (VectorPath->classID() != CLASSID::VECTORPATH) return ERR::Args;

   VectorPath->set(FID_Sequence, CSTRING(nullptr)); // Clear any pre-existing path information.

   // TODO: Apply mPath to VectorPath

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
ArcTo: Appends an arc command to a generated path.

This function appends an arc segment from the current point to `X`,`Y`.  The `LARGE` and `SWEEP` values in `Flags`
control which of the possible arcs is selected.

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
CharWidth: Returns the rendered width of a single character.

This function returns the pixel width of a font character.  The character is specified as a Unicode value in the `Char`
parameter.  Kerning can also be returned for positioning the character against a preceding character.  Pass the
preceding character in `KChar` and a `Kerning` pointer to receive the horizontal adjustment.  If kerning information is
not required, set `KChar` to zero and `Kerning` to `NULL`.

The font's glyph spacing value is not used in calculating the character width.

-INPUT-
ptr FontHandle: The font to use for calculating the character width.
uint Char: A 32-bit unicode character.
uint KChar: A unicode character to use for calculating the font kerning (optional).
&double Kerning: The resulting kerning value (optional).

-RESULT-
double: The pixel width of the character, or `0` if `FontHandle` is `NULL`.

*********************************************************************************************************************/

double CharWidth(APTR Handle, uint32_t Char, uint32_t KChar, double *Kerning)
{
   if (not Handle) return 0;

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
ClosePath: Closes the current sub-path.

This function appends a close-path command that connects the current point to the beginning of the current sub-path.

Closing a path does not terminate the path resource.  Further sub-paths can be added to the sequence, and fill rules
can then be used to control how overlapping areas are rendered.

-INPUT-
ptr Path: The vector path to modify.

*********************************************************************************************************************/

void ClosePath(APTR Vector)
{
   ((SimpleVector *)Vector)->mPath.close_polygon();
}

/*********************************************************************************************************************

-FUNCTION-
Curve3: Appends a quadratic Bezier curve to a generated path.

This function appends a quadratic Bezier segment from the current point to `X`,`Y` using `CtrlX`,`CtrlY` as the control
point.

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
Curve4: Appends a cubic Bezier curve to a generated path.

This function appends a cubic Bezier segment from the current point to `X`,`Y` using two control points.

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
DrawPath: Draws a generated vector path to a target bitmap.

Use DrawPath() to draw a generated path to a @Bitmap, using customised fill and stroke definitions.  This
functionality provides an effective alternative to configuring vector scenes for situations where only
simple vector shapes are required.  However, it is limited in that advanced rendering options and effects are not
available to the client.

At least one of `StrokeStyle` or `FillStyle` is required to render visible output.  Valid styles are allocated and
configured using recognised vector style objects, specifically @VectorImage, @VectorPattern and @VectorGradient.  If a
fill or stroke operation is not required, set the relevant parameter to `NULL`.  Stroke rendering is also disabled when
`StrokeWidth` is less than `0.001`.

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
   kt::Log log(__FUNCTION__);

   if ((not Bitmap) or (not Path)) return log.warning(ERR::NullArgs);
   if (StrokeWidth < 0.001) StrokeStyle = nullptr;

   if ((not StrokeStyle) and (not FillStyle)) {
      log.traceWarning("No Stroke or Fill parameter provided.");
      return ERR::Okay;
   }

   ((SimpleVector *)Path)->DrawPath(Bitmap, StrokeWidth, StrokeStyle, FillStyle);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
FlushMatrix: Marks a vector transform as changed after direct matrix edits.

If a vector's matrix values have been modified directly by the client, call FlushMatrix() before the next draw cycle so
the owning vector is marked for transform recalculation.

Note that if the client uses API functions to modify a !VectorMatrix, a call to FlushMatrix() is unnecessary as the
vector will have already been marked for an update.

-INPUT-
struct(*VectorMatrix) Matrix: The matrix to be flushed.

-ERRORS-
Okay
NullArgs

*********************************************************************************************************************/

ERR FlushMatrix(VectorMatrix *Matrix)
{
   if (not Matrix) return kt::Log(__FUNCTION__).warning(ERR::NullArgs);

   if (Matrix->Vector) mark_dirty(Matrix->Vector, RC::TRANSFORM);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
GetVertex: Retrieves the next vertex from a generated path.

The coordinates of the next vertex are returned in `X` and `Y`.  The return value is the internal command value for
that vertex, or the path stop command when there are no more vertices to read.

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
GenerateEllipse: Generates an elliptical path resource.

Use GenerateEllipse() to create an elliptical path suitable for passing to vector functions that receive a Path
parameter.  The path must be manually deallocated with ~Core.FreeResource() once it is no longer required.

-INPUT-
double CX: Horizontal centre point of the ellipse.
double CY: Vertical centre point of the ellipse.
double RX: Horizontal radius of the ellipse.
double RY: Vertical radius of the ellipse.
int Vertices: Optional.  If this is `3` or greater, the ellipse will be generated with exactly this many vertices.
&ptr Path: A pointer variable that will receive the resulting path.

-ERRORS-
Okay
NullArgs
CreateResource

*********************************************************************************************************************/

ERR GenerateEllipse(double CX, double CY, double RX, double RY, int Vertices, APTR *Path)
{
   kt::Log log(__FUNCTION__);

   if (not Path) return log.warning(ERR::NullArgs);

   auto vector = new_simplevector();
   if (not vector) return log.warning(ERR::CreateResource);

   uint32_t steps;

   if (Vertices >= 3) steps = Vertices;
   else {
      const double ra = (fabs(RX) + fabs(RY)) * 0.5;
      const double da = acos(ra / (ra + 0.125)) * 2.0;
      steps = agg::uround(2.0 * agg::pi / da);
      if (steps < 3) steps = 3; // Because you need at least 3 vertices to create a shape.
   }

   const double angle_step = 2.0 * agg::pi / double(steps);
   const double cos_step = cos(angle_step);
   const double sin_step = sin(angle_step);
   double cos_angle = 1.0;
   double sin_angle = 0;

   for (uint32_t step=0; step < steps; step++) {
      const double x = CX + cos_angle * RX;
      const double y = CY + sin_angle * RY;
      if (step IS 0) vector->mPath.move_to(x, y);
      else vector->mPath.line_to(x, y);

      const double next_cos_angle = (cos_angle * cos_step) - (sin_angle * sin_step);
      sin_angle = (sin_angle * cos_step) + (cos_angle * sin_step);
      cos_angle = next_cos_angle;
   }
   vector->mPath.close_polygon();

   *Path = vector;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
GenerateRectangle: Generates a rectangular path resource.

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
CreateResource

*********************************************************************************************************************/

ERR GenerateRectangle(double X, double Y, double Width, double Height, APTR *Path)
{
   kt::Log log(__FUNCTION__);

   if (not Path) return log.warning(ERR::NullArgs);

   auto vector = new_simplevector();
   if (not vector) return log.warning(ERR::CreateResource);

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
GeneratePath: Generates a path resource from an SVG path command sequence.

This function generates a vector path from a sequence of coordinates and path instructions.  The resulting path can
then be passed to vector functions that receive a Path parameter.  The path must be manually
deallocated with ~Core.FreeResource() once it is no longer required.

The `Sequence` string is based on the SVG `path` element's `d` attribute.  Uppercase commands use absolute coordinates;
lowercase commands use coordinates relative to the previous command.

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

If `Sequence` is `NULL`, an empty path resource will be generated.  This path can be populated with functions such as
~MoveTo() and ~LineTo().

-INPUT-
cstr Sequence: The command sequence to process.  If no sequence is specified then the path will be empty.
&ptr Path: A pointer variable that will receive the resulting path.

-ERRORS-
Okay
NullArgs
AllocMemory
InvalidValue
InvalidData
BufferOverflow

*********************************************************************************************************************/

ERR GeneratePath(CSTRING Sequence, APTR *Path)
{
   if (not Path) return ERR::NullArgs;

   ERR error = ERR::Okay;

   if (not Sequence) {
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
GetFontHandle: Returns a cached handle for a font family, style and size.

For a given font family, style and size, this function returns a `Handle` that can be passed to font querying
functions.

The handle is deterministic and permanent, remaining valid for the lifetime of the program.

-INPUT-
cpp(strview) Family: The name of the font family to access.
cpp(strview) Style: The preferred style to choose from the family.  Use `Regular` or `NULL` for the default.
int Weight: Equivalent to CSS font-weight; a value of 400 or 0 will equate to normal.
int Size: The font-size, measured in pixels @ 72 DPI.
&ptr Handle: The resulting font handle is returned here.

-ERRORS-
Okay
Args
AccessObject
CreateObject
Failed
File
ResolvePath
Search
-END-

*********************************************************************************************************************/

ERR GetFontHandle(const std::string_view &Family, const std::string_view &Style, int Weight, int Size, APTR *Handle)
{
   kt::Log log(__FUNCTION__);

   if (Size < 1) return log.warning(ERR::Args);

   common_font *handle;
   if (auto error = get_font(log, Family, Style.empty() ? "Regular" : Style, Weight, Size, &handle); error IS ERR::Okay) {
      *Handle = handle;
      return ERR::Okay;
   }
   else return error;
}

/*********************************************************************************************************************

-FUNCTION-
GetFontMetrics: Returns display metrics for a font handle.

Call GetFontMetrics() to retrieve the font height, line spacing, ascent and descent values for a font handle.

-INPUT-
ptr Handle: A font handle obtained from ~GetFontHandle().
struct(*FontMetrics) Info: The font metrics for the `Handle` will be stored here.

-ERRORS-
Okay
NullArgs
-END-

*********************************************************************************************************************/

ERR GetFontMetrics(APTR Handle, struct FontMetrics *Metrics)
{
   if ((not Handle) or (not Metrics)) return ERR::NullArgs;

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
TracePath: Calls a function for each traced coordinate in a generated path.

Any generated path can be traced by calling this function.  Tracing allows the caller to follow the `Path`
point-by-point as it would be rendered with a stroke.  The `Callback` function signature is
`ERR Function(*Path, INT Index, INT Command, DOUBLE X, DOUBLE Y, *Meta)`.

The `Index` is an incrementing counter for each plotted point.  The `Command` value identifies the internal path
command, and `X`,`Y` are the traced coordinates.

If the `Callback` returns `ERR::Terminate`, processing stops and the function returns immediately.

-INPUT-
ptr Path:      The vector path to trace.
ptr(func) Callback: A function to call with the path coordinates.
double Scale:  Set to 1.0 (recommended) to trace the path at a scale of 1 to 1.

-ERRORS-
Okay
NullArgs
Function

*********************************************************************************************************************/

ERR TracePath(APTR Path, FUNCTION *Callback, double Scale)
{
   if ((not Path) or (not Callback)) return ERR::NullArgs;

   auto vector = (SimpleVector *)Path;
   auto &path = vector->mPath;
   path.rewind(0);
   path.approximation_scale(Scale);

   double x, y;
   int cmd = -1;
   int index = 0;

   if (Callback->isC()) {
      auto routine = ((ERR (*)(SimpleVector *, int, int, double, double, APTR))(Callback->Routine));

      kt::SwitchContext context(ParentContext());

      do {
         cmd = path.vertex(&x, &y);
         if (agg::is_vertex(cmd)) {
            if (routine(vector, index++, cmd, x, y, Callback->Meta) IS ERR::Terminate) {
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
         cmd = path.vertex(&x, &y);
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
LineTo: Appends a line segment to a generated path.

This function appends a straight line from the current point to `X`,`Y`.

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
MoveTo: Moves the current point in a generated path.

This function appends a move command.  The current point is moved to `X`,`Y` without drawing a line.

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
Multiply: Multiplies a matrix by explicit affine transform values.

This function multiplies the target !VectorMatrix by the supplied affine transform values.

-INPUT-
struct(*VectorMatrix) Matrix: The target transformation matrix.
double ScaleX: Matrix value A.
double ShearY: Matrix value B.
double ShearX: Matrix value C.
double ScaleY: Matrix value D.
double TranslateX: Matrix value E.
double TranslateY: Matrix value F.

-ERRORS-
Okay
NullArgs
-END-

*********************************************************************************************************************/

ERR Multiply(VectorMatrix *Matrix, double ScaleX, double ShearY, double ShearX, double ScaleY,
   double TranslateX, double TranslateY)
{
   if (not Matrix) return kt::Log(__FUNCTION__).warning(ERR::NullArgs);

   multiply_values(*Matrix, ScaleX, ShearY, ShearX, ScaleY, TranslateX, TranslateY);
   mark_matrix_dirty(Matrix);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
MultiplyMatrix: Multiplies a target matrix by a source matrix.

This function multiplies `Target` by `Source` and stores the result in `Target`.

-INPUT-
struct(*VectorMatrix) Target: The target transformation matrix.
struct(*VectorMatrix) Source: The source transformation matrix.

-ERRORS-
Okay
NullArgs
-END-

*********************************************************************************************************************/

ERR MultiplyMatrix(VectorMatrix *Target, VectorMatrix *Source)
{
   if ((not Target) or (not Source)) return kt::Log(__FUNCTION__).warning(ERR::NullArgs);

   multiply_matrix(*Target, *Source);
   mark_matrix_dirty(Target);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
ParseTransform: Parses an SVG transform string and applies it to a matrix.

This function parses a sequence of SVG transform instructions and applies them to a matrix.

The string must be written using SVG transform attribute syntax.  For example, `skewX(20) rotate(45 50 50)` is valid.
Transform instructions are applied in reverse order, as required by the SVG transform model.  Unsupported transform
text is skipped.

Any existing values in `Matrix` are reset before the parsed transforms are applied.  If existing matrix values need to
be retained, parse into a separate matrix and use ~MultiplyMatrix() to combine the result.

-INPUT-
struct(*VectorMatrix) Matrix: The target transformation matrix.
cpp(strview) Transform: The transform to apply, expressed as a string instruction.

-ERRORS-
Okay
NullArgs
-END-

*********************************************************************************************************************/

ERR ParseTransform(VectorMatrix *Matrix, const std::string_view &Transform)
{
   if (not Matrix) return kt::Log(__FUNCTION__).warning(ERR::NullArgs);

   enum { M_MUL, M_TRANSLATE, M_ROTATE, M_SCALE, M_SKEW };
   class cmd {
      public:
      int8_t type;
      double sx, sy, shx, shy, tx, ty;
      double angle;
      cmd(int8_t pType) : type(pType), sx(0), sy(0), shx(0), shy(0), tx(0), ty(0), angle(0) {};
   };

   std::vector<cmd> list;
   list.reserve(4);

   std::string_view str(Transform);
   while (not str.empty()) {
      if ((str.front() >= 'a') and (str.front() <= 'z')) {
         if (str.starts_with("matrix")) {
            cmd m(M_MUL);
            str.remove_prefix(6);
            read_numseq(str, { &m.sx, &m.shy, &m.shx, &m.sy, &m.tx, &m.ty });
            list.push_back(std::move(m));
         }
         else if (str.starts_with("translate")) {
            cmd m(M_TRANSLATE);
            str.remove_prefix(9);
            next_value(str);
            read_transform_unit(str, m.tx);
            next_value(str);
            read_transform_unit(str, m.ty);
            list.push_back(std::move(m));
         }
         else if (str.starts_with("rotate")) {
            cmd m(M_ROTATE);
            str.remove_prefix(6);
            read_numseq(str, { &m.angle, &m.tx, &m.ty });
            list.push_back(std::move(m));
         }
         else if (str.starts_with("scale")) {
            cmd m(M_SCALE);
            m.tx = 1.0;
            m.ty = DBL_EPSILON;
            str.remove_prefix(5);
            read_numseq(str, { &m.tx, &m.ty });
            if (m.ty IS DBL_EPSILON) m.ty = m.tx;
            list.push_back(std::move(m));
         }
         else if (str.starts_with("skewX")) {
            cmd m(M_SKEW);
            m.ty = 0;
            str.remove_prefix(5);
            read_numseq(str, { &m.tx });
            list.push_back(std::move(m));
         }
         else if (str.starts_with("skewY")) {
            cmd m(M_SKEW);
            m.tx = 0;
            str.remove_prefix(5);
            read_numseq(str, { &m.ty });
            list.push_back(std::move(m));
         }
         else str.remove_prefix(1);
      }
      else str.remove_prefix(1);
   }

   Matrix->ScaleX = 1.0;
   Matrix->ShearY = 0;
   Matrix->ShearX = 0;
   Matrix->ScaleY = 1.0;
   Matrix->TranslateX = 0;
   Matrix->TranslateY = 0;

   for (auto it = list.rbegin(); it != list.rend(); it++) {
      const auto &m = *it;
      switch (m.type) {
         case M_MUL:
            multiply_values(*Matrix, m.sx, m.shy, m.shx, m.sy, m.tx, m.ty);
            break;

         case M_TRANSLATE:
            Matrix->TranslateX += m.tx;
            Matrix->TranslateY += m.ty;
            break;

         case M_ROTATE:
            rotate_matrix(*Matrix, m.angle, m.tx, m.ty);
            break;

         case M_SCALE:
            scale_matrix(*Matrix, m.tx, m.ty);
            break;

         case M_SKEW:
            skew_matrix(*Matrix, m.tx, m.ty);
            break;
      }
   }

   mark_matrix_dirty(Matrix);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
ResetMatrix: Resets a transformation matrix to the identity transform.

Call ResetMatrix() to clear all scaling, skewing, rotation and translation from a !VectorMatrix.

-INPUT-
struct(*VectorMatrix) Matrix: The target transformation matrix.

-ERRORS-
Okay
NullArgs

*********************************************************************************************************************/

ERR ResetMatrix(VectorMatrix *Matrix)
{
   if (not Matrix) return kt::Log(__FUNCTION__).warning(ERR::NullArgs);

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
RewindPath: Resets path iteration to the first vertex.

Rewinding a path resets its internal vertex iterator to the first vertex.  The next call to ~GetVertex() will read from
the start of the path.

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
Rotate: Applies a rotation transform to a matrix.

This function rotates a matrix by `Angle` degrees.  Rotation occurs around `(0, 0)` unless `CenterX` and `CenterY`
specify another centre point.

-INPUT-
struct(*VectorMatrix) Matrix: The target transformation matrix.
double Angle: Angle of rotation, in degrees.
double CenterX: Centre of rotation on the horizontal axis.
double CenterY: Centre of rotation on the vertical axis.

-ERRORS-
Okay
NullArgs

*********************************************************************************************************************/

ERR Rotate(VectorMatrix *Matrix, double Angle, double CenterX, double CenterY)
{
   if (not Matrix) {
      kt::Log log(__FUNCTION__);
      return log.warning(ERR::NullArgs);
   }

   rotate_matrix(*Matrix, Angle, CenterX, CenterY);
   mark_matrix_dirty(Matrix);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
Scale: Applies a scale transform to a matrix.

This function scales a matrix on the x and y axes.  Values less than `1.0` shrink the affected vector path, while
values greater than `1.0` enlarge it.

Scaling is relative to position `(0, 0)`.  If the width and height of the vector path needs to be transformed without
affecting its top-left position, the client must translate the path to `(0, 0)` around its centre point.  The path
should then be scaled before being transformed back to its original top-left coordinate.

The scale operation can also be used to flip a vector path if negative values are used.  For instance, a value of
`-1.0` on the x axis results in a `1:1` horizontal flip.

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
   if (not Matrix) return kt::Log(__FUNCTION__).warning(ERR::NullArgs);

   scale_matrix(*Matrix, X, Y);
   mark_matrix_dirty(Matrix);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
Skew: Applies a skew transform to a matrix.

The Skew function applies a skew transformation to the horizontal and/or vertical axis of the matrix.  Valid `X` and
`Y` values are in the range `-90 < Angle < 90`.

If `X` is valid and `Y` is outside the valid range, the horizontal skew is applied before `ERR::OutOfRange` is
returned.

-INPUT-
struct(*VectorMatrix) Matrix: The target transformation matrix.
double X: The angle to skew along the horizontal.
double Y: The angle to skew along the vertical.

-ERRORS-
Okay
NullArgs
OutOfRange: At least one of the angles is out of the allowable range.
-END-

*********************************************************************************************************************/

ERR Skew(VectorMatrix *Matrix, double X, double Y)
{
   kt::Log log(__FUNCTION__);

   if (not Matrix) return log.warning(ERR::NullArgs);

   if (auto error = skew_matrix(*Matrix, X, Y); error IS ERR::Okay) {
      mark_matrix_dirty(Matrix);
      return ERR::Okay;
   }
   else {
      if ((X > -90) and (X < 90)) mark_matrix_dirty(Matrix);
      return log.warning(error);
   }
}

/*********************************************************************************************************************

-FUNCTION-
Smooth3: Appends a smooth quadratic Bezier curve to a generated path.

This function appends a quadratic Bezier segment from the current point to `X`,`Y`.  The control point is reflected
from the previous curve command.

-INPUT-
ptr Path: The vector path to modify.
double X: The horizontal end point for the smooth3 command.
double Y: The vertical end point for the smooth3 command.

*********************************************************************************************************************/

void Smooth3(APTR Vector, double X, double Y)
{
   if (not Vector) return;
   ((SimpleVector *)Vector)->mPath.curve3(X, Y);
}

/*********************************************************************************************************************

-FUNCTION-
Smooth4: Appends a smooth cubic Bezier curve to a generated path.

This function appends a cubic Bezier segment from the current point to `X`,`Y`.  The previous curve command supplies
the reflected first control point, and `CtrlX`,`CtrlY` supply the second control point.

-INPUT-
ptr Path: The vector path to modify.
double CtrlX: Control point horizontal coordinate.
double CtrlY: Control point vertical coordinate.
double X: The horizontal end point for the smooth4 instruction.
double Y: The vertical end point for the smooth4 instruction.

*********************************************************************************************************************/

void Smooth4(APTR Vector, double CtrlX, double CtrlY, double X, double Y)
{
   if (not Vector) return;
   ((SimpleVector *)Vector)->mPath.curve4(CtrlX, CtrlY, X, Y);
}

/*********************************************************************************************************************

-FUNCTION-
StringWidth: Calculates the rendered width of a UTF-8 string.

This function calculates the pixel width of `String` for a known font.  Line-feeds are treated as line breaks, so for
multi-line strings the width of the longest line is returned.

For scalable fonts, kerning is included when computing the distance between glyphs.

-INPUT-
ptr FontHandle: A font handle obtained from ~GetFontHandle().
cpp(strview) String: Pointer to a null-terminated string.
int Chars: Maximum number of bytes to process from `String`.  Set to `-1` for the full string.

-RESULT-
double: The pixel width of the string, or `0` if `FontHandle` or `String` is `NULL`.
-END-

*********************************************************************************************************************/

double StringWidth(APTR Handle, const std::string_view &String, int Chars)
{
   kt::Log log(__FUNCTION__);

   if ((not Handle) or (String.empty())) { log.warning(ERR::NullArgs); return 0; }

   const std::lock_guard lock(glFontMutex);

   if (((common_font *)Handle)->type IS CF_FREETYPE) {
      auto pt = (freetype_font::ft_point *)Handle;
      FT_Activate_Size(pt->ft_size);

      if (Chars IS -1) Chars = 0x7fffffff;

      double len     = 0;
      double widest  = 0;
      int prev_glyph = 0;
      int i = 0;
      const bool has_kerning = FT_HAS_KERNING(pt->ft_size->face);
      const size_t end = std::min(size_t(Chars), String.size());
      const auto limited_string = String.substr(0, end);
      while (i < std::ssize(limited_string)) {
         if (limited_string[i] IS '\n') {
            if (widest < len) widest = len;
            len = 0;
            i++;
         }
         else {
            uint32_t unicode;
            auto charlen = get_utf8(limited_string, unicode, i);
            auto &glyph  = pt->get_glyph(unicode);
            len += glyph.adv_x;
            if ((has_kerning) and (prev_glyph)) {
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
Translate: Applies a translation transform to a matrix.

This function adds the supplied x and y offsets to the matrix translation values.

-INPUT-
struct(*VectorMatrix) Matrix: The target transformation matrix.
double X: Translation along the x-axis.
double Y: Translation along the y-axis.

-ERRORS-
Okay
NullArgs
-END-

*********************************************************************************************************************/

ERR Translate(VectorMatrix *Matrix, double X, double Y)
{
   if (not Matrix) {
      kt::Log log(__FUNCTION__);
      return log.warning(ERR::NullArgs);
   }

   Matrix->TranslateX += X;
   Matrix->TranslateY += Y;

   if (Matrix->Vector) mark_dirty(Matrix->Vector, RC::TRANSFORM);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
TranslatePath: Translates every vertex in a generated path.

This function offsets all vertices in `Path` by `X`,`Y`.  If `Path` is `NULL`, the call is ignored.

-INPUT-
ptr Path: Pointer to a generated path.
double X: Horizontal offset.
double Y: Vertical offset.

-END-

*********************************************************************************************************************/

void TranslatePath(APTR Vector, double X, double Y)
{
   if (not Vector) return;
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

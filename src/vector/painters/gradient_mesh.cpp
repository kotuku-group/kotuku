/*********************************************************************************************************************

-CLASS-
GradientMesh: Bilinear Coons patch mesh gradient paint server.

GradientMesh interpolates colour across a grid of curved patches.  Each patch is bounded by four cubic Bezier edges
and carries one colour at each corner.  Rendering tessellates the patches into Gouraud triangles, so colour follows
the patch geometry rather than a one-dimensional colour ramp.

The inherited colour-ramp fields `Stops`, `ColourMap`, `SpreadMethod` and `Colour` are not supported by this class.
Patch geometry and corner colours are supplied through #Patches.

-END-

TODO:
- Add tensor-product bicubic mesh patches, including the four internal control points from the SVG proposal.
- Replace the current uniform tessellation with adaptive subdivision driven by patch flatness and screen size.
- Add a richer structured authoring API once mesh patch editing needs per-edge or per-corner mutation without
  replacing the full #Patches array.
- Extend SVG conformance tests to cover multi-row shared top-edge inheritance, malformed patch path recovery and
  colour-space interpolation through `linearRGB`.

*********************************************************************************************************************/

static void invalidate_mesh(extGradientMesh *Self) noexcept
{
   Self->MeshTriangles.Valid = false;
   if (Self->initialised()) Self->modified();
}

static ERR validate_mesh_dimensions(extGradientMesh *Self, int Rows, int Columns) noexcept
{
   if ((Rows < 0) or (Columns < 0)) return kt::Log().warning(ERR::InvalidValue);

   if ((Self->Mesh) and (not Self->Mesh->patches.empty()) and (Rows > 0) and (Columns > 0)) {
      if ((Rows * Columns) != int(Self->Mesh->patches.size())) return kt::Log().warning(ERR::InvalidValue);
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Drop fields that are irrelevant to mesh gradients.

static ERR GRADIENTMESH_SET_Colour(extGradientMesh *, const std::span<const float> &)
{
   return kt::Log().warning(ERR::UnsupportedField);
}

static ERR GRADIENTMESH_SET_ColourMap(extGradientMesh *, const std::string_view &)
{
   return kt::Log().warning(ERR::UnsupportedField);
}

static ERR GRADIENTMESH_SET_SpreadMethod(extGradientMesh *, VSPREAD)
{
   return kt::Log().warning(ERR::UnsupportedField);
}

static ERR GRADIENTMESH_SET_Stops(extGradientMesh *, const std::span<const GradientStop> &)
{
   return kt::Log().warning(ERR::UnsupportedField);
}

//********************************************************************************************************************

static MeshPatchRecord mesh_patch_to_record(const MeshPatch &Patch)
{
   MeshPatchRecord record = {};

   record.Top.StartX = Patch.edge[0].p0.x;    record.Top.StartY = Patch.edge[0].p0.y;
   record.Top.X1 = Patch.edge[0].c0.x;        record.Top.Y1 = Patch.edge[0].c0.y;
   record.Top.X2 = Patch.edge[0].c1.x;        record.Top.Y2 = Patch.edge[0].c1.y;
   record.Top.EndX = Patch.edge[0].p1.x;      record.Top.EndY = Patch.edge[0].p1.y;
   record.Right.StartX = Patch.edge[1].p0.x;  record.Right.StartY = Patch.edge[1].p0.y;
   record.Right.X1 = Patch.edge[1].c0.x;      record.Right.Y1 = Patch.edge[1].c0.y;
   record.Right.X2 = Patch.edge[1].c1.x;      record.Right.Y2 = Patch.edge[1].c1.y;
   record.Right.EndX = Patch.edge[1].p1.x;    record.Right.EndY = Patch.edge[1].p1.y;
   record.Bottom.StartX = Patch.edge[2].p0.x; record.Bottom.StartY = Patch.edge[2].p0.y;
   record.Bottom.X1 = Patch.edge[2].c0.x;     record.Bottom.Y1 = Patch.edge[2].c0.y;
   record.Bottom.X2 = Patch.edge[2].c1.x;     record.Bottom.Y2 = Patch.edge[2].c1.y;
   record.Bottom.EndX = Patch.edge[2].p1.x;   record.Bottom.EndY = Patch.edge[2].p1.y;
   record.Left.StartX = Patch.edge[3].p0.x;   record.Left.StartY = Patch.edge[3].p0.y;
   record.Left.X1 = Patch.edge[3].c0.x;       record.Left.Y1 = Patch.edge[3].c0.y;
   record.Left.X2 = Patch.edge[3].c1.x;       record.Left.Y2 = Patch.edge[3].c1.y;
   record.Left.EndX = Patch.edge[3].p1.x;     record.Left.EndY = Patch.edge[3].p1.y;

   record.TopLeft     = Patch.corner[0];
   record.TopRight    = Patch.corner[1];
   record.BottomRight = Patch.corner[2];
   record.BottomLeft  = Patch.corner[3];

   return record;
}

static MeshPatch record_to_mesh_patch(const MeshPatchRecord &Record)
{
   MeshPatch patch = {};

   patch.edge[0] = {
      { Record.Top.StartX, Record.Top.StartY }, { Record.Top.X1, Record.Top.Y1 },
      { Record.Top.X2, Record.Top.Y2 }, { Record.Top.EndX, Record.Top.EndY }
   };
   patch.edge[1] = {
      { Record.Right.StartX, Record.Right.StartY }, { Record.Right.X1, Record.Right.Y1 },
      { Record.Right.X2, Record.Right.Y2 }, { Record.Right.EndX, Record.Right.EndY }
   };
   patch.edge[2] = {
      { Record.Bottom.StartX, Record.Bottom.StartY }, { Record.Bottom.X1, Record.Bottom.Y1 },
      { Record.Bottom.X2, Record.Bottom.Y2 }, { Record.Bottom.EndX, Record.Bottom.EndY }
   };
   patch.edge[3] = {
      { Record.Left.StartX, Record.Left.StartY }, { Record.Left.X1, Record.Left.Y1 },
      { Record.Left.X2, Record.Left.Y2 }, { Record.Left.EndX, Record.Left.EndY }
   };

   patch.corner[0] = Record.TopLeft;
   patch.corner[1] = Record.TopRight;
   patch.corner[2] = Record.BottomRight;
   patch.corner[3] = Record.BottomLeft;

   return patch;
}

/*********************************************************************************************************************

-FIELD-
Patches: Defines the Coons patches for a mesh gradient.

The Patches array defines one or more bilinear Coons patches.  Each !MeshPatchRecord contains four cubic Bezier
edges in clockwise order (`Top`, `Right`, `Bottom`, `Left`) plus the top-left, top-right, bottom-right and bottom-left
corner colours.

The coordinate system for patch positions is determined by @Gradient.Units.  Under `BOUNDING_BOX`, normalised patch
coordinates in the range `0 - 1.0` are scaled into the target path's bounds.  Under `USERSPACE`, positions are taken
directly in the viewport's coordinate system.

-END-
*********************************************************************************************************************/

static ERR GRADIENTMESH_GET_Patches(extGradientMesh *Self, std::span<MeshPatchRecord> &Array)
{
   thread_local std::vector<MeshPatchRecord> records;
   records.clear();

   if (Self->Mesh) {
      records.reserve(Self->Mesh->patches.size());
      for (auto &patch : Self->Mesh->patches) records.push_back(mesh_patch_to_record(patch));
   }

   Array = std::span<MeshPatchRecord>(records.data(), records.size());
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Rows: The number of rows represented by the flat Patches array.

Rows stores the row count for the mesh grid while #Patches remains a flat row-major array.  When valid row and column
metadata is supplied, SVG export can preserve standard meshgradient shared-edge syntax across multiple rows.  If no
metadata is supplied, assigning #Patches defaults to a single-row mesh for simple programmatic use.

-END-
*********************************************************************************************************************/

static ERR GRADIENTMESH_GET_Rows(extGradientMesh *Self, int &Value)
{
   Value = (Self->Mesh) ? Self->Mesh->rows : 0;
   return ERR::Okay;
}

static ERR GRADIENTMESH_SET_Rows(extGradientMesh *Self, int Value)
{
   if (Value <= 0) return kt::Log().warning(ERR::InvalidValue);

   if (not Self->Mesh) Self->Mesh = std::make_unique<MeshGradient>();

   int columns = Self->Mesh->cols;
   if (not Self->Mesh->patches.empty()) {
      if ((int(Self->Mesh->patches.size()) % Value) != 0) return kt::Log().warning(ERR::InvalidValue);
      columns = int(Self->Mesh->patches.size()) / Value;
   }

   if (auto error = validate_mesh_dimensions(Self, Value, columns); error != ERR::Okay) return error;

   Self->Mesh->rows = Value;
   Self->Mesh->cols = columns;
   invalidate_mesh(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Columns: The number of columns represented by the flat Patches array.

Columns stores the column count for the mesh grid while #Patches remains a flat row-major array.  The row and column
counts must multiply to the number of patch records when both dimensions and patches are present.

-END-
*********************************************************************************************************************/

static ERR GRADIENTMESH_GET_Columns(extGradientMesh *Self, int &Value)
{
   Value = (Self->Mesh) ? Self->Mesh->cols : 0;
   return ERR::Okay;
}

static ERR GRADIENTMESH_SET_Columns(extGradientMesh *Self, int Value)
{
   if (Value <= 0) return kt::Log().warning(ERR::InvalidValue);

   if (not Self->Mesh) Self->Mesh = std::make_unique<MeshGradient>();

   int rows = Self->Mesh->rows;
   if (not Self->Mesh->patches.empty()) {
      if ((int(Self->Mesh->patches.size()) % Value) != 0) return kt::Log().warning(ERR::InvalidValue);
      rows = int(Self->Mesh->patches.size()) / Value;
   }

   if (auto error = validate_mesh_dimensions(Self, rows, Value); error != ERR::Okay) return error;

   Self->Mesh->rows = rows;
   Self->Mesh->cols = Value;
   invalidate_mesh(Self);
   return ERR::Okay;
}

static ERR GRADIENTMESH_SET_Patches(extGradientMesh *Self, std::span<const MeshPatchRecord> &Array)
{
   if ((not Array.data()) or (Array.empty())) return kt::Log().warning(ERR::InvalidValue);

   if (not Self->Mesh) Self->Mesh = std::make_unique<MeshGradient>();

   int rows = Self->Mesh->rows;
   int columns = Self->Mesh->cols;
   const int patch_count = int(Array.size());

   if ((rows > 0) and (columns > 0) and ((rows * columns) IS patch_count)) {
      // Existing row metadata remains valid.
   }
   else if ((rows > 0) and ((patch_count % rows) IS 0)) {
      columns = patch_count / rows;
   }
   else if ((columns > 0) and ((patch_count % columns) IS 0)) {
      rows = patch_count / columns;
   }
   else {
      rows = 1;
      columns = patch_count;
   }

   Self->Mesh->rows = rows;
   Self->Mesh->cols = columns;
   Self->Mesh->bicubic = false;
   Self->Mesh->patches.clear();
   Self->Mesh->patches.reserve(Array.size());
   for (auto &record : Array) Self->Mesh->patches.push_back(record_to_mesh_patch(record));

   invalidate_mesh(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the effect.
-END-
*********************************************************************************************************************/

static ERR GRADIENTMESH_GET_XMLDef(extGradientMesh *, std::string_view &Value)
{
   return gradient_xml_result("meshgradient", Value);
}

//********************************************************************************************************************

#include "gradient_mesh_def.cpp"

static const FieldArray clGradientMeshFields[] = {
   // Mark these fields as unsupported
   { "Colour",       FDF_SYSTEM|FDF_VIRTUAL|FD_FLOAT|FDF_ARRAY|FD_RW|FDF_PURE, nullptr, GRADIENTMESH_SET_Colour },
   { "ColourMap",    FDF_SYSTEM|FDF_VIRTUAL|FDF_CPPSTRING|FDF_W|FDF_PURE, nullptr, GRADIENTMESH_SET_ColourMap },
   { "SpreadMethod", FDF_SYSTEM|FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, GRADIENTMESH_SET_SpreadMethod, &clGradientSpreadMethod },
   { "Stops",        FDF_SYSTEM|FDF_VIRTUAL|FDF_ARRAY|FDF_STRUCT|FDF_RW|FDF_PURE, nullptr, GRADIENTMESH_SET_Stops, "GradientStop" },
   // Mesh fields
   { "Rows",         FDF_VIRTUAL|FDF_INT|FDF_RW|FDF_PURE, GRADIENTMESH_GET_Rows, GRADIENTMESH_SET_Rows },
   { "Columns",      FDF_VIRTUAL|FDF_INT|FDF_RW|FDF_PURE, GRADIENTMESH_GET_Columns, GRADIENTMESH_SET_Columns },
   { "Patches",      FDF_VIRTUAL|FDF_VECTOR|FDF_STRUCT|FDF_RW|FDF_PURE, GRADIENTMESH_GET_Patches, GRADIENTMESH_SET_Patches, "MeshPatchRecord" },
   { "XMLDef",       FDF_VIRTUAL|FDF_CPPSTRING|FDF_ALLOC|FDF_R, GRADIENTMESH_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_gradient_mesh(void)
{
   clGradientMesh = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::GRADIENT),
      fl::ClassID(CLASSID::GRADIENTMESH),
      fl::Name("GradientMesh"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clGradientMeshActions),
      fl::Fields(clGradientMeshFields),
      fl::Size(sizeof(extGradientMesh)),
      fl::Path(MOD_PATH));

   return clGradientMesh ? ERR::Okay : ERR::AddClass;
}

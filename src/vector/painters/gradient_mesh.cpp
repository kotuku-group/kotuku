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
- Preserve mesh row and column metadata through the public authoring API so SVG round-tripping can retain compact
  shared-edge syntax for multi-row meshes.
- Add a richer structured authoring API once mesh patch editing needs per-edge or per-corner mutation without
  replacing the full #Patches array.
- Extend SVG conformance tests to cover multi-row shared top-edge inheritance, malformed patch path recovery and
  colour-space interpolation through `linearRGB`.

*********************************************************************************************************************/

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

   record.TopP0X = Patch.edge[0].p0.x;       record.TopP0Y = Patch.edge[0].p0.y;
   record.TopC0X = Patch.edge[0].c0.x;       record.TopC0Y = Patch.edge[0].c0.y;
   record.TopC1X = Patch.edge[0].c1.x;       record.TopC1Y = Patch.edge[0].c1.y;
   record.TopP1X = Patch.edge[0].p1.x;       record.TopP1Y = Patch.edge[0].p1.y;
   record.RightP0X = Patch.edge[1].p0.x;     record.RightP0Y = Patch.edge[1].p0.y;
   record.RightC0X = Patch.edge[1].c0.x;     record.RightC0Y = Patch.edge[1].c0.y;
   record.RightC1X = Patch.edge[1].c1.x;     record.RightC1Y = Patch.edge[1].c1.y;
   record.RightP1X = Patch.edge[1].p1.x;     record.RightP1Y = Patch.edge[1].p1.y;
   record.BottomP0X = Patch.edge[2].p0.x;    record.BottomP0Y = Patch.edge[2].p0.y;
   record.BottomC0X = Patch.edge[2].c0.x;    record.BottomC0Y = Patch.edge[2].c0.y;
   record.BottomC1X = Patch.edge[2].c1.x;    record.BottomC1Y = Patch.edge[2].c1.y;
   record.BottomP1X = Patch.edge[2].p1.x;    record.BottomP1Y = Patch.edge[2].p1.y;
   record.LeftP0X = Patch.edge[3].p0.x;      record.LeftP0Y = Patch.edge[3].p0.y;
   record.LeftC0X = Patch.edge[3].c0.x;      record.LeftC0Y = Patch.edge[3].c0.y;
   record.LeftC1X = Patch.edge[3].c1.x;      record.LeftC1Y = Patch.edge[3].c1.y;
   record.LeftP1X = Patch.edge[3].p1.x;      record.LeftP1Y = Patch.edge[3].p1.y;

   record.TopLeft = Patch.corner[0];
   record.TopRight = Patch.corner[1];
   record.BottomRight = Patch.corner[2];
   record.BottomLeft = Patch.corner[3];

   return record;
}

static MeshPatch record_to_mesh_patch(const MeshPatchRecord &Record)
{
   MeshPatch patch = {};

   patch.edge[0] = {
      { Record.TopP0X, Record.TopP0Y }, { Record.TopC0X, Record.TopC0Y },
      { Record.TopC1X, Record.TopC1Y }, { Record.TopP1X, Record.TopP1Y }
   };
   patch.edge[1] = {
      { Record.RightP0X, Record.RightP0Y }, { Record.RightC0X, Record.RightC0Y },
      { Record.RightC1X, Record.RightC1Y }, { Record.RightP1X, Record.RightP1Y }
   };
   patch.edge[2] = {
      { Record.BottomP0X, Record.BottomP0Y }, { Record.BottomC0X, Record.BottomC0Y },
      { Record.BottomC1X, Record.BottomC1Y }, { Record.BottomP1X, Record.BottomP1Y }
   };
   patch.edge[3] = {
      { Record.LeftP0X, Record.LeftP0Y }, { Record.LeftC0X, Record.LeftC0Y },
      { Record.LeftC1X, Record.LeftC1Y }, { Record.LeftP1X, Record.LeftP1Y }
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

static ERR GRADIENTMESH_SET_Patches(extGradientMesh *Self, std::span<const MeshPatchRecord> &Array)
{
   if ((not Array.data()) or (Array.empty())) return kt::Log().warning(ERR::InvalidValue);

   if (not Self->Mesh) Self->Mesh = std::make_unique<MeshGradient>();

   Self->Mesh->rows = 1;
   Self->Mesh->cols = int(Array.size());
   Self->Mesh->bicubic = false;
   Self->Mesh->patches.clear();
   Self->Mesh->patches.reserve(Array.size());
   for (auto &record : Array) Self->Mesh->patches.push_back(record_to_mesh_patch(record));

   Self->MeshTriangles.Valid = false;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

static ERR GRADIENTMESH_GET_XMLDef(extGradientMesh *, std::string_view &Value)
{
   return gradient_xml_result("meshgradient", Value);
}

//********************************************************************************************************************

#include "gradient_mesh_def.cpp"

static const FieldArray clGradientMeshFields[] = {
   { "Colour",       FDF_SYSTEM|FDF_VIRTUAL|FD_FLOAT|FDF_ARRAY|FD_RW|FDF_PURE, nullptr, GRADIENTMESH_SET_Colour },
   { "ColourMap",    FDF_SYSTEM|FDF_VIRTUAL|FDF_CPPSTRING|FDF_W|FDF_PURE, nullptr, GRADIENTMESH_SET_ColourMap },
   { "SpreadMethod", FDF_SYSTEM|FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, GRADIENTMESH_SET_SpreadMethod, &clGradientSpreadMethod },
   { "Stops",        FDF_SYSTEM|FDF_VIRTUAL|FDF_ARRAY|FDF_STRUCT|FDF_RW|FDF_PURE, nullptr, GRADIENTMESH_SET_Stops, "GradientStop" },
   { "Patches",      FDF_VIRTUAL|FDF_VECTOR|FDF_STRUCT|FDF_RW|FDF_PURE, GRADIENTMESH_GET_Patches,
      GRADIENTMESH_SET_Patches, "MeshPatchRecord" },
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

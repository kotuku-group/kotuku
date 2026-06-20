/*********************************************************************************************************************

-CLASS-
GradientGouraud: Gouraud mesh colour gradient paint server.

GradientGouraud interpolates colour barycentrically across a caller-supplied mesh of coloured triangles.  The mesh is
defined by #Vertices and optional #Indices.

The inherited colour-ramp fields `Stops`, `ColourMap`, `SpreadMethod` and `Colour` are not supported by this class.
Per-vertex colour values are supplied directly through #Vertices.

-END-

*********************************************************************************************************************/

static ERR GRADIENTGOURAUD_SET_Colour(extGradientGouraud *, const std::span<const float> &)
{
   return kt::Log().warning(ERR::UnsupportedField);
}

static ERR GRADIENTGOURAUD_SET_ColourMap(extGradientGouraud *, const std::string_view &)
{
   return kt::Log().warning(ERR::UnsupportedField);
}

static ERR GRADIENTGOURAUD_SET_SpreadMethod(extGradientGouraud *, VSPREAD)
{
   return kt::Log().warning(ERR::UnsupportedField);
}

static ERR GRADIENTGOURAUD_SET_Stops(extGradientGouraud *, const std::span<const GradientStop> &)
{
   return kt::Log().warning(ERR::UnsupportedField);
}

/*********************************************************************************************************************

-FIELD-
Indices: Defines triangle connectivity for a Gouraud gradient.

The Indices array provides triangle connectivity for a Gouraud gradient's #Vertices, with three indices per triangle
referencing vertices in counter-clockwise order.  Supplying indices allows vertices to be shared between adjacent
triangles.

If left empty, the #Vertices array is treated as a flat triangle list, where every three consecutive vertices form
one triangle.

*********************************************************************************************************************/

static ERR GRADIENTGOURAUD_GET_Indices(extGradientGouraud *Self, std::span<int> &Array)
{
   if (Self->Gouraud) Array = std::span<int>(Self->Gouraud->Indices.data(), Self->Gouraud->Indices.size());
   else Array = std::span<int>{};
   return ERR::Okay;
}

static ERR GRADIENTGOURAUD_SET_Indices(extGradientGouraud *Self, std::span<const int> &Array)
{
   if (not Self->Gouraud) Self->Gouraud = std::make_unique<GouraudMesh>();

   const auto elements = Array.size();
   if ((not Array.data()) or (elements <= 0)) Self->Gouraud->Indices.clear();
   else if ((elements % 3) != 0) return kt::Log().warning(ERR::InvalidValue);
   else {
      const auto total_vertices = Self->Gouraud->Vertices.size();
      for (int i=0; i < elements; i++) {
         if (Array[i] < 0) {
            kt::Log().warning("Gouraud index %d at position %d is negative.", Array[i], i);
            return ERR::OutOfRange;
         }
         if ((total_vertices > 0) and (size_t(Array[i]) >= total_vertices)) {
            kt::Log().warning("Gouraud index %d at position %d is outside the vertex range 0 - %d.",
               Array[i], i, int(total_vertices) - 1);
            return ERR::OutOfRange;
         }
      }

      Self->Gouraud->Indices.assign(Array.begin(), Array.end());
   }

   Self->GouraudTriangles.Valid = false;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Vertices: Defines the coloured vertices for a Gouraud gradient.

The Vertices array defines the mesh of coloured points across which colour is interpolated barycentrically.  Each
!GouraudVertex carries an `(X, Y)` position and an `FRGB` colour.

By default the vertices are treated as a flat triangle list, where every three consecutive vertices form one
triangle.  Supplying an #Indices array instead expresses triangle connectivity explicitly.

The coordinate system for vertex positions is determined by @Gradient.Units.  Under `BOUNDING_BOX`, a normalised mesh in the
range `0 - 1.0` is scaled into the target path's bounds.  Under `USERSPACE`, the positions are taken directly in the
viewport's coordinate space.

-END-
*********************************************************************************************************************/

static ERR GRADIENTGOURAUD_GET_Vertices(extGradientGouraud *Self, std::span<GouraudVertex> &Array)
{
   if (Self->Gouraud) {
      Array = std::span<GouraudVertex>(Self->Gouraud->Vertices.data(), Self->Gouraud->Vertices.size());
   }
   else Array = std::span<GouraudVertex>{};
   return ERR::Okay;
}

static ERR GRADIENTGOURAUD_SET_Vertices(extGradientGouraud *Self, std::span<const GouraudVertex> &Array)
{
   const int elements = Array.size();
   if ((not Array.data()) or (elements < 3)) return kt::Log().warning(ERR::InvalidValue);

   if (not Self->Gouraud) Self->Gouraud = std::make_unique<GouraudMesh>();

   if (not Self->Gouraud->Indices.empty()) {
      for (auto idx : Self->Gouraud->Indices) {
         if ((idx < 0) or (idx >= elements)) {
            kt::Log().warning("Gouraud index %d is outside the new vertex range 0 - %d.", idx, elements - 1);
            return ERR::OutOfRange;
         }
      }
   }

   Self->Gouraud->Vertices.assign(Array.begin(), Array.end());
   Self->GouraudTriangles.Valid = false;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "gradient_gouraud_def.cpp"

static const FieldArray clGradientGouraudFields[] = {
   { "Colour",       FDF_SYSTEM|FDF_VIRTUAL|FD_FLOAT|FDF_ARRAY|FD_RW|FDF_PURE, nullptr, GRADIENTGOURAUD_SET_Colour },
   { "ColourMap",    FDF_SYSTEM|FDF_VIRTUAL|FDF_CPPSTRING|FDF_W|FDF_PURE, nullptr, GRADIENTGOURAUD_SET_ColourMap },
   { "SpreadMethod", FDF_SYSTEM|FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, GRADIENTGOURAUD_SET_SpreadMethod, &clGradientSpreadMethod },
   { "Stops",        FDF_SYSTEM|FDF_VIRTUAL|FDF_ARRAY|FDF_STRUCT|FDF_RW|FDF_PURE, nullptr, GRADIENTGOURAUD_SET_Stops, "GradientStop" },
   { "Vertices",     FDF_VIRTUAL|FDF_VECTOR|FDF_STRUCT|FDF_RW|FDF_PURE, GRADIENTGOURAUD_GET_Vertices, GRADIENTGOURAUD_SET_Vertices, "GouraudVertex" },
   { "Indices",      FDF_VIRTUAL|FDF_VECTOR|FDF_INT|FDF_RW|FDF_PURE, GRADIENTGOURAUD_GET_Indices, GRADIENTGOURAUD_SET_Indices },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_gradient_gouraud(void)
{
   clGradientGouraud = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::GRADIENT),
      fl::ClassID(CLASSID::GRADIENTGOURAUD),
      fl::Name("GradientGouraud"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clGradientGouraudActions),
      fl::Fields(clGradientGouraudFields),
      fl::Size(sizeof(extGradientGouraud)),
      fl::Path(MOD_PATH));

   return clGradientGouraud ? ERR::Okay : ERR::AddClass;
}

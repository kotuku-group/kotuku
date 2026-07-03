# Implementation Plan — SVG 2 Mesh Gradients

Status: Row-aware SVG round-trip implemented; remaining work tracked below
Date: 2026-06-13 (revised 2026-06-15 for the refactored Gradient class hierarchy; updated 2026-07-02
for implementation status; updated 2026-07-03 for row-aware round-tripping)
Related: [vector_gradients.md](vector_gradients.md), [sdf_gradients.md](sdf_gradients.md)

## Current Implementation Status

The core bilinear mesh-gradient path is now present:

- `GradientMesh` is registered as a first-class `Gradient` subclass, with generated `CLASSID::GRADIENTMESH`
  support and a `MeshPatchRecord` authoring structure.
- `src/vector/painters/gradient_mesh.cpp` suppresses inherited one-dimensional ramp fields and exposes a
  `Patches` field for replacing the full bilinear patch list.
- `src/vector/scene/scene_fill.cpp` contains the shared Gouraud triangle mask/render core, Coons patch evaluation,
  uniform tessellation, mesh cache fingerprinting, and `fill_mesh()` dispatch.
- SVG parsing recognises `<meshgradient>`, reconstructs basic shared edges, creates a `GradientMesh`, and feeds the
  `Patches`, `Rows` and `Columns` fields.
- SVG saving emits row-aware `<meshgradient>` definitions.  Standard grids use compact inherited top/left edge syntax;
  independent patch metadata is only emitted when a patch cannot be represented by the standard inheritance rules.
- Flute coverage exists for programmatic `GradientMesh` creation/rendering, row metadata, an SVG hash fixture, and
  parse/save/reparse mesh equivalence.

The implementation is therefore past the original scaffold/render/parser milestones, but it is not yet a complete
SVG 2 mesh-gradient implementation.  The remaining work is mostly cache/render conformance and deeper tests.

> **Design note (2026-06-15):** This plan predates two changes that materially affect it:
>
> 1. **The monolithic `VectorGradient` class was split** into a thin `Gradient` base plus one
>    registered subclass per gradient kind (`GradientLinear`, `GradientRadial`, `GradientConic`,
>    `GradientDiamond`, `GradientContour`, `GradientGouraud`, `GradientDistal`).  Each subclass is its
>    own registered class (`CLASSID::GRADIENT*`, `base='Gradient'`), owns its shape-specific fields and
>    caches, and rendering dispatches on `Gradient.classID()` rather than a `VGT` type enum.  The mesh
>    gradient is therefore a new `GradientMesh` subclass, not a `VGT::MESH` branch.
> 2. **A Gouraud mesh gradient now exists** (`GradientGouraud`, `src/vector/painters/gradient_gouraud.cpp`).
>    It already implements precisely the render strategy this plan proposes — interpolate colour
>    barycentrically across a triangle mesh via `agg::span_gouraud_rgba`, with the inherited 1D colour-ramp
>    fields suppressed and a cached transformed-triangle list.  The mesh gradient should be built as a
>    sibling of `GradientGouraud` and **reuse its triangle pipeline**: tessellate each Coons patch into
>    coloured triangles and feed them through the same Gouraud span machinery.  This plan has been updated
>    to model the new class on `GradientGouraud` throughout.

## Goal

Support mesh gradients compatible with the SVG 2 `<meshgradient>` proposal (the same model carried
over from SVG 1.2 / used by PDF type 6 & 7 shadings).  A mesh gradient fills a region with a grid
of **Coons / tensor-product patches**: each patch is bounded by up to four cubic Bézier edges with
a colour assigned at each corner, and colour is bilinearly interpolated (in patch parameter space)
across the curved patch interior.  This bends the gradient with the geometry — the showpiece
capability that field-based gradients cannot express.

## Why This Is Architecturally Different

The field-based gradients (LINEAR, RADIAL, CONIC, DIAMOND, CONTOUR, and the proposed DISTAL/Voronoi)
share one model: the gradient carries a list of `GradientStop`s baked into a 256-entry
`GRADIENT_TABLE`, and rendering is a per-pixel `calculate(x, y)` span function that maps a scalar
field through that table.

**Mesh gradients do not fit that model:**
- There is no 1D colour ramp — colour is defined at 2D patch corners, not at stops along a line.
- There is no closed-form `calculate(x, y)` (point-in-which-patch + inverse Bézier mapping is not
  cheaply invertible).
- The natural rendering strategy is **forward tessellation**: subdivide each patch into small
  triangles/quads and render them with AGG's `span_gouraud_rgba` (barycentric colour interpolation),
  at `src/vector/agg/include/agg_span_gouraud_rgba.h`.

**The `GradientGouraud` class already established this exact parallel render path.**  It is *not* a
branch in the `calculate()`-style dispatch — it is a dedicated `fill_gouraud(...)` routine invoked at
the top of `render_fill` when `gradient.classID() IS CLASSID::GRADIENTGOURAUD`
(`src/vector/scene/scene_fill.cpp` ~line 55), running before the `get_fill_gradient_table` ramp path.
It rasterises the target fill into a greyscale alpha mask, then renders coloured triangles through it
with `span_gouraud_rgba`, caching the transformed/coloured triangle list on the gradient
(`GouraudCache GouraudTriangles`).

So mesh support is a sibling of `GradientGouraud`: a new `GradientMesh` subclass with its own patch
data, whose renderer **tessellates Coons patches down to the same `GouraudTriangle` representation**
and hands them to the existing Gouraud span machinery.  It reuses the `Gradient` object shell
(naming, scene registration, `url(#...)` referencing, transforms, units) and, ideally, much of the
Gouraud triangle-cache and mask-render plumbing.

## SVG 2 Data Model (what we must represent)

```
<meshgradient id="m" x="0" y="0" gradientUnits="userSpaceOnUse">
  <meshrow>
    <meshpatch>
      <stop path="C ..." stop-color="..."/>   <!-- up to 4 stops; edges 1..4 -->
      <stop path="C ..." stop-color="..."/>
      <stop path="C ..." stop-color="..."/>
      <stop path="C ..." stop-color="..."/>
    </meshpatch>
    <meshpatch> ... </meshpatch>
  </meshrow>
  <meshrow> ... </meshrow>
</meshgradient>
```

Key SVG 2 rules to honour:
- A patch has 4 edges (each a cubic Bézier: 2 control points per edge in the `stop`'s `path`
  attribute, using `C`/`c`/`L`/`l` commands).
- **Shared edges are implicit:** patches after the first in a row reuse the previous patch's right
  edge as their left edge; rows after the first reuse the row above's bottom edges as their top
  edges.  So only the *new* edges are specified (3 for the first patch of a non-first row/column,
  2 for interior patches) — the parser must reconstruct shared geometry and shared corner colours.
- Corner colours come from the `stop-color` of the stop that *introduces* that corner; shared
  corners inherit.
- `type="bilinear"` (Coons) or `type="bicubic"` (tensor-product, adds 4 internal control points).
  Start with bilinear (Coons); bicubic is an extension.
- `gradientUnits`, `gradientTransform`, and `href` inheritance behave as for other gradients.

## Design

### 1. New patch data structures (`src/vector/vector.h`, near `GouraudMesh`)

Place the mesh structs alongside the existing `GouraudMesh`/`GouraudTriangle`/`GouraudCache`
definitions in `src/vector/vector.h` (~line 336), since the renderer will lower mesh patches into the
same `GouraudTriangle` list:

```cpp
struct MeshPatchEdge { agg::point_d p0, c0, c1, p1; }; // cubic Bézier (p0..p1, controls c0,c1)
struct MeshPatch {
   MeshPatchEdge edge[4];   // top, right, bottom, left (clockwise per SVG 2)
   FRGB corner[4];          // colour at each corner
   // optional: agg::point_d internal[4] for tensor-product (bicubic) patches
};
struct MeshGradient {
   int rows, cols;
   std::vector<MeshPatch> patches;   // row-major
   bool bicubic = false;
};
```

These hang off the new `extGradientMesh` subclass (see §2) as a `std::unique_ptr<MeshGradient> Mesh;`
member.  The inherited `Stops`/`Colours` members stay null/unused for mesh gradients.

### 2. New `GradientMesh` subclass (TDL + `vector.h` + painter)

Introduce the gradient as a first-class subclass of `Gradient`, modelled on `GradientGouraud` (which
already suppresses the colour-ramp fields and renders a coloured triangle mesh).  There is **no `VGT`
enum to extend** — the gradient kind is identified by `CLASSID::GRADIENTMESH`, generated from the new
`klass` entry.

**TDL (`src/vector/vector.tdl`).**  Add a `klass` entry next to the other gradient subclasses (after
`GradientGouraud`):

```lua
klass('GradientMesh', { base='Gradient', src='painters/gradient_mesh.cpp',
   output='painters/gradient_mesh_def.cpp', extended=true })
```

As with the other subclasses, the API fields are declared as virtual fields in the class's own
`FieldArray` inside the `.cpp` (no fixed TDL field block).

**Object class (`src/vector/vector.h`).**  Add `extGradientMesh`, modelled on `extGradientGouraud`
(which carries a `std::unique_ptr<GouraudMesh> Gouraud` plus a `GouraudCache GouraudTriangles`):

```cpp
class extGradientMesh : public extGradient {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::GRADIENTMESH;
   static constexpr CSTRING CLASS_NAME = "GradientMesh";
   using create = kt::Create<extGradientMesh>;

   std::unique_ptr<MeshGradient> Mesh;  // Patch data for the mesh gradient
   GouraudCache MeshTriangles;          // Cached tessellated/coloured triangle list (reuses the Gouraud cache type)
};
```

Reusing `GouraudCache` for the tessellated output means the cache invalidation (`MeshHash`/transform/
opacity/colour-space `matches()`) and the `GouraudTriangle` render loop are shared with the Gouraud
path; only the *source* fingerprint differs (patch geometry instead of vertex list).

**Painter (`src/vector/painters/gradient_mesh.cpp`), plus generated `gradient_mesh_def.cpp`.**  Model
the file on `gradient_gouraud.cpp`:

- Suppress the inherited 1D-ramp fields exactly as `GradientGouraud` does — `Colour`, `ColourMap`,
  `SpreadMethod` and `Stops` are declared `FDF_SYSTEM|FDF_VIRTUAL...` with setters that return
  `ERR::UnsupportedField` (mesh colour comes from patch corners, not a ramp).
- Add the mesh authoring field(s) (see §6) as virtual fields, e.g. a `Patches`/`SetMesh` setter that
  populates `Self->Mesh` and invalidates `Self->MeshTriangles.Valid`.
- A `clGradientMeshFields[]` array and an `init_gradient_mesh()` registration calling
  `objMetaClass::create::global` with `fl::BaseClassID(CLASSID::GRADIENT)`,
  `fl::ClassID(CLASSID::GRADIENTMESH)`, `fl::Size(sizeof(extGradientMesh))`, etc., exactly like
  `init_gradient_gouraud`.
- Generated placement/destructor glue goes into `gradient_mesh_def.cpp` via the IDL tools.

**Class registration chaining (`src/vector/painters/gradient.cpp`).**  Add the forward declaration
`static ERR init_gradient_mesh(void);` and call it from `init_gradient()` after the existing subclass
initialisers (`if ((error = init_gradient_mesh()) != ERR::Okay) return error;`).

**CMake (`src/vector/CMakeLists.txt`).**  Register `painters/gradient_mesh.cpp` next to the other
`gradient_*.cpp` sources.

### 3. SVG parser (`src/svg/gradients.cpp`)

Add a `<meshgradient>` handler alongside the existing linear/radial parsing
(`process_gradient_stops` and friends).  Responsibilities:
- Parse `<meshrow>` / `<meshpatch>` / `<stop path=... stop-color=...>` nesting.
- Reconstruct **shared edges and corners** per the SVG 2 inheritance rules above — this is the
  fiddly part and deserves focused unit tests.
- Parse each `stop`'s `path` attribute as a single Bézier edge (reuse the existing path-data
  tokeniser used for `d=` parsing rather than writing a new one).
- Construct a `GradientMesh` object (`CLASSID::GRADIENTMESH`) and populate its `Mesh` member; the SVG
  loader registers it as a definition like the other gradient classes.
- Support `gradientUnits`, `gradientTransform`, `href` inheritance like other gradients.

`save_svg.cpp` should round-trip the mesh back out (emit `<meshgradient>` / `<meshrow>` /
`<meshpatch>`), so add the symmetric writer keyed on `CLASSID::GRADIENTMESH`.

### 4. Renderer — tessellate + Gouraud (new `fill_mesh`, `scene_fill.cpp`)

Add a `fill_mesh(...)` routine and dispatch to it at the top of `render_fill` with a new
`else if (gradient.classID() IS CLASSID::GRADIENTMESH)` clause, immediately beside the existing
`CLASSID::GRADIENTGOURAUD` → `fill_gouraud(...)` dispatch (~line 55).  Like `fill_gouraud`, it does
**not** call the ramp-table path; it rasterises the target fill into an alpha mask and renders
triangles through it.  Per patch:

1. **Subdivide** the patch into an N×N grid (N chosen from patch screen size / the inherited
   `Resolution` field — reuse the existing gradient `Resolution` knob for quality control).  For each
   cell corner, evaluate the Coons surface point `S(u,v)` from the four boundary Béziers (standard
   Coons bilinear blend of the four edge curves minus the bilinear blend of the corners).
2. **Interpolate corner colour** bilinearly in `(u,v)` for each grid vertex.  Honour the gradient's
   `ColourSpace` (sRGB vs linearRGB) exactly as the Gouraud cache does — it stores sRGB-encoded
   colours for `VCS::SRGB`/`INHERIT` and linear-decoded colours for `VCS::LINEAR_RGB`, then selects
   `span_gouraud_rgba` vs `span_gouraud_rgba_linear` accordingly.  Mirror that so mesh and Gouraud
   colour handling stay identical.
3. **Emit two `GouraudTriangle`s per cell** into the cached triangle list and render them through the
   same Gouraud span generators and alpha mask that `fill_gouraud` uses.

Notes:
- **Reuse the Gouraud render core.**  Steps 2–3 are the `rebuild_gouraud_cache` + render-loop logic
  already in `scene_fill.cpp`; factor the shared parts so `fill_mesh` differs only in how it
  *produces* triangles (tessellating patches) versus how `fill_gouraud` does (transforming the
  supplied vertex mesh).
- Tessellation density: adaptive by patch flatness is ideal, but a uniform N driven by `Resolution`
  is a fine first version.  Gouraud already does sub-pixel-accurate edge interpolation so seams
  between cells are continuous.
- Patch-to-patch seams: because shared edges are reconstructed exactly and corner colours are shared,
  adjacent patches meet without cracks provided both tessellate the shared edge with the same N.
  Tessellate edges, not interiors, to guarantee matching vertices along shared edges.
- Bicubic (tensor-product) patches: same pipeline, but `S(u,v)` uses the full 4×4 control net.  Defer
  to a follow-up.

### 5. Caching

The tessellated triangle list depends only on patch geometry + resolution + placement transform +
opacity + colour space — the same invalidation inputs `GouraudCache::matches()` already checks.
Reuse `GouraudCache` (the `MeshTriangles` member) and fingerprint the patch data (positions, control
points and corner colours) into `MeshHash`, rebuilding on a fingerprint/transform/opacity/colour-space
change just as the Gouraud path does.

## Files Touched

| File | Change |
|------|--------|
| `src/vector/vector.h` | `MeshPatchEdge`/`MeshPatch`/`MeshGradient` structs (near `GouraudMesh`); new `extGradientMesh` subclass reusing `GouraudCache` |
| `src/vector/vector.tdl` | New `klass('GradientMesh', ...)` entry |
| `src/vector/painters/gradient_mesh.cpp` | New painter: ramp-field suppression, mesh authoring field(s), `clGradientMeshFields[]`, `init_gradient_mesh()` |
| `src/vector/painters/gradient_mesh_def.cpp` | Generated placement/action glue (IDL output) |
| `src/vector/painters/gradient.cpp` | Forward-declare + chain `init_gradient_mesh()` from `init_gradient()` |
| `src/vector/scene/scene_fill.cpp` | New `fill_mesh` routine + `CLASSID::GRADIENTMESH` dispatch; factor shared triangle-cache/render core out of `fill_gouraud` |
| `src/vector/CMakeLists.txt` | Register `gradient_mesh.cpp` source |
| `src/svg/gradients.cpp` | `<meshgradient>` parser with shared-edge reconstruction |
| `src/svg/save_svg.cpp` | `<meshgradient>` writer (round-trip) |
| (reuse) `agg_span_gouraud_rgba.h` | Existing — no change, used as the fill primitive |
| `include/kotuku/...` (generated) | `CLASSID::GRADIENTMESH`, class header and Tiri constants from TDL/`BUILD_DEFS` |
| `docs/xml` (generated) | Picked up automatically from TDL/comments |

## Tiri / API Authoring

SVG is the primary authoring route, but a programmatic path is valuable for dynamic meshes.  The
`GradientGouraud` class already demonstrates the structured-array authoring pattern (a `Vertices`
array of `GouraudVertex` structs plus an `Indices` array), so follow that precedent.  Options for
exposing patches to Tiri:
- A structured patch array field/method on `GradientMesh` taking patch corners/edges/colours (mirrors
  how `GradientGouraud.Vertices` is set today, and how `Stops` is set on the ramp gradients), **or**
- Accept an SVG `<meshgradient>` fragment string.

Recommend the structured array route for runtime manipulation, consistent with `GradientGouraud` and
the framework's runtime-mutable scene-graph selling point.  Consider a dedicated `MeshPatch`/
`MeshVertex` struct registered with the IDL (as `GouraudVertex` is) so the array is Tiri-addressable.

## Testing

Current coverage:

- `src/vector/tests/test_mesh_gradient.tiri` covers programmatic `GradientMesh` creation, a simple single-patch render,
  row/column metadata, and rejection of inherited ramp fields.
- `src/svg/tests/test_svg.tiri` includes a hash fixture for `gradients/meshgradient.svg`.
- `src/svg/tests/test_meshgradient_roundtrip.tiri` parses a 2x2 mesh, checks same-row and multi-row shared-edge
  reconstruction, saves it, reparses it, and compares rows, columns, units, colour space, patch geometry and corner
  colours.  It also verifies the saved mesh remains compact and preserves `gradientTransform` across a second save,
  and rejects malformed stop paths outside the supported single `L/l/C/c` edge-command subset.

Remaining coverage:

- Extend render tests beyond corner dominance:
  1. **Seam continuity** — sample across a shared edge between two patches; no discontinuity beyond AA tolerance.
  2. **Curved interpolation** — a curved patch bends an iso-colour line; compare against a golden/SSIM check if stable.
  3. **Colour space** — confirm `LINEAR_RGB` interpolation differs from `SRGB`, matching the Gouraud renderer path.
  4. **Cache invalidation** — mutate patch geometry/colour, transform, resolution and colour space, then verify the
     render output changes as expected.
- Use SVG 2 proposal sample meshes as fixtures where possible for conformance evidence.

## Decisions / Remaining Questions

Settled by the current implementation:

- v1 is bilinear Coons patches only; bicubic tensor-product support is a follow-up.
- Programmatic authoring uses a structured `Patches` field of `MeshPatchRecord` values.
- Row-aware authoring uses explicit `Rows` and `Columns` fields while preserving flat row-major `Patches` access.
- Rendering uses uniform tessellation driven by `Resolution`.
- Mesh gradients reuse the Gouraud mask/span renderer and honour `color-interpolation` via the existing colour-space
  path.
- SVG export uses standard compact inherited-edge syntax for normal grids and Kotuku-specific independent-patch
  metadata only when a patch breaks the standard shared-edge representation.

Still open:

1. **Conformance bar for v1** — should v1 require complete SVG proposal shared-edge behaviour, including multi-row
   inheritance and `href`, before the plan is considered complete?

## Remaining Sequence

1. Add cache invalidation tests for patch replacement/mutation, transform changes, opacity, colour space and resolution.
   The cache fingerprinting exists, but it still needs regression coverage.
2. Extend render tests for seam continuity, curved interpolation and `linearRGB` vs `sRGB` behaviour.  Use headless Flute
   tests and SSIM/golden comparison only where the output is stable enough for CI.
3. Follow-up: add tensor-product bicubic patches and adaptive tessellation driven by patch flatness and screen size.

## Compatibility Note

The SVG 2 `<meshgradient>` element was dropped from the core SVG 2 Candidate Recommendation and
lives on as a separate spec proposal; browser support is limited (Firefox had a prefixed
implementation that was later removed).  Targeting the proposal's data model is still the right
move for interoperability with PDF shadings and existing authoring tools, but we should document
that Kotuku's mesh gradient follows the *proposal* rather than a ratified standard, and keep the
internal model decoupled from the exact element syntax so a future spec revision is absorbable.

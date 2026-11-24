/*********************************************************************************************************************

-CLASS-
SVG: Provides comprehensive support for parsing, rendering and animating SVG documents.

The SVG class serves as a complete solution for integrating Scalable Vector Graphics documents into applications.  It
parses SVG statements into a scene graph consisting of @Vector objects and related constructs, providing direct
programmatic access to all graphical elements.  The generated scene graph is accessible via the #Scene and #Viewport
fields, enabling real-time manipulation of individual elements.

Key capabilities include:

<list type="bullet">
<li>W3C-compliant SVG parsing with support for advanced features including gradients, filters, and patterns</li>
<li>SMIL animation support with automatic frame-based playback</li>
<li>Dynamic scene graph manipulation for real-time graphics modification</li>
<li>Flexible rendering targets via the #Target field for integration with existing UI components</li>
<li>Symbol-based graphics with macro-like functionality through #ParseSymbol()</li>
<li>Resolution-independent scaling with automatic adaptation to display characteristics</li>
<li>Export capabilities to multiple formats including PNG images</li>
</list>

The class supports both file-based loading via #Path and direct string-based parsing via #Statement.  SVG documents
can be integrated into existing scene graphs by setting the #Target field, or rendered independently through the
automatically created scene structure.

SVG content is composited in sRGB, as in web browsers.  The top-level @VectorViewport of each parsed document has its
@Vector.ColourSpace set to `SRGB`, and the `color-interpolation="linearRGB"` attribute opts an element and its
descendants into gamma-correct blending.  Symbols instantiated with #ParseSymbol() inherit the colour space of the
target viewport instead, as does a document parsed into a #Target scene that already has a viewport.

<header>Parameters</header>

SVG documents can be parameterised, allowing a client to alter colours, text and other values without editing the
source document.  Values are supplied with #SetKey() prior to initialisation, or through the query string of the
#Path, for example `button.svg?glaze=seagreen&amp;label=Buy%20now`.  Query values are percent-decoded.  If a name is
defined by both #SetKey() and the query string, the #SetKey() value takes precedence.  A leading `--` is removed from
names supplied by either route, so `--glaze` and `glaze` refer to the same parameter.

A document can read a parameter in two ways.  The recommended way for styling is the CSS `env()` function, following
the CSSWG CSS Linked Parameters draft.  The document binds each parameter to a CSS custom property, with a default
value, and uses the property in its rules:

<pre>
&lt;style&gt;
  :root { --glaze: env(--glaze, #3f533b); --shade: env(--shade, #26331f); }
  .tile { fill: var(--glaze); stroke: var(--shade); }
&lt;/style&gt;
</pre>

Because this is standard CSS, web browsers render such a document with its default values.  `env(--name, fallback)`
reads the parameter `name`.  It can appear anywhere in a property value, including within a longer value such as
`stroke-dasharray` and within custom property values.  If the parameter is undefined, the fallback is used.  If there
is no fallback, the declaration is invalid and the property is unset.  Parameters never override custom properties
implicitly; a document opts in by binding them with `env()`.

The second way is the `param()` notation of the W3C SVG Parameters draft, which is written at the start of an
attribute value and is optionally followed by a fallback:

<pre>
&lt;rect fill="param(colour) red" stroke="param(outline) black"/&gt;
&lt;text content-value="param(label)"&gt;Default label&lt;/text&gt;
</pre>

The following rules apply to `param()`:

<list type="bullet">
<li>An attribute value that starts with `param(name)` is substituted as a whole: the entire value is replaced, and
any text following the closing parenthesis is the fallback.</li>
<li>As a Kōtuku extension, the list-valued attributes `points`, `values`, `transform`, `gradientTransform` and
`patternTransform` also accept inline expressions as individual items, e.g. `points="0,0 param(x, 10),20"`,
`values="0;param(mid, 0.5);1"` and `transform="translate(param(x, 0), param(y, 0)) scale(2)"`.  An inline expression
is written `param(name)` or `param(name, fallback)`, and its fallback ends at the matching closing parenthesis.  A
fallback may contain further inline expressions.  A value that starts with `param(name)` without a comma keeps the
whole-attribute meaning, although inline expressions within its fallback are still substituted.  Text in quotes and
names that merely end in `param`, such as `myparam(x)`, are left unchanged.  If an inline expression is undefined and
has no fallback, or is malformed (e.g. unbalanced or with an invalid name), the entire attribute is treated as absent.
Supplied values are inserted as-is and are not expanded further.</li>
<li>Names are case sensitive.  If a name is undefined, the fallback is used.  If there is no fallback, the attribute
is treated as though it were absent and its initial value applies.  A parameter that is defined with an empty value
is not replaced by the fallback.</li>
<li>`param()` is also recognised in `style` attributes and `&lt;style&gt;` sheets, and in custom property values,
provided that it forms the entire value of the property after substitution.</li>
<li>Inline expressions are also recognised in `style` declarations of the supported attributes, e.g.
`transform: translate(param(x, 0), 0)`.</li>
<li>Other viewers do not implement `param()` and treat the attribute as invalid.</li>
</list>

The following rules apply to both forms:

<list type="bullet">
<li>The `id` attribute is never substituted, so that references to the element remain stable.</li>
<li>The `content-value` attribute of a `&lt;text&gt;` element replaces its text content.  If the parameter is
undefined and there is no fallback, the element's own text is retained.</li>
<li>A `&lt;use&gt;` element may contain `&lt;param name="..." value="..."/&gt;` children that apply to that instance
only.  They override document values for `param()` and `env()` references inside the instance, and are inherited by
nested `&lt;use&gt;` instances.  These values apply before resolving `env()` bindings in the instance's copy of the
`&lt;use&gt;` element's custom properties, including inherited `:root` bindings and properties derived from them.
Elements outside the instance retain their document values.  Explicit custom property values on `&lt;use&gt;` and
declarations inside the content retain their priority.  The `&lt;param&gt;` values themselves resolve against the
enclosing scope, so `&lt;param name="glaze" value="param(glaze) red"/&gt;` forwards an enclosing value.
Gradients that reference parameters are instantiated separately for each distinct set of values.  Clip paths, masks,
filters, markers and patterns use document values.</li>
<li>An `&lt;image&gt;` element that references an SVG document may also contain `&lt;param&gt;` children.  They are
appended to the href as query parameters, overriding values of the same name in the href, and are resolved against the
active scope so that values can be forwarded, e.g. `&lt;param name="colour" value="param(colour) red"/&gt;`.
`&lt;param&gt;` children are ignored for other image formats.</li>
</list>

Values set with #SetKey() after initialisation do not alter the existing scene graph.  They are applied by the next
parse, e.g. via #DataFeed(), and to symbols instantiated with #ParseSymbol().

<header>Conditional processing</header>

Use `kotuku:requiredParams` to select whole elements by SVG parameter value.  On an eligible element a failed
condition suppresses the element and its descendants, including outside a `&lt;switch&gt;`.  A `&lt;switch&gt;`
selects its first child whose conditions all pass.  `requiredExtensions` requires every URI in its list to be
supported.  A non-empty `systemLanguage` passes in any locale.

The attribute accepts white-space-separated tests, all of which must pass:

<pre>
Test             Passes when
name             The parameter is defined, including a defined empty value
!name            The parameter is undefined
name=value       The parameter is defined and equals the value
name=a,b         The parameter is defined and equals a or b
name!=value      The parameter is undefined or differs from the value
name!=a,b        The parameter is undefined or differs from every listed value
</pre>

Names and values are case sensitive.  A leading `--` is removed from a name.  Use `name=` to match a defined
empty value.  Values are percent-decoded after splitting, so `%20`, `%2C` and `%3D` represent a space, comma
and equals sign respectively.  The condition is evaluated as written, without `param()` substitution.  Empty
or malformed conditions fail and generate a warning with the source line number.

Bind the attribute prefix to `http://www.kotuku.dev/xmlns/svg` or `http://www.kotuku.dev/namespaces/kotuku`.
Declaring either namespace sets @VectorScene.Compliance to `NONE` on scenes owned by the SVG object.

Tests read the active `&lt;use&gt;` instance's `&lt;param&gt;` values, then #SetKey(), then the query string.
CSS custom properties are outside this lookup, including those declared on a `&lt;use&gt;`.  A referenced graphic
is tested in its instance scope.  #ParseSymbol() uses the current document values and creates fresh nested
instances.  For existing content, values set with #SetKey() take effect at the next parse.

Definitions such as gradients, symbols, clip paths, masks, filters and patterns are exempt from ordinary gating.
An explicit `&lt;switch&gt;` in a pattern tile uses document parameter values.  Clip paths, masks and filters also
use document values.  To select between filter effects, switch the element that uses the filter.

<pre>
&lt;switch&gt;
  &lt;rect kotuku:requiredParams="lighting=off" requiredExtensions="http://www.kotuku.dev/TR/Kotuku/1.0"
    class="swatch" filter="url(#flat)"/&gt;
  &lt;rect class="swatch" filter="url(#lit)"/&gt;
&lt;/switch&gt;
</pre>

Put the default branch last.  Renderers that ignore the Kōtuku attribute may choose a guarded branch whenever
it comes first.  Use `requiredExtensions="http://www.kotuku.dev/TR/Kotuku/1.0"` to reserve a branch for Kōtuku.

<header>CSS Custom Properties</header>

CSS custom properties (`--name: value`) and `var()` are supported in `&lt;style&gt;` sheets and `style` attributes,
with the semantics of CSS Custom Properties Level 1:

<list type="bullet">
<li>Custom properties are inherited, and the cascade follows importance, specificity and source order, as for other
properties.  Names are case sensitive.  `:root` matches the outermost `&lt;svg&gt;` element.</li>
<li>`var(--name, fallback)` uses the fallback if the property is undefined.  A fallback may contain further `var()`
and `env()` references.</li>
<li>A custom property that refers to itself through a cycle of `var()` references is invalid.</li>
<li>A declaration that uses `var()` or `env()` replaces any presentation attribute of the same name.  If its value
cannot be resolved, the property is unset, as in CSS, rather than falling back to the presentation attribute.  A
declaration that is syntactically invalid is ignored and earlier values are kept.</li>
<li>Custom properties declared on a `&lt;use&gt;` element are inherited by the referenced content, and declarations
within that content override them.  Nested instances inherit from the enclosing `&lt;use&gt;`.  As in web browsers,
the content does not inherit from the ancestors of the referenced element.</li>
<li>Gradients, patterns, clip paths, masks, filters and markers resolve `var()` from their own position in the
document, not from the element that references them.</li>
</list>

Custom property values are re-serialised by the CSS parser rather than kept as raw token text, so spacing and the
spelling of some tokens may differ from the source.  `var()` and `env()` are not supported in presentation attributes
such as `fill="var(--glaze)"`, consistent with web browsers.  `@property` and pseudo-classes other than `:root` are
not supported.

<header>Images in Filters</header>

An `feImage` primitive that references an element, such as `href="#map"`, draws that element at its own size with the
origin of its user space at the top-left corner of the primitive subregion.  This matches web browsers.  If the
subregion is not set it defaults to the filter region, so the element moves with the filtered element.  It is then
offset from content drawn in user space, such as a pattern fill of the same map.

To draw the referenced element in the user space of the filtered element, give `feImage` `x="0"` and `y="0"`, and a
`width` and `height` that cover the filter region.  This applies when `primitiveUnits` is `userSpaceOnUse`, which is
the default.  The output is clipped to the filter region as usual.  Use the same attributes on every `feImage` that
supplies a texture map, so that the albedo, height and normal maps of a material align with each other and with the
document.

<pre>
&lt;filter id="material" x="0" y="0" width="1" height="1"&gt;
  &lt;feImage href="#albedoMap" x="0" y="0" width="1000" height="600" result="albedo"/&gt;
  &lt;feImage href="#normalMap" x="0" y="0" width="1000" height="600" result="normals"/&gt;
  ...
&lt;/filter&gt;
</pre>

<header>Lighting Extensions</header>

A document that declares a Kōtuku namespace is rendered with a @VectorScene.Compliance of `NONE` (see #Init), which
enables the enhanced lighting of @LightingFX.  The following attributes of `feDiffuseLighting` and
`feSpecularLighting` configure it, using any prefix that is bound to the Kōtuku namespace:

<list type="bullet">
<li>`kotuku:features`: `all`, `none`, or a space-separated list of `dequantise`, `sharpen`, `cavity`, `detail`,
`shadows`, `specular-aa` and `dequantise-wide`.  Sets @LightingFX.Features.  The default is `all`, which excludes the opt-in 
`dequantise-wide`.</li>
<li>`kotuku:cavity`: strength of the darkening of creases and pits, 0 or greater, default 0.25.  Sets
@LightingFX.Cavity.</li>
<li>`kotuku:detail`: relief multiplier for fine detail, 1 or greater, default 1.5.  Sets @LightingFX.Detail.</li>
<li>`kotuku:penumbra`: width of the soft edge of shadows in degrees, 0 or greater, default 2.  Sets
@LightingFX.Penumbra.</li>
<li>`kotuku:ambient`: a colour, or `none` (the default).  Ambient light added to the diffuse light; its alpha scales
it.  Sets @LightingFX.Ambient.  It has no effect on `feSpecularLighting`.</li>
<li>`kotuku:normalMap` selects the named image result of an earlier primitive for RGB surface directions.
Sets @LightingFX.NormalMap.  The primary input still supplies height.</li>
<li>`kotuku:normalConvention`: `y-down` (the default) or `y-up`, the direction of Y in the green channel of the
normal map.  Most texture libraries and Blender export `y-up` (OpenGL) maps; DirectX tools export `y-down`.  A wrong
value swaps the shading of slopes that face up and down the image.  Sets @LightingFX.NormalConvention.</li>
<li>`kotuku:normalStrength`: 0 or greater, default 1.  Multiplies the X and Y components of the normal map, so 0
flattens it and values above 1 exaggerate it.  Sets @LightingFX.NormalStrength.</li>
<li>`kotuku:occlusionMap` selects the named result of an earlier primitive as an ambient occlusion map, in which white
is fully exposed and black fully occluded.  It darkens the ambient light and the matcap, but not the light of the light
sources.  Sets @LightingFX.OcclusionMap.  It has no effect on `feSpecularLighting`.</li>
<li>`kotuku:occlusionChannel`: `R` (the default), `G`, `B`, `A` or `luminance`, the channel of the occlusion map to
read.  Packed ORM maps store occlusion in `R`.  Sets @LightingFX.OcclusionChannel.</li>
<li>`kotuku:occlusionStrength`: 0 or greater, default 1.  The occlusion `ao` is applied as
`1 + strength * (ao - 1)`, so 0 disables the map.  Sets @LightingFX.OcclusionStrength.</li>
<li>`kotuku:roughnessMap` selects the named result of an earlier primitive as a roughness map, in which black is
smooth and white fully rough.  Each pixel's roughness sets its own specular exponent, so smooth areas have narrow,
bright highlights and rough areas broad, dim ones.  `specularExponent` remains the exponent at which the specular
constant is calibrated, and pixels that the map does not cover use it.  Sets @LightingFX.RoughnessMap.  It has no
effect on `feDiffuseLighting`.</li>
<li>`kotuku:roughnessChannel`: `R` (the default), `G`, `B`, `A` or `luminance`, the channel of the roughness map to
read.  Packed ORM maps store roughness in `G`.  Sets @LightingFX.RoughnessChannel.</li>
</list>

The light source elements `feDistantLight`, `fePointLight` and `feSpotLight` accept these attributes, which set the
light's options with @LightingFX.SetLightOptions():

<list type="bullet">
<li>`kotuku:colour`: the colour of this light, in the syntax of `lighting-color`.  If it is omitted the light takes
the primitive's `lighting-color`, which defaults to white.</li>
<li>`kotuku:intensity`: a multiplier of the light's colour, 0 or greater, default 1.  Zero turns off the light's
direct light.</li>
<li>`kotuku:range`: for point and spot lights, the distance at which the light fades to nothing, in the units of its
`z` coordinate, or `none`.  The light is at full strength at the point of the surface nearest to it and falls with the
square of the distance from there.  Omitting it, `none` and 0 leave the light unlimited.  A distant light cannot
have a range; the attribute is reported and ignored.</li>
<li>`kotuku:shadows`: `true` (the default) or `false`.  With `false` a distant light casts no shadows.</li>
</list>

An invalid value is ignored and the default is kept.  Other renderers are expected to ignore attributes in an unknown
namespace and draw the standard SVG lighting.

<pre>
&lt;svg xmlns="http://www.w3.org/2000/svg" xmlns:kotuku="http://www.kotuku.dev/xmlns/svg"&gt;
  &lt;filter id="relief"&gt;
    &lt;feDiffuseLighting in="SourceAlpha" surfaceScale="8" kotuku:features="shadows cavity" kotuku:penumbra="4"&gt;
      &lt;feDistantLight azimuth="225" elevation="35"/&gt;
    &lt;/feDiffuseLighting&gt;
  &lt;/filter&gt;
&lt;/svg&gt;
</pre>

The `kotuku:feLighting` element is the `COMBINED` type of @LightingFX.  It lights an albedo with diffuse, specular and
ambient light in one primitive.  The `in` attribute selects a floating-point height result or an image whose alpha
supplies the height.  The `in2` attribute selects the albedo, which defaults to opaque white.  It accepts
`surfaceScale`, `diffuseConstant`, `specularConstant`, `specularExponent`, `kernelUnitLength` and `lighting-color`.
Use `normalMap` to supply an RGB normal map from an earlier primitive, such as an image loaded by `feImage`,
`occlusionMap` to supply an ambient occlusion map, and `roughnessMap` to supply a roughness map.
The extension attributes above can omit their prefix because the element is already in the Kōtuku namespace.
The element also accepts these material attributes:

<list type="bullet">
<li>`metalness`: 0 for a dielectric surface (the default) to 1 for a metal, whose highlights take the albedo's colour
and which has no diffuse light.  Sets @LightingFX.Metalness.</li>
<li>`fresnel`: reflectance at normal incidence, 0 to 1.  A non-zero value enables a Fresnel term that brightens
highlights on steep slopes and edges; 0.04 suits most dielectrics.  The default is 0, which disables it.  Sets
@LightingFX.Fresnel.</li>
<li>`wrap`: 0 to 1, default 0.  Extends diffuse light past the terminator, for wax, paper and skin.  Sets
@LightingFX.Wrap.</li>
<li>`matcap`: the path of a matcap image, an image of a sphere that reflects an environment, which adds reflections
such as those of chrome or glass.  The path is relative to the document's folder.  Absolute paths, volume names,
parent folder components and paths that resolve outside that folder are refused.  If the image cannot be loaded, a
warning is logged and the surface is drawn without it.  Sets @LightingFX.MatcapPath.</li>
</list>

The element accepts any number of light source elements, which light the surface together in document order.
Their light is added before the result is limited to full strength, so a key light and fill lights can share one
primitive.  `desc`, `title` and `metadata` children are ignored, and without a light source the default light is
used.  The standard lighting primitives keep the SVG rule of one light source, their first child; combine separate
primitives with `feComposite` to light a standard document with several sources.  The element is drawn whatever the
document's compliance, but other renderers do not draw it.

<pre>
&lt;kotuku:feLighting in="SourceAlpha" in2="paint" surfaceScale="6" specularConstant="0.6" specularExponent="24"&gt;
  &lt;feDistantLight azimuth="225" elevation="40"/&gt;
  &lt;fePointLight x="40" y="120" z="60" kotuku:colour="#8af" kotuku:intensity="0.6" kotuku:range="200"/&gt;
&lt;/kotuku:feLighting&gt;
</pre>

In documents using the Kōtuku namespace, `kotuku:outputMode="height"` on `feTurbulence` or `feGaussianBlur`
produces a floating-point height result for enhanced lighting.  A height-output blur accepts both a height result
and an image's alpha.  Supply the albedo through a separate image input.

<pre>
&lt;filter id="brass" xmlns:kotuku="http://www.kotuku.dev/xmlns/svg"&gt;
  &lt;feTurbulence type="fractalNoise" baseFrequency="0.04" numOctaves="3" kotuku:outputMode="height" result="raw"/&gt;
  &lt;feGaussianBlur in="raw" stdDeviation="1.5" kotuku:outputMode="height" result="smooth"/&gt;
  &lt;kotuku:feLighting in="smooth" in2="SourceGraphic" surfaceScale="6" diffuseConstant="0.9"
      specularConstant="0.6" specularExponent="40" ambient="#202830"&gt;
    &lt;feDistantLight azimuth="225" elevation="35" kotuku:intensity="1.4"/&gt;
  &lt;/kotuku:feLighting&gt;
&lt;/filter&gt;
</pre>

The `kotuku:feNormalMap` element writes the surface normals from the height selected by `in` or from the map
selected by `normalMap`.  It accepts `surfaceScale`, `kernelUnitLength`, `features` and `detail`.  Use `kernelUnitX`
or `kernelUnitY` to set a kernel unit on one axis and keep the default on the other.  See @LightingFX.UnitX for how
the default is resolved.

Use the normal map's named result as `in2` on `feDisplacementMap` with red for X and green for Y.  For example,
these primitives distort the graphic at changes in its alpha.  To refract an image behind a surface, select that
image as the displacement primitive's `in`.

<pre>
&lt;filter id="refraction" xmlns:kotuku="http://www.kotuku.dev/xmlns/svg"&gt;
  &lt;kotuku:feNormalMap in="SourceAlpha" surfaceScale="8" result="normals"/&gt;
  &lt;feDisplacementMap in="SourceGraphic" in2="normals" xChannelSelector="R" yChannelSelector="G" scale="12"/&gt;
&lt;/filter&gt;
</pre>

To refract through a normal map loaded by `feImage`, add `kotuku:normalConvention` to `feDisplacementMap` with the
map's convention, `y-down` or `y-up`.  The map is then read as signed normal components in either colour space, as
lighting reads it, rather than as colour.  The default, `auto`, does this only for the result of
`kotuku:feNormalMap`.  Sets @DisplacementFX.NormalConvention.

Animation timing is controlled through the #FrameRate field, with callback support via #FrameCallback for custom
rendering workflows.  The implementation maintains compatibility with the complete SVG specification while providing
enhanced programmatic access unique to Kotuku.

Please refer to the W3C's online documentation for exhaustive information on the SVG standard.

*********************************************************************************************************************/

static void notify_free_scene(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   auto Self = (extSVG *)CurrentContext();
   if (Self->AnimationTimer) { UpdateTimer(Self->AnimationTimer, 0); Self->AnimationTimer = nullptr; }
}

/*********************************************************************************************************************
-ACTION-
Activate: Initiates playback of SVG animations.

SVG documents containing SMIL animation features will remain static until activated through this action.  Upon
activation, the animation system begins processing animation sequences in the background according to the configured
#FrameRate.  The #Scene will be automatically redrawn as each frame is computed, ensuring smooth visual transitions.

To integrate custom rendering logic with the animation cycle, configure the #FrameCallback field with an appropriate
function.  This callback will be triggered after each frame preparation, enabling applications to implement custom
rendering workflows or capture animation frames.

<b>Note:</b> If the SVG document contains no animation elements, this action completes successfully but has no visual
effect.

-END-
*********************************************************************************************************************/

static ERR SVG_Activate(extSVG *Self)
{
   if (!Self->Animations.empty()) {
      if (Self->PauseStart) { // Resume from a paused state; the paused interval is excluded from the animation clock
         Self->PausedTime += (double(PreciseTime()) / 1000000.0) - Self->PauseStart;
         Self->PauseStart = 0;
      }

      if (!Self->AnimationTimer) {
         SubscribeTimer(1.0 / (double)Self->FrameRate, C_FUNCTION(animation_timer), &Self->AnimationTimer);
         SubscribeAction(Self->Scene, AC::Free, C_FUNCTION(notify_free_scene));
      }
      else UpdateTimer(Self->AnimationTimer, 1.0 / (double)Self->FrameRate);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Deactivate: Halts all SVG animation playback and suspends frame processing.

This action immediately terminates any active animation playback, stopping all animation timers and suspending
frame processing.  The SVG document will remain visible in its current state, but no further animation updates will
occur until the object is reactivated.

The deactivation process is immediate and does not affect the underlying scene graph structure.  Animation sequences
can be resumed from their current positions by calling the #Activate() action again.

This action is particularly useful for implementing pause functionality or conserving system resources when
animations are not required.

-END-
*********************************************************************************************************************/

static ERR SVG_Deactivate(extSVG *Self)
{
   if (Self->AnimationTimer) {
      UpdateTimer(Self->AnimationTimer, 0);
      Self->AnimationTimer = nullptr;
      if (Self->Scene) UnsubscribeAction(Self->Scene, AC::Free); // Activate() will resubscribe
      Self->PauseStart = double(PreciseTime()) / 1000000.0;
   }
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Seek: Moves SVG animation playback to a document time in seconds.

Seek updates the scene immediately and invokes #FrameCallback, including while playback is paused or before the first
#Activate().  It preserves the playing or paused state.  Seeking before activation leaves playback paused at the new
position until #Activate() is called.

`SEEK::START` uses `Offset` as the document time.  `SEEK::CURRENT` adds `Offset` to the current document time.
`SEEK::END` subtracts `Offset` from the document duration, and `SEEK::RELATIVE` multiplies that duration by `Offset`
(e.g. `0.5` selects the midpoint).  Negative results are clamped to zero; positions beyond the end are allowed.

The document duration is the latest resolved animation end, as reported by #Duration.  If any animation has an
unresolved or indefinite end, `SEEK::END` and `SEEK::RELATIVE` return `ERR::NoSupport`.  Start and current positions
remain available.  Seeking backwards discards recorded event-triggered begins after the new position.  Documents
without animations return success without rendering or invoking the callback.  The selected time can be read back
from #Position.

-ERRORS-
Okay
NullArgs
Args: The position is invalid, the offset is not finite, or the resulting time is not finite.
NoSupport: The document duration is indefinite and the position requires a finite duration.
-TAGS-
mutates-object, callback-inlines
-END-
*********************************************************************************************************************/

static ERR SVG_Seek(extSVG *Self, struct acSeek *Args)
{
   if (!Args) return ERR::NullArgs;
   if (!std::isfinite(Args->Offset)) return ERR::Args;
   if ((Args->Position != SEEK::START) and (Args->Position != SEEK::CURRENT) and
       (Args->Position != SEEK::END) and (Args->Position != SEEK::RELATIVE)) return ERR::Args;
   if (Self->Animations.empty()) return ERR::Okay;

   const double now = Self->PauseStart ? Self->PauseStart : double(PreciseTime()) / 1000000.0;
   const double clock = now - Self->PausedTime;
   double time = Args->Offset;
   if (Args->Position IS SEEK::CURRENT) {
      time += Self->AnimEpoch ? std::max(0.0, clock - Self->AnimEpoch) : 0;
   }
   else if ((Args->Position IS SEEK::END) or (Args->Position IS SEEK::RELATIVE)) {
      resolve_animations(Self);
      if (!std::isfinite(Self->AnimationDuration)) return ERR::NoSupport;
      time = (Args->Position IS SEEK::END) ? Self->AnimationDuration - time : Self->AnimationDuration * time;
   }
   if (!std::isfinite(time)) return ERR::Args;
   time = std::max(0.0, time);

   if (!Self->AnimEpoch) Self->AnimEpoch = clock;
   Self->PausedTime = now - Self->AnimEpoch - time;
   if (!Self->AnimationTimer) Self->PauseStart = now;
   render_frame(Self, time);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
DataFeed: Processes SVG data streams for incremental document parsing.

The DataFeed action enables real-time processing of SVG data streams, allowing documents to be parsed incrementally
as data becomes available.  This is particularly useful for network-based loading scenarios or when processing large
SVG documents that may arrive in segments.

The action accepts XML data streams and integrates them into the existing document structure.  Multiple DataFeed
calls can be made to build up complex SVG documents progressively.

<b>Supported data types:</b> `DATA::XML` for SVG content streams.

This mechanism provides an alternative to the static #Statement field for scenarios requiring dynamic content
loading or streaming workflows.

-END-
*********************************************************************************************************************/

static ERR SVG_DataFeed(extSVG *Self, struct acDataFeed *Args)
{
   if (!Args) return ERR::NullArgs;

   if (Args->Datatype IS DATA::XML) {
      return parse_svg(Self, std::string_view{},
         std::string_view((const char *)Args->Buffer.data(), Args->Buffer.size()));
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
GetKey: Retrieves the value of a document parameter.

GetKey() returns the document-level value of a parameter that was defined with #SetKey() or the query string of
the #Path.  A value defined by #SetKey() takes precedence.  A leading `--` is removed from the key.  Values declared
by `&lt;param&gt;` elements are scoped to their `&lt;use&gt;` instance and cannot be retrieved.

-ERRORS-
Okay
Args
UnsupportedField: The parameter is not defined.
-END-
*********************************************************************************************************************/

static ERR SVG_GetKey(extSVG *Self, struct acGetKey *Args)
{
   if ((not Args) or (not Args->Value)) return ERR::Args;

   if (auto value = find_param(Self, nullptr, param_key(Args->Key))) {
      Args->Value->assign(*value);
      return ERR::Okay;
   }

   Args->Value->clear();
   return ERR::UnsupportedField;
}

/*********************************************************************************************************************
-ACTION-
Init: Initialises the SVG object and processes source content.

The initialisation process establishes the scene graph structure and processes any specified SVG source content.  If
a #Path has been configured, the referenced SVG file will be loaded and parsed immediately.  Alternatively, if
#Statement contains SVG data, that content will be processed instead.

The default behaviour creates a local @VectorScene object to contain the generated scene graph.  Its
@VectorScene.Compliance is set to `SVG2` so that the document is rendered to the SVG standard.  If the document
declares a Kōtuku namespace, for instance `xmlns:kotuku="http://www.kotuku.dev/xmlns/svg"`, it is asking for
features beyond the standard and the Compliance is set to `NONE` instead.  The declaration is sufficient; the prefix
does not need to be used.

The local scene can be overridden by setting the #Target field to redirect content into an existing scene graph
structure, enabling integration with existing UI components.  The @VectorScene.Compliance of a target's scene is not
modified.

The initialisation sequence includes:

<list type="ordered">
<li>Scene graph structure creation or validation of the specified #Target</li>
<li>SVG document parsing and scene graph population</li>
<li>Resolution of SVG references, definitions, and symbol libraries</li>
<li>Animation sequence preparation for documents containing SMIL features</li>
</list>

Successfully initialised SVG objects provide immediate access to the generated scene graph via the #Scene and
#Viewport fields, enabling programmatic manipulation of individual graphic elements.

-END-
*********************************************************************************************************************/

static ERR SVG_Init(extSVG *Self)
{
   if (!Self->Target) {
      if ((Self->Target = objVectorScene::create::local())) {
         Self->Scene = (objVectorScene *)Self->Target;
         Self->Scene->setCompliance(VSC::SVG2);
      }
      else return ERR::NewObject;
   }

   if (not Self->Path.empty()) return parse_svg(Self, Self->Path, std::string_view{});
   else if (not Self->Statement.empty()) return parse_svg(Self, std::string_view{}, Self->Statement);

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
ParseSymbol: Instantiates an SVG symbol definition within a target viewport.

ParseSymbol() enables dynamic instantiation of SVG symbol definitions that were declared within the loaded document.
This method provides macro-like functionality, allowing complex graphical elements to be replicated and positioned as
needed throughout the application.  This approach promotes efficient memory usage and consistent visual design while
enabling dynamic scene graph construction.

The specified `ID` must correspond to a symbol element that exists within the current document's definition library.
The generated content will be structured within the provided @VectorViewport, which must be part of an established
scene graph.

-INPUT-
strview ID: Name of the symbol to parse.
obj(VectorViewport) Viewport: The target viewport.

-RESULT-
Okay: Symbol successfully parsed and instantiated.
NullArgs: Required parameters were not provided.
NotFound: The specified symbol ID does not exist in the document.

-TAGS-
mutates-input
-END-

*********************************************************************************************************************/

static ERR SVG_ParseSymbol(extSVG *Self, struct svg::ParseSymbol *Args)
{
   kt::Log log;

   if ((!Args) or (Args->ID.empty()) or (!Args->Viewport)) return log.warning(ERR::NullArgs);

   if (auto tagref = find_href_tag(Self, Args->ID)) {
      TagInstance symbol(Self, *tagref, nullptr);
      ParamScope scope;
      svgState state(Self);
      state.m_param_scope = &scope;
      state.process_children(*symbol.tag, Args->Viewport);
      Self->AnimTimingDirty = true;
      prepare_value_animations(Self);
      resolve_animations(Self);
      return ERR::Okay;
   }
   else {
      log.warning("Symbol '%.*s' not found.", int(Args->ID.size()), Args->ID.data());
      return ERR::NotFound;
   }
}

//********************************************************************************************************************
// An SVG with a Target shares the client's scene, so drawing that scene would draw the client's content as well.
// Render() and SaveImage() use a private copy of the document instead, parsed from the same source with the same
// parameters and colour.  Documents that were only supplied through DataFeed() cannot be copied.

static ERR create_private_copy(extSVG *Self, extSVG **Result)
{
   kt::Log log(__FUNCTION__);

   if ((Self->Path.empty()) and (Self->Statement.empty())) return log.warning(ERR::NoSupport);

   extSVG *copy;
   if (NewLocalObject(CLASSID::SVG, &copy) != ERR::Okay) return log.warning(ERR::NewObject);

   copy->Path      = Self->Path;
   copy->Statement = Self->Statement;
   copy->Colour    = Self->Colour;
   copy->Params    = Self->Params;
   copy->Flags     = Self->Flags & ~(SVF::AUTOSCALE|SVF::ENFORCE_TRACKING);

   if (InitObject(copy) != ERR::Okay) {
      FreeResource(copy);
      return log.warning(ERR::Init);
   }

   *Result = copy;
   return ERR::Okay;
}

//********************************************************************************************************************
// Returns true if the SVG draws into a client's scene (#Target) rather than one that it owns.

static bool is_targeted(extSVG *Self)
{
   return Self->Scene->ownerID() != Self->UID;
}

//********************************************************************************************************************
// The native size of a document is the width and height of its root viewport, else its view box, else 1920x1080.

static void document_size(extSVG *Self, double &Width, double &Height)
{
   Width  = 0;
   Height = 0;

   if (auto view = Self->Viewport) {
      Unit width(0), height(0);
      if (Self->AutoscaleWidth.defined()) Width = Self->AutoscaleWidth;
      else if (view->getWidth(width) IS ERR::Okay) Width = width;
      if (Self->AutoscaleHeight.defined()) Height = Self->AutoscaleHeight;
      else if (view->getHeight(height) IS ERR::Okay) Height = height;
      if (Width <= 0)  view->getViewWidth(Width);
      if (Height <= 0) view->getViewHeight(Height);
   }

   if (Width <= 0)  Width  = 1920;
   if (Height <= 0) Height = 1080;
}

//********************************************************************************************************************
// Draws a document that owns its scene into the (X, Y, Width, Height) area of Bitmap.  The scene's bitmap and page
// size, the root viewport's size and the bitmap's clip region are restored before returning.

static ERR render_document(extSVG *Self, objBitmap *Bitmap, int X, int Y, int Width, int Height)
{
   kt::Log log(__FUNCTION__);

   auto scene = Self->Scene;
   auto view  = Self->Viewport;
   if (not view) return log.warning(ERR::NoData);

   auto clip = Bitmap->Clip;
   ClipRectangle area = { std::max(clip.Left, X), std::max(clip.Top, Y),
      std::min(clip.Right, X + Width), std::min(clip.Bottom, Y + Height) };
   if ((area.Right <= area.Left) or (area.Bottom <= area.Top)) return ERR::Okay;

   double doc_width, doc_height;
   document_size(Self, doc_width, doc_height);

   Unit saved_width(0, FD_PURE), saved_height(0, FD_PURE);
   view->getWidth(saved_width);
   view->getHeight(saved_height);
   saved_width.Type  &= ~FD_PURE;
   saved_height.Type &= ~FD_PURE;

   auto saved_bitmap      = scene->Bitmap;
   auto saved_page_width  = scene->PageWidth;
   auto saved_page_height = scene->PageHeight;

   // Fix the root viewport at the document's native size, then scale it uniformly into the target area and centre
   // it.  Without a fixed size, a percentage-sized root would follow the temporary page size.

   view->setWidth(Unit(doc_width));
   view->setHeight(Unit(doc_height));

   VectorMatrix *matrix = nullptr;
   if (auto error = view->newMatrix(&matrix, true); error != ERR::Okay) {
      view->setWidth(saved_width);
      view->setHeight(saved_height);
      return log.warning(error);
   }

   auto scale = std::min(double(Width) / doc_width, double(Height) / doc_height);
   vec::Scale(matrix, scale, scale);
   vec::Translate(matrix, X + ((Width - (doc_width * scale)) * 0.5), Y + ((Height - (doc_height * scale)) * 0.5));
   vec::FlushMatrix(matrix);

   scene->setBitmap(Bitmap);
   scene->setPageWidth(Bitmap->Width);
   scene->setPageHeight(Bitmap->Height);
   Bitmap->Clip = area;

   auto error = Action(AC::Draw, scene, nullptr);

   Bitmap->Clip = clip;
   scene->setBitmap(saved_bitmap);
   scene->PageWidth  = saved_page_width; // Direct assignment because the setters do not accept zero
   scene->PageHeight = saved_page_height;
   view->freeMatrix(matrix);
   view->setWidth(saved_width);
   view->setHeight(saved_height);
   acRefresh(view);
   return error;
}

/*********************************************************************************************************************

-METHOD-
Render: Performs high-quality rasterisation of the SVG document to a target bitmap.

This method rasterises the SVG document into the `(X, Y, Width, Height)` area of the target bitmap.  The rendering
process handles all vector elements, gradients, filters, and effects with full anti-aliasing and precision.

The document is scaled uniformly from its native size to fit the area, preserving its aspect ratio, and is centred
within it.  The native size is the width and height of the root viewport, else its view box, else 1920x1080.  Output
is clipped to the area and to the bitmap's clip region.  The document is drawn over the existing content of the
bitmap, which is not cleared first.

The SVG object's scene is restored after drawing, so rendering does not affect the scene graph or a later draw.  If
the SVG has a #Target, it shares the client's scene; a private copy of the document is parsed from the #Path or
#Statement and drawn instead, so the client's content is not included.  The copy uses the same parameters and #Colour
and starts at the beginning of any animation.

<b>Performance considerations:</b> Rendering complex SVG documents with multiple effects and high resolutions may
require significant processing time.  Consider using appropriate dimensions that balance quality requirements with
performance constraints.

-INPUT-
obj(Bitmap) Bitmap: The target bitmap object to receive the rendered content.
int X: Horizontal position within the target bitmap.
int Y: Vertical position within the target bitmap.
int Width: Desired width of the rendered output in pixels.
int Height: Desired height of the rendered output in pixels.

-RESULT-
Okay: Rendering completed successfully.
NullArgs: Required bitmap parameter was not provided.
Args: The width or height is less than 1.
NoData: The document has no root viewport.
NoSupport: The SVG has a #Target, and its document was not supplied through #Path or #Statement.

-TAGS-
blocking, mutates-input
-END-

*********************************************************************************************************************/

static ERR SVG_Render(extSVG *Self, struct svg::Render *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Bitmap)) return log.warning(ERR::NullArgs);
   if ((Args->Width < 1) or (Args->Height < 1)) return log.warning(ERR::Args);

   if (is_targeted(Self)) {
      extSVG *copy;
      if (auto error = create_private_copy(Self, &copy); error != ERR::Okay) return error;
      auto error = render_document(copy, Args->Bitmap, Args->X, Args->Y, Args->Width, Args->Height);
      FreeResource(copy);
      return error;
   }

   return render_document(Self, Args->Bitmap, Args->X, Args->Y, Args->Width, Args->Height);
}

/*********************************************************************************************************************
-ACTION-
SaveImage: Saves the SVG document as a PNG image.

This action renders the SVG document at its native size and saves the result.  The native size is the width and
height of the root viewport, else its view box, else 1920x1080.  Fractional sizes are rounded up.  Areas that the
document does not paint are transparent.  If the SVG has a #Target, a private copy of the document is rendered, as
described for #Render().

The image will be saved in PNG format by default, but can be changed by specifying an alternate `ClassID`.  PNG
is recommended in particular because it supports an alpha channel.

-ERRORS-
Okay
NullArgs
CreateObject
NoData: The document has no root viewport.
NoSupport: The SVG has a #Target, and its document was not supplied through #Path or #Statement.
-END-
*********************************************************************************************************************/

static ERR SVG_SaveImage(extSVG *Self, struct acSaveImage *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);

   if (is_targeted(Self)) {
      extSVG *copy;
      if (auto error = create_private_copy(Self, &copy); error != ERR::Okay) return error;
      auto error = SVG_SaveImage(copy, Args);
      FreeResource(copy);
      return error;
   }

   if (not Self->Viewport) return log.warning(ERR::NoData);

   double doc_width, doc_height;
   document_size(Self, doc_width, doc_height);
   auto width  = int(std::ceil(doc_width));
   auto height = int(std::ceil(doc_height));

   auto pic = objImage::create { fl::Width(width), fl::Height(height), fl::Flags(PCF::NEW|PCF::FORCE_ALPHA_32) };
   if (not pic.ok()) return log.warning(ERR::CreateObject);

   gfx::DrawRectangle(pic->Bitmap, 0, 0, width, height, 0, BAF::FILL); // Unblended, so the image is transparent

   if (auto error = render_document(Self, pic->Bitmap, 0, 0, width, height); error != ERR::Okay) return error;
   return acSaveImage(*pic, Args->Dest, Args->ClassID);
}

/*********************************************************************************************************************
-ACTION-
SaveToObject: Saves the SVG document to a data object.
-END-
*********************************************************************************************************************/

static ERR SVG_SaveToObject(extSVG *Self, struct acSaveToObject *Args)
{
   kt::Log log;
   std::span<struct ActionEntry> actions;

   if (!Self->Viewport) return log.warning(ERR::NoData);

   if ((Args->ClassID != CLASSID::NIL) and (Args->ClassID != CLASSID::SVG)) {
      auto mc = (objMetaClass *)FindClass(Args->ClassID);
      if ((!mc->getActionTable(actions)) and (not actions.empty())) {
         if ((actions[int(AC::SaveToObject)].PerformAction) and (actions[int(AC::SaveToObject)].PerformAction != (APTR)SVG_SaveToObject)) {
            return actions[int(AC::SaveToObject)].PerformAction(Self, Args);
         }
         else if ((actions[int(AC::SaveImage)].PerformAction) and (actions[int(AC::SaveImage)].PerformAction != (APTR)SVG_SaveImage)) {
            struct acSaveImage saveimage = { .Dest = Args->Dest };
            return actions[int(AC::SaveImage)].PerformAction(Self, &saveimage);
         }
         else return log.warning(ERR::NoSupport);
      }
      else return log.warning(ERR::GetField);
   }
   else {
      // Seed the document with the XML header and root <svg> element in a single statement.  The XML API requires
      // an existing anchor tag for InsertXML(), so an empty document cannot be populated incrementally - the initial
      // tree must be supplied via the Statement field at creation time.

      static const std::string root_doc =
         "<?xml version=\"1.0\" standalone=\"no\"?>\n"
         "<svg version=\"1.2\" xmlns=\"http://www.w3.org/2000/svg\" "
         "xmlns:xlink=\"http://www.w3.org/1999/xlink\" xmlns:kotuku=\"http://www.kotuku.dev/xmlns/svg\"/>";

      auto xml = objXML::create { fl::Flags(XMF::NEW|XMF::READABLE), fl::Statement(root_doc) };

      if (xml.ok()) {
         Self->XML = *xml;

         ERR error = ERR::Okay;

         // Locate the root <svg> tag; child content and definitions are attached beneath it.

         XTag *tag = nullptr;
         for (auto &scan : xml->Tags) {
            if ((scan.isTag()) and (kt::iequals(scan.name(), "svg"))) { tag = &scan; break; }
         }
         if (not tag) error = ERR::Search;

         int index = (tag) ? tag->ID : 0;

         if (!error) {
            bool multiple_viewports = (Self->Scene->Viewport->Next) ? true : false;
            if (multiple_viewports) {
               if (!(error = save_svg_defs(Self, *xml, Self->Scene, index))) {
                  for (auto scan=Self->Scene->Viewport; scan; scan=(objVectorViewport *)scan->Next) {
                     if (!scan->Child) continue; // Ignore dummy viewports with no content
                     save_svg_scan(Self, *xml, scan, index);
                  }

                  error = embed_collection_paints(Self, *xml, index);
                  if (!error) error = xml->saveToObject(Args->Dest);
               }
            }
            else {
               double x, y, width, height;

               if (!error) error = Self->Viewport->getViewX(x);
               if (!error) error = Self->Viewport->getViewY(y);
               if (!error) error = Self->Viewport->getViewWidth(width);
               if (!error) error = Self->Viewport->getViewHeight(height);

               if (!error) {
                  char buffer[80];
                  snprintf(buffer, sizeof(buffer), "%g %g %g %g", x, y, width, height);
                  xml::NewAttrib(tag, "viewBox", buffer);
               }

               if (!error) {
                  Unit unit(0, FD_PURE); // Request original client setting

                  if ((!error) and (!Self->Viewport->getX(unit))) set_dimension(tag, "x", unit);
                  if ((!error) and (!Self->Viewport->getY(unit))) set_dimension(tag, "y", unit);
                  if ((!error) and (!Self->Viewport->getWidth(unit))) set_dimension(tag, "width", unit);
                  if ((!error) and (!Self->Viewport->getHeight(unit))) set_dimension(tag, "height", unit);
               }

               // The root composites in sRGB by default when the document is parsed, so only linearRGB is written.

               VCS colour_space;
               if ((!error) and (!Self->Viewport->getColourSpace(colour_space)) and
                   (colour_space IS VCS::LINEAR_RGB)) {
                  xml::NewAttrib(tag, "color-interpolation", "linearRGB");
               }

               if (!error) {
                  if (!(error = save_svg_defs(Self, *xml, Self->Scene, index))) {
                     for (auto scan=((objVector *)Self->Viewport)->Child; scan; scan=scan->Next) {
                        save_svg_scan(Self, *xml, scan, index);
                     }

                     error = embed_collection_paints(Self, *xml, index);
                     if (!error) error = xml->saveToObject(Args->Dest);
                  }
               }
            }

            Self->XML = nullptr;
         }

         return error;
      }
      else return ERR::CreateObject;
   }
}

/*********************************************************************************************************************
-ACTION-
SetKey: Defines a document parameter.

SetKey() defines a parameter value for substitution into `env()` and `param()` references in the SVG document.
Please refer to the Parameters section of the class documentation for the syntax and rules.  A leading `--` is removed
from the key, so `--glaze` and `glaze` define the same parameter.  An existing value of the same name is replaced, and
zero-length values are permitted.

Parameters should be set prior to initialisation.  Setting a value afterwards does not alter the existing scene graph;
the value is applied by the next parse and by #ParseSymbol().

-ERRORS-
Okay
NullArgs: The key is empty.
InvalidValue: The key is `--` alone, or contains white-space or parentheses, so cannot be referenced.
-END-
*********************************************************************************************************************/

static ERR SVG_SetKey(extSVG *Self, struct acSetKey *Args)
{
   if ((not Args) or (Args->Key.empty())) return ERR::NullArgs;

   const auto key = param_key(Args->Key);
   if (key.empty()) return ERR::InvalidValue;

   for (auto ch : key) {
      if ((unsigned(ch) <= 0x20) or (ch IS '(') or (ch IS ')')) return ERR::InvalidValue;
   }

   if (auto it = Self->Params.find(key); it != Self->Params.end()) it->second.assign(Args->Value);
   else Self->Params.emplace(std::string(key), std::string(Args->Value));
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Colour: Defines the default fill to use for `currentColor` references.

Set the Colour value to alter the default fill that is used for `currentColor` references.  Typically a standard RGB
painter fill reference should be used for this purpose, e.g. `rgb(255,255,255)`.  It is however, also acceptable to use
URL references to named definitions such as gradients and images.  This will work as long as the named definition is
registered in the top-level @VectorScene object.

<b>Supported formats:</b>
<list type="bullet">
<li>RGB values: `rgb(red, green, blue)`</li>
<li>Hexadecimal notation: six-digit triplets or three-digit shorthand triplets</li>
<li>Named colours: Standard SVG colour names</li>
<li>URL references: `url(#gradientId)` for complex paint definitions</li>
</list>

-FIELD-
Duration: The resolved length of the document's animations, measured in seconds.

Duration reports the latest resolved end of all animations in the document.  It is the same duration that #Seek()
uses for `SEEK::END` and `SEEK::RELATIVE`, accounting for begin offsets, multiple begin instances, repeat counts,
`repeatDur`, `min` and `max` bounds, restart interruptions and syncbase chains.  Documents without animations, and
documents whose animations all end at or before zero, report `0`.

A value of `-1` indicates that at least one animation has an indefinite or unresolved end, for instance an indefinite
repeat without a finite bound, a begin that waits on a user event, a missing syncbase reference or a timing cycle.  In
this state `SEEK::END` and `SEEK::RELATIVE` are unavailable, while absolute and current seeking remain supported.
Clients should only enable a proportional scrubber when the duration is greater than zero.

The value is a snapshot of the resolved timing and does not count down during playback.  It can change when a symbol
is instantiated through #ParseSymbol(), when a user event records a new begin instance, when a seek backwards
discards a recorded event instance, and when a new document is parsed.  Future event instances are not predicted.

Reading this field does not start playback, render a frame or alter the #Position.

*********************************************************************************************************************/

static ERR GET_Duration(extSVG *Self, double *Value)
{
   resolve_animations(Self);
   if (std::isfinite(Self->AnimationDuration)) *Value = std::max(0.0, Self->AnimationDuration);
   else *Value = -1;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Flags:Configuration flags that modify SVG processing behaviour.
Lookup: SVF

-FIELD-
Frame: Constrains rendering to a specific frame number for frame-based display systems.

This field enables frame-synchronised rendering by restricting graphics display to specific frame numbers within
frame-based container systems.  When set to a non-zero value, the SVG content will only be rendered when the
container's current frame matches this field's value.

The default value of 0 disables frame-based filtering, allowing the SVG content to be rendered continuously regardless
of the container's frame state.

-FIELD-
FrameCallback: Function callback executed after each animation frame preparation.

This field enables integration of custom logic into the animation processing pipeline by specifying a callback
function that executes after each animation frame is computed.  The callback mechanism provides precise timing for
implementing custom rendering workflows, frame capture systems, or animation synchronisation logic.

The callback function receives a pointer to the SVG object, enabling access to the current scene state and rendering
control.  This is commonly used for rendering the animated SVG content to target bitmaps, implementing video capture,
or synchronising with external animation systems.

<b>Timing behaviour:</b> The callback executes immediately after frame preparation but before automatic scene
redrawing, ensuring that custom logic can modify or capture the scene state at the optimal moment.

<b>Animation dependency:</b> Callbacks are only triggered for SVG documents containing SMIL animation features.
Static documents will not invoke the callback function.

<b>Playback position:</b> Read #Position for the current document time and #Duration for the resolved length of the
animation.  #Position follows the animation clock and can be marginally ahead of the frame being delivered.

<b>Function prototype:</b> `void Function(*SVG)`

*********************************************************************************************************************/

static ERR GET_FrameCallback(extSVG *Self, FUNCTION **Value)
{
   if (Self->FrameCallback.defined()) {
      *Value = &Self->FrameCallback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_FrameCallback(extSVG *Self, FUNCTION *Value)
{
   if (Self->FrameCallback.defined()) Self->FrameCallback.unpin();
   if (Value) {
      Self->FrameCallback = *Value;
      if (Self->FrameCallback.defined()) Self->FrameCallback.pin();
   }
   else Self->FrameCallback.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
FrameRate: Controls the maximum frame rate for SVG animation playback.

This field establishes the upper limit for animation frame processing, measured in frames per second.  The frame rate
directly impacts animation smoothness and system resource consumption, requiring careful balance between visual
quality and performance efficiency.

<b>Recommended ranges:</b>
<list type="bullet">
<li><b>Standard displays:</b> 60 FPS matches most modern display refresh rates</li>
<li><b>Balanced performance:</b> 30-50 FPS provides smooth animation with moderate resource usage</li>
<li><b>Low-power devices:</b> 20-30 FPS conserves battery while maintaining acceptable quality</li>
</list>

<b>Performance considerations:</b> Higher frame rates increase CPU usage proportionately.  A frame rate of 100 FPS
consumes approximately twice the processing power of 50 FPS, with corresponding impact on power consumption and
thermal characteristics.

<b>Valid range:</b> 20-1000 FPS, though values above 120 FPS rarely provide perceptible improvements on standard
displays.

The frame rate only affects animated SVG documents containing SMIL features.  Static documents are unaffected by
this setting.

*********************************************************************************************************************/

static ERR SET_FrameRate(extSVG *Self, int Value)
{
   if ((Value >= 20) and (Value <= 1000)) {
      Self->FrameRate = Value;
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
ParamNames: Lists the names of the parameters that the document references.

This field returns the sorted, unique names of parameters referenced by `param()`, `env()` or a valid
`kotuku:requiredParams` test, including parameters without supplied values.  Names are reported without a leading
`--`.  An `env()` binding in a custom property is reported when it reaches an element.  Use this field to check
which #SetKey() values the document references.  The array is empty before parsing or when the document references
no parameters.
-END-

*********************************************************************************************************************/

static ERR GET_ParamNames(extSVG *Self, std::span<std::string> &Value)
{
   Self->ParamNames = Self->ConditionParamNames;

   for (auto &[id, attribs] : Self->ParamAttribs) {
      for (int a=1; a < std::ssize(attribs); a++) {
         if (not can_substitute_param(attribs[a].Name)) continue;
         for_each_param_name(attribs[a].Value, [&](std::string_view Name) { Self->ParamNames.emplace_back(Name); });
      }
   }

   std::sort(Self->ParamNames.begin(), Self->ParamNames.end());
   Self->ParamNames.erase(std::unique(Self->ParamNames.begin(), Self->ParamNames.end()), Self->ParamNames.end());

   Value = std::span<std::string>(Self->ParamNames.data(), Self->ParamNames.size());
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Path: File system path to the source SVG document.

This field specifies the location of the SVG file to be loaded and processed during object initialisation.  The path
supports both absolute and relative file references, with relative paths resolved according to the current working
directory context.

The loading process occurs automatically during initialisation when a valid path is specified.  The referenced file
must contain well-formed SVG content that conforms to W3C SVG standards for successful parsing.

<b>Supported file types:</b> Standard SVG files (*.svg) and compressed SVG files (*.svgz) are both supported, with
automatic decompression handling for compressed formats.

<b>Path resolution:</b> The file system path is resolved through the standard Kotuku file access mechanisms,
supporting virtual file systems, archives, and network-accessible resources where configured.

When both #Path and #Statement are specified, the Path field takes precedence and the Statement content is ignored
during initialisation.

*********************************************************************************************************************/

static ERR SET_Path(extSVG *Self, const std::string_view &Value)
{
   Self->Folder.clear();
   Self->Path = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Position: The current animation time of the document, measured in seconds.

Position reports the document time of the animation clock.  It is `0` for documents without animations, and for an
animated document until the first playback frame or a successful #Seek() establishes the clock.  The value is never
negative.

The Position advances while playback is active and remains constant while playback is paused through #Deactivate().
Time spent paused is excluded, so #Activate() continues from the paused position.  A successful #Seek() sets the
Position to the selected time, which is not clamped to the #Duration.  Parsing a new document returns the Position to
`0`, while instantiating a symbol with #ParseSymbol() leaves it unchanged.

The value describes the document clock rather than the most recently rendered frame.  During playback it advances
between frames and can be slightly ahead of the scene state observed by #FrameCallback.

Reading this field does not start playback, render a frame or alter the animation state.

*********************************************************************************************************************/

static ERR GET_Position(extSVG *Self, double *Value)
{
   if ((Self->Animations.empty()) or (!Self->AnimEpoch)) *Value = 0;
   else *Value = std::max(0.0, Self->anim_clock() - Self->AnimEpoch);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Scene: Reference to the @VectorScene object containing the SVG scene graph.

This read-only field provides direct access to the @VectorScene object that manages the complete SVG scene graph structure.  The scene object serves as the root container for all generated vector elements and provides essential rendering coordination.

The scene reference remains valid throughout the SVG object's lifetime and enables direct manipulation of scene-wide properties including page dimensions, rendering settings, and global definitions.  This field simplifies access to the scene graph for applications requiring programmatic control over the complete document structure.

<b>Scene relationship:</b> When a #Target is specified, the Scene field references the @VectorScene that owns the target object.  For automatically generated scenes, this field references the internally created scene object.

-FIELD-
Statement: String containing complete SVG document markup.

SVG data can be loaded from a string by specifying it here prior to initialisation.  If the #Path field has been
defined, it will take precedent and the Statement is ignored.

For incremental data parsing after initialisation, consider using the #DataFeed() action instead, which supports
progressive document construction from data streams.

-FIELD-
Target: Destination container for the generated SVG scene graph elements.

This field redirects the generated SVG scene graph to an existing container object instead of creating an independent
scene structure.  The target approach enables seamless integration of SVG content into established UI hierarchies and
composite scene graphs.

<b>Default behaviour:</b> When no target is specified, the SVG object creates and manages a dedicated @VectorViewport
to contain the generated content.  This viewport and its children remain under direct SVG object ownership.

<b>Target requirements:</b> The target object must be part of an existing scene graph owned by a @VectorScene object.
While any vector object can serve as a target, @VectorViewport objects are recommended for optimal compatibility and
performance.

<b>Standards compliance:</b> The @VectorScene.Compliance of the target's scene is not modified, because it applies to
all content in that scene.  Set it to `SVG2` before parsing if the document must be rendered to the SVG standard.
For the same reason, if the target is a @VectorScene with an existing viewport, the document reuses that viewport
and its @Vector.ColourSpace is not reset to `SRGB`, so the document composites in the host's colour space.  Attributes
declared on the root `svg` tag, including `color-interpolation`, are still applied to the reused viewport.

<b>Ownership implications:</b> Specifying a target makes the generated scene graph independent of the SVG object
lifecycle.  The SVG object can be terminated without affecting the created vector elements, enabling flexible
resource management patterns.

<b>Resource tracking:</b> When independent operation is not desired, enable the `ENFORCE_TRACKING` flag to maintain
resource tracking relationships between the SVG object and generated definitions, ensuring proper cleanup on object
destruction.

*********************************************************************************************************************/

static ERR SET_Target(extSVG *Self, OBJECTPTR Value)
{
   if (Value->classID() IS CLASSID::VECTORSCENE) {
      Self->Target = Value;
      Self->Scene = (objVectorScene *)Value;
      if (Self->Scene->Viewport) Self->Viewport = Self->Scene->Viewport;
   }
   else {
      auto owner = Value->Owner;
      while ((owner) and (owner->classID() != CLASSID::VECTORSCENE)) {
         owner = owner->Owner;
      }

      if (owner) {
         Self->Scene = (objVectorScene *)owner;
         Self->Target = Value;
      }
      else return ERR::UnsupportedOwner;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Title: The title of the SVG document.

The title of an SVG document is declared with a title element that can embedded anywhere in the document.  In cases
where a title has been specified, it will be possible to read it from this field.

-FIELD-
Viewport: Reference to the primary @VectorViewport containing the SVG document content.

This read-only field provides direct access to the main @VectorViewport object that contains the root-level SVG
content.

-END-

*********************************************************************************************************************/

#include "anim_metrics.cpp"
#include "anim_timing.cpp"
#include "anim_parsing.cpp"
#include "anim_motion.cpp"
#include "anim_transform.cpp"
#include "anim_value.cpp"
#include "gradients.cpp"
#include "parser.cpp"

//********************************************************************************************************************

extSVG::~extSVG()
{
   if (AnimationTimer) {
      UpdateTimer(AnimationTimer, 0);
      if (Scene) UnsubscribeAction(Scene, AC::Free);
   }

   if (FrameCallback.defined()) {
      FrameCallback.unpin();
      FrameCallback.clear();
   }

   if ((Target) and (Target IS Scene) and (Scene->Owner IS this)) FreeResource(Target);

   if (XML) FreeResource(XML);

   if (!Resources.empty()) {
      for (auto id : Resources) FreeResource(id);
   }
}

//********************************************************************************************************************

#include "class_svg_def.c"

static const FieldArray clSVGFields[] = {
   { "Target",    FDF_OBJECT|FDF_RI, nullptr, SET_Target },
   { "Path",      FDF_CPPSTRING|FDF_RW, nullptr, SET_Path },
   { "Src",       FDF_SYNONYM },
   { "Title",     FDF_CPPSTRING|FDF_RW },
   { "Statement", FDF_CPPSTRING|FDF_RW },
   { "Colour",    FDF_CPPSTRING|FDF_RW },
   { "Viewport",  FDF_OBJECT|FDF_R },
   { "Scene",     FDF_OBJECT|FDF_R },
   { "Frame",     FDF_INT|FDF_RW, nullptr, nullptr },
   { "Flags",     FDF_INTFLAGS|FDF_RW, nullptr, nullptr, &clSVGFlags },
   { "FrameRate", FDF_INT|FDF_RW, nullptr, SET_FrameRate },
   // Virtual Fields
   { "FrameCallback", FDF_VIRTUAL|FDF_FUNCTION|FDF_RW|FDF_PURE, GET_FrameCallback, SET_FrameCallback },
   { "ParamNames",    FDF_VIRTUAL|FDF_VECTOR|FDF_CPPSTRING|FDF_R, GET_ParamNames },
   { "Position",      FDF_VIRTUAL|FDF_DOUBLE|FDF_R, GET_Position },
   { "Duration",      FDF_VIRTUAL|FDF_DOUBLE|FDF_R, GET_Duration },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_svg(void)
{
   clSVG = objMetaClass::create::global(
      fl::ClassVersion(VER_SVG),
      fl::Name("SVG"),
      fl::FileExtension("svg"),
      fl::FileDescription("Scalable Vector Graphics (SVG)"),
      fl::Icon("filetypes/vectorgfx"),
      fl::Category(CCF::GUI),
      fl::Actions(clSVGActions),
      fl::Methods(clSVGMethods),
      fl::Fields(clSVGFields),
      fl::Flags(CLF::INHERIT_LOCAL),
      fl::Size(sizeof(extSVG)),
      fl::Path(MOD_PATH));

   return clSVG ? ERR::Okay : ERR::AddClass;
}

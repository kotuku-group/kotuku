/*********************************************************************************************************************

-CLASS-
VectorClip: Clips are used to define complex clipping regions for vectors.

The VectorClip defines a clipping path that can be used by other vectors as a mask.  The clipping path is defined by
creating Vector shapes that are initialised to the VectorClip's #Viewport as child objects.

Vector shapes can utilise a VectorClip by referring to it via the Vector's @Vector.Mask field.

VectorClip objects must be owned by a @VectorScene.  It is valid for a VectorClip to be shared amongst multiple vector
objects within the same scene.  If optimum drawing efficiency is required, we recommend that each VectorClip is
referenced by one vector only.  This will reduce the frequency of path recomputation and redrawing of the clipping
path.

The SVG standard makes a distinction between clipping paths and masks.  Consequently, this distinction also exists
in the VectorClip design, and by default VectorClip objects will operate in path clipping mode.  This means that
the clipping path is constructed as a solid filled area, and stroke instructions are completely ignored.  To create
more complex masks, such as one with a filled gradient, use the `VCLF::APPLY_FILLS` option in #Flags.  If stroking
operations are required, define `VCLF::APPLY_STROKES`.

Finally, for the purposes of UI development it may often be beneficial to set #Units to `VUNIT::BOUNDING_BOX` so that
the clipping path is sized to match the target vector.  A viewbox size of `0 0 1 1` is applied by default, but if a
1:1 match to the target vector is preferred, set the #Viewport @VectorViewport.ViewWidth and
@VectorViewport.ViewHeight to match the target vector's dimensions.

-END-

*********************************************************************************************************************/

static ERR CLIP_Free(extVectorClip *Self)
{
   if (Self->ViewportID) { FreeResource(Self->ViewportID); Self->ViewportID = 0; Self->Viewport = nullptr; }
   Self->~extVectorClip();
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR CLIP_Init(extVectorClip *Self)
{
   kt::Log log;

   if ((int(Self->Units) <= 0) or (int(Self->Units) >= int(VUNIT::END))) {
      log.traceWarning("Invalid Units value of %d", Self->Units);
      return ERR::OutOfRange;
   }

   // A viewport hosts the shapes for determining the clipping path.

   if ((Self->Owner) and (Self->Owner->classID() IS CLASSID::VECTORSCENE)) {
      if ((Self->Viewport = (objVectorViewport *)objVectorViewport::create::global(
            fl::Owner(Self->ownerID()),
            fl::Visibility(VIS::HIDDEN),
            fl::AspectRatio(ARF::NONE),
            fl::X(0), fl::Y(0), fl::Width(1), fl::Height(1) // Target dimensions are defined when drawing
         ))) {

         Self->ViewportID = Self->Viewport->UID;

         if (Self->Units IS VUNIT::BOUNDING_BOX) {
            // In BOUNDING_BOX mode the clip paths will be sized within a viewbox of (0 0 1 1) as required by SVG
            Self->Viewport->setFields(fl::ViewWidth(1.0), fl::ViewHeight(1.0));
         }

         return ERR::Okay;
      }
      else return ERR::CreateObject;
   }
   else return ERR::UnsupportedOwner;
}

//********************************************************************************************************************

static ERR CLIP_NewChild(extVectorClip *Self, struct acNewChild *Args)
{
   if (Self->initialised()) {
      kt::Log log;
      log.warning("Child objects not supported - assign this %s to Viewport instead.", Args->Object->className());
      return ERR::NoSupport;
   }
   else return ERR::Okay;
}

//********************************************************************************************************************

static ERR CLIP_NewPlacement(extVectorClip *Self)
{
   new (Self) extVectorClip;

   Self->Units  = VUNIT::USERSPACE; // SVG default is userSpaceOnUse
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Flags: Optional flags.
Lookup: VCLF

-END-
*********************************************************************************************************************/

static ERR CLIP_GET_Flags(extVectorClip *Self, VCLF *Value)
{
   *Value = Self->Flags;
   return ERR::Okay;
}

static ERR CLIP_SET_Flags(extVectorClip *Self, VCLF Value)
{
   Self->Flags = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Units: Defines the coordinate system for fields X, Y, Width and Height.

The default coordinate system for clip-paths is `BOUNDING_BOX`, which positions the clipping region relative to the
vector that references it.  The alternative is `USERSPACE`, which positions the path relative to the vector's parent
viewport.
-END-
*********************************************************************************************************************/

static ERR CLIP_GET_Units(extVectorClip *Self, VUNIT *Value)
{
   *Value = Self->Units;
   return ERR::Okay;
}

static ERR CLIP_SET_Units(extVectorClip *Self, VUNIT Value)
{
   Self->Units = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Viewport: This viewport hosts the Vector objects that will contribute to the clip path.

To define the path(s) that will be used to build the clipping mask, add at least one @Vector object to the viewport
declared here.
-END-
*********************************************************************************************************************/

static ERR CLIP_GET_Viewport(extVectorClip *Self, objVectorViewport **Value)
{
   *Value = Self->Viewport;
   return ERR::Okay;
}

//********************************************************************************************************************

#include "clip_def.cpp"

static const ActionArray clClipActions[] = {
   { AC::Free,      CLIP_Free },
   { AC::Init,      CLIP_Init },
   { AC::NewChild,  CLIP_NewChild },
   { AC::NewPlacement, CLIP_NewPlacement },
   { AC::NIL, nullptr }
};

static const FieldArray clClipFields[] = {
   { "Viewport", FDF_OBJECT|FDF_R|FDF_PURE, CLIP_GET_Viewport },
   { "Units",    FDF_INT|FDF_LOOKUP|FDF_RW|FDF_PURE, CLIP_GET_Units, CLIP_SET_Units, &clVectorClipUnits },
   { "Flags",    FDF_INTFLAGS|FDF_RW|FDF_PURE, CLIP_GET_Flags, CLIP_SET_Flags, &clVectorClipFlags },
   END_FIELD
};

static ERR init_clip(void)
{
   clVectorClip = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTORCLIP),
      fl::Name("VectorClip"),
      fl::Actions(clClipActions),
      fl::Fields(clClipFields),
      fl::Category(CCF::GRAPHICS),
      fl::Size(sizeof(extVectorClip)),
      fl::Path(MOD_PATH));

   return clVectorClip ? ERR::Okay : ERR::AddClass;
}


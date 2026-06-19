/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
VectorViewport: Provides support for viewport definitions within a vector tree.

This class is used to declare a viewport within a vector scene graph.  A master viewport is required as the first object
in a @VectorScene and it must contain all vector graphics content.

The size of the viewport is initially set to `(0,0,100%,100%)` so as to be all inclusive.  Setting the #X, #Y, #Width and
#Height fields will determine the position and clipping of the displayed content (the 'target area').  The #ViewX, #ViewY,
#ViewWidth and #ViewHeight fields declare the viewbox ('source area') that will be sampled for the target.

To configure the scaling and alignment method that is applied to the viewport content, set the #AspectRatio field.

-END-

NOTE: Refer to gen_vector_path() for the code that manages viewport dimensions in a live state.

*********************************************************************************************************************/

static double viewport_parent_width(extVectorViewport *Self)
{
   if (Self->ParentView) return viewport_coordinate_width(Self->ParentView);
   else if (Self->Scene) return Self->Scene->PageWidth;
   else return 0;
}

static double viewport_parent_height(extVectorViewport *Self)
{
   if (Self->ParentView) return viewport_coordinate_height(Self->ParentView);
   else if (Self->Scene) return Self->Scene->PageHeight;
   else return 0;
}

static Unit fixed_to_requested_unit(double Value, double ParentSize, bool Scaled)
{
   if (Scaled) return Unit(ParentSize ? Value / ParentSize : 0, FD_SCALED);
   else return Unit(Value);
}

//********************************************************************************************************************
// Input event handler for the dragging of viewports by the user.  Requires the client to set the DragCallback field
// to be active.

static ERR drag_callback(extVectorViewport *Viewport, const InputEvent *Events)
{
   static double glAnchorX = 0, glAnchorY = 0; // Anchoring is process-exclusive, so we can store the coordinates as global variables
   static double glDragOriginX = 0, glDragOriginY = 0;

   gen_vector_tree(Viewport);

   for (auto event=Events; event; event=event->Next) {
      // Process events that support consolidation first.

      if ((event->Flags & (JTYPE::ANCHORED|JTYPE::MOVEMENT)) != JTYPE::NIL) {
         if (Viewport->vpDragging) {
            while ((event->Next) and ((event->Next->Flags & JTYPE::MOVEMENT) != JTYPE::NIL)) { // Consolidate movement
               event = event->Next;
            }

            double x = glDragOriginX + (event->AbsX - glAnchorX);
            double y = glDragOriginY + (event->AbsY - glAnchorY);

            if (Viewport->vpDragCallback.isC()) {
               kt::SwitchContext context(Viewport->vpDragCallback.Context);
               auto routine = (void (*)(extVectorViewport *, double, double, double, double, APTR Meta))Viewport->vpDragCallback.Routine;
               routine(Viewport, x, y, glDragOriginX, glDragOriginY, Viewport->vpDragCallback.Meta);
            }
            else if (Viewport->vpDragCallback.isScript()) {
               sc::Call(Viewport->vpDragCallback, std::to_array<ScriptArg>({
                  { "Viewport", Viewport, FD_OBJECTPTR }, { "X", x }, { "Y", y },
                  { "OriginX", glDragOriginX }, { "OriginY", glDragOriginY }
               }));
            }
         }
      }
      else if ((event->Type IS JET::LMB) and ((event->Flags & JTYPE::REPEATED) IS JTYPE::NIL)) {
         if (event->Value > 0) {
            if (Viewport->Visibility != VIS::VISIBLE) continue;
            Viewport->vpDragging = 1;
            glAnchorX = event->AbsX;
            glAnchorY = event->AbsY;

            Viewport->get(FID_X, glDragOriginX);
            Viewport->get(FID_Y, glDragOriginY);
         }
         else Viewport->vpDragging = 0; // Released
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Clear: Free all child objects contained by the viewport.
-END-
*********************************************************************************************************************/

static ERR VECTORVIEWPORT_Clear(extVectorViewport *Self)
{
   kt::vector<ChildEntry> list;
   if (!ListChildren(Self->UID, &list)) {
      for (unsigned i=0; i < list.size(); i++) FreeResource(list[i].ObjectID);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORVIEWPORT_Free(extVectorViewport *Self)
{
   if ((Self->Scene) and (!Self->Scene->collecting()) and (!((extVectorScene *)Self->Scene)->ResizeSubscriptions.empty())) {
      if (((extVectorScene *)Self->Scene)->ResizeSubscriptions.contains(Self)) {
         ((extVectorScene *)Self->Scene)->ResizeSubscriptions.erase(Self);
      }
   }

   if (Self->vpDragCallback.defined()) {
      Self->subscribeInput(JTYPE::NIL, C_FUNCTION(drag_callback));
   }

   if (Self->vpBuffer) {
      if (Self->vpBufferData) {
         FreeResource(Self->vpBufferData);
         Self->vpBufferData = nullptr;
      }
      FreeResource(Self->vpBuffer);
      Self->vpBuffer = nullptr;
      if (Self->Scene) ((extVectorScene *)Self->Scene)->BufferCount--;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORVIEWPORT_Init(extVectorViewport *Self)
{
   // Initialisation is performed by VECTOR_Init()

   // Please refer to gen_vector_path() for the initialisation of vpFixedX/Y/Width/Height, which has
   // its own section for dealing with viewports.

   if (Self->vpBuffered) {
      ((extVectorScene *)Self->Scene)->BufferCount++;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Move: Move the position of the viewport by delta X, Y.

*********************************************************************************************************************/

static ERR VECTORVIEWPORT_Move(extVectorViewport *Self, struct acMove *Args)
{
   if (!Args) return ERR::NullArgs;

   Unit x, y;
   Self->get(FID_X, x);
   Self->get(FID_Y, y);

   Self->vpTargetX = Unit(x + Args->DeltaX);
   Self->vpTargetY = Unit(y + Args->DeltaY);

   mark_dirty((extVector *)Self, RC::FINAL_PATH|RC::TRANSFORM);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
MoveToPoint: Move the position of the viewport to a fixed point.
-END-
*********************************************************************************************************************/

static ERR VECTORVIEWPORT_MoveToPoint(extVectorViewport *Self, struct acMoveToPoint *Args)
{
   if (!Args) return ERR::NullArgs;

   if ((Args->Flags & MTF::X) != MTF::NIL) Self->vpTargetX = Args->X;
   if ((Args->Flags & MTF::Y) != MTF::NIL) Self->vpTargetY = Args->Y;

   mark_dirty((extVector *)Self, RC::FINAL_PATH|RC::TRANSFORM);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORVIEWPORT_NewObject(extVectorViewport *Self)
{
   Self->vpAspectRatio = ARF::MEET|ARF::X_MID|ARF::Y_MID;
   Self->vpOverflowX   = VOF::VISIBLE;
   Self->vpOverflowY   = VOF::VISIBLE;

   // NB: vpTargetWidth and vpTargetHeight are not set to a default because we need to know if the client has
   // intentionally avoided setting the viewport and/or viewbox dimensions (which typically means that the viewport
   // will expand to fit the parent).
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Redimension: Reposition and resize a viewport to a fixed size.
-END-
*********************************************************************************************************************/

static ERR VECTORVIEWPORT_Redimension(extVectorViewport *Self, struct acRedimension *Args)
{
   if (!Args) return ERR::NullArgs;

   Self->vpTargetX      = Unit(Args->X);
   Self->vpTargetY      = Unit(Args->Y);
   Self->vpTargetWidth  = Unit(Args->Width);
   Self->vpTargetHeight = Unit(Args->Height);

   if (Self->vpTargetWidth < 1) Self->vpTargetWidth = Unit(1);
   if (Self->vpTargetHeight < 1) Self->vpTargetHeight = Unit(1);
   mark_dirty((extVector *)Self, RC::FINAL_PATH|RC::TRANSFORM);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Resize: Resize a viewport to a fixed size.
-END-
*********************************************************************************************************************/

static ERR VECTORVIEWPORT_Resize(extVectorViewport *Self, struct acResize *Args)
{
   if (!Args) return ERR::NullArgs;

   Self->vpTargetWidth  = Unit(Args->Width);
   Self->vpTargetHeight = Unit(Args->Height);

   if (Self->vpTargetWidth < 1) Self->vpTargetWidth = Unit(1);
   if (Self->vpTargetHeight < 1) Self->vpTargetHeight = Unit(1);
   mark_dirty((extVector *)Self, RC::FINAL_PATH|RC::TRANSFORM);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
AbsX: The horizontal position of the viewport, relative to `(0, 0)`.

This field will return the left-most boundary of the viewport, relative to point `(0, 0)` of the scene
graph.  Transforms are taken into consideration when calculating this value.

*********************************************************************************************************************/

static ERR VIEW_GET_AbsX(extVectorViewport *Self, int &Value)
{
   gen_vector_tree(Self);

   Value = Self->vpBounds.left;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
AbsY: The vertical position of the viewport, relative to `(0, 0)`.

This field will return the top-most boundary of the viewport, relative to point `(0, 0)` of the scene
graph.  Transforms are taken into consideration when calculating this value.

*********************************************************************************************************************/

static ERR VIEW_GET_AbsY(extVectorViewport *Self, int &Value)
{
   gen_vector_tree(Self);

   Value = Self->vpBounds.top;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
AspectRatio: Flags that affect the aspect ratio of vectors within the viewport.
Lookup: ARF

Defining an aspect ratio allows finer control over the position and scale of the viewport's content within its target
area.

<types lookup="ARF"/>

*********************************************************************************************************************/

static ERR VIEW_GET_AspectRatio(extVectorViewport *Self, ARF &Value)
{
   Value = Self->vpAspectRatio;
   return ERR::Okay;
}

static ERR VIEW_SET_AspectRatio(extVectorViewport *Self, ARF Value)
{
   Self->vpAspectRatio = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Buffer: Returns the bitmap buffer that the viewport is using.

*********************************************************************************************************************/

static ERR VIEW_GET_Buffer(extVectorViewport* Self, objBitmap * &Value)
{
   Value = Self->vpBuffer;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Buffered: Set to true if the viewport should buffer its content.

Viewport buffering is enabled by setting this field to `true` prior to initialisation.  A @Bitmap buffer will be
created at the first drawing operation, and is available for the client to read from the #Buffer field.

Potential reasons for enabling viewport buffering include: 1. Allows the client to read the rendered graphics
directly from the #Buffer; 2. Overall rendering will be faster if the content of the viewport rarely changes; 3. The
use of multiple buffers can improve threaded rendering.

Buffering comes at a cost of using extra memory, and rendering may be less efficient if the buffered content
changes frequently (e.g. is animated).  Buffering also enforces overflow (clipping) restrictions, equivalent to
#Overflow being permanently set to `HIDDEN`.

*********************************************************************************************************************/

static ERR VIEW_GET_Buffered(extVectorViewport *Self, int &Value)
{
   Value = Self->vpBuffered;
   return ERR::Okay;
}

static ERR VIEW_SET_Buffered(extVectorViewport *Self, int Value)
{
   Self->vpBuffered = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
DragCallback: Receiver for drag requests originating from the viewport.

Set the DragCallback field with a callback function to receive drag requests from the viewport's user input.  When the
user drags the viewport, the callback will receive the user's desired `(X, Y)` target coordinates.  For unimpeded
dragging, have the callback set the viewport's #X and #Y values to match the incoming coordinates, then redraw the scene.

The prototype for the callback function is as follows, where `OriginX` and `OriginY` refer to the (#X,#Y) position of
the vector at initiation of the drag.

`void function(*VectorViewport, DOUBLE X, DOUBLE Y, DOUBLE OriginX, DOUBLE OriginY)`

Setting this field to `NULL` will turn off the callback.

It is required that the parent @VectorScene is associated with a @Surface for this feature to work.

*********************************************************************************************************************/

static ERR VIEW_GET_DragCallback(extVectorViewport *Self, FUNCTION * &Value)
{
   if (Self->vpDragCallback.defined()) {
      Value = &Self->vpDragCallback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR VIEW_SET_DragCallback(extVectorViewport *Self, FUNCTION *Value)
{
   if (Value) {
      if ((!Self->Scene) or (!Self->Scene->SurfaceID)) {
         kt::Log log;
         return log.warning(ERR::FieldNotSet);
      }

      if (Self->subscribeInput(JTYPE::MOVEMENT|JTYPE::BUTTON, C_FUNCTION(drag_callback)) != ERR::Okay) {
         return ERR::Function;
      }

      Self->vpDragCallback = *Value;
   }
   else {
      Self->vpDragCallback.clear();
      Self->subscribeInput(JTYPE::NIL, C_FUNCTION(drag_callback));
   }
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Height: The height of the viewport's target area.

The height of the viewport's target area is defined here as a fixed or scaled value.  The default value is `100%` for
full coverage.

*********************************************************************************************************************/

static ERR VIEW_GET_Height(extVectorViewport *Self, Unit &Value)
{
   gen_vector_tree(Self);

   if (Value.verbatim()) {
      Value = Self->vpTargetHeight;
      Value.Type |= FD_PURE;
      return ERR::Okay;
   }

   auto parent_height = viewport_parent_height(Self);
   double val;

   if (Self->vpTargetHeight.defined()) val = unit_to_fixed(Self->vpTargetHeight, parent_height);
   else if (Self->vpTargetYO.defined()) {
      auto y = Self->vpTargetY.defined() ? unit_to_fixed(Self->vpTargetY, parent_height) : 0;
      val = parent_height - unit_to_fixed(Self->vpTargetYO, parent_height) - y;
   }
   else val = parent_height;

   Value = fixed_to_requested_unit(val, parent_height, Value.scaled());
   return ERR::Okay;
}

static ERR VIEW_SET_Height(extVectorViewport *Self, Unit &Value)
{
   Self->vpTargetHeight = Value;
   mark_dirty((extVector *)Self, RC::DIRTY);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Overflow: Clipping options for the viewport's boundary.

Choose an overflow option to enforce or disable clipping of the viewport's content.  The default state is `VISIBLE`.
Altering the overflow state affects both the X and Y axis.  To set either axis independently, set #OverflowX and
#OverflowY.

If the viewport's #AspectRatio is set to `SLICE` then it will have priority over the overflow setting.

*********************************************************************************************************************/

static ERR VIEW_GET_Overflow(extVectorViewport *Self, VOF &Value)
{
   Value = Self->vpOverflowX;
   return ERR::Okay;
}

static ERR VIEW_SET_Overflow(extVectorViewport *Self, VOF Value)
{
   Self->vpOverflowX = Value;
   Self->vpOverflowY = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OverflowX: Clipping options for the viewport's boundary on the x axis.

Choose an overflow option to enforce or disable clipping of the viewport's content.  The default state is `VISIBLE`.
If the viewport's #AspectRatio is set to `SLICE` then it will have priority over the overflow setting.

This option controls the x axis only.

*********************************************************************************************************************/

static ERR VIEW_GET_OverflowX(extVectorViewport *Self, VOF &Value)
{
   Value = Self->vpOverflowX;
   return ERR::Okay;
}

static ERR VIEW_SET_OverflowX(extVectorViewport *Self, VOF Value)
{
   Self->vpOverflowX = Value;
   mark_buffers_for_refresh(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OverflowY: Clipping options for the viewport's boundary on the y axis.

Choose an overflow option to enforce or disable clipping of the viewport's content.  The default state is `VISIBLE`.
If the viewport's #AspectRatio is set to `SLICE` then it will have priority over the overflow setting.

This option controls the y axis only.

*********************************************************************************************************************/

static ERR VIEW_GET_OverflowY(extVectorViewport *Self, VOF &Value)
{
   Value = Self->vpOverflowY;
   return ERR::Okay;
}

static ERR VIEW_SET_OverflowY(extVectorViewport *Self, VOF Value)
{
   Self->vpOverflowY = Value;
   mark_buffers_for_refresh(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
ViewHeight: The height of the viewport's source area.

The area defined by (#ViewX,#ViewY) and (#ViewWidth,#ViewHeight) declare the source area covered by the viewport.  The
rendered graphics in the source area will be repositioned and scaled to the area defined by `(X,Y)` and
`(Width,Height)`.

*********************************************************************************************************************/

static ERR VIEW_GET_ViewHeight(extVectorViewport *Self, double &Value)
{
   Value = Self->vpViewHeight;
   return ERR::Okay;
}

static ERR VIEW_SET_ViewHeight(extVectorViewport *Self, double Value)
{
   if (Value > 0.0) {
      Self->vpViewHeight = Value;
      mark_dirty((extVector *)Self, RC::DIRTY);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
ViewX: The horizontal position of the viewport's source area.

The area defined by (#ViewX,#ViewY) and (#ViewWidth,#ViewHeight) declare the source area covered by the viewport.  The
rendered graphics in the source area will be repositioned and scaled to the area defined by (#X,#Y) and
(#Width,#Height).

*********************************************************************************************************************/

static ERR VIEW_GET_ViewX(extVectorViewport *Self, double &Value)
{
   Value = Self->vpViewX;
   return ERR::Okay;
}

static ERR VIEW_SET_ViewX(extVectorViewport *Self, double Value)
{
   Self->vpViewX = Value;
   mark_dirty((extVector *)Self, RC::DIRTY);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
ViewWidth: The width of the viewport's source area.

The area defined by (#ViewX,#ViewY) and (#ViewWidth,#ViewHeight) declare the source area covered by the viewport.  The
rendered graphics in the source area will be repositioned and scaled to the area defined by (#X,#Y) and
(#Width,#Height).

*********************************************************************************************************************/

static ERR VIEW_GET_ViewWidth(extVectorViewport *Self, double &Value)
{
   Value = Self->vpViewWidth;
   return ERR::Okay;
}

static ERR VIEW_SET_ViewWidth(extVectorViewport *Self, double Value)
{
   if (Value > 0.0) {
      Self->vpViewWidth = Value;
      mark_dirty((extVector *)Self, RC::DIRTY);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
ViewY: The vertical position of the viewport's source area.

The area defined by (#ViewX,#ViewY) and (#ViewWidth,#ViewHeight) declare the source area covered by the viewport.  The
rendered graphics in the source area will be repositioned and scaled to the area defined by (#X,#Y) and
(#Width,#Height).

*********************************************************************************************************************/

static ERR VIEW_GET_ViewY(extVectorViewport *Self, double &Value)
{
   Value = Self->vpViewY;
   return ERR::Okay;
}

static ERR VIEW_SET_ViewY(extVectorViewport *Self, double Value)
{
   Self->vpViewY = Value;
   mark_dirty((extVector *)Self, RC::DIRTY);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Width: The width of the viewport's target area.

The width of the viewport's target area is defined here as a fixed or scaled value.  The default value is `100%` for
full coverage.

*********************************************************************************************************************/

static ERR VIEW_GET_Width(extVectorViewport *Self, Unit &Value)
{
   gen_vector_tree(Self);

   if (Value.verbatim()) {
      Value = Self->vpTargetWidth;
      Value.Type |= FD_PURE;
      return ERR::Okay;
   }

   auto parent_width = viewport_parent_width(Self);
   double val;

   if (Self->vpTargetWidth.defined()) val = unit_to_fixed(Self->vpTargetWidth, parent_width);
   else if (Self->vpTargetXO.defined()) {
      auto x = Self->vpTargetX.defined() ? unit_to_fixed(Self->vpTargetX, parent_width) : 0;
      val = parent_width - unit_to_fixed(Self->vpTargetXO, parent_width) - x;
   }
   else val = parent_width;

   Value = fixed_to_requested_unit(val, parent_width, Value.scaled());
   return ERR::Okay;
}

static ERR VIEW_SET_Width(extVectorViewport *Self, Unit &Value)
{
   Self->vpTargetWidth = Value;
   mark_dirty((extVector *)Self, RC::DIRTY);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
X: Positions the viewport on the x-axis.

The display position targeted by the viewport is declared by the (#X,#Y) field values.  Coordinates can be expressed
as fixed or scaled pixel units.

If an offset from the edge of the parent is desired, the #XOffset field must be defined.  If a X and #XOffset value
are defined together, the width of the viewport is computed on-the-fly and will change in response to the parent's
width.

*********************************************************************************************************************/

static ERR VIEW_GET_X(extVectorViewport *Self, Unit &Value)
{
   gen_vector_tree(Self);

   if (Value.verbatim()) {
      Value = Self->vpTargetX;
      Value.Type |= FD_PURE;
      return ERR::Okay;
   }

   auto parent_width = viewport_parent_width(Self);
   double value = 0;

   if (Self->vpTargetX.defined()) value = unit_to_fixed(Self->vpTargetX, parent_width);
   else if (Self->vpTargetWidth.defined() and Self->vpTargetXO.defined()) {
      value = parent_width - unit_to_fixed(Self->vpTargetWidth, parent_width) -
         unit_to_fixed(Self->vpTargetXO, parent_width);
   }

   Value = fixed_to_requested_unit(value, parent_width, Value.scaled());
   return ERR::Okay;
}

static ERR VIEW_SET_X(extVectorViewport *Self, Unit &Value)
{
   Self->vpTargetX = Value;
   mark_dirty((extVector *)Self, RC::DIRTY);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
XOffset: Positions the viewport on the x-axis.

The display position targeted by the viewport is declared by the (#X,#Y) field values.  Coordinates can be expressed
as fixed or scaled pixel units.

If an offset from the edge of the parent is desired, the #XOffset field must be defined.  If the #X and XOffset
values are defined together, the width of the viewport is computed on-the-fly and will change in response to the
parent's width.

*********************************************************************************************************************/

static ERR VIEW_GET_XOffset(extVectorViewport *Self, Unit &Value)
{
   gen_vector_tree(Self);

   if (Value.verbatim()) {
      Value = Self->vpTargetXO;
      Value.Type |= FD_PURE;
      return ERR::Okay;
   }

   auto parent_width = viewport_parent_width(Self);
   double value = 0;

   if (Self->vpTargetXO.defined()) value = unit_to_fixed(Self->vpTargetXO, parent_width);
   else if (Self->vpTargetX.defined() and Self->vpTargetWidth.defined()) {
      value = parent_width - unit_to_fixed(Self->vpTargetX, parent_width) -
         unit_to_fixed(Self->vpTargetWidth, parent_width);
   }

   Value = fixed_to_requested_unit(value, parent_width, Value.scaled());
   return ERR::Okay;
}

static ERR VIEW_SET_XOffset(extVectorViewport *Self, Unit &Value)
{
   Self->vpTargetXO = Value;
   mark_dirty((extVector *)Self, RC::DIRTY);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Y: Positions the viewport on the y-axis.

The display position targeted by the viewport is declared by the (#X,#Y) field values.  Coordinates can be expressed as
fixed or scaled pixel units.

If an offset from the edge of the parent is desired, the #YOffset must be defined.  If the Y and #YOffset values are
defined together, the height of the viewport is computed on-the-fly and will change in response to the parent's
height.

*********************************************************************************************************************/

static ERR VIEW_GET_Y(extVectorViewport *Self, Unit &Value)
{
   gen_vector_tree(Self);

   if (Value.verbatim()) {
      Value = Self->vpTargetY;
      Value.Type |= FD_PURE;
      return ERR::Okay;
   }

   auto parent_height = viewport_parent_height(Self);
   double value = 0;

   if (Self->vpTargetY.defined()) value = unit_to_fixed(Self->vpTargetY, parent_height);
   else if (Self->vpTargetHeight.defined() and Self->vpTargetYO.defined()) {
      value = parent_height - unit_to_fixed(Self->vpTargetHeight, parent_height) -
         unit_to_fixed(Self->vpTargetYO, parent_height);
   }

   Value = fixed_to_requested_unit(value, parent_height, Value.scaled());
   return ERR::Okay;
}

static ERR VIEW_SET_Y(extVectorViewport *Self, Unit &Value)
{
   Self->vpTargetY = Value;
   mark_dirty((extVector *)Self, RC::DIRTY);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
YOffset: Positions the viewport on the y-axis.

The display position targeted by the viewport is declared by the (#X,#Y) field values.  Coordinates can be expressed as
fixed or scaled pixel units.

If an offset from the edge of the parent is desired, the #YOffset must be defined.  If a #Y and YOffset value are
defined together, the height of the viewport is computed on-the-fly and will change in response to the parent's
height.

*********************************************************************************************************************/

static ERR VIEW_GET_YOffset(extVectorViewport *Self, Unit &Value)
{
   gen_vector_tree(Self);

   if (Value.verbatim()) {
      Value = Self->vpTargetYO;
      Value.Type |= FD_PURE;
      return ERR::Okay;
   }

   auto parent_height = viewport_parent_height(Self);
   double value = 0;

   if (Self->vpTargetYO.defined()) value = unit_to_fixed(Self->vpTargetYO, parent_height);
   else if (Self->vpTargetY.defined() and Self->vpTargetHeight.defined()) {
      value = parent_height - unit_to_fixed(Self->vpTargetY, parent_height) -
         unit_to_fixed(Self->vpTargetHeight, parent_height);
   }

   Value = fixed_to_requested_unit(value, parent_height, Value.scaled());
   return ERR::Okay;
}

static ERR VIEW_SET_YOffset(extVectorViewport *Self, Unit &Value)
{
   Self->vpTargetYO = Value;
   mark_dirty((extVector *)Self, RC::DIRTY);
   return ERR::Okay;
}

//********************************************************************************************************************

#include "viewport_def.cpp"

static const FieldArray clViewFields[] = {
   { "AbsX",         FDF_VIRTUAL|FDF_INT|FDF_R, VIEW_GET_AbsX },
   { "AbsY",         FDF_VIRTUAL|FDF_INT|FDF_R, VIEW_GET_AbsY },
   { "AspectRatio",  FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW|FDF_PURE, VIEW_GET_AspectRatio, VIEW_SET_AspectRatio, &clAspectRatio },
   { "Buffer",       FDF_VIRTUAL|FDF_OBJECT|FDF_R|FDF_PURE, VIEW_GET_Buffer },
   { "Buffered",     FDF_VIRTUAL|FDF_INT|FDF_RI|FDF_PURE, VIEW_GET_Buffered, VIEW_SET_Buffered },
   { "DragCallback", FDF_VIRTUAL|FDF_FUNCTION|FDF_RW|FDF_PURE, VIEW_GET_DragCallback, VIEW_SET_DragCallback },
   { "Overflow",     FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW|FDF_PURE, VIEW_GET_Overflow, VIEW_SET_Overflow, &clVectorViewportVOF },
   { "OverflowX",    FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW|FDF_PURE, VIEW_GET_OverflowX, VIEW_SET_OverflowX, &clVectorViewportVOF },
   { "OverflowY",    FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW|FDF_PURE, VIEW_GET_OverflowY, VIEW_SET_OverflowY, &clVectorViewportVOF },
   { "X",            FDF_VIRTUAL|FDF_UNIT|FDF_SCALED|FDF_RW, VIEW_GET_X,       VIEW_SET_X },
   { "Y",            FDF_VIRTUAL|FDF_UNIT|FDF_SCALED|FDF_RW, VIEW_GET_Y,       VIEW_SET_Y },
   { "XOffset",      FDF_VIRTUAL|FDF_UNIT|FDF_SCALED|FDF_RW, VIEW_GET_XOffset, VIEW_SET_XOffset },
   { "YOffset",      FDF_VIRTUAL|FDF_UNIT|FDF_SCALED|FDF_RW, VIEW_GET_YOffset, VIEW_SET_YOffset },
   { "Width",        FDF_VIRTUAL|FDF_UNIT|FDF_SCALED|FDF_RW, VIEW_GET_Width,   VIEW_SET_Width },
   { "Height",       FDF_VIRTUAL|FDF_UNIT|FDF_SCALED|FDF_RW, VIEW_GET_Height,  VIEW_SET_Height },
   { "ViewX",        FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, VIEW_GET_ViewX,      VIEW_SET_ViewX },
   { "ViewY",        FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, VIEW_GET_ViewY,      VIEW_SET_ViewY },
   { "ViewWidth",    FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, VIEW_GET_ViewWidth,  VIEW_SET_ViewWidth },
   { "ViewHeight",   FDF_VIRTUAL|FDF_DOUBLE|FDF_RW|FDF_PURE, VIEW_GET_ViewHeight, VIEW_SET_ViewHeight },
   END_FIELD
};

static ERR init_viewport(void)
{
   clVectorViewport = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTOR),
      fl::ClassID(CLASSID::VECTORVIEWPORT),
      fl::Name("VectorViewport"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clVectorViewportActions),
      fl::Fields(clViewFields),
      fl::Size(sizeof(extVectorViewport)),
      fl::Path(MOD_PATH));

   return clVectorViewport ? ERR::Okay : ERR::AddClass;
}

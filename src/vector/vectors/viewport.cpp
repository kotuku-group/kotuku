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
               pf::SwitchContext context(Viewport->vpDragCallback.Context);
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

            // Ensure that the X,Y coordinates are fixed.

            Viewport->vpDimensions = (Viewport->vpDimensions | DMF::FIXED_X | DMF::FIXED_Y) &
               (~(DMF::SCALED_X|DMF::SCALED_Y|DMF::SCALED_X_OFFSET|DMF::SCALED_Y_OFFSET));

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
   pf::vector<ChildEntry> list;
   if (ListChildren(Self->UID, &list) IS ERR::Okay) {
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

   Self->vpDimensions = (Self->vpDimensions|DMF::FIXED_X|DMF::FIXED_Y) & (~(DMF::SCALED_X|DMF::SCALED_Y));
   Self->vpTargetX += Args->DeltaX;
   Self->vpTargetY += Args->DeltaY;

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

   if ((Args->Flags & MTF::X) != MTF::NIL) {
      Self->vpDimensions = (Self->vpDimensions | DMF::FIXED_X) & (~DMF::SCALED_X);
      Self->vpTargetX = Args->X;
   }

   if ((Args->Flags & MTF::Y) != MTF::NIL) {
      Self->vpDimensions = (Self->vpDimensions | DMF::FIXED_Y) & (~DMF::SCALED_Y);
      Self->vpTargetY = Args->Y;
   }

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

   Self->vpDimensions = (Self->vpDimensions|DMF::FIXED_X|DMF::FIXED_Y|DMF::FIXED_WIDTH|DMF::FIXED_HEIGHT) &
      (~(DMF::SCALED_X|DMF::SCALED_Y|DMF::SCALED_WIDTH|DMF::SCALED_HEIGHT));

   Self->vpTargetX      = Args->X;
   Self->vpTargetY      = Args->Y;
   Self->vpTargetWidth  = Args->Width;
   Self->vpTargetHeight = Args->Height;

   if (Self->vpTargetWidth < 1) Self->vpTargetWidth = 1;
   if (Self->vpTargetHeight < 1) Self->vpTargetHeight = 1;
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

   Self->vpDimensions = (Self->vpDimensions | DMF::FIXED_WIDTH) & (~DMF::SCALED_WIDTH);
   Self->vpTargetWidth = Args->Width;

   Self->vpDimensions = (Self->vpDimensions | DMF::FIXED_HEIGHT) & (~DMF::SCALED_HEIGHT);
   Self->vpTargetHeight = Args->Height;

   if (Self->vpTargetWidth < 1) Self->vpTargetWidth = 1;
   if (Self->vpTargetHeight < 1) Self->vpTargetHeight = 1;
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
Dimensions: Dimension flags define whether individual dimension fields contain fixed or scaled values.
Lookup: DMF

<types lookup="DMF"/>

*********************************************************************************************************************/

static ERR VIEW_GET_Dimensions(extVectorViewport *Self, DMF &Value)
{
   Value = Self->vpDimensions;
   return ERR::Okay;
}

static ERR VIEW_SET_Dimensions(extVectorViewport *Self, DMF Value)
{
   Self->vpDimensions = Value;
   mark_dirty((extVector *)Self, RC::DIRTY);
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
         pf::Log log;
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

The fixed value is always returned when retrieving the height.

*********************************************************************************************************************/

static ERR VIEW_GET_Height(extVectorViewport *Self, Unit &Value)
{
   double val;

   gen_vector_tree(Self);

   if (dmf::hasHeight(Self->vpDimensions)) { // Working with a fixed dimension
      if (Value.Type & FD_SCALED) {
         if (Self->ParentView) val = Self->vpFixedHeight * Self->ParentView->vpFixedHeight;
         else val = Self->vpFixedHeight * Self->Scene->PageHeight;
      }
      else val = Self->vpTargetHeight;
   }
   else if (dmf::hasScaledHeight(Self->vpDimensions)) { // Working with a scaled dimension
      if (Value.Type & FD_SCALED) val = Self->vpTargetHeight;
      else if (Self->ParentView) val = Self->vpTargetHeight * Self->ParentView->vpFixedHeight;
      else val = Self->vpTargetHeight * Self->Scene->PageHeight;
   }
   else if (dmf::hasAnyYOffset(Self->vpDimensions)) {
      double y, parent_height;

      if (dmf::hasScaledY(Self->vpDimensions)) y = Self->vpTargetY * Self->ParentView->vpFixedHeight;
      else y = Self->vpTargetY;

      if (Self->ParentView) Self->ParentView->get(FID_Height, parent_height);
      else parent_height = Self->Scene->PageHeight;

      if (dmf::hasYOffset(Self->vpDimensions)) val = parent_height - Self->vpTargetYO - y;
      else val = parent_height - (Self->vpTargetYO * parent_height) - y;
   }
   else { // If no height set by the client, the full height is inherited from the parent
      if (Self->ParentView) return Self->ParentView->get(FID_Height, Value);
      else Self->Scene->get(FID_PageHeight, val);
   }

   Value.set(val);
   return ERR::Okay;
}

static ERR VIEW_SET_Height(extVectorViewport *Self, Unit &Value)
{
   Self->vpTargetHeight = Value;
   if (Value.scaled()) Self->vpDimensions = (Self->vpDimensions | DMF::SCALED_HEIGHT) & (~DMF::FIXED_HEIGHT);
   else Self->vpDimensions = (Self->vpDimensions | DMF::FIXED_HEIGHT) & (~DMF::SCALED_HEIGHT);
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
   double val;

   gen_vector_tree(Self);

   if (dmf::hasWidth(Self->vpDimensions)) { // Working with a fixed dimension
      if (Value.Type & FD_SCALED) {
         if (Self->ParentView) val = Self->vpFixedWidth * Self->ParentView->vpFixedWidth;
         else val = Self->vpFixedWidth * Self->Scene->PageWidth;
      }
      else val = Self->vpTargetWidth;
   }
   else if (dmf::hasScaledWidth(Self->vpDimensions)) { // Working with a scaled dimension
      if (Value.Type & FD_SCALED) val = Self->vpTargetWidth;
      else if (Self->ParentView) val = Self->vpTargetWidth * Self->ParentView->vpFixedWidth;
      else val = Self->vpTargetWidth * Self->Scene->PageWidth;
   }
   else if (dmf::hasAnyXOffset(Self->vpDimensions)) {
      double x, parent_width;

      if (dmf::hasScaledX(Self->vpDimensions)) x = Self->vpTargetX * Self->ParentView->vpFixedWidth;
      else x = Self->vpTargetX;

      if (Self->ParentView) Self->ParentView->get(FID_Width, parent_width);
      else parent_width = Self->Scene->PageWidth;

      if (dmf::hasXOffset(Self->vpDimensions)) val = parent_width - Self->vpTargetXO - x;
      else val = parent_width - (Self->vpTargetXO * parent_width) - x;
   }
   else { // If no width set by the client, the full width is inherited from the parent
      if (Self->ParentView) return Self->ParentView->get(FID_Width, Value);
      else Self->Scene->get(FID_PageWidth, val);
   }

   Value.set(val);
   return ERR::Okay;
}

static ERR VIEW_SET_Width(extVectorViewport *Self, Unit &Value)
{
   Self->vpTargetWidth = Value;
   if (Value.scaled()) Self->vpDimensions = (Self->vpDimensions | DMF::SCALED_WIDTH) & (~DMF::FIXED_WIDTH);
   else Self->vpDimensions = (Self->vpDimensions | DMF::FIXED_WIDTH) & (~DMF::SCALED_WIDTH);
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
   double width, value;

   gen_vector_tree(Self);

   if (dmf::hasX(Self->vpDimensions)) value = Self->vpTargetX;
   else if (dmf::hasScaledX(Self->vpDimensions)) {
      value = Self->vpTargetX * Self->ParentView->vpFixedWidth;
   }
   else if ((dmf::hasAnyWidth(Self->vpDimensions)) and (dmf::hasAnyXOffset(Self->vpDimensions))) {
      if (dmf::hasWidth(Self->vpDimensions)) width = Self->vpTargetWidth;
      else width = Self->ParentView->vpFixedWidth * Self->vpTargetWidth;
      if (dmf::hasXOffset(Self->vpDimensions)) value = Self->ParentView->vpFixedWidth - width - Self->vpTargetXO;
      else value = Self->ParentView->vpFixedWidth - width - (Self->ParentView->vpFixedWidth * Self->vpTargetXO);
   }
   else value = 0;

   if (Value.scaled()) Value.set(value / Self->ParentView->vpFixedWidth);
   else Value.set(value);
   return ERR::Okay;
}

static ERR VIEW_SET_X(extVectorViewport *Self, Unit &Value)
{
   Self->vpTargetX = Value;
   if (Value.scaled()) Self->vpDimensions = (Self->vpDimensions | DMF::SCALED_X) & (~DMF::FIXED_X);
   else Self->vpDimensions = (Self->vpDimensions | DMF::FIXED_X) & (~DMF::SCALED_X);
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
   double width;
   double value = 0;

   gen_vector_tree(Self);

   if (dmf::hasXOffset(Self->vpDimensions)) value = Self->vpTargetXO;
   else if (dmf::hasScaledYOffset(Self->vpDimensions)) {
      value = Self->vpTargetXO * Self->ParentView->vpFixedWidth;
   }
   else if ((dmf::hasAnyX(Self->vpDimensions)) and (dmf::hasAnyWidth(Self->vpDimensions))) {
      if (dmf::hasWidth(Self->vpDimensions)) width = Self->vpTargetWidth;
      else width = Self->ParentView->vpFixedWidth * Self->vpTargetWidth;

      if (dmf::hasX(Self->vpDimensions)) value = Self->ParentView->vpFixedWidth - (Self->vpTargetX + width);
      else value = Self->ParentView->vpFixedWidth - ((Self->vpTargetX * Self->ParentView->vpFixedWidth) + width);
   }
   else value = 0;

   if (Value.scaled()) Value.set(value / Self->ParentView->vpFixedWidth);
   else Value.set(value);
   return ERR::Okay;
}

static ERR VIEW_SET_XOffset(extVectorViewport *Self, Unit &Value)
{
   Self->vpTargetXO = Value;
   if (Value.scaled()) Self->vpDimensions = (Self->vpDimensions | DMF::SCALED_X_OFFSET) & (~DMF::FIXED_X_OFFSET);
   else Self->vpDimensions = (Self->vpDimensions | DMF::FIXED_X_OFFSET) & (~DMF::SCALED_X_OFFSET);
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
   double value, height;

   gen_vector_tree(Self);

   if (dmf::hasY(Self->vpDimensions)) value = Self->vpTargetY;
   else if (dmf::hasScaledY(Self->vpDimensions)) {
      value = Self->vpTargetY * Self->ParentView->vpFixedHeight;
   }
   else if ((dmf::hasAnyHeight(Self->vpDimensions)) and (dmf::hasAnyYOffset(Self->vpDimensions))) {
      if (dmf::hasHeight(Self->vpDimensions)) height = Self->vpTargetHeight;
      else height = Self->ParentView->vpFixedHeight * Self->vpTargetHeight;

      if (dmf::hasYOffset(Self->vpDimensions)) value = Self->ParentView->vpFixedHeight - height - Self->vpTargetYO;
      else value = Self->ParentView->vpFixedHeight - height - (Self->ParentView->vpFixedHeight * Self->vpTargetYO);
   }
   else value = 0;

   if (Value.scaled()) Value.set(value / Self->ParentView->vpFixedHeight);
   else Value.set(value);
   return ERR::Okay;
}

static ERR VIEW_SET_Y(extVectorViewport *Self, Unit &Value)
{
   Self->vpTargetY = Value;
   if (Value.scaled()) Self->vpDimensions = (Self->vpDimensions | DMF::SCALED_Y) & (~DMF::FIXED_Y);
   else Self->vpDimensions = (Self->vpDimensions | DMF::FIXED_Y) & (~DMF::SCALED_Y);
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
   double height;
   double value = 0;

   gen_vector_tree(Self);

   if (dmf::hasYOffset(Self->vpDimensions)) value = Self->vpTargetYO;
   else if (dmf::hasScaledYOffset(Self->vpDimensions)) {
      value = Self->vpTargetYO * Self->ParentView->vpFixedHeight;
   }
   else if ((dmf::hasAnyY(Self->vpDimensions)) and (dmf::hasAnyHeight(Self->vpDimensions))) {
      if (dmf::hasHeight(Self->vpDimensions)) height = Self->vpTargetHeight;
      else height = Self->ParentView->vpFixedHeight * Self->vpTargetHeight;

      if (dmf::hasY(Self->vpDimensions)) value = Self->ParentView->vpFixedHeight - (Self->vpTargetY + height);
      else value = Self->ParentView->vpFixedHeight - ((Self->vpTargetY * Self->ParentView->vpFixedHeight) + height);
   }
   else value = 0;

   if (Value.scaled()) Value.set(value / Self->ParentView->vpFixedHeight);
   else Value.set(value);
   return ERR::Okay;
}

static ERR VIEW_SET_YOffset(extVectorViewport *Self, Unit &Value)
{
   Self->vpTargetYO = Value;
   if (Value.scaled()) Self->vpDimensions = (Self->vpDimensions | DMF::SCALED_Y_OFFSET) & (~DMF::FIXED_Y_OFFSET);
   else Self->vpDimensions = (Self->vpDimensions | DMF::FIXED_Y_OFFSET) & (~DMF::SCALED_Y_OFFSET);
   mark_dirty((extVector *)Self, RC::DIRTY);
   return ERR::Okay;
}

//********************************************************************************************************************

#include "viewport_def.cpp"

static const FieldDef clViewDimensions[] = {
   { "ScaledX",      DMF::SCALED_X },
   { "ScaledY",      DMF::SCALED_Y },
   { "ScaledWidth",  DMF::SCALED_WIDTH },
   { "ScaledHeight", DMF::SCALED_HEIGHT },
   { "FixedX",       DMF::FIXED_X },
   { "FixedY",       DMF::FIXED_Y },
   { "FixedWidth",   DMF::FIXED_WIDTH },
   { "FixedHeight",  DMF::FIXED_HEIGHT },
   { nullptr, 0 }
};

static const FieldArray clViewFields[] = {
   { "AbsX",         FDF_VIRTUAL|FDF_INT|FDF_R, VIEW_GET_AbsX },
   { "AbsY",         FDF_VIRTUAL|FDF_INT|FDF_R, VIEW_GET_AbsY },
   { "AspectRatio",  FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW, VIEW_GET_AspectRatio, VIEW_SET_AspectRatio, &clAspectRatio },
   { "Buffer",       FDF_VIRTUAL|FDF_OBJECT|FDF_R, VIEW_GET_Buffer },
   { "Buffered",     FDF_VIRTUAL|FDF_INT|FDF_RI, VIEW_GET_Buffered, VIEW_SET_Buffered },
   { "Dimensions",   FDF_VIRTUAL|FDF_INTFLAGS|FDF_R, VIEW_GET_Dimensions, VIEW_SET_Dimensions, &clViewDimensions },
   { "DragCallback", FDF_VIRTUAL|FDF_FUNCTIONPTR|FDF_RW, VIEW_GET_DragCallback, VIEW_SET_DragCallback },
   { "Overflow",     FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW, VIEW_GET_Overflow, VIEW_SET_Overflow, &clVectorViewportVOF },
   { "OverflowX",    FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW, VIEW_GET_OverflowX, VIEW_SET_OverflowX, &clVectorViewportVOF },
   { "OverflowY",    FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW, VIEW_GET_OverflowY, VIEW_SET_OverflowY, &clVectorViewportVOF },
   { "X",            FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VIEW_GET_X,       VIEW_SET_X },
   { "Y",            FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VIEW_GET_Y,       VIEW_SET_Y },
   { "XOffset",      FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VIEW_GET_XOffset, VIEW_SET_XOffset },
   { "YOffset",      FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VIEW_GET_YOffset, VIEW_SET_YOffset },
   { "Width",        FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VIEW_GET_Width,   VIEW_SET_Width },
   { "Height",       FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VIEW_GET_Height,  VIEW_SET_Height },
   { "ViewX",        FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, VIEW_GET_ViewX,      VIEW_SET_ViewX },
   { "ViewY",        FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, VIEW_GET_ViewY,      VIEW_SET_ViewY },
   { "ViewWidth",    FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, VIEW_GET_ViewWidth,  VIEW_SET_ViewWidth },
   { "ViewHeight",   FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, VIEW_GET_ViewHeight, VIEW_SET_ViewHeight },
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

/*********************************************************************************************************************

-FIELD-
BitsPerPixel: Defines the pixel depth used by the surface buffer.

Set `BitsPerPixel` before initialisation to request a fixed pixel depth for the surface buffer.  If the requested
depth differs from the display or parent buffer, the graphics system converts pixels when the surface is drawn, which
is usually slower than using the native display depth.

Reading this field returns the active tracked depth for the surface.  A value of zero is returned if the surface is not
available in the display surface list.

*********************************************************************************************************************/

static ERR GET_BitsPerPixel(extSurface *Self, int *Value)
{
   SURFACEINFO *info;
   if (!gfx::GetSurfaceInfo(Self->UID, &info)) {
      *Value = info->BitsPerPixel;
   }
   else *Value = 0;
   return ERR::Okay;
}

static ERR SET_BitsPerPixel(extSurface *Self, int Value)
{
   Self->BitsPerPixel = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Buffer: Refers to the @Bitmap that stores the surface's graphics.

Each surface is assigned a bitmap buffer for drawing.  In many cases this buffer is shared between multiple surfaces,
so clients should treat it as an implementation detail unless no public drawing API provides the required access.

The bitmap is an off-screen buffer.  Direct changes to it are not exposed to the display automatically.

-FIELD-
Colour: Defines the background colour used when clearing the surface.

Set this field when a surface should be cleared to a solid background colour before drawing.  String values may use
`#RRGGBB` hexadecimal notation or `Red,Green,Blue` decimal components.

A surface that does not define a colour is not cleared during redraw.  In that case, draw the full background manually
to avoid stale or uninitialised graphics appearing in exposed areas.

A preset colour can be disabled by writing `NULL`, which clears the colour by setting its alpha component to zero.
Changing `Colour` does not schedule a redraw.

-FIELD-
Cursor: Sets the pointer image used while the mouse is over the surface.

The pointer automatically switches to the selected cursor image when it enters the surface area.

The available cursor image settings are listed in the @Pointer.CursorID documentation.

The `Cursor` field may be written with a valid cursor name or cursor ID.

*********************************************************************************************************************/

static ERR SET_Cursor(extSurface *Self, PTC Value)
{
   Self->Cursor = Value;
   if (Self->initialised()) {
      UpdateSurfaceField(Self, &SurfaceRecord::Cursor, (int8_t)Self->Cursor);

      if (auto pointer = gfx::AccessPointer()) {
         acRefresh(pointer);
         ReleaseObject(pointer);
      }
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Display: Refers to the @Display object that manages the surface's graphics.

All surfaces belong to a @Display object that manages drawing to the user's video display.  This field identifies the
display that owns the surface.

-FIELD-
Drag: Defines the @Surface that moves when this surface is dragged.

Set this field to the @Surface that should move when the user starts a click-drag operation over the current surface.
For example, a window title bar would set `Drag` to the window surface.  A surface may also set `Drag` to itself for
icon-like or small draggable objects.

Set `Drag` to zero to disable drag handling.

*********************************************************************************************************************/

static ERR SET_Drag(extSurface *Self, OBJECTID Value)
{
   if (Value) {
      auto callback = C_FUNCTION(consume_input_events);
      if (!gfx::SubscribeInput(&callback, Self->UID, JTYPE::MOVEMENT|JTYPE::BUTTON, 0, &Self->InputHandle)) {
         Self->DragID = Value;
         return ERR::Okay;
      }
      else return ERR::Failed;
   }
   else {
      if (Self->InputHandle) { gfx::UnsubscribeInput(Self->InputHandle); Self->InputHandle = 0; }
      Self->DragID = 0;
      return ERR::Okay;
   }
}

/*********************************************************************************************************************

-FIELD-
DragStatus: Reports the current drag state when dragging is enabled.

Read this field to determine whether the surface is idle, anchored for dragging or actively being moved.

-FIELD-
Flags: Controls optional surface behaviour.

Use `Flags` to enable optional surface behaviours.  Preserve existing flags when updating this field, typically by
combining the current value with the flags being added.

Read-only flags cannot be changed by writing this field.  Initialisation-only flags can be set before the surface is
initialised, but are preserved once the surface is active.

*********************************************************************************************************************/

static ERR SET_Flags(extSurface *Self, RNF Value)
{
   auto flags = (Self->Flags & RNF::READ_ONLY) | (Value & (~RNF::READ_ONLY));

   if (Self->initialised()) flags = flags & (~RNF::INIT_ONLY);

   if (flags != Self->Flags) {
      Self->Flags = flags;
      UpdateSurfaceField(Self, &SurfaceRecord::Flags, flags);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Modal: Sets the surface as modal (prevents user interaction with other surfaces).

If set to `true`, the surface becomes the program's modal surface when shown.  This prevents interaction with other
surfaces until the modal surface is hidden, destroyed or no longer modal.  Children of the modal surface remain
interactive.

Clearing this field restores the previous modal surface if one was recorded.

*********************************************************************************************************************/

static ERR SET_Modal(extSurface *Self, int Value)
{
   if ((!Value) and (Self->Modal)) {
      if (Self->PrevModalID) {
         gfx::SetModalSurface(Self->PrevModalID);
         Self->PrevModalID = 0;
      }
      else if (gfx::GetModalSurface() IS Self->UID) gfx::SetModalSurface(0);
   }

   Self->Modal = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Movement: Limits the movement of a surface object to vertical or horizontal shifts.
Lookup: MOVE

Set `Movement` to limit movement performed by #Move() to horizontal movement, vertical movement, both axes or neither
axis.  By default, surfaces can move horizontally and vertically.

This field affects #Move() only.  Direct writes to coordinate fields bypass the movement restriction.

*********************************************************************************************************************/

static ERR GET_Movement(extSurface *Self, int *Value)
{
   *Value = 0;
   if ((Self->Flags & RNF::NO_HORIZONTAL) IS RNF::NIL) *Value |= MOVE_HORIZONTAL;
   if ((Self->Flags & RNF::NO_VERTICAL) IS RNF::NIL) *Value |= MOVE_VERTICAL;
   return ERR::Okay;
}

static ERR SET_Movement(extSurface *Self, int Flags)
{
   if (Flags IS MOVE_HORIZONTAL) Self->Flags = (Self->Flags & RNF::NO_HORIZONTAL) | RNF::NO_VERTICAL;
   else if (Flags IS MOVE_VERTICAL) Self->Flags = (Self->Flags & RNF::NO_VERTICAL) | RNF::NO_HORIZONTAL;
   else if (Flags IS (MOVE_HORIZONTAL|MOVE_VERTICAL)) Self->Flags &= ~(RNF::NO_VERTICAL | RNF::NO_HORIZONTAL);
   else Self->Flags |= RNF::NO_HORIZONTAL|RNF::NO_VERTICAL;

   UpdateSurfaceField(Self, &SurfaceRecord::Flags, Self->Flags);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Opacity: Defines the translucency applied when drawing a surface.

`Opacity` is expressed as a normalised multiplier.  The default value of `1.0` draws the surface as fully opaque.  Lower
values make the surface more transparent.  Values outside the allowable range are clipped.

Non-opaque surfaces are drawn by rendering the surface content to its internal buffer and blending it with the
background graphics.  This can be costly, and the pre-copy feature may give better results for some compositions.

Translucency can significantly increase CPU usage.

*********************************************************************************************************************/

static ERR GET_Opacity(extSurface *Self, double *Value)
{
   *Value = Self->Opacity;
   return ERR::Okay;
}

static ERR SET_Opacity(extSurface *Self, double Value)
{
   double opacity;

   // NB: It is OK to set the opacity on a surface object when it does not own its own bitmap, as the aftercopy
   // routines will refer the copy so that it starts from the bitmap owner.

   if (Value >= 1.0) {
      opacity = 1.0;
      if (opacity IS Self->Opacity) return ERR::Okay;
      Self->Flags &= ~RNF::AFTER_COPY;
   }
   else {
      if (Value < 0.0) opacity = 0.0;
      else opacity = Value;
      if (opacity IS Self->Opacity) return ERR::Okay;
      Self->Flags |= RNF::AFTER_COPY; // See PrepareBackground() to see what these flags are for

      // NB: Currently the combination of PRECOPY and AFTERCOPY at the same time is permissible,
      // e.g. icons need this feature so that they can fade in and out of the desktop.
   }

   Self->Opacity = opacity;
   UpdateSurfaceRecord(Self); // Update Opacity, Flags

   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Parent: Identifies the parent @Surface.

Child surfaces use this field to identify their parent.  Top-level surfaces have no parent.

If `Parent` is not set before initialisation, the surface class searches the ownership chain for the nearest @Surface.
Set `Parent` to zero before initialisation to disable that automatic lookup.  An initialised child surface may be
reparented, but a top-level surface cannot be converted into a child surface after initialisation.

*********************************************************************************************************************/

static ERR SET_Parent(extSurface *Self, int Value)
{
   // To change the parent post-initialisation, we have to re-track the surface so that it is correctly repositioned
   // within the surface lists.

   if (Self->initialised()) {
      if (!Self->ParentID) return ERR::InvalidState; // Top level surfaces cannot be re-parented
      if (Self->ParentID IS Value) return ERR::Okay;

      acHide(Self);

      Self->ParentID = Value;
      Self->ParentDefined = true;

      const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

      int index, parent;
      if ((index = find_surface_list(Self)) != -1) {
         if (!Value) parent = 0;
         else for (parent=0; (glSurfaces[parent].SurfaceID) and (glSurfaces[parent].SurfaceID != Self->ParentID); parent++);

         if (glSurfaces[parent].SurfaceID) move_layer_pos(glSurfaces, index, parent + 1);

         // Reset bitmap and buffer information in the list


      }

      acShow(Self);
   }
   else {
      Self->ParentID = Value;
      Self->ParentDefined = true;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
PopOver: Keeps a surface in front of another surface in the Z order.

Set `PopOver` before initialisation to the object ID of a sibling @Surface that this surface should stay in front of.
For dialog windows, combine this field with #Modal to keep the dialog in front and prevent interaction with other
surfaces in the current program.

Set `PopOver` to zero before initialisation to use normal Z-order behaviour.

This field cannot be changed after initialisation.  The value must identify a @Surface; otherwise `ERR::InvalidObject`
is returned.

*********************************************************************************************************************/

static ERR SET_PopOver(extSurface *Self, OBJECTID Value)
{
   kt::Log log;

   if (Value IS Self->UID) return ERR::Okay;

   if (Self->initialised()) return log.warning(ERR::Immutable);

   if (Value) {
      CLASSID class_id = GetClassID(Value);
      if (class_id != CLASSID::SURFACE) {
         if (ScopedObjectLock obj(Value, 3000); obj.granted()) {
            Value = obj->get<OBJECTID>(FID_Surface);
         }
         else return ERR::AccessObject;

         if (class_id != CLASSID::SURFACE) return log.warning(ERR::InvalidObject);
      }
   }

   Self->PopOverID = Value;
   return ERR::Okay;
}

static ERR SET_RevertFocus(extSurface *Self, OBJECTID Value)
{
   Self->RevertFocusID = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
RootLayer: Private

*********************************************************************************************************************/

static ERR SET_RootLayer(extSurface *Self, OBJECTID Value)
{
   Self->RootID = Value;
   UpdateSurfaceField(Self, &SurfaceRecord::RootID, Value); // Update RootLayer
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
UserFocus: Refers to the surface object that has the current user focus.

Returns the object ID of the surface that has the primary user focus.  Returns zero if no surface has focus.

*********************************************************************************************************************/

static ERR GET_UserFocus(extSurface *Self, OBJECTID *Value)
{
   const std::lock_guard<std::recursive_mutex> lock(glFocusLock);
   *Value = glFocusList[0];
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Visible: Indicates the visibility of a surface object.

Read this field to determine whether the surface itself is marked visible.  A `true` value means the surface is shown;
`false` means it is hidden.

Effective on-screen visibility also depends on the parent chain.  A visible child of a hidden parent is not displayed.

Set this field, or call #Hide() and #Show(), to change the surface visibility.

*********************************************************************************************************************/

static ERR GET_Visible(extSurface *Self, int *Value)
{
   if (Self->visible()) *Value = TRUE;
   else *Value = FALSE;
   return ERR::Okay;
}

static ERR SET_Visible(extSurface *Self, int Value)
{
   if (Value) acShow(Self);
   else acHide(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
WindowType: Defines how a top-level surface is represented by a hosted desktop.
Lookup: SWIN

This field affects hosted desktops such as Windows and X11.  It only applies to top-level surfaces that have no parent.
Child surfaces ignore this field, and surfaces created inside the desktop area treat the desktop as their parent.

Custom surfaces remain responsible for their own window controls, such as title bars and resize borders.

*********************************************************************************************************************/

static ERR GET_WindowType(extSurface *Self, SWIN *Value)
{
   *Value = Self->WindowType;
   return ERR::Okay;
}

static ERR SET_WindowType(extSurface *Self, SWIN Value)
{
   if (Self->initialised()) {
      kt::Log log;

      if (Self->WindowType IS Value) {
         log.trace("WindowType == %d", Value);
         return ERR::Okay;
      }

      if (Self->DisplayID) {
         if (ScopedObjectLock<objDisplay> display(Self->DisplayID, 2000); display.granted()) {
            log.trace("Changing window type to %d.", Value);

            bool border;
            switch(Value) {
               case SWIN::TASKBAR:
               case SWIN::ICON_TRAY:
               case SWIN::NONE:
                  border = false;
                  break;
               default:
                  border = true;
                  break;
            }

            SCR flags;
            if (border) {
               if ((display->Flags & SCR::BORDERLESS) != SCR::NIL) {
                  flags = display->Flags & (~SCR::BORDERLESS);
                  display->setFlags(flags);
               }
            }
            else if ((display->Flags & SCR::BORDERLESS) IS SCR::NIL) {
               flags = display->Flags | SCR::BORDERLESS;
               display->setFlags(flags);
            }

            Self->WindowType = Value;
         }
         else return ERR::AccessObject;
      }
      else return log.warning(ERR::NoSupport);
   }
   else Self->WindowType = Value;

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
WindowHandle: Refers to the surface's window handle (host dependent).

This field exposes the host window handle for platforms that provide one.  It is currently relevant when creating a
primary surface within an X11 window manager or Microsoft Windows.

Set `WindowHandle` before initialisation to attach the surface to an existing native window.  The field is immutable
after initialisation.
-END-

*********************************************************************************************************************/

static ERR GET_WindowHandle(extSurface *Self, APTR *Value)
{
   *Value = (APTR)Self->DisplayWindow;
   return ERR::Okay;
}

static ERR SET_WindowHandle(extSurface *Self, APTR Value)
{
   if (Self->initialised()) return ERR::Immutable;
   if (Value) Self->DisplayWindow = Value;
   return ERR::Okay;
}

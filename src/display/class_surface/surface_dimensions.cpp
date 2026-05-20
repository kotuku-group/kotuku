
/*********************************************************************************************************************

-FIELD-
AbsX: The absolute horizontal position of a surface object.

This field returns the surface's horizontal position relative to the top-level surface in its local hierarchy.

Writing `AbsX` moves the surface so that its absolute horizontal position matches the supplied value.  The surface
must be initialised before this field can be changed.

*********************************************************************************************************************/

static ERR GET_AbsX(extSurface *Self, int *Value)
{
   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

   if (auto i = find_surface_list(Self); i != -1) {
      *Value = glSurfaces[i].Left;
      return ERR::Okay;
   }
   else return ERR::Search;
}

static ERR SET_AbsX(extSurface *Self, int Value)
{
   kt::Log log;

   if (Self->initialised()) {
      const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

      if (auto parent = find_parent_list(glSurfaces, Self); parent != -1) {
         int x = Value - glSurfaces[parent].Left;
         move_layer(Self, x, Self->Y);
         return ERR::Okay;
      }
      else return log.warning(ERR::Search);
   }
   else return log.warning(ERR::NotInitialised);
}

/*********************************************************************************************************************

-FIELD-
AbsY: The absolute vertical position of a surface object.

This field returns the surface's vertical position relative to the top-level surface in its local hierarchy.

Writing `AbsY` moves the surface so that its absolute vertical position matches the supplied value.  The surface must
be initialised before this field can be changed.

*********************************************************************************************************************/

static ERR GET_AbsY(extSurface *Self, int *Value)
{
   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

   if (auto i = find_surface_list(Self); i != -1) {
      *Value = glSurfaces[i].Top;
      return ERR::Okay;
   }
   else return ERR::Search;
}

static ERR SET_AbsY(extSurface *Self, int Value)
{
   kt::Log log;

   if (Self->initialised()) {
      const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

      if (auto parent = find_parent_list(glSurfaces, Self); parent != -1) {
         int y = Value - glSurfaces[parent].Top;
         move_layer(Self, Self->X, y);
         return ERR::Okay;
      }

      return log.warning(ERR::Search);
   }
   else return log.warning(ERR::NotInitialised);
}

/*********************************************************************************************************************

-FIELD-
Align: This field allows you to align a surface area within its owner.

Use `Align` to position a surface within its owner without calculating explicit coordinates.  It is most useful for
horizontal or vertical centring; edge alignment is normally clearer when expressed through #X, #Y, #XOffset and
#YOffset.

Setting `Align` replaces related coordinate settings.  Valid alignment flags are `BOTTOM`, `CENTER`, `MIDDLE`, `LEFT`,
`HORIZONTAL`, `RIGHT`, `TOP` and `VERTICAL`.

-FIELD-
Bottom: Returns the bottom-most coordinate of a surface object, `Y + Height`.

*********************************************************************************************************************/

static ERR GET_Bottom(extSurface *Self, int *Bottom)
{
   *Bottom = Self->Y + Self->Height;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
BottomLimit: Prevents a surface object from moving beyond a given point at the bottom of its container.

Set `BottomLimit` to reserve a margin at the bottom of the parent container.  For example, a value of `5` prevents
#Move() from placing the surface inside the bottom-most five coordinate units.

Movement limits apply only to #Move().  Direct writes to coordinate fields bypass them.

*********************************************************************************************************************/

static ERR SET_BottomLimit(extSurface *Self, int Value)
{
   Self->BottomLimit = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Dimensions: Indicates currently active dimension settings.
Lookup: DMF

The `Dimensions` flags identify which coordinate and size fields currently define the surface layout, and whether
those values are fixed or scaled against the parent.

This field is normally managed automatically when fields such as #X, #Y, #Width, #Height, #XOffset and #YOffset are
changed.  If manual configuration is required, avoid conflicting fixed and scaled variants on the same axis.

*********************************************************************************************************************/

static ERR SET_Dimensions(extSurface *Self, DMF Value)
{
   SURFACEINFO *parent;
   const auto HORIZONTAL_FLAGS = DMF::FIXED_WIDTH|DMF::SCALED_WIDTH|DMF::FIXED_X_OFFSET|DMF::SCALED_X_OFFSET|DMF::FIXED_X|DMF::SCALED_X;
   const auto VERTICAL_FLAGS   = DMF::FIXED_HEIGHT|DMF::SCALED_HEIGHT|DMF::FIXED_Y_OFFSET|DMF::SCALED_Y_OFFSET|DMF::FIXED_Y|DMF::SCALED_Y;

   if (gfx::GetSurfaceInfo(Self->ParentID, &parent) IS ERR::Okay) {
      if (dmf::hasAnyY(Value)) {
         if (dmf::hasAnyHeight(Value) or dmf::hasAnyYOffset(Value)) {
            Self->Dimensions &= ~VERTICAL_FLAGS;
            Self->Dimensions |= Value & VERTICAL_FLAGS;
         }
      }
      else if (dmf::hasAnyHeight(Value) and dmf::hasAnyYOffset(Value)) {
         Self->Dimensions &= ~VERTICAL_FLAGS;
         Self->Dimensions |= Value & VERTICAL_FLAGS;
      }

      if (dmf::hasAnyX(Value)) {
         if (dmf::hasAnyWidth(Value) or dmf::hasAnyXOffset(Value)) {
            Self->Dimensions &= ~HORIZONTAL_FLAGS;
            Self->Dimensions |= Value & HORIZONTAL_FLAGS;
         }
      }
      else if (dmf::hasAnyWidth(Value) and dmf::hasAnyXOffset(Value)) {
         Self->Dimensions &= ~HORIZONTAL_FLAGS;
         Self->Dimensions |= Value & HORIZONTAL_FLAGS;
      }

      struct acRedimension resize;
      if (dmf::hasX(Self->Dimensions)) resize.X = Self->X;
      else if (dmf::hasScaledX(Self->Dimensions)) resize.X = parent->Width * std::lrint(Self->XPercent);
      else if (dmf::hasXOffset(Self->Dimensions)) resize.X = parent->Width - Self->XOffset;
      else if (dmf::hasScaledXOffset(Self->Dimensions)) resize.X = parent->Width - ((parent->Width * std::lrint(Self->XOffsetPercent)));
      else resize.X = 0;

      if (dmf::hasY(Self->Dimensions)) resize.Y = Self->Y;
      else if (dmf::hasScaledY(Self->Dimensions)) resize.Y = parent->Height * std::lrint(Self->YPercent);
      else if (dmf::hasYOffset(Self->Dimensions)) resize.Y = parent->Height - Self->YOffset;
      else if (dmf::hasScaledYOffset(Self->Dimensions)) resize.Y = parent->Height - ((parent->Height * std::lrint(Self->YOffsetPercent)));
      else resize.Y = 0;

      if (dmf::hasWidth(Self->Dimensions)) resize.Width = Self->Width;
      else if (dmf::hasScaledWidth(Self->Dimensions)) resize.Width = parent->Width * std::lrint(Self->WidthPercent);
      else {
         if (dmf::hasScaledXOffset(Self->Dimensions)) resize.Width = parent->Width - (parent->Width * std::lrint(Self->XOffsetPercent));
         else resize.Width = parent->Width - Self->XOffset;

         if (dmf::hasScaledX(Self->Dimensions)) resize.Width = resize.Width - ((parent->Width * std::lrint(Self->XPercent)));
         else resize.Width = resize.Width - Self->X;
      }

      if (dmf::hasHeight(Self->Dimensions)) resize.Height = Self->Height;
      else if (dmf::hasScaledHeight(Self->Dimensions)) resize.Height = parent->Height * std::lrint(Self->HeightPercent);
      else {
         if (dmf::hasScaledYOffset(Self->Dimensions)) resize.Height = parent->Height - (parent->Height * std::lrint(Self->YOffsetPercent));
         else resize.Height = parent->Height - Self->YOffset;

         if (dmf::hasScaledY(Self->Dimensions)) resize.Height = resize.Height - ((parent->Height * std::lrint(Self->YPercent)));
         else resize.Height = resize.Height - Self->Y;
      }

      resize.Z = 0;
      resize.Depth  = 0;
      return Action(acRedimension::id, Self, &resize);
   }
   else return ERR::Search;
}

/*********************************************************************************************************************

-FIELD-
Height: Defines the height of a surface object.

Use `Height` to read or change the surface height.  Alternatively, call #Resize() to change #Width and `Height`
together.

By default the value is a fixed coordinate unit.  With the `FD_SCALED` flag, the value is treated as a multiplier of
the parent surface height.

Changing `Height` on a visible surface updates the displayed area immediately, including any child surfaces that need
to be redrawn or resized.

Before initialisation, setting `Height` to zero or less clears the height dimension so that #Y and #YOffset can define
the height dynamically.  After initialisation, values of zero or less are invalid.

*********************************************************************************************************************/

static ERR GET_Height(extSurface *Self, Unit *Value)
{
   if (Value->scaled()) {
      if (dmf::hasScaledHeight(Self->Dimensions)) {
         Value->set(Self->HeightPercent);
         return ERR::Okay;
      }
      else return ERR::FieldTypeMismatch;
   }
   else {
      Value->set(Self->Height);
      return ERR::Okay;
   }
}

static ERR SET_Height(extSurface *Self, Unit *Value)
{
   kt::Log log;

   auto value = Value->Value;

   if (value <= 0) {
      if (Self->initialised()) return ERR::InvalidDimension;
      else {
         Self->Dimensions &= ~(DMF::FIXED_HEIGHT|DMF::SCALED_HEIGHT);
         return ERR::Okay;
      }
   }
   if (value > 0x7fffffff) value = 0x7fffffff;

   if (Value->scaled()) {
      if (Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            Self->HeightPercent = value;
            Self->Dimensions = (Self->Dimensions & (~DMF::FIXED_HEIGHT)) | DMF::SCALED_HEIGHT;
            resize_layer(Self, Self->X, Self->Y, 0, parent->Height * value, 0, 0, 0, 0, 0);
         }
         else return log.warning(ERR::AccessObject);
      }
      else {
         Self->HeightPercent = value;
         Self->Dimensions    = (Self->Dimensions & (~DMF::FIXED_HEIGHT)) | DMF::SCALED_HEIGHT;
      }
   }
   else {
      if (value != Self->Height) resize_layer(Self, Self->X, Self->Y, 0, value, 0, 0, 0, 0, 0);

      Self->Dimensions = (Self->Dimensions & (~DMF::SCALED_HEIGHT)) | DMF::FIXED_HEIGHT;

      // If the offset flags are used, adjust the vertical position

      if (dmf::hasScaledYOffset(Self->Dimensions)) {
         auto var = Unit(Self->YOffsetPercent, FD_SCALED);
         SET_YOffset(Self, &var);
      }
      else if (dmf::hasYOffset(Self->Dimensions)) {
         Unit var(Self->YOffset);
         SET_YOffset(Self, &var);
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
LeftLimit: Prevents a surface object from moving beyond a given point on the left-hand side.

Set `LeftLimit` to reserve a margin at the left-hand side of the parent container.  For example, a value of `3`
prevents #Move() from placing the surface inside the left-most three coordinate units.

Movement limits apply only to #Move().  Direct writes to coordinate fields bypass them.

*********************************************************************************************************************/

static ERR SET_LeftLimit(extSurface *Self, int Value)
{
   Self->LeftLimit = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
MaxHeight: Prevents the height of a surface object from exceeding a certain value.

Set `MaxHeight` to limit the maximum height that can be applied through resizing.  #Resize() cannot increase the
surface beyond this value.

Direct writes to #Height bypass this limit.

*********************************************************************************************************************/

static ERR SET_MaxHeight(extSurface *Self, int Value)
{
   Self->MaxHeight = Value;

   if ((!Self->ParentID) and (Self->DisplayID)) {
      kt::ScopedObjectLock<extDisplay> display(Self->DisplayID);
      if (display.granted()) display->sizeHints(-1, -1,
         (Self->MaxWidth > 0) ? (Self->MaxWidth) : -1,
         (Self->MaxHeight > 0) ? (Self->MaxHeight) : -1,
         (Self->Flags & RNF::ASPECT_RATIO) != RNF::NIL);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
MaxWidth: Prevents the width of a surface object from exceeding a certain value.

Set `MaxWidth` to limit the maximum width that can be applied through resizing.  #Resize() cannot increase the surface
beyond this value.

Direct writes to #Width bypass this limit.

*********************************************************************************************************************/

static ERR SET_MaxWidth(extSurface *Self, int Value)
{
   Self->MaxWidth = Value;

   if ((!Self->ParentID) and (Self->DisplayID)) {
      if (kt::ScopedObjectLock<extDisplay> display(Self->DisplayID); display.granted()) {
         display->sizeHints(-1, -1,
            (Self->MaxWidth > 0) ? (Self->MaxWidth) : -1,
            (Self->MaxHeight > 0) ? (Self->MaxHeight) : -1,
            (Self->Flags & RNF::ASPECT_RATIO) != RNF::NIL);
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
MinHeight: Prevents the height of a surface object from shrinking beyond a certain value.

Set `MinHeight` to limit the minimum height that can be applied through resizing.  #Resize() cannot shrink the surface
below this value.

Direct writes to #Height bypass this limit.  Values less than `1` are clamped to `1`.

*********************************************************************************************************************/

static ERR SET_MinHeight(extSurface *Self, int Value)
{
   Self->MinHeight = Value;
   if (Self->MinHeight < 1) Self->MinHeight = 1;

   if ((!Self->ParentID) and (Self->DisplayID)) {
      if (kt::ScopedObjectLock<extDisplay> display(Self->DisplayID); display.granted()) {
         display->sizeHints(Self->MinWidth, Self->MinHeight,
            -1, -1, (Self->Flags & RNF::ASPECT_RATIO) != RNF::NIL);
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
MinWidth: Prevents the width of a surface object from shrinking beyond a certain value.

Set `MinWidth` to limit the minimum width that can be applied through resizing.  #Resize() cannot shrink the surface
below this value.

Direct writes to #Width bypass this limit.  Values less than `1` are clamped to `1`.

*********************************************************************************************************************/

static ERR SET_MinWidth(extSurface *Self, int Value)
{
   Self->MinWidth = Value;
   if (Self->MinWidth < 1) Self->MinWidth = 1;

   if ((!Self->ParentID) and (Self->DisplayID)) {
      if (kt::ScopedObjectLock<extDisplay> display(Self->DisplayID); display.granted()) {
         display->sizeHints(Self->MinWidth, Self->MinHeight,
            -1, -1, (Self->Flags & RNF::ASPECT_RATIO) != RNF::NIL);
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Right: Returns the right-most coordinate of a surface object, `X + Width`.

*********************************************************************************************************************/

static ERR GET_Right(extSurface *Self, int *Value)
{
   *Value = Self->X + Self->Width;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
RightLimit: Prevents a surface object from moving beyond a given point on the right-hand side.

Set `RightLimit` to reserve a margin at the right-hand side of the parent container.  For example, a value of `8`
prevents #Move() from placing the surface inside the right-most eight coordinate units.

Movement limits apply only to #Move().  Direct writes to coordinate fields bypass them.

*********************************************************************************************************************/

static ERR SET_RightLimit(extSurface *Self, int Value)
{
   Self->RightLimit = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
TopLimit: Prevents a surface object from moving beyond a given point at the top of its container.

Set `TopLimit` to reserve a margin at the top of the parent container.  For example, a value of `10` prevents #Move()
from placing the surface inside the top-most ten coordinate units.

Movement limits apply only to #Move().  Direct writes to coordinate fields bypass them.

*********************************************************************************************************************/

static ERR SET_TopLimit(extSurface *Self, int Value)
{
   Self->TopLimit = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
VisibleHeight: The visible height of the surface area, relative to its parents.

Read #VisibleX, #VisibleY, #VisibleWidth and `VisibleHeight` to determine the portion of the surface that is visible
within the parent chain.

The visible area is calculated by clipping the surface area against its parent surfaces.  If a 100-unit-high surface is
inside a 50-unit-high parent, at most 50 units can be visible, depending on the surface position.

If none of the surface area is visible, zero is returned.  The result is never negative.

*********************************************************************************************************************/

static ERR GET_VisibleHeight(extSurface *Self, int *Value)
{
   if (!Self->ParentID) {
      *Value = Self->Height;
      return ERR::Okay;
   }
   else return gfx::GetVisibleArea(Self->UID, nullptr, nullptr, nullptr, nullptr, nullptr, Value);
}

/*********************************************************************************************************************

-FIELD-
VisibleWidth: The visible width of the surface area, relative to its parents.

Read #VisibleX, #VisibleY, `VisibleWidth` and #VisibleHeight to determine the portion of the surface that is visible
within the parent chain.

The visible area is calculated by clipping the surface area against its parent surfaces.  If a 100-unit-wide surface is
inside a 50-unit-wide parent, at most 50 units can be visible, depending on the surface position.

If none of the surface area is visible, zero is returned.  The result is never negative.

*********************************************************************************************************************/

static ERR GET_VisibleWidth(extSurface *Self, int *Value)
{
   if (!Self->ParentID) {
      *Value = Self->Width;
      return ERR::Okay;
   }
   else return gfx::GetVisibleArea(Self->UID, nullptr, nullptr, nullptr, nullptr, Value, nullptr);
}

/*********************************************************************************************************************

-FIELD-
VisibleX: The first visible X coordinate of the surface area, relative to its parents.

Read `VisibleX`, #VisibleY, #VisibleWidth and #VisibleHeight to determine the portion of the surface that is visible
within the parent chain.

`VisibleX` is the first visible horizontal coordinate inside the surface's own coordinate space.  It is calculated by
clipping the surface area against its parent surfaces.

If none of the surface area is visible, zero is returned.  The result is never negative.

*********************************************************************************************************************/

static ERR GET_VisibleX(extSurface *Self, int *Value)
{
   if (!Self->ParentID) {
      *Value = Self->X;
      return ERR::Okay;
   }
   else return gfx::GetVisibleArea(Self->UID, Value, nullptr, nullptr, nullptr, nullptr, nullptr);
}

/*********************************************************************************************************************

-FIELD-
VisibleY: The first visible Y coordinate of the surface area, relative to its parents.

Read #VisibleX, `VisibleY`, #VisibleWidth and #VisibleHeight to determine the portion of the surface that is visible
within the parent chain.

`VisibleY` is the first visible vertical coordinate inside the surface's own coordinate space.  It is calculated by
clipping the surface area against its parent surfaces.

If none of the surface area is visible, zero is returned.  The result is never negative.

*********************************************************************************************************************/

static ERR GET_VisibleY(extSurface *Self, int *Value)
{
   if (!Self->ParentID) {
      *Value = Self->Y;
      return ERR::Okay;
   }
   else return gfx::GetVisibleArea(Self->UID, nullptr, Value, nullptr, nullptr, nullptr, nullptr);
}

/*********************************************************************************************************************

-FIELD-
Width: Defines the width of a surface object.

Use `Width` to read or change the surface width.  Alternatively, call #Resize() to change `Width` and #Height
together.

By default the value is a fixed coordinate unit.  With the `FD_SCALED` flag, the value is treated as a multiplier of
the parent surface width.

Changing `Width` on a visible surface updates the displayed area immediately, including any child surfaces that need
to be redrawn or resized.

Before initialisation, setting `Width` to zero or less clears the width dimension so that #X and #XOffset can define
the width dynamically.  After initialisation, values of zero or less are invalid.

*********************************************************************************************************************/

static ERR GET_Width(extSurface *Self, Unit *Value)
{
   if (Value->scaled()) {
      if (dmf::hasScaledWidth(Self->Dimensions)) {
         Value->set(Self->WidthPercent);
      }
      else return ERR::FieldTypeMismatch;
   }
   else Value->set(Self->Width);
   return ERR::Okay;
}

static ERR SET_Width(extSurface *Self, Unit *Value)
{
   auto value = Value->Value;

   if (value <= 0) {
      if (Self->initialised()) return ERR::InvalidDimension;
      else {
         Self->Dimensions &= ~(DMF::FIXED_WIDTH|DMF::SCALED_WIDTH);
         return ERR::Okay;
      }
   }
   if (value > 0x7fffffff) value = 0x7fffffff;

   if (Value->scaled()) {
      if (Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            Self->WidthPercent = value;
            Self->Dimensions   = (Self->Dimensions & (~DMF::FIXED_WIDTH)) | DMF::SCALED_WIDTH;
            resize_layer(Self, Self->X, Self->Y, parent->Width * value, 0, 0, 0, 0, 0, 0);
         }
         else return ERR::AccessObject;
      }
      else {
         Self->WidthPercent = value;
         Self->Dimensions   = (Self->Dimensions & (~DMF::FIXED_WIDTH)) | DMF::SCALED_WIDTH;
      }
   }
   else {
      if (value != Self->Width) resize_layer(Self, Self->X, Self->Y, value, 0, 0, 0, 0, 0, 0);

      Self->Dimensions = (Self->Dimensions & (~DMF::SCALED_WIDTH)) | DMF::FIXED_WIDTH;

      // If the offset flags are used, adjust the horizontal position
      if (dmf::hasScaledXOffset(Self->Dimensions)) {
         auto val = Unit(Self->XOffsetPercent, FD_SCALED);
         SET_XOffset(Self, &val);
      }
      else if (dmf::hasXOffset(Self->Dimensions)) {
         auto val = Unit(Self->XOffset);
         SET_XOffset(Self, &val);
      }
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
X: Determines the horizontal position of a surface object.

Use `X` to read or change the horizontal position of the surface relative to its parent.

By default the value is a fixed coordinate unit.  With the `FD_SCALED` flag, the value is treated as a multiplier of
the parent surface width.

Changing `X` on a visible surface updates its position immediately.  If #XOffset also defines the right-hand edge, the
surface width is recalculated to preserve that offset.

*********************************************************************************************************************/

static ERR GET_XCoord(extSurface *Self, Unit *Value)
{
   Value->set(Value->scaled() ? Self->XPercent : Self->X);
   return ERR::Okay;
}

static ERR SET_XCoord(extSurface *Self, Unit *Value)
{
   auto value = Value->Value;

   if (Value->scaled()) {
      Self->Dimensions = (Self->Dimensions & (~DMF::FIXED_X)) | DMF::SCALED_X;
      Self->XPercent   = value;
      if (Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            move_layer(Self, parent->Width * value, Self->Y);
         }
         else return ERR::AccessObject;
      }
   }
   else {
      Self->Dimensions = (Self->Dimensions & (~DMF::SCALED_X)) | DMF::FIXED_X;
      move_layer(Self, value, Self->Y);

      // If our right-hand side is relative, we need to resize our surface to counteract the movement.

      if ((Self->ParentID) and (dmf::hasAnyXOffset(Self->Dimensions))) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            resize_layer(Self, Self->X, Self->Y, parent->Width - Self->X - Self->XOffset, 0, 0, 0, 0, 0, 0);
         }
         else return ERR::AccessObject;
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XOffset: Determines the horizontal offset of a surface object.

`XOffset` defines a distance from the right-hand edge of the parent surface.

When #X is set and #Width is not set, `XOffset` makes the surface width dynamic.  The width extends from #X to the
parent width minus `XOffset`.

When #Width is set, `XOffset` positions the surface from the parent right-hand edge:
`X = ParentWidth - Width - XOffset`.
-END-

*********************************************************************************************************************/

static ERR GET_XOffset(extSurface *Self, Unit *Value)
{
   kt::Log log;
   double value;

   if (Value->scaled()) {
      Unit xoffset;
      if (GET_XOffset(Self, &xoffset) IS ERR::Okay) {
         value = xoffset / Self->Width;
      }
      else value = 0;
   }
   else {
      if (dmf::hasAnyXOffset(Self->Dimensions)) {
         value = Self->XOffset;
      }
      else if ((dmf::hasAnyWidth(Self->Dimensions)) and (dmf::hasAnyX(Self->Dimensions)) and (Self->ParentID)) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            value = parent->Width - Self->X - Self->Width;
         }
         else return log.warning(ERR::AccessObject);
      }
      else value = 0;
   }

   Value->set(value);
   return ERR::Okay;
}

static ERR SET_XOffset(extSurface *Self, Unit *Value)
{
   auto value = Value->Value;
   if (value < 0) value = -value;

   if (Value->scaled()) {
      Self->Dimensions = (Self->Dimensions & (~DMF::FIXED_X_OFFSET)) | DMF::SCALED_X_OFFSET;
      Self->XOffsetPercent = value;

      if (Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            Self->XOffset = parent->Width * std::lrint(Self->XOffsetPercent);
            if (!dmf::hasAnyX(Self->Dimensions)) Self->X = parent->Width - Self->XOffset - Self->Width;
            if (!dmf::hasAnyWidth(Self->Dimensions)) {
               resize_layer(Self, Self->X, Self->Y, parent->Width - Self->X - Self->XOffset, 0, 0, 0, 0, 0, 0);
            }
         }
         else return ERR::AccessObject;
      }
   }
   else {
      Self->Dimensions = (Self->Dimensions & (~DMF::SCALED_X_OFFSET)) | DMF::FIXED_X_OFFSET;
      Self->XOffset = value;

      if (dmf::hasAnyWidth(Self->Dimensions) and Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            move_layer(Self, parent->Width - Self->XOffset - Self->Width, Self->Y);
         }
         else return ERR::AccessObject;
      }
      else if (dmf::hasAnyX(Self->Dimensions) and Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            resize_layer(Self, Self->X, Self->Y, parent->Width - Self->X - Self->XOffset, 0, 0, 0, 0, 0, 0);
         }
         else return ERR::AccessObject;
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Y: Determines the vertical position of a surface object.

Use `Y` to read or change the vertical position of the surface relative to its parent.

By default the value is a fixed coordinate unit.  With the `FD_SCALED` flag, the value is treated as a multiplier of
the parent surface height.

Changing `Y` on a visible surface updates its position immediately.

*********************************************************************************************************************/

static ERR GET_YCoord(extSurface *Self, Unit *Value)
{
   Value->set(Value->scaled() ? Self->YPercent : Self->Y);
   return ERR::Okay;
}

static ERR SET_YCoord(extSurface *Self, Unit *Value)
{
   if (Value->scaled()) {
      Self->Dimensions = (Self->Dimensions & (~DMF::FIXED_Y)) | DMF::SCALED_Y;
      Self->YPercent = Value->Value;
      if (Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            move_layer(Self, Self->X, parent->Height * Value->Value);
         }
         else return ERR::AccessObject;
      }
   }
   else {
      Self->Dimensions = (Self->Dimensions & (~DMF::SCALED_Y)) | DMF::FIXED_Y;
      move_layer(Self, Self->X, Value->Value);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
YOffset: Determines the vertical offset of a surface object.

`YOffset` defines a distance from the bottom edge of the parent surface.

When #Y is set and #Height is not set, `YOffset` makes the surface height dynamic.  The height extends from #Y to the
parent height minus `YOffset`.

When #Height is set, `YOffset` positions the surface from the parent bottom edge:
`Y = ParentHeight - Height - YOffset`.
-END-

*********************************************************************************************************************/

static ERR GET_YOffset(extSurface *Self, Unit *Value)
{
   double value;

   if (Value->scaled()) {
      Unit yoffset;
      if (GET_YOffset(Self, &yoffset) IS ERR::Okay) value = yoffset / Self->Height;
      else value = 0;
   }
   else {
      if (dmf::hasAnyYOffset(Self->Dimensions)) {
         value = Self->YOffset;
      }
      else if (dmf::hasAnyHeight(Self->Dimensions) and dmf::hasY(Self->Dimensions) and Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            value = parent->Height - Self->Y - Self->Height;
         }
         else return ERR::AccessObject;
      }
      else value = 0;
   }

   Value->set(value);
   return ERR::Okay;
}

static ERR SET_YOffset(extSurface *Self, Unit *Value)
{
   auto value = Value->Value;

   if (value < 0) value = -value;

   if (Value->scaled()) {
      Self->Dimensions = (Self->Dimensions & (~DMF::FIXED_Y_OFFSET)) | DMF::SCALED_Y_OFFSET;
      Self->YOffsetPercent = value;

      if (Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            Self->YOffset = parent->Height * std::lrint(Self->YOffsetPercent);
            if (!dmf::hasAnyY(Self->Dimensions)) Self->Y = parent->Height - Self->YOffset - Self->Height;
            if (!dmf::hasAnyHeight(Self->Dimensions)) {
               resize_layer(Self, Self->X, Self->Y, 0, parent->Height - Self->Y - Self->YOffset, 0, 0, 0, 0, 0);
            }
            else move_layer(Self, Self->X, parent->Height - Self->YOffset - Self->Height);
         }
         else return ERR::AccessObject;
      }
   }
   else {
      Self->Dimensions = (Self->Dimensions & (~DMF::SCALED_Y_OFFSET)) | DMF::FIXED_Y_OFFSET;
      Self->YOffset = value;

      if (dmf::hasAnyHeight(Self->Dimensions) and Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            if (!dmf::hasAnyHeight(Self->Dimensions)) {
               resize_layer(Self, Self->X, Self->Y, 0, parent->Height - Self->Y - Self->YOffset, 0, 0, 0, 0, 0);
            }
            else move_layer(Self, Self->X, parent->Height - Self->YOffset - Self->Height);
         }
         else return ERR::AccessObject;
      }
      else if (dmf::hasAnyY(Self->Dimensions) and Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            resize_layer(Self, Self->X, Self->Y, 0, parent->Height - Self->Y - Self->YOffset, 0, 0, 0, 0, 0);
         }
         else return ERR::AccessObject;
      }
   }
   return ERR::Okay;
}

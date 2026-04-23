
/*********************************************************************************************************************

-FIELD-
AbsX: The absolute horizontal position of a surface object.

This field returns the absolute horizontal position of a surface object. The absolute value is calculated based on the
surface object's position relative to the top most surface object in the local hierarchy.

It is possible to set this field, but only after initialisation of the surface object has occurred.

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
   pf::Log log;

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

This field returns the absolute vertical position of a surface object. The absolute value is calculated based on the
surface object's position relative to the top most surface object in the local hierarchy.

It is possible to set this field, but only after initialisation of the surface object has occurred.

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
   pf::Log log;

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

If you would like to set an abstract position for a surface area, you can give it an alignment.  This feature is most
commonly used for horizontal and vertical centring, as aligning to the the edges of a surface area is already handled
by existing dimension fields.  Note that setting the alignment overrides any settings in related coordinate fields.
Valid alignment flags are `BOTTOM`, `CENTER/MIDDLE`, `LEFT`, `HORIZONTAL`, `RIGHT`, `TOP`, `VERTICAL`.

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

A client can prevent a surface object from moving beyond a given point at the bottom of its container by setting this field.
If for example you were to set the BottomLimit to 5, then any attempt to move the surface object into or beyond the 5
units at the bottom of its container would fail.

Limits only apply to movement, as induced through the #Move() action.  This means that limits can be over-ridden by
setting the coordinate fields directly (which can be useful in certain cases).

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

The dimension settings of a surface object can be read from this field.  The flags indicate the dimension fields that
are in use, and whether the values are fixed or relative.

It is strongly recommended that this field is never set manually, because the flags are automatically managed for the
client when setting fields such as #X and #Width.  If circumstances require manual configuration, take care to ensure
that the flags do not conflict.  For instance, `FIXED_X` and `SCALED_X` cannot be paired, nor could `FIXED_X`,
`FIXED_XOFFSET` and `FIXED_WIDTH` simultaneously.

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
      Action(acRedimension::id, Self, &resize);

      return ERR::Okay;
   }
   else return ERR::Search;
}

/*********************************************************************************************************************

-FIELD-
Height: Defines the height of a surface object.

The height of a surface object is manipulated through this field.  Alternatively, use the #Resize() action to adjust the
Width and Height at the same time.  A client can set the Height as a fixed value by default, or as a scaled value in
conjunction with the `FD_SCALED` flag.  Scaled values are multiplied by the height of their parent container.

Setting the Height while a surface object is on display causes an immediate graphical update to reflect the change.
Any objects that are within the surface area will be re-drawn and resized as necessary.

If a value less than zero is passed to an initialised surface, the height will be 'turned off' - this is convenient
for pairing the #Y and #YOffset fields together for dynamic height adjustment.

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
   pf::Log log;

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

A client can prevent a surface object from moving beyond a given point at the left-hand side of its container by setting
this field.  If for example you were to set the LeftLimit to 3, then any attempt to move the surface object into or
beyond the 3 units at the left of its container would fail.

Limits only apply to movement, as induced through the #Move() action.  This means it is possible to override limits by
setting the coordinate fields directly.

*********************************************************************************************************************/

static ERR SET_LeftLimit(extSurface *Self, int Value)
{
   Self->LeftLimit = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
MaxHeight: Prevents the height of a surface object from exceeding a certain value.

A client can limit the maximum height of a surface object by setting this field.  Limiting the height affects resizing,
making it impossible to use the Resize() action to extend beyond the height you specify.

It is possible to circumvent the MaxHeight by setting the Height field directly.

*********************************************************************************************************************/

static ERR SET_MaxHeight(extSurface *Self, int Value)
{
   Self->MaxHeight = Value;

   if ((!Self->ParentID) and (Self->DisplayID)) {
      pf::ScopedObjectLock<extDisplay> display(Self->DisplayID);
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

A client can limit the maximum width of a surface object by setting this field.  Limiting the width affects resizing, making
it impossible to use the #Resize() action to extend beyond the width you specify.

It is possible to circumvent the MaxWidth by setting the Width field directly.

*********************************************************************************************************************/

static ERR SET_MaxWidth(extSurface *Self, int Value)
{
   Self->MaxWidth = Value;

   if ((!Self->ParentID) and (Self->DisplayID)) {
      if (pf::ScopedObjectLock<extDisplay> display(Self->DisplayID); display.granted()) {
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

A client can prevent the height of a surface object from shrinking too far by setting this field.  This feature specifically
affects resizing, making it impossible to use the Resize() action to shrink the height of a surface object to a value
less than the one you specify.

It is possible to circumvent the MinHeight by setting the #Height field directly.

*********************************************************************************************************************/

static ERR SET_MinHeight(extSurface *Self, int Value)
{
   Self->MinHeight = Value;
   if (Self->MinHeight < 1) Self->MinHeight = 1;

   if ((!Self->ParentID) and (Self->DisplayID)) {
      if (pf::ScopedObjectLock<extDisplay> display(Self->DisplayID); display.granted()) {
         display->sizeHints(Self->MinWidth, Self->MinHeight,
            -1, -1, (Self->Flags & RNF::ASPECT_RATIO) != RNF::NIL);
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
MinWidth: Prevents the width of a surface object from shrinking beyond a certain value.

A client can prevent the width of a surface object from shrinking too far by setting this field.  This feature specifically
affects resizing, making it impossible to use the #Resize() action to shrink the width of a surface object to a value
less than the one you specify.

It is possible to circumvent the MinWidth by setting the #Width field directly.

*********************************************************************************************************************/

static ERR SET_MinWidth(extSurface *Self, int Value)
{
   Self->MinWidth = Value;
   if (Self->MinWidth < 1) Self->MinWidth = 1;

   if ((!Self->ParentID) and (Self->DisplayID)) {
      if (pf::ScopedObjectLock<extDisplay> display(Self->DisplayID); display.granted()) {
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

A client can prevent a surface object from moving beyond a given point at the right-hand side of its container by setting
this field.  If for example you were to set the RightLimit to 8, then any attempt to move the surface object into or
beyond the 8 units at the right-hand side of its container would fail.

Limits only apply to movement, as induced through the #Move() action.  This means that limits can be over-ridden by
setting the coordinate fields directly (which can be useful in certain cases).

*********************************************************************************************************************/

static ERR SET_RightLimit(extSurface *Self, int Value)
{
   Self->RightLimit = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
TopLimit: Prevents a surface object from moving beyond a given point at the top of its container.

A client can prevent a surface object from moving beyond a given point at the top of its container by setting this field.
If for example you were to set the TopLimit to 10, then any attempt to move the surface object into or beyond the 10
units at the top of its container would fail.

Limits only apply to movement, as induced through the #Move() action.  This means that limits can be over-ridden by
setting the coordinate fields directly (which can be useful in certain cases).

*********************************************************************************************************************/

static ERR SET_TopLimit(extSurface *Self, int Value)
{
   Self->TopLimit = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
VisibleHeight: The visible height of the surface area, relative to its parents.

To determine the visible area of a surface, read the #VisibleX, #VisibleY, #VisibleWidth and VisibleHeight fields.

The 'visible area' is determined by the position of the surface relative to its parents.  For example, if the surface
is 100 pixels across and smallest parent is 50 pixels across, the number of pixels visible to the user must be 50
pixels or less, depending on the position of the surface.

If none of the surface area is visible then zero is returned.  The result is never negative.

*********************************************************************************************************************/

static ERR GET_VisibleHeight(extSurface *Self, int *Value)
{
   if (!Self->ParentID) {
      *Value = Self->Height;
      return ERR::Okay;
   }
   else {
      const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

      int16_t i;
      if ((i = find_surface_list(Self)) IS -1) return ERR::Search;

      auto clip = glSurfaces[i].area();
      restrict_region_to_parents(glSurfaces, i, clip, false);
      *Value = clip.height();
      return ERR::Okay;
   }
}

/*********************************************************************************************************************

-FIELD-
VisibleWidth: The visible width of the surface area, relative to its parents.

To determine the visible area of a surface, read the VisibleX, VisibleY, VisibleWidth and VisibleHeight fields.

The 'visible area' is determined by the position of the surface relative to its parents.  For example, if the surface
is 100 pixels across and smallest parent is 50 pixels across, the number of pixels visible to the user must be 50
pixels or less, depending on the position of the surface.

If none of the surface area is visible then zero is returned.  The result is never negative.

*********************************************************************************************************************/

static ERR GET_VisibleWidth(extSurface *Self, int *Value)
{
   if (!Self->ParentID) {
      *Value = Self->Height;
      return ERR::Okay;
   }
   else {
      const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

      if (auto i = find_surface_list(Self); i != -1) {
         auto clip = glSurfaces[i].area();
         restrict_region_to_parents(glSurfaces, i, clip, false);
         *Value = clip.width();
         return ERR::Okay;
      }
      else return ERR::Search;
   }
}

/*********************************************************************************************************************

-FIELD-
VisibleX: The first visible X coordinate of the surface area, relative to its parents.

To determine the visible area of a surface, read the VisibleX, #VisibleY, #VisibleWidth and #VisibleHeight fields.

The 'visible area' is determined by the position of the surface relative to its parents.  For example, if the surface
is 100 pixels across and smallest parent is 50 pixels across, the number of pixels visible to the user must be 50
pixels or less, depending on the position of the surface.

If none of the surface area is visible then zero is returned.  The result is never negative.

*********************************************************************************************************************/

static ERR GET_VisibleX(extSurface *Self, int *Value)
{
   if (!Self->ParentID) {
      *Value = Self->Height;
      return ERR::Okay;
   }
   else {
      const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

      if (auto i = find_surface_list(Self); i != -1) {
         auto clip = glSurfaces[i].area();
         restrict_region_to_parents(glSurfaces, i, clip, false);
         *Value = clip.Left - glSurfaces[i].Left;
         return ERR::Okay;
      }
      else return ERR::Search;
   }
}

/*********************************************************************************************************************

-FIELD-
VisibleY: The first visible Y coordinate of the surface area, relative to its parents.

To determine the visible area of a surface, read the #VisibleX, VisibleY, #VisibleWidth and #VisibleHeight fields.

The 'visible area' is determined by the position of the surface relative to its parents.  For example, if the surface
is 100 pixels across and smallest parent is 50 pixels across, the number of pixels visible to the user must be 50
pixels or less, depending on the position of the surface.

If none of the surface area is visible then zero is returned.  The result is never negative.

*********************************************************************************************************************/

static ERR GET_VisibleY(extSurface *Self, int *Value)
{
   if (!Self->ParentID) {
      *Value = Self->Height;
      return ERR::Okay;
   }
   else {
      const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

      if (auto i = find_surface_list(Self); i != -1) {
         auto clip = glSurfaces[i].area();
         restrict_region_to_parents(glSurfaces, i, clip, false);
         *Value = clip.Top - glSurfaces[i].Top;
         return ERR::Okay;
      }
      else return ERR::Search;
   }
}

/*********************************************************************************************************************

-FIELD-
Width: Defines the width of a surface object.

The width of a surface object is manipulated through this field.  Alternatively, use the #Resize() action to adjust the
Width and #Height at the same time.  A client can set the Width as a fixed value by default, or as a scaled value in
conjunction with the `FD_SCALED` flag.  Scaled values are multiplied by the width of their parent container.

Setting the Width while a surface object is on display causes an immediate graphical update to reflect the change.  Any
objects that are within the surface area will be re-drawn and resized as necessary.

Width values of 0 or less are illegal, and will result in an `ERR::OutOfRange` error-code.

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

The horizontal position of a surface object can be set through this field.  You have the choice of setting a fixed
coordinate (the default) or a scaled coordinate if you use the `FD_SCALED` flag.

If you set the X while the surface object is on display, the position of the surface area will be updated
immediately.

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

The XOffset has a dual purpose depending on whether or not it is set in conjunction with the #X or #Width fields.

If set in conjunction with the #X field, the width of the surface object will be from that X coordinate up to the width
of the container, minus the value given in the XOffset.  This means that the width of the surface object is dynamically
calculated in relation to the width of its container.

If the XOffset field is set in conjunction with a fixed or scaled width then the surface object will be positioned at
an X coordinate calculated from the formula `X = ContainerWidth - SurfaceWidth - XOffset`.
-END-

*********************************************************************************************************************/

static ERR GET_XOffset(extSurface *Self, Unit *Value)
{
   pf::Log log;
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

The vertical position of a surface object can be set through this field.  You have the choice of setting a fixed
coordinate (the default) or a scaled coordinate if you use the `FD_SCALED` flag.

If the value is changed while the surface is on display, its position will be updated immediately.

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

The YOffset has a dual purpose depending on whether or not it is set in conjunction with the #Y or #Height fields.

If set in conjunction with the #Y field, the height of the surface object will be from that Y coordinate up to the
height of the container, minus the value given in the YOffset.  This means that the height of the surface object is
dynamically calculated in relation to the height of its container.

If the YOffset field is set in conjunction with a fixed or scaled height then the surface object will be positioned
at a Y coordinate calculated from the formula `Y = ContainerHeight - SurfaceHeight - YOffset`.
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


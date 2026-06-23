
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
   if (Self->initialised()) {
      const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

      if (auto parent = find_parent_list(glSurfaces, Self); parent != -1) {
         int x = Value - glSurfaces[parent].Left;
         move_layer(Self, x, Self->FixedY);
         return ERR::Okay;
      }
      else return ERR::Search;
   }
   else return ERR::NotInitialised;
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
   if (Self->initialised()) {
      const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

      if (auto parent = find_parent_list(glSurfaces, Self); parent != -1) {
         int y = Value - glSurfaces[parent].Top;
         move_layer(Self, Self->FixedX, y);
         return ERR::Okay;
      }

      return ERR::Search;
   }
   else return ERR::NotInitialised;
}

/*********************************************************************************************************************

-FIELD-
Bottom: Returns the bottom-most coordinate of a surface object, `Y + Height`.

*********************************************************************************************************************/

static ERR GET_Bottom(extSurface *Self, int *Bottom)
{
   *Bottom = Self->FixedY + Self->FixedHeight;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Height: Defines the height of a surface object.

Set `Height` to change the surface height.  Alternatively, call #Resize() to change #Width and `Height` together.

By default the value is a fixed coordinate unit.  With the `FD_SCALED` flag, the value is treated as a multiplier of
the parent surface height.

A read returns the value exactly as it was last defined.  When the height was set as a scaled value, the multiplier is
returned rather than a resolved pixel count.  Read #VisibleHeight to determine the height of the surface area that is
actually presented in pixels.

Changing `Height` on a visible surface updates the displayed area immediately, including any child surfaces that need
to be redrawn or resized.

Before initialisation, setting `Height` to zero or less clears the height dimension so that #Y and #YOffset can define
the height dynamically.  After initialisation, values of zero or less are invalid.

*********************************************************************************************************************/

static ERR GET_Height(extSurface *Self, Unit &Value)
{
   if (Value.verbatim() or (not Self->initialised())){
      Value = Self->Height;
      Value.Type |= FD_PURE;
      return ERR::Okay;
   }
   else if (Value.scaled()) { // Client requested scaled unit
      if (Self->Height.scaled()) Value = Self->Height;
      else if (Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            Value = Unit(double(Self->FixedHeight) / double(parent->FixedHeight), FD_SCALED);
         }
         else return ERR::AccessObject;
      }
      else Value = Unit();
   }
   else { // Client requested a fixed (resolved pixel) unit
      Value = Unit(Self->FixedHeight);
   }

   return ERR::Okay;
}

static ERR SET_Height(extSurface *Self, Unit &Value)
{
   if (Value <= 0) {
      if (Self->initialised()) return ERR::InvalidDimension;
      else {
         Self->Height = Unit(); // Revert to undefined state
         return ERR::Okay;
      }
   }

   if (Value.scaled()) {
      Self->Height = Value;
      if (Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            resize_layer(Self, Self->FixedX, Self->FixedY, 0, parent->FixedHeight * Value, 0, 0, 0, 0, 0);
         }
         else return ERR::AccessObject;
      }
   }
   else {
      if (Value != Self->Height) {
         Self->Height = Value;
         resize_layer(Self, Self->FixedX, Self->FixedY, 0, Value, 0, 0, 0, 0, 0);
      }

      // If offset, adjust the vertical position

      if (Self->YOffset.defined()) SET_YOffset(Self, Self->YOffset);
   }

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
   *Value = Self->FixedX + Self->FixedWidth;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Width: Defines the width of a surface object.

Set `Width` to change the surface width.  Alternatively, call #Resize() to change `Width` and #Height together.

By default the value is a fixed coordinate unit.  With the `FD_SCALED` flag, the value is treated as a multiplier of
the parent surface width.

A read returns the value exactly as it was last defined.  When the width was set as a scaled value, the multiplier is
returned rather than a resolved pixel count.  Read #VisibleWidth to determine the width of the surface area that is
actually presented in pixels.

Changing `Width` on a visible surface updates the displayed area immediately, including any child surfaces that need
to be redrawn or resized.

Before initialisation, setting `Width` to zero or less clears the width dimension so that #X and #XOffset can define
the width dynamically.  After initialisation, values of zero or less are invalid.

*********************************************************************************************************************/

static ERR GET_Width(extSurface *Self, Unit &Value)
{
   if (Value.verbatim() or (not Self->initialised())){
      Value = Self->Width;
      Value.Type |= FD_PURE;
      return ERR::Okay;
   }
   else if (Value.scaled()) { // Client requested scaled unit
      if (Self->Width.scaled()) Value = Self->Width;
      else if (Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            Value = Unit(double(Self->FixedWidth) / double(parent->FixedWidth), FD_SCALED);
         }
         else return ERR::AccessObject;
      }
      else Value = Unit();
   }
   else { // Client requested a fixed (resolved pixel) unit
      Value = Unit(Self->FixedWidth);
   }

   return ERR::Okay;
}

static ERR SET_Width(extSurface *Self, Unit &Value)
{
   if (Value <= 0) {
      if (Self->initialised()) return ERR::InvalidDimension;
      else {
         Self->Width = Unit(); // Revert to undefined state; client should follow-up by setting an XOffset
         return ERR::Okay;
      }
   }

   if (Value.scaled()) {
      Self->Width = Value;
      if (Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            resize_layer(Self, Self->FixedX, Self->FixedY, parent->FixedWidth * Value, 0, 0, 0, 0, 0, 0);
         }
         else return ERR::AccessObject;
      }
   }
   else {
      if (Value != Self->Width) {
         Self->Width = Value;
         resize_layer(Self, Self->FixedX, Self->FixedY, Value, 0, 0, 0, 0, 0, 0);
      }

      // If the offset flags are used, adjust the horizontal position

      if (Self->XOffset.defined()) SET_XOffset(Self, Self->XOffset);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
X: Determines the horizontal position of a surface object.

Set `X` to change the horizontal position of the surface relative to its parent.

By default the value is a fixed coordinate unit.  With the `FD_SCALED` flag, the value is treated as a multiplier of
the parent surface width.

A read returns the value exactly as it was last defined.  When `X` was set as a scaled value, the multiplier is
returned rather than a resolved pixel coordinate.  Read #AbsX for the absolute horizontal pixel position within the
surface hierarchy.

Changing `X` on a visible surface updates its position immediately.  If #XOffset also defines the right-hand edge, the
surface width is recalculated to preserve that offset.

*********************************************************************************************************************/

static ERR GET_X(extSurface *Self, Unit &Value)
{
   if (Value.verbatim() or (not Self->initialised())){
      Value = Self->X;
      Value.Type |= FD_PURE;
      return ERR::Okay;
   }
   else if (Value.scaled()) { // Client requested scaled unit
      if (Self->X.scaled()) Value = Self->X;
      else if (Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            Value = Unit(double(Self->FixedX) / double(parent->FixedWidth), FD_SCALED);
         }
         else return ERR::AccessObject;
      }
      else Value = Unit();
   }
   else { // Client requested a fixed (resolved pixel) unit
      Value = Unit(Self->FixedX);
   }

   return ERR::Okay;
}

static ERR SET_X(extSurface *Self, Unit &Value)
{
   if (Value.scaled()) {
      Self->X = Value;
      if (Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            move_layer(Self, parent->FixedWidth * Value, Self->FixedY);
         }
         else return ERR::AccessObject;
      }
   }
   else {
      Self->X = Value;
      move_layer(Self, Value, Self->FixedY);

      // If our right-hand side is relative, we need to resize our surface to counteract the movement.

      if (Self->ParentID and Self->XOffset.defined()) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            resize_layer(Self, Self->FixedX, Self->FixedY, parent->FixedWidth - Self->FixedX - Self->FixedXO, 0, 0, 0, 0, 0, 0);
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

A plain read returns the offset resolved to a fixed pixel distance, even when the offset was defined as a scaled value.
If no offset has been defined but both #X and #Width are present, the offset is derived from the parent width.  A scaled
read returns the offset as a multiplier of the surface width.
-END-

*********************************************************************************************************************/

static ERR GET_XOffset(extSurface *Self, Unit &Value)
{
   if (Value.verbatim() or (not Self->initialised())) {
      Value = Self->XOffset;
      Value.Type |= FD_PURE;
   }
   else if (Value.scaled()) {
      Unit fixed_offset;
      if (!GET_XOffset(Self, fixed_offset)) Value = Unit(fixed_offset / Self->FixedWidth, FD_SCALED);
      else Value = Unit(0, FD_SCALED);
   }
   else if (Self->XOffset.defined()) {
      if (Self->XOffset.scaled()) Value = Unit(Self->FixedXO);
      else Value = Self->XOffset;
   }
   else if (Self->Width.defined() and Self->X.defined() and Self->ParentID) {
      if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
         Value = Unit(parent->FixedWidth - Self->FixedX - Self->FixedWidth);
      }
      else return ERR::AccessObject;
   }
   else Value = Unit(0);

   return ERR::Okay;
}

static ERR SET_XOffset(extSurface *Self, Unit &Value)
{
   auto value = Value;
   if (value.Value < 0) value.Value = -value.Value;

   Self->XOffset = value;

   if (value.scaled()) {
      if (Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            Self->FixedXO = int16_t(parent->FixedWidth * value);
            if (not Self->X.defined()) Self->FixedX = parent->FixedWidth - Self->FixedXO - Self->FixedWidth;
            if (not Self->Width.defined()) {
               resize_layer(Self, Self->FixedX, Self->FixedY,
                  parent->FixedWidth - Self->FixedX - Self->FixedXO, 0, 0, 0, 0, 0, 0);
            }
            else move_layer(Self, parent->FixedWidth - Self->FixedXO - Self->FixedWidth, Self->FixedY);
         }
         else return ERR::AccessObject;
      }
   }
   else {
      Self->FixedXO = int16_t(value);

      if (Self->Width.defined() and Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            move_layer(Self, parent->FixedWidth - Self->FixedXO - Self->FixedWidth, Self->FixedY);
         }
         else return ERR::AccessObject;
      }
      else if (Self->X.defined() and Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            resize_layer(Self, Self->FixedX, Self->FixedY, parent->FixedWidth - Self->FixedX - Self->FixedXO, 0, 0, 0, 0, 0, 0);
         }
         else return ERR::AccessObject;
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Y: Determines the vertical position of a surface object.

Set `Y` to change the vertical position of the surface relative to its parent.

By default the value is a fixed coordinate unit.  With the `FD_SCALED` flag, the value is treated as a multiplier of
the parent surface height.

A read returns the value exactly as it was last defined.  When `Y` was set as a scaled value, the multiplier is
returned rather than a resolved pixel coordinate.  Read #AbsY for the absolute vertical pixel position within the
surface hierarchy.

Changing `Y` on a visible surface updates its position immediately.

*********************************************************************************************************************/

static ERR GET_Y(extSurface *Self, Unit &Value)
{
   if (Value.verbatim() or (not Self->initialised())){
      Value = Self->Y;
      Value.Type |= FD_PURE;
      return ERR::Okay;
   }
   else if (Value.scaled()) { // Client requested scaled unit
      if (Self->Y.scaled()) Value = Self->Y;
      else if (Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            Value = Unit(double(Self->FixedY) / double(parent->FixedHeight), FD_SCALED);
         }
         else return ERR::AccessObject;
      }
      else Value = Unit();
   }
   else { // Client requested a fixed (resolved pixel) unit
      Value = Unit(Self->FixedY);
   }

   return ERR::Okay;
}

static ERR SET_Y(extSurface *Self, Unit &Value)
{
   if (Value.scaled()) {
      Self->Y = Value;
      if (Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            move_layer(Self, Self->FixedX, parent->FixedHeight * Value);
         }
         else return ERR::AccessObject;
      }
   }
   else {
      Self->Y = Value;
      move_layer(Self, Self->FixedX, int(Value));
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

A plain read returns the offset resolved to a fixed pixel distance, even when the offset was defined as a scaled value.
If no offset has been defined but both #Y and #Height are present, the offset is derived from the parent height.  A
scaled read returns the offset as a multiplier of the surface height.
-END-

*********************************************************************************************************************/

static ERR GET_YOffset(extSurface *Self, Unit &Value)
{
   if (Value.verbatim() or (not Self->initialised())) {
      Value = Self->YOffset;
      Value.Type |= FD_PURE;
   }
   else if (Value.scaled()) {
      Unit fixed_offset;
      if (!GET_YOffset(Self, fixed_offset)) Value = Unit(fixed_offset / Self->FixedHeight, FD_SCALED);
      else Value = Unit(0);
   }
   else if (Self->YOffset.defined()) {
      if (Self->YOffset.scaled()) Value = Unit(Self->FixedYO);
      else Value = Self->YOffset;
   }
   else if (Self->Height.defined() and Self->Y.defined() and Self->ParentID) {
      if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
         Value = Unit(parent->FixedHeight - Self->FixedY - Self->FixedHeight);
      }
      else return ERR::AccessObject;
   }
   else Value = Unit(0);

   return ERR::Okay;
}

static ERR SET_YOffset(extSurface *Self, Unit &Value)
{
   auto value = Value;

   if (value.Value < 0) value.Value = -value.Value;

   Self->YOffset = value;

   if (value.scaled()) {
      if (Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            Self->FixedYO = int16_t(parent->FixedHeight * value);
            if (not Self->Y.defined()) Self->FixedY = parent->FixedHeight - Self->FixedYO - Self->FixedHeight;
            if (not Self->Height.defined()) {
               resize_layer(Self, Self->FixedX, Self->FixedY, 0, parent->FixedHeight - Self->FixedY - Self->FixedYO, 0, 0, 0, 0, 0);
            }
            else move_layer(Self, Self->FixedX, parent->FixedHeight - Self->FixedYO - Self->FixedHeight);
         }
         else return ERR::AccessObject;
      }
   }
   else {
      Self->FixedYO = int16_t(value);

      if (Self->Height.defined() and Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            if (not Self->Height.defined()) {
               resize_layer(Self, Self->FixedX, Self->FixedY, 0,
                  parent->FixedHeight - Self->FixedY - Self->FixedYO, 0, 0, 0, 0, 0);
            }
            else move_layer(Self, Self->FixedX, parent->FixedHeight - Self->FixedYO - Self->FixedHeight);
         }
         else return ERR::AccessObject;
      }
      else if (Self->Y.defined() and Self->ParentID) {
         if (ScopedObjectLock<extSurface> parent(Self->ParentID, 500); parent.granted()) {
            resize_layer(Self, Self->FixedX, Self->FixedY, 0,
               parent->FixedHeight - Self->FixedY - Self->FixedYO, 0, 0, 0, 0, 0);
         }
         else return ERR::AccessObject;
      }
   }
   return ERR::Okay;
}

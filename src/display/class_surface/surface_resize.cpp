
/*********************************************************************************************************************
-ACTION-
Redimension: Moves and resizes a surface object in a single action call.
-END-
*********************************************************************************************************************/

static ERR SURFACE_Redimension(extSurface *Self, struct acRedimension *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs)|ERR::Notified;

   if ((Args->Width < 0) or (Args->Height < 0)) {
      log.trace("Bad width/height: %.0fx%.0f", Args->Width, Args->Height);
      return ERR::Args|ERR::Notified;
   }

   if (auto msg = GetActionMsg()) { // If this action was called as a message, then it could have been delayed and thus superseded by a more recent call.
      if (msg->Time < Self->LastRedimension) {
         log.trace("Ignoring superseded redimension message (%" PF64 " < %" PF64 ").", msg->Time, Self->LastRedimension);
         return ERR::Okay|ERR::Notified;
      }
   }

   // 2013-12: For a long time this sub-routine was commented out.  Have brought it back to try and keep the queue
   // clear of redundant redimension messages.  Seems fine...

   if (Self->visible()) { // Visibility check because this sub-routine doesn't play nice with hidden surfaces.
      uint8_t msgbuffer[sizeof(Message) + sizeof(ActionMessage)];
      int index = 0;
      while (ScanMessages(&index, MSGID::ACTION, msgbuffer, sizeof(msgbuffer)) IS ERR::Okay) {
         auto action = (ActionMessage *)(msgbuffer + sizeof(Message));
         if ((action->ActionID IS AC::Redimension) and (action->ObjectID IS Self->UID)) {
            return ERR::Okay|ERR::Notified;
         }
      }
   }

   Self->LastRedimension = PreciseTime();

   int oldx      = Self->X;
   int oldy      = Self->Y;
   int oldwidth  = Self->Width;
   int oldheight = Self->Height;

   // Extract the new dimensions from the arguments

   int newx = int(Args->X);
   int newy = int(Args->Y);
   int newwidth  = (!Args->Width) ? Self->Width : int(Args->Width);
   int newheight = (!Args->Height) ? Self->Height : int(Args->Height);

   // Ensure that the requested width does not exceed minimum and maximum values

   if ((Self->MinWidth > 0) and (newwidth < Self->MinWidth)) {
      if (oldwidth > newwidth) {
         if (oldwidth > Self->MinWidth) newwidth = Self->MinWidth;
         else newwidth = oldwidth; // Maintain the current width because it is < MinWidth
      }
   }

   if ((Self->MaxWidth > 0) and (newwidth > Self->MaxWidth)) newwidth = Self->MaxWidth;

   if (newwidth < 2) newwidth = 2;

   // Check requested height against minimum and maximum height values

   if ((Self->MinHeight > 0) and (newheight < Self->MinHeight)) {
      if (oldheight > newheight) {
         if (oldheight > Self->MinHeight) newheight = Self->MinHeight;
         else newheight = oldheight;
      }
   }

   if ((Self->MaxHeight > 0) and (newheight > Self->MaxHeight)) {
      newheight = Self->MaxHeight;
   }

   if (newheight < 2) newheight = 2;

   // Check for changes

   if ((newx IS oldx) and (newy IS oldy) and (newwidth IS oldwidth) and (newheight IS oldheight)) {
      return ERR::Okay|ERR::Notified;
   }

   log.traceBranch("%dx%d %dx%d (req. %dx%d, %dx%d) Depth: %.0f $%.8x", newx, newy, newwidth, newheight, int(Args->X), int(Args->Y), int(Args->Width), int(Args->Height), Args->Depth, int(Self->Flags));

   ERR error = resize_layer(Self, newx, newy, newwidth, newheight, newwidth, newheight, int(Args->Depth), 0.0, 0);
   return error|ERR::Notified;
}

/*********************************************************************************************************************
-ACTION-
Resize: Alters the dimensions of a surface object.
-END-
*********************************************************************************************************************/

static ERR SURFACE_Resize(extSurface *Self, struct acResize *Args)
{
   if (!Args) return ERR::NullArgs|ERR::Notified;

   if (((!Args->Width) or (Args->Width IS Self->Width)) and
       ((!Args->Height) or (Args->Height IS Self->Height))) return ERR::Okay|ERR::Notified;

   struct acRedimension redimension = { (double)Self->X, (double)Self->Y, 0, Args->Width, Args->Height, Args->Depth };
   return Action(AC::Redimension, Self, &redimension)|ERR::Notified;
}

/*********************************************************************************************************************

-METHOD-
SetDisplay: Changes the screen resolution (applies to top-level surface objects only).

The SetDisplay method is used to change the screen resolution of the top-level surface object (which represents the
screen display).  It allows you to set the size of the display and you may also change the bitmap depth and the
monitor's refresh rate.  If successful, the change is immediate.

This method exercises some intelligence in adjusting the display to your requested settings.  For instance, if the
requested width and/or height is not available, the closest display setting will be chosen.

This method does not work on anything other than top-level surface objects.  The current top-level surface object is
usually named "SystemSurface" by default and can be searched for by that name.

-INPUT-
int X: The horizontal coordinate/offset for the display.
int Y: The vertical coordinate/offset for the display.
int Width: The width of the display.
int Height: The height of the display.
int InsideWidth: The page width of the display must be the same as Width or greater.
int InsideHeight: The page height of the display must be the same as Height or greater.
int BitsPerPixel: Bits per pixel - 15, 16, 24 or 32.
double RefreshRate: Refresh rate.
int Flags: Optional flags.

-ERRORS-
Okay
Args
InvalidState: The surface is not a top-level surface object.
-END-

*********************************************************************************************************************/

static ERR SURFACE_SetDisplay(extSurface *Self, struct gfx::SetDisplay *Args)
{
   pf::Log log;

   if ((!Args) or (Args->Width < 0) or (Args->Height < 0)) return log.warning(ERR::Args);
   if (Self->ParentID) return log.warning(ERR::InvalidState);

   int newx = Args->X;
   int newy = Args->Y;
   int newwidth  = (!Args->Width) ? Self->Width : Args->Width;
   int newheight = (!Args->Height) ? Self->Height : Args->Height;

   //if ((newx IS Self->X) and (newy IS Self->Y) and (newwidth IS Self->Width) and (newheight IS Self->Height)) return ERR::Okay;

   log.branch("%dx%d,%dx%d, BPP %d", newx, newy, newwidth, newheight, Args->BitsPerPixel);

   ERR error = resize_layer(Self, newx, newy, newwidth, newheight,
      Args->InsideWidth, Args->InsideHeight, Args->BitsPerPixel,
      Args->RefreshRate, Args->Flags);

   return error;
}

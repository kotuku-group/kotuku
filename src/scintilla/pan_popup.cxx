/*********************************************************************************************************************
** Code assistance pop-ups
**
** Scintilla displays call tips and completion lists in Windows of their own, which it positions in the client
** coordinates of the editor.  Each pop-up is a PanPopup, which is drawn as a separate scene-level VectorViewport
** so that it inherits neither the root viewport's transforms nor its clipping.  It contains a rectangle that is filled
** with a bitmap, into which the content of the pop-up is painted with SurfacePan.  The vectors are created on first
** use, are retained while the pop-up is hidden, and are freed with the pop-up.  The editor frees its pop-ups before
** the editor itself is freed, so they cannot outlive it.  If the scene is freed first, the pop-up drops its references
** to the vectors.
**
** Pop-ups are not transformed with the host.  They are drawn at the editor's font size, and are attached to the scene
** position of the document position that they refer to (see place()).  Positioning and painting are deferred to the
** Repaint method, which follows any scrolling of the editor and any change to the position of the host.  Scrolling
** the referenced position out of the View closes the pop-ups.
**
** A click on a pop-up gives the focus to its viewport, which would close the pop-ups because Scintilla cancels them
** when it loses the focus.  update_focus() ignores the loss while a pop-up has the focus, and input_popup() returns
** the focus to the editor.
*/

static std::vector<PanPopup *> glPopups; // All existing pop-ups.  Scintilla objects are only used from the main thread.

static const int POPUP_BORDER = 1;    // Width of the border that surrounds each pop-up

static ERR input_popup(objVectorRectangle *, const InputEvent *);
static void notify_free_popup(OBJECTPTR, ACTIONID, ERR, APTR);

//********************************************************************************************************************
// Returns the PanPopup that a WindowID refers to, or nullptr if the ID refers to an editor or nothing.

PanPopup * PanPopup::find(Scintilla::WindowID ID)
{
   if (not ID) return nullptr;
   for (auto popup : glPopups) {
      if ((Scintilla::WindowID)popup IS ID) return popup;
   }
   return nullptr;
}

//********************************************************************************************************************
// Pop-ups are registered from construction so that a Window that refers to one is never mistaken for an editor.

PanPopup::PanPopup()
{
   glPopups.push_back(this);
}

PanPopup::~PanPopup()
{
   release();
   std::erase(glPopups, this);
}

//********************************************************************************************************************
// Create the vectors of the pop-up, if they do not already exist.  The viewport is a sibling of the scene's root,
// so its coordinates and font size remain in scene pixels.  The bitmap is owned by the editor, and the image
// definition by the scene.

ERR PanPopup::attach(extScintilla *Scintilla, CSTRING Name)
{
   kt::Log log(__FUNCTION__);

   if ((viewport) and (image) and (scintilla IS Scintilla)) return ERR::Okay;

   release();
   scintilla = Scintilla;

   if ((not Scintilla) or (not Scintilla->Viewport) or (not Scintilla->Scene) or (not Scintilla->Scene->Viewport)) {
      return log.warning(ERR::NotPossible);
   }

   kt::SwitchContext ctx(Scintilla); // The editor receives the notifications and input of the pop-up

   auto scene = Scintilla->Scene;

   if (!(bitmap = objBitmap::create::local(
         fl::Width(1), fl::Height(1),
         fl::BitsPerPixel(32),
         fl::Flags(BMF::ALPHA_CHANNEL)))) return log.warning(ERR::CreateObject);

   if (!(viewport = objVectorViewport::create::global(
         fl::Name(Name),
         fl::Owner(scene->UID),
         fl::X(0), fl::Y(0), fl::Width(1), fl::Height(1),
         fl::Visibility(VIS::HIDDEN)))) {
      release();
      return log.warning(ERR::CreateObject);
   }

   SubscribeAction(viewport, AC::Free, C_FUNCTION(notify_free_popup));

   if (!(image = objVectorImage::create::global(
         fl::Owner(scene->UID),
         fl::Bitmap(bitmap),
         fl::Units(VUNIT::BOUNDING_BOX),
         fl::SpreadMethod(VSPREAD::CLIP),
         fl::AspectRatio(ARF::X_MIN|ARF::Y_MIN)))) {
      release();
      return log.warning(ERR::CreateObject);
   }

   SubscribeAction(image, AC::Free, C_FUNCTION(notify_free_popup));

   auto def_name = std::format("sciPopup{}", image->UID);
   if (auto error = scene->addDef(def_name, image); error != ERR::Okay) {
      release();
      return log.warning(error);
   }

   if (!(canvas = objVectorRectangle::create::global(
         fl::Name("sciPopupCanvas"),
         fl::Owner(viewport->UID),
         fl::X(0), fl::Y(0), fl::Width(1), fl::Height(1),
         fl::Cursor(PTC::DEFAULT),
         fl::Fill(std::format("url(#{})", def_name))))) {
      release();
      return log.warning(ERR::CreateObject);
   }

   if (scene->SurfaceID) {
      canvas->subscribeInput(JTYPE::BUTTON|JTYPE::MOVEMENT|JTYPE::EXT_MOVEMENT, C_FUNCTION(input_popup));
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Free the vectors and the bitmap.  Freeing the viewport also frees the canvas.

void PanPopup::release()
{
   shown = false;
   dirty = false;

   if (not scintilla) return;

   kt::SwitchContext ctx(scintilla); // The subscriptions belong to the editor

   if (image) {
      UnsubscribeAction(image, AC::NIL);
      FreeResource(image);
      image = nullptr;
   }

   if (viewport) {
      UnsubscribeAction(viewport, AC::NIL);
      bool redraw = viewport->Visibility != VIS::HIDDEN;
      FreeResource(viewport);
      viewport = nullptr;
      canvas = nullptr;
      if ((redraw) and (scintilla->Viewport)) acDraw(scintilla->Viewport);
   }

   if (bitmap) { FreeResource(bitmap); bitmap = nullptr; }
}

//********************************************************************************************************************
// Set the area of the pop-up in the client coordinates of the editor.  The anchor is recorded so that the pop-up can
// follow the referenced text when the editor scrolls.

void PanPopup::set_position(Scintilla::PRectangle Rect)
{
   rect = Rect;
   if ((not scintilla) or (not scintilla->API)) return;

   anchor = scintilla->API->panLocation(anchor_position());

   int width = std::max(1, rect.Width());
   int height = std::max(1, rect.Height());

   if ((bitmap) and ((bitmap->Width != width) or (bitmap->Height != height))) {
      if (acResize(bitmap, width, height, 0) != ERR::Okay) return;
   }

   if (canvas) {
      canvas->setWidth(width);
      canvas->setHeight(height);
   }

   if (viewport) {
      viewport->setWidth(width);
      viewport->setHeight(height);
   }

   invalidate();
}

//********************************************************************************************************************
// A shown pop-up becomes visible once it has been positioned and painted by update_popups().  Hiding is immediate.

void PanPopup::show(bool Show)
{
   if (Show IS shown) return;
   shown = Show;

   if (Show) invalidate();
   else if ((viewport) and (viewport->Visibility != VIS::HIDDEN)) {
      viewport->setVisibility(VIS::HIDDEN);
      if ((scintilla->Scene) and (scintilla->Scene->SurfaceID)) acDraw(viewport);
   }
}

//********************************************************************************************************************

void PanPopup::invalidate()
{
   dirty = true;
   if (scintilla) queue_repaint(scintilla);
}

/*********************************************************************************************************************
** Position the pop-up in the scene.  The anchor is the client position of the document position that the pop-up
** refers to, and is mapped to the scene through the transformed bounds of the View.  Offsets from the anchor are
** not scaled, because the pop-up is not scaled.  A pop-up that would extend beyond the bottom or top of the scene is
** moved to the other side of the anchor's line if it fits there, and the result is kept within the scene.  Returns
** false if the anchor is not visible in the View, in which case the caller closes the pop-ups.
*/

bool PanPopup::place()
{
   auto Self = scintilla;
   if ((not viewport) or (not Self->API) or (not Self->Scene)) return true;

   double bx, by, sx, sy;
   if (not view_mapping(Self, bx, by, sx, sy)) return true;

   auto point = Self->API->panLocation(anchor_position());
   int line_height = Self->API->panLineHeight();

   if ((point.x < 0) or (point.x > Self->ViewWidth) or (point.y + line_height <= 0) or (point.y >= Self->ViewHeight)) {
      return false;
   }

   const double width  = rect.Width();
   const double height = rect.Height();
   const int left      = rect.left + point.x - anchor.x;
   const int top       = rect.top + point.y - anchor.y;
   const double line_top    = by + (point.y * sy);
   const double line_bottom = by + ((point.y + line_height) * sy);

   double x = bx + (point.x * sx) + (left - point.x);
   double y;
   const bool below = top >= point.y + line_height;
   if (below) y = line_bottom + (top - (point.y + line_height));
   else y = line_top - (point.y - top);

   const double scene_width  = Self->Scene->PageWidth;
   const double scene_height = Self->Scene->PageHeight;
   if ((scene_width > 0) and (scene_height > 0)) {
      if ((below) and (y + height > scene_height) and (line_top - height >= 0)) y = line_top - height;
      else if ((not below) and (y < 0) and (line_bottom + height <= scene_height)) y = line_bottom;

      x = std::clamp(x, 0.0, std::max(0.0, scene_width - width));
      y = std::clamp(y, 0.0, std::max(0.0, scene_height - height));
   }

   x = std::round(x);
   y = std::round(y);
   if (x != scene_x) viewport->setX(x);
   if (y != scene_y) viewport->setY(y);
   scene_x = x;
   scene_y = y;
   return true;
}

//********************************************************************************************************************
// Paint the content of the pop-up into its bitmap.

void PanPopup::redraw()
{
   dirty = false;
   if ((not bitmap) or (not scintilla) or (not scintilla->API)) return;

   SurfacePan surface;
   surface.Init(bitmap, scintilla);
   paint(surface);
}

//********************************************************************************************************************
// Called by the Repaint method to position, paint and reveal the editor's shown pop-ups.

static void update_popups(extScintilla *Self)
{
   bool cancel = false;
   for (auto popup : glPopups) {
      if ((popup->scintilla != Self) or (not popup->shown) or (not popup->viewport)) continue;

      if (not popup->place()) {
         cancel = true;
         continue;
      }

      bool redraw = popup->dirty;
      if (popup->dirty) popup->redraw();

      if (popup->viewport->Visibility IS VIS::HIDDEN) {
         popup->viewport->setVisibility(VIS::VISIBLE);
         acMoveToFront(popup->viewport);
         redraw = true;
      }

      if ((redraw) and (Self->Scene->SurfaceID)) acDraw(popup->canvas);
   }

   if ((cancel) and (Self->API)) Self->API->panCancelPopups();
}

//********************************************************************************************************************
// Returns true if one of the editor's pop-ups has the focus.

static bool popup_has_focus(extScintilla *Self)
{
   for (auto popup : glPopups) {
      if ((popup->scintilla IS Self) and (popup->viewport) and ((popup->viewport->Flags & VF::HAS_FOCUS) != VF::NIL)) {
         return true;
      }
   }
   return false;
}

//********************************************************************************************************************
// The scene or the editor's host is being freed, which takes the vectors of the pop-up with it.

static void notify_free_popup(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   for (auto popup : glPopups) {
      if ((OBJECTPTR)popup->viewport IS Object) {
         popup->viewport = nullptr;
         popup->canvas = nullptr;
      }
      if ((OBJECTPTR)popup->image IS Object) popup->image = nullptr;
   }
}

//********************************************************************************************************************
// Pointer input for a pop-up.  Positions are reported in the coordinate system of the pop-up's viewport, which is
// that of the pop-up's content.

static ERR input_popup(objVectorRectangle *Canvas, const InputEvent *Events)
{
   auto Self = (extScintilla *)CurrentContext();
   if ((not Self->API) or ((Self->Flags & SCIF::DISABLED) != SCIF::NIL)) return ERR::Okay;

   PanPopup *popup = nullptr;
   for (auto p : glPopups) {
      if ((p->scintilla IS Self) and (p->canvas IS Canvas)) { popup = p; break; }
   }
   if (not popup) return ERR::Okay;

   for (auto event=Events; event; event=event->Next) {
      if (not popup->shown) break;

      if (((event->Flags & JTYPE::BUTTON) != JTYPE::NIL) and (event->Type IS JET::LMB) and (event->Value > 0)) {
         if (Self->Focus) acFocus(Self->Focus);
      }

      popup->input(event);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
** The call tip.  CallTip paints its content with the editor's regular font and the colours of STYLE_CALLTIP.
*/

class CallTipPopup : public PanPopup {
public:
   void paint(SurfacePan &Surface) override {
      scintilla->API->panPaintCallTip(&Surface);
   }

   void input(const InputEvent *Event) override {
      if (((Event->Flags & JTYPE::BUTTON) != JTYPE::NIL) and (Event->Type IS JET::LMB) and (Event->Value > 0)) {
         scintilla->API->panCallTipClick(Scintilla::Point(int(std::floor(Event->X)), int(std::floor(Event->Y))));
      }
   }

   int anchor_position() override {
      return scintilla->API->panCallTipStart();
   }
};

//********************************************************************************************************************
// Called by CallTipShow() before the call tip is positioned and shown.  The call tip's Window is null if its vectors
// cannot be created, in which case it is not displayed.

void ScintillaKTK::CreateCallTipWindow(Scintilla::PRectangle rc)
{
   kt::Log(__FUNCTION__).trace("");

   if (not calltip_popup) calltip_popup = std::make_unique<CallTipPopup>();
   if (calltip_popup->attach(scintilla, "sciCallTip") IS ERR::Okay) {
      ct.wCallTip = (Scintilla::WindowID)calltip_popup.get();
   }
}

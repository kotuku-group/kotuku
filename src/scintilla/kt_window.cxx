// Window class (see include/Platform.h)
//
// The 'id' member of the editor's Window references the Kotuku Scintilla object.  The Window class is not the
// application window, but the View viewport that hosts the Scintilla client area.
//
// Windows that Scintilla creates for itself, i.e. the call tip and the completion list, reference a PanPopup instead
// (see pan_popup.cxx).  PanPopup::find() distinguishes them from the editor.

inline extScintilla * get_scintilla(const Scintilla::Internal::Window *Win)
{
   if (PanPopup::find(Win->GetID())) return nullptr;
   return (extScintilla *)Win->GetID();
}

//********************************************************************************************************************
// Retrieve the mapping of the editor's client coordinates to the coordinates of its scene.  The mapping is derived
// from the transformed bounds of the View, which is exact for translated and scaled hosts.  Rotated and skewed hosts
// are approximated by their bounding box.  Client coordinates are device pixels, so the scale is normally 1 unless
// the device scale is limited (see get_view_transform()).

static bool view_mapping(extScintilla *Self, double &X, double &Y, double &ScaleX, double &ScaleY)
{
   if (not Self->View) return false;

   double width, height;
   if (Self->View->getBoundary(VBF::NIL, &X, &Y, &width, &height) != ERR::Okay) return false;

   Unit w, h;
   Self->View->getWidth(w);
   Self->View->getHeight(h);
   ScaleX = (double(w) > 0) ? width / (double(w) * Self->Scale) : 1.0;
   ScaleY = (double(h) > 0) ? height / (double(h) * Self->Scale) : 1.0;
   if ((ScaleX <= 0) or (ScaleY <= 0)) ScaleX = ScaleY = 1.0;
   return true;
}

//********************************************************************************************************************

Scintilla::Internal::Window::~Window() noexcept
{
   Destroy();
}

//********************************************************************************************************************

void Scintilla::Internal::Window::Destroy() noexcept
{
   // The editor's Window does not own the editor.  A pop-up is hidden, and is retained for reuse by its owner.
   if (auto popup = PanPopup::find(wid)) {
      popup->show(false);
      popup->closed();
   }
   wid = 0;
}

//********************************************************************************************************************
// We're returning the position in absolute coordinates

Scintilla::Internal::PRectangle Scintilla::Internal::Window::GetPosition() const
{
   if (auto popup = PanPopup::find(wid)) return popup->rect;

   // Before any size allocated pretend its 1000 wide so not scrolled
   Scintilla::Internal::PRectangle rc(0, 0, 1000, 1000);

   auto scintilla = get_scintilla(this);
   if ((scintilla) and (scintilla->View)) {
      int x = 0, y = 0;
      scintilla->View->getAbsX(x);
      scintilla->View->getAbsY(y);
      rc.left   = x;
      rc.top    = y;
      rc.right  = x + scintilla->ViewWidth;
      rc.bottom = y + scintilla->ViewHeight;
   }

   return rc;
}

//********************************************************************************************************************
// Pop-ups are always positioned in the client coordinates of the editor that owns them.

void Scintilla::Internal::Window::SetPosition(Scintilla::Internal::PRectangle rc)
{
   if (auto popup = PanPopup::find(wid)) popup->set_position(rc);
}

//********************************************************************************************************************

void Scintilla::Internal::Window::SetPositionRelative(Scintilla::Internal::PRectangle Rect,
   const Scintilla::Internal::Window *)
{
   if (auto popup = PanPopup::find(wid)) popup->set_position(Rect);
}

//********************************************************************************************************************

Scintilla::Internal::PRectangle Scintilla::Internal::Window::GetClientPosition() const
{
   if (auto popup = PanPopup::find(wid)) {
      return Scintilla::Internal::PRectangle(0, 0, popup->rect.Width(), popup->rect.Height());
   }

   auto scintilla = get_scintilla(this);
   if (scintilla) return Scintilla::Internal::PRectangle(0, 0, scintilla->ViewWidth, scintilla->ViewHeight);
   else return Scintilla::Internal::PRectangle(0, 0, 0, 0);
}

//********************************************************************************************************************
// Scintilla places pop-ups within these bounds.  Pop-ups are displayed in the editor's scene, so the bounds of the
// scene are returned in client coordinates.

Scintilla::Internal::PRectangle Scintilla::Internal::Window::GetMonitorRect(Scintilla::Internal::Point)
{
   double x, y, sx, sy;
   if (auto scintilla = get_scintilla(this); (scintilla) and (scintilla->Scene) and
       (scintilla->Scene->PageWidth > 0) and (scintilla->Scene->PageHeight > 0) and
       (view_mapping(scintilla, x, y, sx, sy))) {
      auto scene = scintilla->Scene;
      return Scintilla::Internal::PRectangle(int(std::floor(-x / sx)), int(std::floor(-y / sy)),
         int(std::ceil((scene->PageWidth - x) / sx)), int(std::ceil((scene->PageHeight - y) / sy)));
   }

   DisplayInfo *info;
   if (!gfx::GetDisplayInfo(0, &info)) {
      return Scintilla::Internal::PRectangle(0, 0, info->Width, info->Height);
   }
   else return Scintilla::Internal::PRectangle();
}

//********************************************************************************************************************

void Scintilla::Internal::Window::Show(bool show)
{
   if (auto popup = PanPopup::find(wid)) popup->show(show);
}

//********************************************************************************************************************

void Scintilla::Internal::Window::InvalidateAll()
{
   if (auto popup = PanPopup::find(wid)) popup->invalidate();
   else if (auto scintilla = get_scintilla(this)) {
      invalidate_area(scintilla, 0, 0, scintilla->ViewWidth, scintilla->ViewHeight);
   }
}

//********************************************************************************************************************

void Scintilla::Internal::Window::InvalidateRectangle(Scintilla::Internal::PRectangle rc)
{
   if (auto popup = PanPopup::find(wid)) popup->invalidate();
   else if (auto scintilla = get_scintilla(this)) invalidate_area(scintilla, rc.left, rc.top, rc.right, rc.bottom);
}

//********************************************************************************************************************
// Change the pointer cursor that is shown over the client.  The cursor belongs to the Canvas because it receives the
// pointer input; the scene uses the cursor of the top-most vector under the pointer.

void Scintilla::Internal::Window::SetCursor(Cursor Curs)
{
   auto scintilla = get_scintilla(this);
   if ((not scintilla) or (not scintilla->Canvas)) return;

   PTC cursor;
   switch (Curs) {
      case Cursor::text:  cursor = PTC::TEXT; break;
      case Cursor::up:    cursor = PTC::SIZE_TOP; break;
      case Cursor::wait:  cursor = PTC::SLEEP; break;
      case Cursor::horizontal: cursor = PTC::SPLIT_HORIZONTAL; break;
      case Cursor::vertical:  cursor = PTC::SPLIT_VERTICAL; break;
      case Cursor::hand:  cursor = PTC::HAND; break;
      default:          cursor = PTC::DEFAULT; break; // Includes cursorArrow and cursorReverseArrow
   }

   if (scintilla->Canvas->Cursor != cursor) scintilla->Canvas->setCursor(cursor);
}

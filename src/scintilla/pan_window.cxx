// Window class (see include/Platform.h)
//
// The 'id' member of the editor's Window references the Kotuku Scintilla object.  The Window class is not the
// application window, but the View viewport that hosts the Scintilla client area.
//
// Windows that Scintilla creates for itself, i.e. the call tip and the completion list, reference a PanPopup instead
// (see pan_popup.cxx).  PanPopup::find() distinguishes them from the editor.

inline extScintilla * get_scintilla(Scintilla::Window *Win)
{
   if (PanPopup::find(Win->GetID())) return nullptr;
   return (extScintilla *)Win->GetID();
}

//********************************************************************************************************************
// Retrieve the mapping of the editor's client coordinates to the coordinates of its scene.  The mapping is derived
// from the transformed bounds of the View, which is exact for translated and scaled hosts.  Rotated and skewed hosts
// are approximated by their bounding box.

static bool view_mapping(extScintilla *Self, double &X, double &Y, double &ScaleX, double &ScaleY)
{
   if (not Self->View) return false;

   double width, height;
   if (Self->View->getBoundary(VBF::NIL, &X, &Y, &width, &height) != ERR::Okay) return false;

   Unit w, h;
   Self->View->getWidth(w);
   Self->View->getHeight(h);
   ScaleX = (double(w) > 0) ? width / double(w) : 1.0;
   ScaleY = (double(h) > 0) ? height / double(h) : 1.0;
   if ((ScaleX <= 0) or (ScaleY <= 0)) ScaleX = ScaleY = 1.0;
   return true;
}

//********************************************************************************************************************

Scintilla::Window::~Window()
{
   Destroy();
}

//********************************************************************************************************************

void Scintilla::Window::Destroy()
{
   // The editor's Window does not own the editor.  A pop-up is hidden, and is retained for reuse by its owner.
   if (auto popup = PanPopup::find(wid)) {
      popup->show(false);
      popup->closed();
   }
   wid = 0;
}

//********************************************************************************************************************
// The focus state is initialised from the Focus viewport and kept current by feedback_focus().

bool Scintilla::Window::HasFocus()
{
   if (PanPopup::find(wid)) return false;

   auto scintilla = get_scintilla(this);
   return (scintilla) and (scintilla->HasFocus);
}

//********************************************************************************************************************
// We're returning the position in absolute coordinates

Scintilla::PRectangle Scintilla::Window::GetPosition()
{
   if (auto popup = PanPopup::find(wid)) return popup->rect;

   // Before any size allocated pretend its 1000 wide so not scrolled
   Scintilla::PRectangle rc(0, 0, 1000, 1000);

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

void Scintilla::Window::SetPosition(Scintilla::PRectangle rc)
{
   if (auto popup = PanPopup::find(wid)) popup->set_position(rc);
}

//********************************************************************************************************************

void Scintilla::Window::SetPositionRelative(Scintilla::PRectangle rc, Scintilla::Window relativeTo)
{
   if (auto popup = PanPopup::find(wid)) popup->set_position(rc);
}

//********************************************************************************************************************

Scintilla::PRectangle Scintilla::Window::GetClientPosition()
{
   if (auto popup = PanPopup::find(wid)) return Scintilla::PRectangle(0, 0, popup->rect.Width(), popup->rect.Height());

   auto scintilla = get_scintilla(this);
   if (scintilla) return Scintilla::PRectangle(0, 0, scintilla->ViewWidth, scintilla->ViewHeight);
   else return Scintilla::PRectangle(0, 0, 0, 0);
}

//********************************************************************************************************************
// Scintilla places pop-ups within these bounds.  Pop-ups are displayed in the editor's scene, so the bounds of the
// scene are returned in client coordinates.

Scintilla::PRectangle Scintilla::Window::GetMonitorRect(Scintilla::Point)
{
   double x, y, sx, sy;
   if (auto scintilla = get_scintilla(this); (scintilla) and (scintilla->Scene) and
       (scintilla->Scene->PageWidth > 0) and (scintilla->Scene->PageHeight > 0) and
       (view_mapping(scintilla, x, y, sx, sy))) {
      auto scene = scintilla->Scene;
      return Scintilla::PRectangle(int(std::floor(-x / sx)), int(std::floor(-y / sy)),
         int(std::ceil((scene->PageWidth - x) / sx)), int(std::ceil((scene->PageHeight - y) / sy)));
   }

   DisplayInfo *info;
   if (!gfx::GetDisplayInfo(0, &info)) {
      return Scintilla::PRectangle(0, 0, info->Width, info->Height);
   }
   else return 0;
}

//********************************************************************************************************************

void Scintilla::Window::Show(bool show)
{
   if (auto popup = PanPopup::find(wid)) popup->show(show);
}

//********************************************************************************************************************

void Scintilla::Window::InvalidateAll()
{
   if (auto popup = PanPopup::find(wid)) popup->invalidate();
   else if (auto scintilla = get_scintilla(this)) {
      invalidate_area(scintilla, 0, 0, scintilla->ViewWidth, scintilla->ViewHeight);
   }
}

//********************************************************************************************************************

void Scintilla::Window::InvalidateRectangle(Scintilla::PRectangle rc)
{
   if (auto popup = PanPopup::find(wid)) popup->invalidate();
   else if (auto scintilla = get_scintilla(this)) invalidate_area(scintilla, rc.left, rc.top, rc.right, rc.bottom);
}

//********************************************************************************************************************

void Scintilla::Window::SetFont(Scintilla::Font &)
{
   // Can not be done generically but only needed for ListBox
}

//********************************************************************************************************************
// Change the pointer cursor that is shown over the client.  The cursor belongs to the Canvas because it receives the
// pointer input; the scene uses the cursor of the top-most vector under the pointer.

void Scintilla::Window::SetCursor(Cursor Curs)
{
   auto scintilla = get_scintilla(this);
   if ((not scintilla) or (not scintilla->Canvas)) return;

   PTC cursor;
   switch (Curs) {
      case cursorText:  cursor = PTC::TEXT; break;
      case cursorUp:    cursor = PTC::SIZE_TOP; break;
      case cursorWait:  cursor = PTC::SLEEP; break;
      case cursorHoriz: cursor = PTC::SPLIT_HORIZONTAL; break;
      case cursorVert:  cursor = PTC::SPLIT_VERTICAL; break;
      case cursorHand:  cursor = PTC::HAND; break;
      default:          cursor = PTC::DEFAULT; break; // Includes cursorArrow and cursorReverseArrow
   }

   if (scintilla->Canvas->Cursor != cursor) scintilla->Canvas->setCursor(cursor);
}

//********************************************************************************************************************
// Report the title string to Scintilla, do not actively attempt to change the title of the nearest window.

void Scintilla::Window::SetTitle(const char *s)
{
   if (auto scintilla = get_scintilla(this)) scintilla->setName(s);
}

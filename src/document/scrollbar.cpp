
static const char *glSliderBkgd = "rgb(225,225,225)";
static const char *glSliderColour = "rgb(185,195,215)";
static const char *glSliderHighlight = "rgb(245,175,155)";

//********************************************************************************************************************
// Subscription to the slider's drag events.  Moving the page is all that is necessary; this
// will result in downstream callbacks making the necessary updates.

static ERR slider_drag(objVectorViewport *Viewport, double X, double Y, double OriginX, double OriginY, scroll_mgr *Scroll)
{
   auto slider_height = Viewport->get<double>(FID_Height);
   auto host_height = Scroll->m_vbar.m_slider_host->get<double>(FID_Height);
   auto page_height = Scroll->m_page->get<double>(FID_Height);
   auto view_height = Scroll->m_view->get<double>(FID_Height);

   if (Y < 0) Y = 0;
   if (Y + slider_height > host_height) Y = host_height - slider_height;
   if (Viewport->get<double>(FID_Y) IS Y) return ERR::Okay;

   if ((Y != Scroll->m_vbar.m_slider_pos.offset) or (slider_height != Scroll->m_vbar.m_slider_pos.length)) {
      const double pct_pos = Y / (host_height - slider_height);
      Scroll->m_page->setFields(fl::Y(-std::trunc((page_height - view_height) * pct_pos)));
   }

   Scroll->m_page->draw();
   return ERR::Okay;
}

//********************************************************************************************************************
// Hook for input events over the slider

static ERR slider_input(objVectorViewport *Viewport, const InputEvent *Events, scroll_mgr *Scroll)
{
   for (auto msg=Events; msg; msg=msg->Next) {
      if (msg->Type IS JET::CROSSED_IN) {
         if (Viewport IS Scroll->m_vbar.m_slider_vp) {
            Scroll->m_vbar.m_slider_rect->setFields(fl::Fill(glSliderHighlight));
            Scroll->m_vbar.m_slider_vp->draw();
         }
      }
      else if (msg->Type IS JET::CROSSED_OUT) {
         if (Viewport IS Scroll->m_vbar.m_slider_vp) {
            Scroll->m_vbar.m_slider_rect->setFields(fl::Fill(glSliderColour));
            Scroll->m_vbar.m_slider_vp->draw();
         }
      }
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Hook for input events within the slider's background viewport.

static ERR bkgd_input(objVectorViewport *Viewport, const InputEvent *Events, scroll_mgr *Scroll)
{
   for (auto msg=Events; msg; msg=msg->Next) {
      if ((msg->Type IS JET::LMB) and (msg->Value > 0)) {
         if (Scroll->m_vbar.m_slider_host IS Viewport) {
            auto slider_y = Scroll->m_vbar.m_slider_vp->get<double>(FID_Y);
            auto slider_height = Scroll->m_vbar.m_slider_vp->get<double>(FID_Height);
            if (msg->Y < slider_y) { // Scroll up?
               Scroll->scroll_page(0, Scroll->m_view->get<double>(FID_Height) * 0.9);
            }
            else if (msg->Y > slider_y + slider_height) { // Scroll down?
               Scroll->scroll_page(0, -Scroll->m_view->get<double>(FID_Height) * 0.9);
            }
         }
         else if (Scroll->m_hbar.m_slider_host IS Viewport) {
            auto slider_x = Scroll->m_vbar.m_slider_vp->get<double>(FID_X);
            auto slider_width = Scroll->m_vbar.m_slider_vp->get<double>(FID_Width);
            if (msg->X < slider_x) {
               Scroll->scroll_page(Scroll->m_view->get<double>(FID_Width) * 0.9, 0);
            }
            else if (msg->X > slider_x + slider_width) {
               Scroll->scroll_page(-Scroll->m_view->get<double>(FID_Width) * 0.9, 0);
            }
         }
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Viewing area has been modified

static ERR view_path_changed(objVectorViewport *Viewport, FM Event, APTR EventObject, scroll_mgr *Scroll)
{
   auto p_x = Scroll->m_page->get<double>(FID_X);
   auto p_y = Scroll->m_page->get<double>(FID_Y);
   auto p_width  = Scroll->m_page->get<double>(FID_Width);
   auto p_height = Scroll->m_page->get<double>(FID_Height);

   auto view_width  = Scroll->m_view->get<double>(FID_Width);
   auto view_height = Scroll->m_view->get<double>(FID_Height);

   if (!Scroll->m_fixed_mode) {
      auto nw = Scroll->m_min_width;
      if (nw < view_width) { // Maximise page width in dynamic mode
         if (p_x != 0) {
            p_x = 0;
            Scroll->m_page->setFields(fl::X(0));
         }
         nw = view_width;
      }

      if (p_width != nw) {
         Scroll->m_page->setFields(fl::Width(nw));
         p_width = nw;
      }
   }

   if (p_x + p_width < view_width) {
      double x = view_width - p_width;
      if (x > 0) x = 0;
      if (p_x != x) Scroll->m_page->setFields(fl::X(int(x)));
   }

   if (p_y + p_height < view_height) {
      double y = view_height - p_height;
      if (y > 0) y = 0;
      if (p_y != y) Scroll->m_page->setFields(fl::Y(int(y)));
   }

   Scroll->recalc_sliders_from_view();
   return ERR::Okay;
}

//********************************************************************************************************************
// Page area has been modified

static ERR page_path_changed(objVectorViewport *Viewport, FM Event, APTR EventObject, scroll_mgr *Scroll)
{
   Scroll->recalc_sliders_from_view();
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR page_movement(objVectorViewport *Viewport, const InputEvent *Events, scroll_mgr *Scroll)
{
   for (auto ev = Events; ev; ev = ev->Next) {
      if (ev->Type IS JET::WHEEL) {
         auto view_height = Scroll->m_view->get<double>(FID_Height);
         auto page_height = Scroll->m_page->get<double>(FID_Height);
         auto length = page_height - view_height;
         if (length > 0) {
            if (length > view_height) length = view_height;
            Scroll->scroll_page(0, -ev->Value * length * 0.06);
         }
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

scroll_mgr::scroll_slider scroll_mgr::scroll_bar::calc_slider(double ViewLen, double PageLen, double HostLen, double Position)
{
   if (PageLen <= ViewLen) { // Hide the scrollbar if the page is smaller than the view
      return scroll_mgr::scroll_slider(0, 0);
   }

   scroll_slider s(0, HostLen * (ViewLen / PageLen));

   if ((Position + ViewLen) IS PageLen) s.offset = HostLen - s.length;
   else s.offset = (Position * HostLen) / PageLen;

   if (s.offset < 0) s.offset = 0;

   if ((s.length + s.offset) > HostLen) s.length = HostLen - s.offset;

   return s;
}

//********************************************************************************************************************
// Recompute the position of the slider based on the position of the page and view.

void scroll_mgr::recalc_sliders_from_view()
{
   auto v_width  = m_view->get<double>(FID_Width);
   auto v_height = m_view->get<double>(FID_Height);

   auto p_width  = m_page->get<double>(FID_Width);
   auto p_height = m_page->get<double>(FID_Height);

   if ((p_width > v_width) or (p_height > v_height)) {
      // Page exceeds the available view space, scrollbar is required.

      if (p_height > v_height) {
         if (!m_vbar.m_bar_vp) m_vbar.init(this, 'V', m_view);

         acMoveToFront(m_vbar.m_bar_vp);

         auto s = m_vbar.calc_slider(v_height, p_height,
            m_vbar.m_slider_host->get<double>(FID_Height),
            -m_page->get<double>(FID_Y));

         if ((s.offset != m_vbar.m_slider_pos.offset) or
             (s.length != m_vbar.m_slider_pos.length)) {
            m_vbar.m_slider_pos = s;
            m_vbar.m_slider_vp->setFields(fl::Y(s.offset), fl::Height(s.length));

            if (s.length <= 12) {
               m_vbar.m_bar_vp->setFields(fl::Visibility(VIS::HIDDEN));
               m_view->setFields(fl::XOffset(0));
               if (m_hbar.m_bar_vp) m_hbar.m_bar_vp->setFields(fl::XOffset(0));
            }
            else {
               m_vbar.m_bar_vp->setFields(fl::Visibility(VIS::VISIBLE));
               if (m_auto_adjust_view_size) m_view->setFields(fl::XOffset(m_vbar.m_slider_vp->get<double>(FID_Width)));
               if (m_hbar.m_bar_vp) m_hbar.m_bar_vp->setFields(fl::XOffset(m_hbar.m_breadth));
            }
         }
      }

      m_page->draw();
   }
   else {
      m_vbar.clear();
      m_hbar.clear();
   }
}

//********************************************************************************************************************

void scroll_mgr::scroll_bar::init(scroll_mgr *Manager, char Direction, objVectorViewport *Viewport)
{
   pf::Log log(__FUNCTION__);

   log.branch("Target: #%d", Viewport->ownerID());

   m_mgr = Manager;

   // Main scrollbar container

   m_bar_vp = objVectorViewport::create::global({
      fl::Owner(Viewport->ownerID()),
      fl::Y(5), fl::YOffset(5), fl::XOffset(5), fl::Width(m_breadth)
   });

   // Background graphic

   objVectorRectangle::create::global({
      fl::Owner(m_bar_vp->UID),
      fl::X(0), fl::Y(0), fl::Width(SCALE(1.0)), fl::YOffset(0),
      fl::RoundX(m_breadth * 0.5), fl::RoundY(m_breadth * 0.5),
      fl::Fill(glSliderBkgd)
   });

   // Host area for the slider, prevents dragging beyond boundaries and is used for monitoring user input

   m_slider_host = objVectorViewport::create::global({
      fl::Owner(m_bar_vp->UID),
      fl::X(0), fl::Width(SCALE(1.0)),
      fl::Y(2), fl::YOffset(2)
   });

   // Slider widget; draggable

   m_slider_vp = objVectorViewport::create::global({
      fl::Owner(m_slider_host->UID),
      fl::DragCallback(C_FUNCTION(slider_drag, Manager)),
      fl::Width(m_breadth), fl::Height(SCALE(1.0))
   });

   // Slider graphic

   m_slider_rect = objVectorRectangle::create::global({
      fl::Owner(m_slider_vp->UID),
      fl::X(2), fl::Y(0), fl::XOffset(2), fl::Height(SCALE(1.0)),
      fl::RoundX(m_breadth * 0.5), fl::RoundY(m_breadth * 0.5),
      fl::Fill(glSliderColour)
   });

   // Capture user interactivity within the bar area.

   m_slider_host->subscribeInput(JTYPE::BUTTON|JTYPE::REPEATED, C_FUNCTION(bkgd_input, Manager));
   m_slider_vp->subscribeInput(JTYPE::CROSSING, C_FUNCTION(slider_input, Manager));
}

//********************************************************************************************************************

void scroll_mgr::scroll_bar::clear()
{
   if (m_bar_vp) {
      FreeResource(m_bar_vp);
      m_bar_vp = nullptr;
   }
}

//********************************************************************************************************************

void scroll_mgr::scroll_page(double DeltaX, double DeltaY)
{
   const double current_y = m_page->get<double>(FID_Y);
   double y = current_y + DeltaY;
   const double page_height = m_page->get<double>(FID_Height);
   const double view_height = m_view->get<double>(FID_Height);

   if ((y > 0) or (page_height < view_height)) y = 0;
   else if (y + page_height < view_height) y = -page_height + view_height;

   if (y != current_y) {
      m_page->setFields(fl::Y(std::trunc(y)));
      m_page->draw();
   }
}

//********************************************************************************************************************
// NB: As a client you can set the page height and width directly if no mode change is required.

void scroll_mgr::fix_page_size(double Width, double Height)
{
   m_fixed_mode = true;
   if (Width != m_page->get<double>(FID_Width)) m_page->setFields(fl::Width(Width));
   if (Height != m_page->get<double>(FID_Height)) m_page->setFields(fl::Height(Height));
}

//********************************************************************************************************************

void scroll_mgr::dynamic_page_size(double NominalWidth, double MinWidth, double Height)
{
   m_min_width = MinWidth;

   if (NominalWidth < m_min_width) NominalWidth = m_min_width;

   if (NominalWidth >= m_view->get<double>(FID_Width)) acResize(m_page, NominalWidth, Height, 0);
   else m_page->setFields(fl::Width(SCALE(1.0)), fl::Height(Height));
}

//********************************************************************************************************************
// Scrollbar constructor

void scroll_mgr::init(extDocument *pDoc, objVectorViewport *pPage, objVectorViewport *pView)
{
   m_doc  = pDoc;
   m_page = pPage;
   m_view = pView;

   // The slider and possibly the page need to be repositioned whenever the view is resized.

   m_view->subscribeFeedback(FM::PATH_CHANGED, C_FUNCTION(view_path_changed, this));
   m_page->subscribeFeedback(FM::PATH_CHANGED, C_FUNCTION(page_path_changed, this));
   m_page->subscribeInput(JTYPE::EXT_MOVEMENT, C_FUNCTION(page_movement, this));
}

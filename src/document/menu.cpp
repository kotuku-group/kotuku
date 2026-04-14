//********************************************************************************************************************
// Default template for building the menu's layout.  The use of "placement=background" ensures that the content is
// displayed in the view and not the page.
//
// The client can override this default by providing their own template in a <style> tag.

static const char * glSVGHeader = R"LONGSTRING(
<svg placement="background">
  <defs>
    <pattern id="Highlight">
      <rect rx="2%" ry="2%" width="100%" height="100%" fill="rgb(245,175,155)"/>
    </pattern>

    <clipPath id="PageClip">
      <rect x="3" y="4" xOffset="3" yOffset="3"/>
    </clipPath>

)LONGSTRING";

static const char * glSVGTail = R"LONGSTRING(
  </defs>

  <rect rx="3%" ry="3%" x="1" y="1" xOffset="2" yOffset="2" fill="rgb(245,245,245)" stroke="rgb(0,0,0,40)" stroke-width="0.5%"/>
  <rect rx="3%" ry="3%" x="3" y="3" xOffset="1" yOffset="1" fill="none" stroke="rgb(0,0,0,90)" stroke-width="0.5%"/>
</svg>
)LONGSTRING";

static ERR menu_doc_events(extDocument *, DEF, KEYVALUE *, entity *, APTR);
static void menu_hidden(OBJECTPTR, ACTIONID, ERR, APTR, doc_menu *);
static void menu_lost_focus(OBJECTPTR, ACTIONID, ERR, APTR, doc_menu *);

//********************************************************************************************************************

void doc_menu::define_font(font_entry *Font)
{
   m_font_face  = Font->face;
   m_font_size  = Font->font_size;
   m_font_style = Font->style;
}

//********************************************************************************************************************

objSurface * doc_menu::create(double Width)
{
   pf::Log log(__FUNCTION__);

   log.branch();

   if (m_surface.empty()) {
      double height = m_items.size() * 20;
      if (height < 20) height = 20;

      m_surface.set(objSurface::create::global({
         fl::Name("menu"),
         fl::Parent(0),
         fl::Flags(RNF::STICK_TO_FRONT|RNF::COMPOSITE),
         fl::WindowType(SWIN::NONE),
         fl::X(0), fl::Y(0), fl::Width(Width), fl::Height(height)
      }));

      m_scene = objVectorScene::create::global({
         fl::Name("menu_scene"),
         fl::Flags(VPF::RESIZE),
         fl::Surface(m_surface->UID)
      });

      m_view = objVectorViewport::create::global({
         fl::Owner(m_scene->UID),
         fl::X(0), fl::Y(0), fl::Width(SCALE(1.0)), fl::Height(SCALE(1.0))
      });

      m_doc = objDocument::create::global({
         fl::Owner(m_surface->UID),
         fl::Viewport(m_view),
         fl::EventMask(DEF::LINK_ACTIVATED|DEF::ON_CLICK|DEF::ON_CROSSING),
         fl::EventCallback(C_FUNCTION(menu_doc_events))
      });

      m_doc->CreatorMeta = this;

      SubscribeAction(*m_surface, AC::LostFocus, C_FUNCTION(menu_lost_focus, this));
      SubscribeAction(*m_surface, AC::Hide, C_FUNCTION(menu_hidden, this));

      refresh();
   }

   return *m_surface;
}

//********************************************************************************************************************

void doc_menu::refresh()
{
   pf::Log log(__FUNCTION__);

   const double HGAP = std::trunc(m_font_size * 0.2);
   int total_icons = 0;

   std::ostringstream buf;
   buf << "<body margins=\"" << HGAP << " " << HGAP << " " << HGAP << " " << 0 << "\" " <<
      "link=\"rgb(0,0,0)\" v-link=\"rgb(0,0,0)\" " <<
      "font-face=\"" << m_font_face << "\" font-size=\"" << m_font_size << "\"/>\n";

   if (!m_style.empty()) {
      buf << m_style;
   }
   else {
      buf << glSVGHeader;

      for (auto &item : m_items) {
         if (!item.icon.empty()) {
            buf << "    <image id=\"" << item.icon << "\" xlink:href=\"" << item.icon << "\" " <<
               "width=\"" << int(m_font_size * 1.33) << "\" height=\"" << int(m_font_size * 1.33) << "\"/>\n";
            total_icons++;
         }
      }

      buf << glSVGTail;
   }

   buf << "<page name=\"Index\">\n";
   buf << "<table width=\"100%\" v-spacing=\"0.3em\" h-spacing=\"0.2em\" cell-padding=\"6 0 6 0\">\n";

   for (auto &item : m_items) {
      buf << "<row>";

      if (!item.id.empty()) buf << "<cell on-click on-crossing @id=\"" << item.id << "\">";
      else if (!item.value.empty()) buf << "<cell on-click on-crossing @value=\"" << item.value << "\">";
      else buf << "<cell on-click on-crossing>";

      buf << "<p no-wrap v-align=\"middle\">";

      if (item.icon.empty()) {
         if (total_icons) buf << "<advance x=\"[=1.5*[%line-height]]\"/>";
      }
      else buf << "<image src=\"url(#" << item.icon << ")\"/><advance x=\"[=0.5*[%line-height]]\"/>";

      if (!item.content.empty()) buf << item.content;
      else buf << item.value;

      buf << "</p></cell></row>\n";
   }

   buf << "</table>\n";
   buf << "</page>";

   #ifdef DBG_LAYOUT
      log.msg("%s", buf.str().c_str());
   #endif

   acClear(m_doc);
   acDataXML(m_doc, buf.str().c_str());

   // Resize the menu to match the new content.  If the height of the menu is excessive (relative to the height
   // of the display), we reduce it and utilise a scrollbar to see all menu items.

   auto doc_width  = m_doc->get<double>(FID_PageWidth);
   auto doc_height = m_doc->get<double>(FID_PageHeight);

   double view_width  = doc_width;
   double view_height = doc_height;

   DISPLAYINFO *display;
   if (gfx::GetDisplayInfo(0, &display) IS ERR::Okay) {
      if (view_height > display->Height * 0.25) view_height = display->Height * 0.25;
   }

   if (view_width > m_surface->Width) {
      acResize(*m_surface, view_width, view_height, 0);
   }
   else m_surface->setHeight(view_height);

   if (doc_height > view_height) {
      m_view->setFields(fl::Height(view_height));

      objVectorViewport *doc_page, *doc_view;
      if (m_doc->get(FID_Page, doc_page) IS ERR::Okay) {
         if (m_doc->get(FID_View, doc_view) IS ERR::Okay) {
            m_scroll.init((extDocument *)CurrentContext(), doc_page, doc_view);
            m_scroll.m_auto_adjust_view_size = false;

            OBJECTPTR clip;
            if (m_scene->findDef("PageClip", &clip) IS ERR::Okay) {
               doc_page->set(FID_Mask, clip);
            }
         }
      }
   }
}

//********************************************************************************************************************

void doc_menu::reposition(objVectorViewport *RelativeViewport)
{
   DISPLAYINFO *display;
   gfx::GetDisplayInfo(0, &display);

   pf::ScopedObjectLock<objSurface> lk_surface(RelativeViewport->Scene->SurfaceID); // Window surface
   if (lk_surface.granted()) {
      auto w_absx = lk_surface->get<double>(FID_AbsX);
      auto w_absy = lk_surface->get<double>(FID_AbsY);

      auto vp_absx = RelativeViewport->get<double>(FID_AbsX);
      auto vp_absy = RelativeViewport->get<double>(FID_AbsY);
      auto vp_height = RelativeViewport->get<double>(FID_Height);

      // Invert the menu position if it will drop off the display

      double y = w_absy + vp_absy + vp_height;
      if (y + m_surface->Height > (display->Height * 0.97)) {
         y -= m_surface->Height + vp_height;
      }

      acMoveToPoint(*m_surface, w_absx + vp_absx, y, 0, MTF::X|MTF::Y);
   }
}

//********************************************************************************************************************

void doc_menu::toggle(objVectorViewport *Relative)
{
   pf::Log log(__FUNCTION__);
   log.branch();

   const double time_lapse = 20000; // Amount of time that must elapse to trigger the toggle.

   auto current_time = PreciseTime();
   if (m_show_time > m_hide_time) { // Hide the menu
      if (current_time - m_show_time >= time_lapse) {
         acHide(*m_surface);
      }
   }
   else if (current_time - m_hide_time >= time_lapse) {
      reposition(Relative);
      acShow(*m_surface);
      m_show_time = current_time;
   }
}

//********************************************************************************************************************

static void menu_lost_focus(OBJECTPTR Surface, ACTIONID ActionID, ERR Error, APTR Args, doc_menu *Menu)
{
   if (Error != ERR::Okay) return;
   acHide(Surface);
}

//********************************************************************************************************************

static void menu_hidden(OBJECTPTR Surface, ACTIONID ActionID, ERR Error, APTR Args, doc_menu *Menu)
{
   if (Error != ERR::Okay) return;
   Menu->m_hide_time = PreciseTime();
}

//********************************************************************************************************************
// Intercept interactions with menu items

static ERR menu_doc_events(extDocument *DocMenu, DEF Event, KEYVALUE *EventData, entity *Entity, APTR Meta)
{
   pf::Log log(__FUNCTION__);

   if (((Event & DEF::ON_CLICK) != DEF::NIL) or ((Event & DEF::LINK_ACTIVATED) != DEF::NIL)) {
      auto menu = (doc_menu *)DocMenu->CreatorMeta;

      acHide(*menu->m_surface);

      if (!menu->m_callback) return ERR::Okay;

      auto kv_item = EventData->find("id");
      if ((kv_item != EventData->end()) and (!kv_item->second.empty())) {
         for (auto &item : menu->m_items) {
            if (item.id IS kv_item->second) {
               menu->m_callback(*menu, item);
               return ERR::Okay;
            }
         }
      }

      kv_item = EventData->find("value");
      if ((kv_item != EventData->end()) and (!kv_item->second.empty())) {
         for (auto &item : menu->m_items) {
            if (item.value IS kv_item->second) {
               menu->m_callback(*menu, item);
               return ERR::Okay;
            }
         }
      }

      log.warning("No id or value defined for selected menu item.");
   }
   else if ((Event & DEF::ON_CROSSING_IN) != DEF::NIL) {
      auto cell = (bc_cell *)Entity;
      cell->set_fill("url(#Highlight)");
      cell->viewport->draw();
   }
   else if ((Event & DEF::ON_CROSSING_OUT) != DEF::NIL) {
      auto cell = (bc_cell *)Entity;
      cell->set_fill("none");
      cell->viewport->draw();
   }

   return ERR::Okay;
}

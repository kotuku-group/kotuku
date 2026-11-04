/*********************************************************************************************************************
** ListBox class
*/

Scintilla::ListBox::ListBox()
{

}

Scintilla::ListBox::~ListBox()
{

}

/*********************************************************************************************************************
** ListBoxImp class
**
** The completion list of AutoComplete.  The list is a PanPopup (see pan_popup.cxx) that paints its items with the
** editor's regular font.  Rows are the height of the editor's lines, and the selected row is drawn in the editor's
** selection colours.  A narrow bar on the right indicates the scroll position when there are more items than rows.
** Item images (SCI_REGISTERIMAGE) are not supported.
*/

class ListBoxImp : public Scintilla::ListBox, public PanPopup {

public:

   ListBoxImp() { }
   virtual ~ListBoxImp() { }

   virtual void SetFont(Scintilla::Font &font);
   virtual void Create(Scintilla::Window &parent, int ctrlID, Scintilla::Point location_, int lineHeight_, bool unicodeMode_);
   virtual void SetAverageCharWidth(int width);
   virtual void SetVisibleRows(int rows);
   virtual int GetVisibleRows() const;
   virtual Scintilla::PRectangle GetDesiredRect();
   virtual int CaretFromEdge();
   virtual void Clear();
   virtual void Append(char *s, int type = -1);
   virtual int Length();
   virtual void Select(int n);
   virtual int GetSelection();
   virtual int Find(const char *prefix);
   virtual void GetValue(int n, char *value, int len);
   virtual void RegisterImage(int type, const char *xpm_data);
   virtual void RegisterRGBAImage(int type, int width, int height, const unsigned char *pixelsImage);
   virtual void ClearRegisteredImages();
   virtual void SetDoubleClickAction(Scintilla::CallBackAction action, void *data);
   virtual void SetList(const char* list, char separator, char typesep);

   void paint(SurfacePan &Surface) override;
   void input(const InputEvent *Event) override;
   int anchor_position() override;
   void closed() override;

private:
   static const int INSET = 3;        // Space between the border and the text
   static const int SCROLL_WIDTH = 4; // Width of the scroll indicator
   static const int WHEEL_ROWS = 3;

   std::vector<std::string> items;
   Scintilla::Font font;              // A regular style, which SurfacePan resolves to the editor's regular face
   Scintilla::CallBackAction double_click_action = nullptr;
   void *double_click_data = nullptr;
   int selection = -1;
   int top_row = 0;                   // The first visible item
   int visible_rows = 5;              // The maximum number of rows, as set by SCI_AUTOCSETMAXHEIGHT
   int line_height = 1;
   int last_click_row = -1;
   unsigned int last_click_time = 0;

   int rows() const { return std::clamp(int(items.size()), 1, std::max(1, visible_rows)); }
   void scroll_to(int Row);
};

Scintilla::ListBox * Scintilla::ListBox::Allocate()
{
   return new ListBoxImp();
}

//********************************************************************************************************************
// Called by AutoComplete::Start() each time that the list opens.  The Window always refers to the list once created,
// so that AutoComplete can close it, even if the list cannot be displayed.

void ListBoxImp::Create(Scintilla::Window &Parent, int, Scintilla::Point, int LineHeight, bool)
{
   line_height = std::max(1, LineHeight);
   top_row = 0;
   last_click_row = -1;

   if (auto editor = get_scintilla(&Parent)) attach(editor, "sciCompletions");
   wid = (Scintilla::WindowID)(PanPopup *)this;
}

//********************************************************************************************************************

void ListBoxImp::RegisterRGBAImage(int type, int width, int height, const unsigned char *pixelsImage)
{

}

void ListBoxImp::RegisterImage(int type, const char *xpm_data)
{

}

void ListBoxImp::ClearRegisteredImages()
{

}

//********************************************************************************************************************
// Items are always drawn with the editor's regular face.

void ListBoxImp::SetFont(Scintilla::Font &scint_font)
{

}

//********************************************************************************************************************

void ListBoxImp::SetAverageCharWidth(int width)
{

}

//********************************************************************************************************************

void ListBoxImp::SetVisibleRows(int rows)
{
   visible_rows = rows;
}

//********************************************************************************************************************

int ListBoxImp::GetVisibleRows() const
{
   return visible_rows;
}

//********************************************************************************************************************
// The list is wide enough for its longest item and high enough for the visible rows.  Scintilla places the list so
// that the text of its items is aligned with the typed text, i.e. CaretFromEdge() pixels to the left of it.

Scintilla::PRectangle ListBoxImp::GetDesiredRect()
{
   int width = 0;
   if (scintilla) {
      SurfacePan surface;
      surface.Init((Scintilla::WindowID)scintilla);
      for (auto &item : items) {
         width = std::max(width, surface.WidthText(font, item.c_str(), int(item.size())));
      }
   }

   width += 2 * (POPUP_BORDER + INSET);
   if (int(items.size()) > rows()) width += SCROLL_WIDTH;

   return Scintilla::PRectangle(0, 0, width, (rows() * line_height) + (2 * POPUP_BORDER));
}

//********************************************************************************************************************

int ListBoxImp::CaretFromEdge()
{
   return POPUP_BORDER + INSET;
}

//********************************************************************************************************************

void ListBoxImp::Clear()
{
   items.clear();
   selection = -1;
   top_row = 0;
   invalidate();
}

//********************************************************************************************************************

void ListBoxImp::Append(char *s, int type)
{
   if (s) items.emplace_back(s);
   invalidate();
}

//********************************************************************************************************************

int ListBoxImp::Length()
{
   return int(items.size());
}

//********************************************************************************************************************
// Select an item and scroll it into view.  A negative index clears the selection.

void ListBoxImp::Select(int n)
{
   if (n >= int(items.size())) n = int(items.size()) - 1;
   selection = (n < 0) ? -1 : n;
   if (selection >= 0) {
      if (selection < top_row) scroll_to(selection);
      else if (selection >= top_row + rows()) scroll_to(selection - rows() + 1);
   }
   invalidate();
}

//********************************************************************************************************************

int ListBoxImp::GetSelection()
{
   return selection;
}

//********************************************************************************************************************

int ListBoxImp::Find(const char *prefix)
{
   if (not prefix) return -1;
   std::string_view text(prefix);
   for (int i=0; i < int(items.size()); i++) {
      if (items[i].starts_with(text)) return i;
   }
   return -1;
}

//********************************************************************************************************************

void ListBoxImp::GetValue(int n, char *value, int len)
{
   if ((not value) or (len <= 0)) return;
   if ((n < 0) or (n >= int(items.size()))) {
      value[0] = 0;
      return;
   }

   auto &item = items[n];
   auto size = std::min(item.size(), size_t(len - 1));
   memcpy(value, item.data(), size);
   value[size] = 0;
}

//********************************************************************************************************************

void ListBoxImp::SetDoubleClickAction(Scintilla::CallBackAction action, void *data)
{
   double_click_action = action;
   double_click_data = data;
}

//********************************************************************************************************************
// The list is a sequence of items that are divided by the separator.  Type annotations, which select item images, are
// removed.  AutoComplete selects items with a binary search, so the list must already be sorted.

void ListBoxImp::SetList(const char* list, char separator, char typesep)
{
   items.clear();
   selection = -1;
   top_row = 0;

   if (list) {
      std::string_view text(list);
      while (not text.empty()) {
         auto end = text.find(separator);
         auto item = text.substr(0, end);
         if (typesep) item = item.substr(0, item.find(typesep));
         if (not item.empty()) items.emplace_back(item);
         if (end IS std::string_view::npos) break;
         text.remove_prefix(end + 1);
      }
   }

   invalidate();
}

//********************************************************************************************************************

void ListBoxImp::scroll_to(int Row)
{
   top_row = std::clamp(Row, 0, std::max(0, int(items.size()) - rows()));
}

//********************************************************************************************************************

int ListBoxImp::anchor_position()
{
   return scintilla->API->panCompletionStart();
}

//********************************************************************************************************************
// AutoComplete closes the list on acceptance, cancellation, and when typing leaves no matching item.  Only the first
// of these is notified by ScintillaBase, so the editor reports the others from here.  The editor's API is cleared
// while it is being destroyed.

void ListBoxImp::closed()
{
   if ((scintilla) and (scintilla->API)) scintilla->API->panCompletionClosed();
}

//********************************************************************************************************************
// The background and text follow the editor's colours, and the border uses the margin colour.  Selection colours with
// a zero alpha fall back to the inverse of the text and background.

void ListBoxImp::paint(SurfacePan &Surface)
{
   auto colour = [](const RGB8 &Value) {
      return Scintilla::ColourAllocated(SCICOLOUR(Value.Red, Value.Green, Value.Blue));
   };

   auto &bkgd = scintilla->BkgdColour;
   auto &text = scintilla->TextColour;
   auto sel_bkgd = colour((scintilla->SelectBkgd.Alpha > 0) ? scintilla->SelectBkgd : text);
   auto sel_fore = colour((scintilla->SelectFore.Alpha > 0) ? scintilla->SelectFore : bkgd);

   const int width = bitmap->Width, height = bitmap->Height;
   Surface.FillRectangle(Scintilla::PRectangle(0, 0, width, height), colour(scintilla->MarginColour));
   Surface.FillRectangle(Scintilla::PRectangle(POPUP_BORDER, POPUP_BORDER, width - POPUP_BORDER,
      height - POPUP_BORDER), colour(bkgd));

   const bool scrollable = int(items.size()) > rows();
   const int right = width - POPUP_BORDER - (scrollable ? SCROLL_WIDTH : 0);
   const int ascent = Surface.Ascent(font);
   const int baseline = std::max(0, (line_height - Surface.Height(font)) / 2) + ascent;

   for (int row=0; row < rows(); row++) {
      int index = top_row + row;
      if (index >= int(items.size())) break;

      int y = POPUP_BORDER + (row * line_height);
      Scintilla::PRectangle rc(POPUP_BORDER, y, right, y + line_height);
      auto &item = items[index];
      if (index IS selection) {
         Surface.DrawTextClipped(rc, font, y + baseline, item.c_str(), int(item.size()), sel_fore, sel_bkgd);
      }
      else Surface.DrawTextClipped(rc, font, y + baseline, item.c_str(), int(item.size()), colour(text), colour(bkgd));
   }

   if (scrollable) {
      int track = height - (2 * POPUP_BORDER);
      int thumb = std::max(4, (track * rows()) / int(items.size()));
      int top = POPUP_BORDER + ((track - thumb) * top_row) / std::max(1, int(items.size()) - rows());
      Surface.FillRectangle(Scintilla::PRectangle(right, top, right + SCROLL_WIDTH, top + thumb),
         colour(scintilla->MarginColour));
   }
}

//********************************************************************************************************************
// A click selects an item and a double click accepts it.  The wheel scrolls the list without changing the selection.

void ListBoxImp::input(const InputEvent *Event)
{
   if (((Event->Flags & JTYPE::BUTTON) != JTYPE::NIL) and (Event->Type IS JET::LMB) and (Event->Value > 0)) {
      int row = top_row + int(std::floor((Event->Y - POPUP_BORDER) / line_height));
      if ((row < top_row) or (row >= top_row + rows()) or (row >= int(items.size()))) return;

      const auto time = (unsigned int)(Event->Timestamp / 1000LL);
      const bool double_click = (row IS last_click_row) and
         (time - last_click_time < Scintilla::Platform::DoubleClickTime());

      Select(row);

      if ((double_click) and (double_click_action)) {
         last_click_row = -1;
         double_click_action(double_click_data); // Accepts the selection, which closes the list
      }
      else {
         last_click_row = row;
         last_click_time = time;
      }
   }
   else if ((Event->Type IS JET::WHEEL) and (Event->Value != 0)) {
      scroll_to(top_row + ((Event->Value > 0) ? WHEEL_ROWS : -WHEEL_ROWS));
      invalidate();
   }
}

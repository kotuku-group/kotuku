/*********************************************************************************************************************
** ListBox class
*/

Scintilla::Internal::ListBox::ListBox() noexcept
{

}

Scintilla::Internal::ListBox::~ListBox() noexcept
{

}

/*********************************************************************************************************************
** ListBoxImp class
**
** The completion list of AutoComplete.  The list is a PanPopup (see kt_popup.cxx) that paints its items with the
** supplied style font.  Rows use the engine's line height and ListOptions colours, falling back to the editor's
** palette.  A narrow bar on the right indicates the scroll position when there are more items than rows.
** Item images (SCI_REGISTERIMAGE) are not supported.
*/

class ListBoxImp : public Scintilla::Internal::ListBox, public PanPopup {

public:

   ListBoxImp() { }
   ~ListBoxImp() override = default;

   void SetFont(const Scintilla::Internal::Font *Font) override;
   void Create(Scintilla::Internal::Window &Parent, int ID, Scintilla::Internal::Point Location,
      int LineHeight, bool Unicode, Scintilla::Technology Technology) override;
   void SetAverageCharWidth(int width) override;
   void SetVisibleRows(int rows) override;
   virtual int GetVisibleRows() const;
   Scintilla::Internal::PRectangle GetDesiredRect() override;
   int CaretFromEdge() override;
   void Clear() noexcept override;
   void Append(char *s, int type = -1) override;
   int Length() override;
   void Select(int n) override;
   int GetSelection() override;
   int Find(const char *prefix) override;
   std::string GetValue(int Index) override;
   void RegisterImage(int type, const char *xpm_data) override;
   void RegisterRGBAImage(int type, int width, int height, const unsigned char *pixelsImage) override;
   void ClearRegisteredImages() override;
   void SetDelegate(Scintilla::Internal::IListBoxDelegate *Delegate) override { delegate = Delegate; }
   void SetOptions(Scintilla::Internal::ListOptions Options) override { options = Options; invalidate(); }
   void SetList(const char* list, char separator, char typesep) override;

   void paint(SurfacePan &Surface) override;
   void input(const InputEvent *Event) override;
   int anchor_position() override;
   void closed() override;

private:
   static const int INSET = 3;        // Space between the border and the text
   static const int SCROLL_WIDTH = 4; // Width of the scroll indicator
   static const int WHEEL_ROWS = 3;

   std::vector<std::string> items;
   PanFont font;              // Copy the cached handle; the engine's style descriptor may subsequently be released
   Scintilla::Internal::IListBoxDelegate *delegate = nullptr;
   Scintilla::Internal::ListOptions options;
   int selection = -1;
   int top_row = 0;                   // The first visible item
   int visible_rows = 5;              // The maximum number of rows, as set by SCI_AUTOCSETMAXHEIGHT
   int line_height = 1;
   int last_click_row = -1;
   unsigned int last_click_time = 0;

   int rows() const { return std::clamp(int(items.size()), 1, std::max(1, visible_rows)); }
   void scroll_to(int Row);
};

std::unique_ptr<Scintilla::Internal::ListBox> Scintilla::Internal::ListBox::Allocate()
{
   return std::make_unique<ListBoxImp>();
}

//********************************************************************************************************************
// Called by AutoComplete::Start() each time that the list opens.  The Window always refers to the list once created,
// so that AutoComplete can close it, even if the list cannot be displayed.

void ListBoxImp::Create(Scintilla::Internal::Window &Parent, int, Scintilla::Internal::Point,
   int LineHeight, bool, Scintilla::Technology)
{
   line_height = std::max(1, LineHeight);
   top_row = 0;
   last_click_row = -1;

   if (auto editor = get_scintilla(&Parent)) attach(editor, "sciCompletions");
   wid = (Scintilla::Internal::WindowID)(PanPopup *)this;
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
// Font descriptors belong to ViewStyle.  Copy the platform handle, which Vector retains for the process lifetime.

void ListBoxImp::SetFont(const Scintilla::Internal::Font *Font)
{
   font.handle = Font ? ((const PanFont *)Font)->handle : nullptr;
   invalidate();
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

Scintilla::Internal::PRectangle ListBoxImp::GetDesiredRect()
{
   int width = 0;
   if (scintilla) {
      SurfacePan surface;
      surface.Init((Scintilla::Internal::WindowID)scintilla);
      for (auto &item : items) {
         width = std::max(width, int(std::ceil(surface.WidthTextUTF8(&font, item))));
      }
   }

   width += 2 * (POPUP_BORDER + INSET);
   if (int(items.size()) > rows()) width += SCROLL_WIDTH;

   return Scintilla::Internal::PRectangle(0, 0, width, (rows() * line_height) + (2 * POPUP_BORDER));
}

//********************************************************************************************************************

int ListBoxImp::CaretFromEdge()
{
   return POPUP_BORDER + INSET;
}

//********************************************************************************************************************

void ListBoxImp::Clear() noexcept
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
   const auto previous = selection;
   selection = (n < 0) ? -1 : n;
   if (selection >= 0) {
      if (selection < top_row) scroll_to(selection);
      else if (selection >= top_row + rows()) scroll_to(selection - rows() + 1);
   }
   invalidate();
   if ((selection != previous) and delegate) {
      Scintilla::Internal::ListBoxEvent event(Scintilla::Internal::ListBoxEvent::EventType::selectionChange);
      delegate->ListNotify(&event);
   }
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

std::string ListBoxImp::GetValue(int Index)
{
   if ((Index < 0) or (Index >= int(items.size()))) return {};
   return items[Index];
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
      return Scintilla::Internal::ColourRGBA(Value.Red, Value.Green, Value.Blue);
   };

   auto &bkgd = scintilla->BkgdColour;
   auto &text = scintilla->TextColour;
   auto back = options.back.value_or(colour(bkgd));
   auto fore = options.fore.value_or(colour(text));
   auto sel_bkgd = options.backSelected.value_or(
      colour((scintilla->SelectBkgd.Alpha > 0) ? scintilla->SelectBkgd : text));
   auto sel_fore = options.foreSelected.value_or(
      colour((scintilla->SelectFore.Alpha > 0) ? scintilla->SelectFore : bkgd));

   const int width = bitmap->Width, height = bitmap->Height;
   Surface.FillRectangle(Scintilla::Internal::PRectangle(0, 0, width, height), colour(scintilla->MarginColour));
   Surface.FillRectangle(Scintilla::Internal::PRectangle(POPUP_BORDER, POPUP_BORDER, width - POPUP_BORDER,
      height - POPUP_BORDER), back);

   const bool scrollable = int(items.size()) > rows();
   const int right = width - POPUP_BORDER - (scrollable ? SCROLL_WIDTH : 0);
   const int ascent = int(Surface.Ascent(&font));
   const int baseline = std::max(0, (line_height - int(Surface.Height(&font))) / 2) + ascent;

   for (int row=0; row < rows(); row++) {
      int index = top_row + row;
      if (index >= int(items.size())) break;

      int y = POPUP_BORDER + (row * line_height);
      Scintilla::Internal::PRectangle rc(POPUP_BORDER, y, right, y + line_height);
      auto &item = items[index];
      if (index IS selection) Surface.FillRectangle(rc, sel_bkgd);
      rc.left += INSET;
      Surface.SetClip(rc);
      Surface.DrawTextTransparentUTF8(rc, &font, y + baseline, item,
         index IS selection ? sel_fore : fore);
      Surface.PopClip();
   }

   if (scrollable) {
      int track = height - (2 * POPUP_BORDER);
      int thumb = std::max(4, (track * rows()) / int(items.size()));
      int top = POPUP_BORDER + ((track - thumb) * top_row) / std::max(1, int(items.size()) - rows());
      Surface.FillRectangle(Scintilla::Internal::PRectangle(right, top, right + SCROLL_WIDTH, top + thumb),
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
         (time - last_click_time < Scintilla::Internal::Platform::DoubleClickTime());

      Select(row);

      if ((double_click) and (delegate)) {
         last_click_row = -1;
         Scintilla::Internal::ListBoxEvent event(Scintilla::Internal::ListBoxEvent::EventType::doubleClick);
         delegate->ListNotify(&event); // Accepts the selection, which closes the list
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

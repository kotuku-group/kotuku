/*

The layout process involves reading the serialised document stream and generating line segments that declare
regions for graphics content.  These segments have a dual purpose in that they can also be used for user
interaction.

The trickiest parts of the layout process are state management, word wrapping and page-width extension.

TABLES
------
The layout of tables is arranged as follows (left-to-right or top-to-bottom):

Border-Thickness, Cell-Spacing, Cell-Padding, Content, Cell-Padding, Cell-Spacing, ..., Border-Thickness

Table attributes are:

Columns:      The minimum width of each column in the table.
Width/Height: Minimum width and height of the table.
Fill:         Background fill for the table.
Thickness:    Size of the Stroke pattern.
Stroke        Stroke pattern for border.
Padding:      Padding inside each cell (margins)
Spacing:      Spacing between cells.

For complex tables with different colour borders between cells, allocate single-pixel sized cells with the background
colour set to the desired value in order to create the illusion of multi-colour cell borders.

The page area owned by a table is given a clipping zone by the page layout engine, in the same way that objects are
given clipping zones.  This allows text to be laid out around the table with no effort on the part of the developer.

*/

enum struct WTC: uint8_t {
   DO_NOTHING, WRAP_OVER, WRAP_LINE
};

// State machine for the layout process.  This information is discarded post-layout.

struct layout {
   friend class DUNIT;

   extDocument *Self;
   int  m_break_loop = MAXLOOP;
   uint16_t m_depth = 0;     // Section depth - increases when do_layout() recurses, e.g. into table cells

   std::vector<doc_segment> m_segments;
   std::vector<edit_cell>   m_ecells;

private:
   struct line_state {
      stream_char index;       // Stream position for the line's content.
      double descent = 0;      // Vertical spacing accommodated for glyph tails.  Inclusive within the height value, not additive
      double height = 0;       // The complete height of the line, including inline vectors/images/tables.  Text is drawn so that the text descent is aligned to the base line
      double x = 0;            // Starting horizontal position
      double word_height = 0;  // Height of the current word (including inline graphics), utilised for word wrapping

      void reset(double LeftMargin) {
         x       = LeftMargin;
         descent = 0;
         height  = 0;
      }

      void full_reset(double LeftMargin) {
         reset(LeftMargin);
         word_height = 0;
      }

      void apply_word_height() {
         if (word_height > height) height = word_height;
         word_height = 0;
      }
   };

   struct link_marker {
      double x;           // Starting coordinate of the link.  Can change if the link is split across multiple lines.
      double word_width;  // Reflects the m_word_width value at the moment of a link's termination.
      INDEX index;
      ALIGN align;

      constexpr link_marker(double pX, INDEX pIndex, ALIGN pAlign) : x(pX), word_width(0), index(pIndex), align(pAlign) { }
   };

   struct layout_checkpoint {
      int break_loop = 0;
      bc_row *row = nullptr;
      font_entry *font = nullptr;
      double cursor_x = 0;
      double cursor_y = 0;
      double page_width = 0;
      INDEX index = 0;
      stream_char word_index;
      double align_edge = 0;
      int kernchar = 0;
      double left_margin = 0;
      int paragraph_bottom = 0;
      SEGINDEX line_seg_start = 0;
      int word_width = 0;
      int line_count = 0;
      int16_t space_width = 0;
      bool no_wrap = false;
      bool cursor_drawn = false;
      bool edit_mode = false;
      line_state line;
      size_t segment_count = 0;
      size_t segment_restore_start = 0;
      size_t ecell_count = 0;
      size_t clip_count = 0;
      size_t list_depth = 0;
      size_t para_depth = 0;
      size_t font_depth = 0;
      std::vector<doc_segment> segment_tail;
   };

   std::stack<bc_list *>      m_stack_list;
   std::stack<bc_paragraph *> m_stack_para;
   std::stack<bc_font *>      m_stack_font;

   std::vector<doc_clip> m_clips;

   bc_row *m_row = nullptr;                 // Active table row (a persistent state is required in case of loop back)
   font_entry *m_font = nullptr;
   RSTREAM *m_stream = nullptr;
   objVectorViewport *m_viewport = nullptr; // Target viewport (the page)
   padding m_margins;

   double m_cursor_x = 0, m_cursor_y = 0; // Insertion point of the next text character or vector object
   double m_page_width = 0;
   INDEX idx = 0;                 // Current seek position for processing of the stream
   stream_char m_word_index;      // Position of the word currently being operated on
   double m_align_edge = 0;      // Available space for horizontal alignment.  Typically equivalent to wrap_edge(), but can be smaller if a clip region exists on the line.
   int m_kernchar    = 0;        // Previous character of the word being operated on
   double m_left_margin = 0;
   int m_paragraph_bottom = 0;   // Bottom Y coordinate of the current paragraph; defined on paragraph end.
   SEGINDEX m_line_seg_start = 0; // Set to the starting segment of a new line.  Resets on end_line() or wordwrap.  Used for ensuring that all distinct entries on the line use the same line height
   int m_word_width   = 0;       // Pixel width of the current word
   int m_line_count   = 0;       // Increments at every line-end or word-wrap
   int16_t m_space_width  = 0;       // Caches the pixel width of a single space in the current font.
   bool m_no_wrap      = false;   // Set to true when word-wrap is disabled.
   bool m_cursor_drawn = false;   // Set to true when the cursor has been drawn during scene graph creation.
   bool m_edit_mode    = false;   // Set to true when inside an area that allows user editing of the content.

   line_state m_line;

   template <class T> static void restore_stack_depth(std::stack<T *> &Stack, size_t Depth) {
#ifdef _DEBUG
      assert(Stack.size() >= Depth);
#endif

      while (Stack.size() > Depth) Stack.pop();
   }

   layout_checkpoint capture_checkpoint() const {
      layout_checkpoint state;
      state.break_loop          = m_break_loop;
      state.row                 = m_row;
      state.font                = m_font;
      state.cursor_x            = m_cursor_x;
      state.cursor_y            = m_cursor_y;
      state.page_width          = m_page_width;
      state.index               = idx;
      state.word_index          = m_word_index;
      state.align_edge          = m_align_edge;
      state.kernchar            = m_kernchar;
      state.left_margin         = m_left_margin;
      state.paragraph_bottom    = m_paragraph_bottom;
      state.line_seg_start      = m_line_seg_start;
      state.word_width          = m_word_width;
      state.line_count          = m_line_count;
      state.space_width         = m_space_width;
      state.no_wrap             = m_no_wrap;
      state.cursor_drawn        = m_cursor_drawn;
      state.edit_mode           = m_edit_mode;
      state.line                = m_line;
      state.segment_count       = m_segments.size();
      state.segment_restore_start = std::min<size_t>(m_line_seg_start, state.segment_count);
      state.ecell_count         = m_ecells.size();
      state.clip_count          = m_clips.size();
      state.list_depth          = m_stack_list.size();
      state.para_depth          = m_stack_para.size();
      state.font_depth          = m_stack_font.size();
      state.segment_tail.assign(m_segments.begin() + state.segment_restore_start, m_segments.end());
      return state;
   }

   void restore_checkpoint(const layout_checkpoint &State) {
#ifdef _DEBUG
      assert(m_segments.size() >= State.segment_count);
      assert(m_ecells.size() >= State.ecell_count);
      assert(m_clips.size() >= State.clip_count);
      assert(m_stack_list.size() >= State.list_depth);
      assert(m_stack_para.size() >= State.para_depth);
      assert(m_stack_font.size() >= State.font_depth);
#endif

      if (m_segments.size() >= State.segment_count) {
         m_segments.resize(State.segment_count);
         std::copy(State.segment_tail.begin(), State.segment_tail.end(), m_segments.begin() + State.segment_restore_start);
      }

      if (m_ecells.size() >= State.ecell_count) m_ecells.resize(State.ecell_count);
      if (m_clips.size() >= State.clip_count) m_clips.resize(State.clip_count);

      restore_stack_depth(m_stack_list, State.list_depth);
      restore_stack_depth(m_stack_para, State.para_depth);
      restore_stack_depth(m_stack_font, State.font_depth);

      m_break_loop       = State.break_loop;
      m_row              = State.row;
      m_font             = State.font;
      m_cursor_x         = State.cursor_x;
      m_cursor_y         = State.cursor_y;
      m_page_width       = State.page_width;
      idx                = State.index;
      m_word_index       = State.word_index;
      m_align_edge       = State.align_edge;
      m_kernchar         = State.kernchar;
      m_left_margin      = State.left_margin;
      m_paragraph_bottom = State.paragraph_bottom;
      m_line_seg_start   = State.line_seg_start;
      m_word_width       = State.word_width;
      m_line_count       = State.line_count;
      m_space_width      = State.space_width;
      m_no_wrap          = State.no_wrap;
      m_cursor_drawn     = State.cursor_drawn;
      m_edit_mode        = State.edit_mode;
      m_line             = State.line;
   }

   inline void reset() {
      m_clips.clear();
      m_ecells.clear();
      m_segments.clear();

      m_stack_list = {};
      m_stack_para = {};
      m_stack_font = {};

      m_paragraph_bottom = 0;
      m_word_width       = 0;
      m_kernchar         = 0;
      m_no_wrap          = false;
   }

   // Break and reset the content management variables for the active line.  Usually done when a string
   // has been broken up on the current line due to a vector or table graphic for example.

   inline void reset_broken_segment(INDEX Index, double X) {
      m_word_index.reset();

      m_line.index.set(Index);
      m_line.x     = X;
      m_kernchar   = 0;
      m_word_width = 0;
   }

   inline void reset_broken_segment() { reset_broken_segment(idx, m_cursor_x); }

   // Call this prior to new_segment() for ensuring that the content up to this point is correctly finished.

   inline void finish_segment() {
      m_cursor_x  += m_word_width;
      m_word_width = 0;
   }

   // Add a segment for a single byte code at position idx.  This will not include support for text glyphs,
   // so extended information is not required.

   inline void add_graphics_segment() {
      new_segment(stream_char(idx), stream_char(idx + 1), m_cursor_y, 0, 0);
      reset_broken_segment(idx+1, m_cursor_x);
   }

   // Line Management
   // * The LineSpacing of the font determines how tall the segment needs to be.
   // * LineSpacing covers the max height of the font's glyphs, including accents, gutter space and additional
   //   whitespace for clearance.
   // * descent() is used to determine the baseline of the text from the bottom of the segment. It includes both the
   //   gutter and additional whitespace for clearance.

   // When a single line is segmented multiple times, the last segment will store the final height of the line whilst
   // the earlier segments will have the wrong height.  This function ensures that all segments for a line have the
   // same height and descent values.

   inline void sanitise_line_height() {
      auto end = SEGINDEX(m_segments.size());
      if (end > m_line_seg_start) {
         auto final_height = m_segments[end-1].area.Height;
         auto final_descent = m_segments[end-1].descent;

         if (final_height) {
            for (auto i=m_line_seg_start; i < end; i++) {
               m_segments[i].area.Height = final_height;
               m_segments[i].descent     = final_descent;
            }
         }
      }

      m_line_count++;
   }

   // If the current font is larger or equal to the current line height, extend the line height.
   // Note that we use >= because we want to correct the base line in case there is a vector already set on the
   // line that matches the font's line spacing.

   inline void check_line_height() {
      if (m_font->metrics.LineSpacing >= m_line.height) {
         m_line.height  = m_font->metrics.LineSpacing;
         m_line.descent = m_font->descent();
      }
   }

   inline const int wrap_edge() const { // Marks the boundary at which graphics and text will need to wrap.
      return m_page_width - m_margins.right;
   }

   inline size_t clip_limit_count(size_t ClipLimit) const {
      if (ClipLimit < m_clips.size()) return ClipLimit;
      return m_clips.size();
   }

   double glyph_advance(APTR Handle, uint32_t Glyph, uint32_t PrevGlyph = 0) {
      if ((!Self) or (!Handle)) return 0;

      auto key = glyph_cache_key { (uintptr_t)Handle, Glyph, PrevGlyph };
      if (auto it = Self->GlyphAdvanceCache.find(key); it != Self->GlyphAdvanceCache.end()) {
         return it->second.advance;
      }

      double kerning = 0;
      auto advance = vec::CharWidth(Handle, Glyph, PrevGlyph, &kerning) + kerning;
      Self->GlyphAdvanceCache.try_emplace(key, glyph_cache_value { advance });
      return advance;
   }

   void size_widget(widget_mgr &, bool);
   WRAP place_widget(widget_mgr &);
   ERR position_widget(widget_mgr &, doc_segment &, objVectorViewport *, bc_font *, double &, double, bool,
      double &, double &);

   WRAP lay_button(bc_button &);
   CELL lay_cell(bc_table *);
   void lay_font();
   void lay_font_end();
   void lay_index();
   bool lay_list_end();
   void lay_paragraph_end();
   void lay_paragraph();
   void lay_row_end(bc_table *);
   TE   lay_table_end(bc_table &, double, double, double &, double &);
   WRAP lay_text();
   int compute_word_width(const std::string &, const word_token &, int, int &);

   void apply_style(bc_font &);
   double calc_page_height();
   WRAP check_wordwrap(stream_char, double &, double &, double, double, bool = false, size_t = size_t(-1));
   void end_line(NL, stream_char, size_t = size_t(-1));
   void new_code_segment();
   void new_segment(const stream_char, const stream_char, double, double, double);
   WTC wrap_through_clips(double, double, double, double, double &, size_t = size_t(-1));

public:
   layout(extDocument *pSelf, RSTREAM *pStream, objVectorViewport *pViewport, padding &pMargins) :
      Self(pSelf), m_stream(pStream), m_viewport(pViewport), m_margins(pMargins) { }

   ERR do_layout(font_entry **, double &, double &, bool &);
   void gen_scene_graph(objVectorViewport *, std::vector<doc_segment> &);
   ERR gen_scene_init(objVectorViewport *);
   void render_segments(OBJECTID, std::vector<doc_segment> &);
};

//********************************************************************************************************************
static inline void update_layout_metrics(extDocument *Self, size_t SegmentCount, size_t ClipCount)
{
   if (!Self) return;

   if (SegmentCount > Self->LayoutMetrics.segment_count_peak) {
      Self->LayoutMetrics.segment_count_peak = (uint32_t)SegmentCount;
   }

   if (ClipCount > Self->LayoutMetrics.clip_count_peak) {
      Self->LayoutMetrics.clip_count_peak = (uint32_t)ClipCount;
   }
}

struct table_layout_values {
   double stroke_width = 0;
   double cell_h_spacing = 0;
   double cell_v_spacing = 0;
   double min_width = 0;
   double min_height = 0;
   double cell_padding_width = 0;
};

static inline table_layout_values resolve_table_layout(layout &Layout, bc_table &Table)
{
   table_layout_values values;
   values.stroke_width = Table.stroke_width.px(Layout);
   values.cell_h_spacing = Table.cell_h_spacing.px(Layout);
   values.cell_v_spacing = Table.cell_v_spacing.px(Layout);
   values.min_width = Table.min_width.px(Layout);
   values.min_height = Table.min_height.px(Layout);
   values.cell_padding_width = Table.cell_padding.left + Table.cell_padding.right;
   return values;
}

static inline bool segment_merge_barrier(SCODE Code)
{
   switch (Code) {
      case SCODE::BUTTON:
      case SCODE::CHECKBOX:
      case SCODE::COMBOBOX:
      case SCODE::IMAGE:
      case SCODE::INPUT:
      case SCODE::TABLE_START:
      case SCODE::TABLE_END:
      case SCODE::TEXT:
         return true;

      default:
         return false;
   }
}

static inline bool range_has_segment_merge_barrier(RSTREAM *Stream, stream_char Start, stream_char Stop)
{
   for (auto i=Start; i < Stop; i.next_code()) {
      if (segment_merge_barrier(Stream[0][i.index].code)) return true;
   }

   return false;
}

//********************************************************************************************************************
// In the first pass, the size of each cell is calculated with respect to its content.  When SCODE::TABLE_END is
// reached, the max height and width for each row/column will be calculated and a subsequent pass will be made to
// fill out the cells.
//
// If the width of a cell increases, there is a chance that the height of all cells in that column will decrease,
// subsequently lowering the row height of all rows in the table, not just the current row.  Therefore on the second
// pass the row heights need to be recalculated from scratch.

CELL layout::lay_cell(bc_table *Table)
{
   kt::Log log(__FUNCTION__);

   bool vertical_repass = false;

   auto &cell = m_stream->lookup<bc_cell>(idx);

   if (!Table) {
      Self->Error = log.warning(ERR::InvalidData);
      return CELL::ABORT;
   }

   if (cell.column >= std::ssize(Table->columns)) {
      DLAYOUT("Cell %d exceeds total table column limit of %d.", cell.column, (int)std::ssize(Table->columns));
      return CELL::NIL;
   }

   auto values = resolve_table_layout(*this, *Table);
   auto column_count = std::ssize(Table->columns);

   // Adding a segment ensures that the table graphics will be accounted for when drawing.

   new_segment(stream_char(idx), stream_char(idx + 1), m_cursor_y, 0, 0);

   // Set the absolute coordinates of the cell within the viewport.

   cell.x = m_cursor_x - Table->x;
   cell.y = m_cursor_y - Table->y;

   if (Table->collapsed) {
      //if (cell.column IS 0);
      //else cell.AbsX += Table->cell_h_spacing.px(*this);
   }
   else cell.x += values.cell_h_spacing;

   if (cell.column IS 0) cell.x += values.stroke_width;

   cell.width  = Table->columns[cell.column].width; // Minimum width for the cell's column
   cell.height = m_row->row_height;
   //DLAYOUT("%d / %d", escrow->min_height, escrow->row_height);

   DLAYOUT("Index %d, Processing cell at (%g,%gy), size (%g,%g), column %d",
      idx, m_cursor_x, m_cursor_y, cell.width, cell.height, cell.column);

   if (cell.viewport.empty()) {
      cell.viewport.set(objVectorViewport::create::global({
         fl::Name("cell_viewport"),
         fl::Owner(Table->viewport->UID),
         fl::X(0), fl::Y(0),
         fl::Width(1), fl::Height(1)
      }));

      if (cell.hooks.events != JTYPE::NIL) {
         cell.viewport->subscribeInput(cell.hooks.events, C_FUNCTION(inputevent_cell));
      }
   }

   if (!cell.fill.empty()) {
      if (cell.rect_fill.empty()) {
         cell.rect_fill.set(objVectorRectangle::create::global({
            fl::Name("cell_rect"),
            fl::Owner(cell.viewport->UID),
            fl::X(0), fl::Y(0), fl::Width(SCALE(1.0)), fl::Height(SCALE(1.0))
         }));
      }
   }

   if ((!cell.stroke.empty()) or
       ((cell.stroke_width.value > 0) and (cell.stroke_width != Table->stroke_width))) {
      if (cell.border_path.empty()) {
         cell.border_path.set(objVectorPath::create::global({
            fl::Name("cell_border"),
            fl::Owner(cell.viewport->UID)
         }));
      }
   }

   if (!cell.stream->data.empty()) {
      m_edit_mode = (!cell.edit_def.empty()) ? true : false;

      Self->LayoutMetrics.cell_layouts++;

      layout sl(Self, cell.stream, *cell.viewport, Table->cell_padding);
      sl.m_depth = m_depth + 1;
      if (auto error = sl.do_layout(&m_font, cell.width, cell.height, vertical_repass); not (error IS ERR::Okay)) {
         Self->Error = error;
         return CELL::ABORT;
      }

      // The main product of do_layout() are the produced segments

      cell.segments = sl.m_segments;

      if (!cell.edit_def.empty()) {
         m_edit_mode = false;

         // Edit cells have a minimum width/height so that the user can still interact with them when empty.

         if (sl.m_segments.empty()) {
            // No content segments were created, which means that there's nothing for the editing cursor to attach
            // itself too.

            // TODO: Maybe ceate an SCODE::NOP type that gets placed at the start of the edit cell.  If there's no genuine
            // content, then we at least have the SCODE::NOP type for the cursor to be attached to?
         }

         if (cell.width < 16) cell.width = 16;
         if (cell.height < m_font->metrics.LineSpacing) cell.height = m_font->metrics.LineSpacing;
      }
   }

   DLAYOUT("Cell (%d:%d) is size (%gx%g, %gx%g); min. width %g", Table->row_index, cell.column,
      cell.x, cell.y, cell.width, cell.height, Table->columns[cell.column].width);

   // Increase the overall width for the entire column if this cell has increased the column width.
   // This will affect the entire table, so a restart from TABLE_START is required.

   if (Table->columns[cell.column].width < cell.width) {
      DLAYOUT("Increasing column width of cell (%d:%d) from %g to %g (table_start repass required).", Table->row_index, cell.column, Table->columns[cell.column].width, cell.width);
      Table->columns[cell.column].width = cell.width; // This has the effect of increasing the minimum column width for all cells in the column

      // Percentage based and zero columns need to be recalculated.  The easiest thing to do
      // would be for a complete recompute (ComputeColumns = true) with the new minwidth.  The
      // problem with ComputeColumns is that it does it all from scratch - we need to adjust it
      // so that it can operate in a second style of mode where it recognises temporary width values.

      Table->columns[cell.column].min_width = cell.width; // Column must be at least this size
      Table->compute_columns = 2;
      Table->reset_row_height = true; // Row heights need to be reset due to the width increase
      return CELL::WRAP_TABLE_CELL;
   }

   // Advance the width of the entire row and adjust the row height

   Table->row_width += Table->columns[cell.column].width;

   if (!Table->collapsed) Table->row_width += values.cell_h_spacing;
   else if ((cell.column + cell.col_span) < column_count-1) {
      Table->row_width += values.cell_h_spacing;
   }

   if ((cell.height > m_row->row_height) or (m_row->vertical_repass)) {
      // A repass will be required if the row height has increased and vectors or tables have been used
      // in earlier cells, because vectors need to know the final dimensions of their table cell (viewport).

      if (cell.column IS std::ssize(Table->columns)-1) {
         DLAYOUT("Extending row height from %g to %g (row repass required)", m_row->row_height, cell.height);
      }

      m_row->row_height = cell.height;
      if (cell.column + cell.col_span >= std::ssize(Table->columns)) {
         return CELL::REPASS_ROW_HEIGHT;
      }
      else m_row->vertical_repass = true; // Make a note to do a vertical repass once all columns on this row have been processed
   }

   m_cursor_x += Table->columns[cell.column].width;

   if (!Table->collapsed) m_cursor_x += values.cell_h_spacing;
   else if ((cell.column + cell.col_span) < column_count) m_cursor_x += values.cell_h_spacing;

   if (cell.column IS 0) m_cursor_x += values.stroke_width;

   if (!cell.edit_def.empty()) {
      // The area of each edit cell is logged for assisting interaction between the mouse pointer and the cells.
      m_ecells.emplace_back(cell.cell_id, cell.x, cell.y, cell.width, cell.height);
   }

   return CELL::NIL;
}

//********************************************************************************************************************
// Calculate widget dimensions (refer to place_widget() for the x,y values).  The host rectangle is modified in
// gen_scene_graph() as this is the most optimal approach (i.e. if the page width expands during layout).

void layout::size_widget(widget_mgr &Widget, bool ScaleToFont)
{
   if (!Widget.floating_x()) check_line_height(); // Necessary for inline widgets in case they are the first 'character' on the line.

   // Calculate the final width and height.

   if (Widget.width.empty()) {
      if (Widget.height.empty()) Widget.final_width = Widget.def_size.px(*this);
      else if (Widget.height.type IS DU::SCALED) {
         if (Widget.floating_x()) Widget.final_width = Widget.height.value * (m_page_width - m_left_margin - m_margins.right);
         else Widget.final_width = Widget.height.px(*this);
      }
      else Widget.final_width = Widget.height.px(*this);
   }
   else if (Widget.width.type IS DU::SCALED) {
      Widget.final_width = Widget.width.value * (m_page_width - m_left_margin - m_margins.right);
   }
   else Widget.final_width = Widget.width.px(*this);

   if (Widget.height.type IS DU::SCALED) {
      if (Widget.floating_x()) Widget.final_height = Widget.height.value * (m_page_width - m_left_margin - m_margins.right);
      else Widget.final_height = Widget.height.px(*this) * m_font->metrics.Ascent;
   }
   else if (Widget.height.empty()) {
      Widget.final_height = Widget.def_size.px(*this);
   }
   else Widget.final_height = Widget.height.px(*this);

   if (Widget.final_height < 0.01) Widget.final_height = 0.01;
   if (Widget.final_width < 0.01) Widget.final_width = 0.01;

   if (Widget.pad.configured) {
      double scale;
      if (ScaleToFont) scale = m_font->metrics.Height;
      else scale = fast_hypot(Widget.final_width, Widget.final_height);
      Widget.final_pad.left   = Widget.pad.left_scl ? (Widget.pad.left * scale) : Widget.pad.left;
      Widget.final_pad.top    = Widget.pad.top_scl ? (Widget.pad.top * scale) : Widget.pad.top;
      Widget.final_pad.right  = Widget.pad.right_scl ? (Widget.pad.right * scale) : Widget.pad.right;
      Widget.final_pad.bottom = Widget.pad.bottom_scl ? (Widget.pad.bottom * scale) : Widget.pad.bottom;
   }

   if (!Widget.label.empty()) {
      Widget.label_width = vec::StringWidth(m_font->handle, Widget.label.c_str(), -1);
   }
   else Widget.label_width = 0;
}

//********************************************************************************************************************
// Calculate the position of a widget, check for word-wrapping etc.
//
// NOTE: If you ever see a widget unexpectedly appearing at (0,0) it's because it hasn't been included in a draw
// segment.

WRAP layout::place_widget(widget_mgr &Widget)
{
   auto wrap_result = WRAP::DO_NOTHING;
   auto label_pad = Widget.internal_page ? 0.0 : Widget.label_pad.px(*this);
   auto full_width = Widget.final_width + Widget.final_pad.left + Widget.final_pad.right;
   auto full_height = Widget.full_height();
   if (!Widget.internal_page) full_width += Widget.label_width + label_pad;

   if (Widget.floating_x()) {
      // Calculate horizontal position

      if ((Widget.align & ALIGN::LEFT) != ALIGN::NIL) {
         Widget.x = m_left_margin;
      }
      else if ((Widget.align & ALIGN::CENTER) != ALIGN::NIL) {
         // We use the left margin and not the cursor for calculating the center because the widget is floating.
         Widget.x = m_left_margin + ((m_align_edge - full_width) * 0.5);
      }
      else if ((Widget.align & ALIGN::RIGHT) != ALIGN::NIL) {
         Widget.x = m_align_edge - full_width;
      }
      else Widget.x = m_cursor_x;

      if (m_line.index < idx) { // Any outstanding content has to be set prior to add_graphics_segment()
         finish_segment();
         new_segment(m_line.index, stream_char(idx), m_cursor_y, m_cursor_x - m_line.x, m_align_edge - m_line.x);
         reset_broken_segment();
      }

      add_graphics_segment();

      // For a floating widget we need to declare a clip region based on the final widget dimensions.
      // TODO: Add support for masked clipping through SVG paths.

      m_clips.emplace_back(Widget.x, m_cursor_y, Widget.x + full_width, m_cursor_y + full_height,
         idx, false, "Widget");
      update_layout_metrics(Self, m_segments.size(), m_clips.size());
   }
   else { // Widget is inline and must be treated like a text character.
      if (!m_word_width) m_word_index.set(idx); // Save the index of the new word

      // Checking for wordwrap here is optimal, BUT bear in mind that if characters immediately follow the
      // widget then it is also possible for word-wrapping to occur later.  Note that the line height isn't
      // adjusted in this call because if a wrap occurs then the widget won't be in the former segment.

      wrap_result = check_wordwrap(m_word_index,
         m_cursor_x, m_cursor_y, m_word_width + full_width, m_line.height);

      // The inline widget will probably increase the height of the line, but due to the potential for delayed
      // word-wrapping (if we're part of an embedded word) we need to cache the value for now.

      if (full_height > m_line.word_height) m_line.word_height = full_height;

      m_word_width += full_width;
      m_kernchar   = 0;
   }

   return wrap_result;
}

//********************************************************************************************************************
// The button follows the same principles as other widgets, but the internal label is processed like a table cell, and
// this affects size and positioning.

WRAP layout::lay_button(bc_button &Button)
{
   kt::Log log(__FUNCTION__);

   if (!Button.inner_padding.configured) {
      // A default for padding is required if the client hasn't defined any.
      Button.inner_padding.configured = true;
      Button.inner_padding = padding { 1, 0.333, 1, 0.333 };
      Button.inner_padding.scale_all();
   }

   // The size of the button will be determined by its content.  Dimensions specified by the client are interpreted
   // as minimum values.

   size_widget(Button, true); // Defines final_pad, final_width, final_height

   if (Button.viewport.empty()) {
      Button.viewport.set(objVectorViewport::create::global({
         fl::Name("btn_viewport"),
         fl::Owner(m_viewport->UID),
         fl::X(0), fl::Y(0),
         fl::Width(1), fl::Height(1)
      }));

      if (Button.viewport->Scene->SurfaceID) {
         Button.viewport->subscribeInput(JTYPE::BUTTON|JTYPE::CROSSING, C_FUNCTION(inputevent_button));
      }

      Button.viewport->setFill(Button.fill);
   }

   if (!Button.stream->data.empty()) {
      auto scale = m_font->metrics.Height;
      padding inner_pad = {
         Button.inner_padding.left_scl ? (Button.inner_padding.left * scale) : Button.inner_padding.left,
         Button.inner_padding.top_scl ? (Button.inner_padding.top * scale) : Button.inner_padding.top,
         Button.inner_padding.right_scl ? (Button.inner_padding.right * scale) : Button.inner_padding.right,
         Button.inner_padding.bottom_scl ? (Button.inner_padding.bottom * scale) : Button.inner_padding.bottom
      };

      layout sl(Self, Button.stream, *Button.viewport, inner_pad);
      sl.m_depth = m_depth + 1;

      bool vertical_repass = false;
      if (auto error = sl.do_layout(&m_font, Button.final_width, Button.final_height, vertical_repass);
          not (error IS ERR::Okay)) {
         Self->Error = error;
         return WRAP::DO_NOTHING;
      }
      Button.segments = sl.m_segments;  // The main product of do_layout() are the produced segments.
   }

   auto wrap = place_widget(Button);
   return wrap;
}

//********************************************************************************************************************
// Any style change must be followed with a call to this function to ensure that its config is applied.

void layout::apply_style(bc_font &Style) {
   if ((Style.options & FSO::ALIGN_RIGHT) != FSO::NIL) m_font->align = ALIGN::RIGHT;
   else if ((Style.options & FSO::ALIGN_CENTER) != FSO::NIL) m_font->align = ALIGN::HORIZONTAL;
   else m_font->align = ALIGN::NIL;

   m_no_wrap = ((Style.options & FSO::NO_WRAP) != FSO::NIL);
   m_space_width = m_font->space_width;
}

//********************************************************************************************************************

void layout::lay_font()
{
   auto &style = m_stream->lookup<bc_font>(idx);

   if ((m_font = style.layout_font(*this))) {
      apply_style(style);

      // Setting m_word_index ensures that the font code appears in the current segment.

      if (!m_word_width) m_word_index.set(idx);
   }

   m_stack_font.push(&m_stream->lookup<bc_font>(idx));
}

void layout::lay_font_end()
{
   if ((m_word_width > 0) and (m_line.height < m_font->metrics.LineSpacing)) {
      // We need to record the line-height for the active word now, in case we revert to a smaller font.
      m_line.height  = m_font->metrics.LineSpacing;
      m_line.descent = m_font->descent();
   }

   m_stack_font.pop();
   if (!m_stack_font.empty()) {
      m_font = m_stack_font.top()->layout_font(*this);
      apply_style(*m_stack_font.top());
   }
}

//********************************************************************************************************************
// Compute the pixel width of a word token by summing glyph advances with kerning.  Width is accumulated using the
// same integer truncation semantics as the original per-glyph `m_word_width += glyph_advance(...)` path so cached
// words preserve existing wrap points.  The InitKern parameter supports cross-text word continuation where the
// previous character affects the first glyph's kerning.  LastChar receives the last unicode codepoint in the token for
// subsequent kerning.
//
// NOTE: Bear in mind that the first word in a TEXT string could be a direct continuation of a previous TEXT word.
// This can occur if the font colour is changed mid-word for example.

int layout::compute_word_width(const std::string &Str, const word_token &Token, int InitKern, int &LastChar)
{
   int width = 0;
   auto handle = m_font->handle;
   int kern = InitKern;
   for (unsigned i = Token.start; i < Token.stop; ) {
      int unicode;
      i += getutf8(Str.c_str() + i, &unicode);
      width += glyph_advance(handle, unicode, kern);
      kern = unicode;
   }
   LastChar = kern;
   return width;
}

//********************************************************************************************************************
// NOTE: Bear in mind that the first word in a TEXT string could be a direct continuation of a previous TEXT word.
// This can occur if the font colour is changed mid-word for example.

WRAP layout::lay_text()
{
   kt::Log log(__FUNCTION__);
   auto wrap_result = WRAP::DO_NOTHING; // Needs to change to WRAP::EXTEND_PAGE if a word is > width

   m_align_edge = wrap_edge(); // TODO: Not sure about this following the switch to embedded TEXT structures

   auto ascent = m_font->metrics.Ascent;
   auto &text = m_stream->lookup<bc_text>(idx);
   auto &str = text.text;
   text.vector_text.clear();

   // Ensure the token cache is up to date

   if (text.tokens_dirty) text.tokenise();

   if (text.width_cache_generation != Self->WidthCacheGeneration) {
      text.invalidate_widths();
      text.width_cache_generation = Self->WidthCacheGeneration;
   }

   for (auto &tok : text.tokens) {
      if (tok.is_newline()) {
         check_line_height();
         wrap_result = check_wordwrap(m_word_index, m_cursor_x, m_cursor_y, m_word_width,
            (m_line.height < 1) ? 1 : m_line.height);
         if (wrap_result IS WRAP::EXTEND_PAGE) break;

         end_line(NL::PARAGRAPH, stream_char(idx, tok.start));
      }
      else if (tok.is_whitespace()) {
         check_line_height();

         if ((m_word_width) and (!m_no_wrap)) { // Existing word finished, check if it will wordwrap
            wrap_result = check_wordwrap(m_word_index, m_cursor_x, m_cursor_y,
               m_word_width, (m_line.height < 1) ? 1 : m_line.height);
            if (wrap_result IS WRAP::EXTEND_PAGE) break;
         }

         m_line.apply_word_height();

         m_cursor_x += m_word_width + m_space_width;

         // Current word state must be reset.
         m_kernchar   = 0;
         m_word_width = 0;
      }
      else { // WORD token
         int last_char = 0;

         if (!m_word_width) {
            m_word_index.set(idx, tok.start);   // Save the index of the new word
            check_line_height();

            // Use cached width when available, otherwise compute and cache it

            if ((tok.width >= 0) and (!text.widths_dirty)) {
               m_word_width += tok.width;
               last_char = tok.last_char;
            }
            else {
               auto w = compute_word_width(str, tok, 0, last_char);
               if (w > MAX_WORD_TOKEN_WIDTH) {
                  log.warning("Word width %d exceeds the legal limit of %d pixels.", w, MAX_WORD_TOKEN_WIDTH);
                  Self->Error = ERR::InvalidData;
                  return WRAP::DO_NOTHING;
               }

               tok.width = w;
               tok.last_char = last_char;
               m_word_width += w;
            }
         }
         else {
            // Continuation of a previous word (cross-text or consecutive word tokens).
            // Kerning depends on m_kernchar, so we compute character-by-character.

            auto w = compute_word_width(str, tok, m_kernchar, last_char);
            if (w > MAX_WORD_TOKEN_WIDTH) {
               log.warning("Word width %d exceeds the legal limit of %d pixels.", w, MAX_WORD_TOKEN_WIDTH);
               Self->Error = ERR::InvalidData;
               return WRAP::DO_NOTHING;
            }

            m_word_width += w;
         }

         if (m_word_width > MAX_WORD_TOKEN_WIDTH) {
            log.warning("Word width %d exceeds the legal limit of %d pixels.", m_word_width, MAX_WORD_TOKEN_WIDTH);
            Self->Error = ERR::InvalidData;
            return WRAP::DO_NOTHING;
         }

         m_kernchar = last_char;

         if (ascent > m_line.word_height) m_line.word_height = ascent;
      }
   }

   text.widths_dirty = false;

   // Entire text string has been processed, perform one final wrapping check.

   if ((m_word_width) and (!m_no_wrap)) {
      wrap_result = check_wordwrap(m_word_index, m_cursor_x, m_cursor_y, m_word_width,
         (m_line.height < 1) ? 1 : m_line.height);
   }

   if ((m_no_wrap) and (m_cursor_x + m_word_width > m_page_width)) {
      m_page_width = m_cursor_x + m_word_width + m_margins.right;
      wrap_result = WRAP::EXTEND_PAGE;
   }

   return wrap_result;
}

//********************************************************************************************************************
// Returns true if a repass is required

bool layout::lay_list_end()
{
   if (m_stack_list.empty()) return false;

   // If it is a custom list, a repass may be required

   if ((m_stack_list.top()->type IS bc_list::CUSTOM) and (m_stack_list.top()->repass)) {
      return true;
   }

   m_stack_list.pop();

   if (m_stack_list.empty()) {
      // At the end of a list, increase the whitespace to that of a standard paragraph.
      if (!m_stack_para.empty()) end_line(NL::PARAGRAPH, stream_char(idx));
      else end_line(NL::PARAGRAPH, stream_char(idx));
   }

   return false;
}

//********************************************************************************************************************
// Indexes don't do anything, but recording the cursor's y value when they are encountered
// makes it really easy to scroll to a bookmark when requested (show_bookmark()).

void layout::lay_index()
{
   kt::Log log(__FUNCTION__);

   auto escindex = &m_stream->lookup<bc_index>(idx);
   escindex->y = m_cursor_y;

   if (!escindex->visible) {
      // If not visible, all content within the index is not to be displayed

      auto end = idx;
      while (end < INDEX(m_stream[0].size())) {
         if (m_stream[0][end].code IS SCODE::INDEX_END) {
            bc_index_end &iend = m_stream->lookup<bc_index_end>(end);
            if (iend.id IS escindex->id) {
               m_line.index.set(end);
               idx = end;
               return;
            }

         }

         end++;
      }

      log.warning("Failed to find matching index-end.  Document stream is corrupt.");
   }
}

//********************************************************************************************************************

void layout::lay_row_end(bc_table *Table)
{
   kt::Log log;

   auto Row = m_row;
   auto values = resolve_table_layout(*this, *Table);
   Table->row_index++;

   // Increase the table height if the row exceeds it

   auto bottom = Row->y + Row->row_height + values.cell_v_spacing;
   if (bottom > Table->y + Table->height) Table->height = bottom - Table->y;

   // Advance the cursor by the height of this row

   m_cursor_y += Row->row_height + values.cell_v_spacing;
   m_cursor_x = Table->x;
   DLAYOUT("Row ends, advancing down by %g+%g, new height: %g, y-cursor: %g",
      Row->row_height, values.cell_v_spacing, Table->height, m_cursor_y);

   if (Table->row_width > Table->width) Table->width = Table->row_width;

   new_code_segment();
}

//********************************************************************************************************************

void layout::lay_paragraph()
{
   if (!m_stack_para.empty()) { // Embedded paragraph detected - end the current line.
      m_left_margin = m_stack_para.top()->x; // Reset the margin so that the next line will be flush with the parent
      end_line(NL::PARAGRAPH, stream_char(idx));
   }

   auto &para = m_stream->lookup<bc_paragraph>(idx);
   m_stack_para.push(&para);

   if (!m_stack_list.empty()) {
      // If a paragraph is inside a list then it's treated as a list item.
      // Indentation values are inherited from the list.

      auto list = m_stack_list.top();
      if (para.list_item) {
         if (m_stack_para.size() > 1) para.indent = list->block_indent;
         para.item_indent = list->item_indent;

         if (!para.value.empty()) {
            auto strwidth = vec::StringWidth(m_font->handle, para.value.c_str(), -1) + 10;
            auto item_indent = list->item_indent.px(*this);
            if (strwidth > item_indent) {
               list->item_indent = DUNIT(strwidth, DU::PIXEL);
               para.item_indent  = DUNIT(strwidth, DU::PIXEL);
               list->repass      = true;
            }

            if (para.icon.empty()) {
               para.icon.set(objVectorText::create::global({
                  fl::Name("list_point"),
                  fl::Owner(m_viewport->UID),
                  fl::Fill(list->fill),
                  fl::String(para.value),
                  fl::Face(m_font->face),
                  fl::FontSize(m_font->font_size),
                  fl::FontStyle(m_font->style),
                  fl::Fill(list->fill)
               }));
            }
         }
         else if (list->type IS bc_list::BULLET) {
            if (para.icon.empty()) {
               para.icon.set(objVectorEllipse::create::global({
                  fl::Name("bullet_point"),
                  fl::Owner(m_viewport->UID),
                  fl::Fill(list->fill)
               }));
            }
         }
      }
      else para.indent = list->item_indent;
   }

   if (!para.indent.empty()) para.block_indent = para.indent;

   auto block_indent = para.block_indent.px(*this);
   auto item_indent = para.item_indent.px(*this);
   para.x = m_left_margin + block_indent;

   auto advance = block_indent + item_indent;

   m_left_margin += advance;
   m_cursor_x    += advance;
   m_line.x      += advance;

   // Paragraph management variables

   if (!m_stack_list.empty()) para.leading = m_stack_list.top()->v_spacing;

   m_font = para.font.layout_font(*this);

   if (!m_font) {
      kt::Log log;
      DLAYOUT("Failed to lookup font for %s:%d", para.font.face.c_str(), para.font.pixel_size);
      Self->Error = ERR::Failed;
      return;
   }

   apply_style(para.font);

   if (m_line_count > 0) {
      double para_leading;
      if (para.leading.type IS DU::LINE_HEIGHT) {
         // Paragraph leading is block spacing, so resolve it against the document's default line height
         // rather than the paragraph's own font metrics.  This keeps headings from inflating pre-spacing.
         bc_font root_style;
         root_style.face = Self->FontFace;
         root_style.style = Self->FontStyle;
         root_style.req_size = DUNIT(Self->FontSize, DU::PIXEL);

         if (auto root_font = root_style.layout_font(*this)) {
            para_leading = std::trunc(para.leading.value * root_font->metrics.LineSpacing);
         }
         else para_leading = para.leading.px(*this);
      }
      else para_leading = para.leading.px(*this);

      m_cursor_y += para_leading;
   }

   para.y = m_cursor_y;
   para.height = 0;

   m_stack_font.push(&para.font);
}

//********************************************************************************************************************

void layout::lay_paragraph_end()
{
   if (!m_stack_para.empty()) {
      // The paragraph height reflects the true size of the paragraph after we take into account
      // any inline graphics within the paragraph.

      auto para = m_stack_para.top();
      m_paragraph_bottom = para->y + para->height;

      end_line(NL::PARAGRAPH, stream_char(idx + 1));

      auto x = para->x - para->block_indent.px(*this);
      m_left_margin = x;
      m_cursor_x    = x;
      m_line.x      = x;
      m_stack_para.pop();
   }
   else end_line(NL::PARAGRAPH, stream_char(idx + 1)); // Technically an error when there's no matching PS code.

   m_stack_font.pop();
   if (!m_stack_font.empty()) {
      m_font = m_stack_font.top()->layout_font(*this);
      apply_style(*m_stack_font.top());
   }
}

//********************************************************************************************************************

TE layout::lay_table_end(bc_table &Table, double TopMargin, double BottomMargin, double &Height, double &Width)
{
   kt::Log log(__FUNCTION__);

   double min_height;
   auto values = resolve_table_layout(*this, Table);
   auto column_count = Table.columns.size();

   if (Table.cells_expanded IS false) {
      int unfixed;
      double colwidth;

      // Table cells need to match the available width inside the table.  This routine checks for that - if the cells
      // are short then the table processing is restarted.

      DLAYOUT("Checking table @ index %d for cell/table widening.  Table width: %g", idx, Table.width);

      Table.cells_expanded = true;

      if (!Table.columns.empty()) {
         colwidth = (values.stroke_width * 2) + values.cell_h_spacing;
         for (auto &col : Table.columns) {
            colwidth += col.width + values.cell_h_spacing;
         }
         if (Table.collapsed) colwidth -= values.cell_h_spacing * 2; // Collapsed tables have no spacing allocated on the sides

         if (colwidth < Table.width) { // Cell layout is less than the pre-determined table width
            // Calculate the amount of additional space that is available for cells to expand into

            double avail_width = Table.width - (values.stroke_width * 2) -
               (values.cell_h_spacing * (column_count - 1));

            if (!Table.collapsed) avail_width -= (values.cell_h_spacing * 2);

            // Count the number of columns that do not have a fixed size

            unfixed = 0;
            for (unsigned j=0; j < Table.columns.size(); j++) {
               if (Table.columns[j].preset_width) avail_width -= Table.columns[j].width;
               else unfixed++;
            }

            // Adjust for expandable columns that we know have exceeded the pre-calculated cell width
            // on previous passes (we want to treat them the same as the preset_width columns)  Such cells
            // will often exist that contain large graphics for example.

            if (unfixed > 0) {
               double cell_width = avail_width / unfixed;
               for (unsigned j=0; j < Table.columns.size(); j++) {
                  if ((Table.columns[j].min_width) and (Table.columns[j].min_width > cell_width)) {
                     avail_width -= Table.columns[j].min_width;
                     unfixed--;
                  }
               }

               if (unfixed > 0) {
                  cell_width = avail_width / unfixed;
                  bool expanded = false;

                  //total = 0;
                  for (unsigned j=0; j < Table.columns.size(); j++) {
                     if (Table.columns[j].preset_width) continue; // Columns with preset-widths are never auto-expanded
                     if (Table.columns[j].min_width > cell_width) continue;

                     if (Table.columns[j].width < cell_width) {
                        DLAYOUT("Expanding column %d from width %g to %g", j, Table.columns[j].width, cell_width);
                        Table.columns[j].width = cell_width;
                        //if (total - (double)F2I(total) >= 0.5) Table.Columns[j].width++; // Fractional correction

                        expanded = true;
                     }
                     //total += cell_width;
                  }

                  if (expanded) {
                     DLAYOUT("At least one cell was widened - will repass table layout.");
                     return TE::WRAP_TABLE;
                  }
               }
            }
         }
      }
      else DLAYOUT("Table is missing its columns array.");
   }
   else DLAYOUT("Cells already widened - keeping table width of %g.", Table.width);

   // Cater for the minimum height requested

   if (Table.min_height.type IS DU::SCALED) {
      // If the table height is expressed as a percentage, it is calculated against the line width
      // so that it remains proportional.

      min_height = (wrap_edge() - m_margins.left) * Table.min_height.value;
      if (min_height < 0) min_height = 0;
   }
   else min_height = values.min_height;

   if (min_height > Table.height + values.cell_v_spacing + values.stroke_width) {
      // The last row in the table needs its height increased
      if (m_row) {
         auto h = min_height - (Table.height + values.cell_v_spacing + values.stroke_width);
         DLAYOUT("Extending table height to %g (row %g+%g) due to a minimum height of %g at coord %g",
            min_height, m_row->row_height, h, values.min_height, Table.y);
         m_row->row_height += h;
         return TE::REPASS_ROW_HEIGHT;
      }
      else log.warning("No last row defined for table height extension.");
   }

   // Adjust for cellspacing at the bottom

   Table.height += values.cell_v_spacing + values.stroke_width;

   // Restart if the width of the table will force an extension of the page.

   double right_side = Table.x + Table.width + m_margins.right;
   if ((right_side > Width) and (Width < WIDTH_LIMIT)) {
      DLAYOUT("Table width (%g+%g) increases page width to %g, layout restart forced.",
         Table.x, Table.width, right_side);
      Width = right_side;
      return TE::EXTEND_PAGE;
   }

   if (Table.floating_x()) {
      if ((Table.align & ALIGN::CENTER) != ALIGN::NIL) {
         Table.x = m_left_margin + ((m_align_edge - Table.width) * 0.5);
      }
      else if ((Table.align & ALIGN::RIGHT) != ALIGN::NIL) {
         Table.x = m_align_edge - Table.width;
      }
      else Table.x = m_left_margin;
   }

   // Inline tables extend the line height.  We also extend the line height if the table covers the
   // entire page width (a valuable optimisation for the layout routine).

   if ((!Table.floating_x()) or ((Table.x <= m_left_margin) and (Table.x + Table.width >= wrap_edge()))) {
      if (Table.height > m_line.height) m_line.height = Table.height;
   }

   if (!m_stack_para.empty()) {
      double j = (Table.y + Table.height) - m_stack_para.top()->y;
      if (j > m_stack_para.top()->height) m_stack_para.top()->height = j;
   }

   // Check if the table collides with clipping boundaries and adjust its position accordingly.
   // Such a check is performed in SCODE::TABLE_START - this second check is required only if the width
   // of the table has been extended.
   //
   // Note that the total number of clips is adjusted so that only clips up to the TABLE_START are
   // considered (otherwise, clips inside the table cells will cause collisions against the parent
   // table).

   DLAYOUT("Checking table collisions (%gx%g, %gx%g).", Table.x, Table.y, Table.width, Table.height);

   auto ww = check_wordwrap(idx, Table.x, Table.y, Table.width, Table.height, Table.floating_x(), Table.total_clips);

   if (ww IS WRAP::EXTEND_PAGE) {
      DLAYOUT("Table wrapped - expanding page width due to table size/position.");
      return TE::EXTEND_PAGE;
   }
   else if (ww IS WRAP::WRAPPED) {
      // A repass is necessary as everything in the table will need to be rearranged
      DLAYOUT("Table wrapped - rearrangement necessary.");
      return TE::WRAP_TABLE;
   }

   // The table sets a clipping region because its surrounds are available as whitespace for
   // other content.

   m_clips.emplace_back(Table.x, Table.y, Table.x + Table.width, Table.y + Table.height, idx, false, "Table");
   update_layout_metrics(Self, m_segments.size(), m_clips.size());

   m_cursor_x = Table.x + Table.width;
   m_cursor_y = Table.y;

   DLAYOUT("Final Table Size: %gx%g,%gx%g", Table.x, Table.y, Table.width, Table.height);

   add_graphics_segment(); // Table-end is a graphics segment for inline handling
   return TE::NIL;
}

//********************************************************************************************************************
// This function creates segments that will be used in the final stage of the layout process to draw the graphics.
// They can also assist with user interactivity, e.g. to determine the character that the mouse is positioned over.

void layout::new_segment(const stream_char Start, const stream_char Stop, double Y, double Width, double AlignWidth)
{
   kt::Log log(__FUNCTION__);
   Self->LayoutMetrics.new_segment_calls++;

   if (Width > AlignWidth) {
      log.trace("Content width of %g exceeds align width of %g", Width, AlignWidth);
   }

   // Process trailing whitespace at the end of the line.  This helps to prevent situations such as underlining
   // occurring in whitespace at the end of the line during word-wrapping.

   auto trim_stop = Stop;
   while ((unsigned(trim_stop.get_prev_char_or_inline(m_stream[0])) <= 0x20) and (trim_stop > Start)) {
      if (!trim_stop.get_prev_char_or_inline(m_stream[0])) break;
      trim_stop.prev_char(m_stream[0]);
   }

   if (Start >= Stop) {
      DLAYOUT("Cancelling new segment, no content in range %d-%d \"%.20s\"",
         Start.index, Stop.index, printable(*m_stream, Start).c_str());
      return;
   }

   // If allow_merge is true, this segment can be merged with prior segment(s) on the line to create one continuous
   // segment.  The basic rule is that any code that produces a graphics element is not safe to merge.

   bool allow_merge;
   if (Width > 0) {
      allow_merge = false;
   }
   else if ((Start.offset IS 0) and (Stop.offset IS 0) and (Start.index + 1 IS Stop.index)) {
      allow_merge = !segment_merge_barrier(m_stream[0][Start.index].code);
   }
   else allow_merge = !range_has_segment_merge_barrier(m_stream, Start, Stop);

   auto line_height = m_line.height;
   auto descent     = m_line.descent;
   if (Width) {
      if (line_height <= 0) {
         // Use the most recent font to determine the line height
         line_height = m_font->metrics.LineSpacing;
         descent     = m_font->descent();
      }

      if (!descent) { // Sanity check: In case descent is missing for some reason
         descent = m_font->descent();
      }
   }

#ifdef DBG_STREAM
   log.branch("#%d %d:%d - %d:%d, Area: %gx%g,%g:%gx%g, WordWidth: %d [%.20s]...[%.20s]",
      int(m_segments.size()), Start.index, int(Start.offset), Stop.index, int(Stop.offset), m_line.x, Y, Width,
      AlignWidth, line_height, m_word_width, printable(*m_stream, Start).c_str(),
      printable(*m_stream, Stop).c_str());
#endif

   if ((!m_segments.empty()) and (Start < m_segments.back().stop)) {
      // Patching: Segments should never overlap each other.  If the start of this segment is less than the end of
      // the previous segment, adjust the previous segment so that it stops at the beginning of our new segment.

      if (Start <= m_segments.back().start) {
         // If the start of the new segment retraces to an index that has already been configured,
         // then we have actually encountered a coding flaw and the caller should be investigated.

         log.warning("New segment at index %d overlaps previously registered segment starting at %d.",
            Start.index, m_segments.back().start.index);
         return;
      }
      else {
         DLAYOUT("New segment #%d start index is less than (%d < %d) the end of previous segment - will patch up.",
            m_segments.back().start.index, Start.index, m_segments.back().stop.index);
         m_segments.back().stop = Start;
      }
   }

   // The new segment can be a continuation of the previous if both are content-free.

   if ((allow_merge) and (!m_segments.empty()) and (m_segments.back().stop IS Start) and
       (m_segments.back().allow_merge)) {
      auto &segment = m_segments.back();

      if (line_height > segment.area.Height) {
         segment.area.Height = line_height;
         segment.descent = descent;
      }

      segment.area.Width  += Width;
      segment.align_width += AlignWidth;
      segment.stop      = Stop;
      segment.trim_stop = trim_stop;
   }
   else {
      m_segments.emplace_back(doc_segment {
         .start       = Start,
         .stop        = Stop,
         .trim_stop   = trim_stop,
         .area        = { m_line.x, Y, Width, line_height },
         .descent     = descent,
         .align_width = AlignWidth,
         .stream      = m_stream,
         .edit        = m_edit_mode,
         .allow_merge = allow_merge
      });
   }

   update_layout_metrics(Self, m_segments.size(), m_clips.size());
}

//********************************************************************************************************************
// Add a segment for a single byte code at position idx.  For use by non-graphical codes only (i.e. no graphics region).

void layout::new_code_segment()
{
   stream_char start(idx), stop(idx + 1);

#ifdef DBG_STREAM
   kt::Log log(__FUNCTION__);
   log.branch("#%d %d:0 [%s]", int(m_segments.size()), start.index, std::string(strCodes[int(m_stream[0][idx].code)]).c_str());
#endif

   if ((!m_segments.empty()) and (m_segments.back().stop IS start) and (m_segments.back().allow_merge)) {
      // We can extend the previous segment.

      m_segments.back().stop = stop;
   }
   else {
      m_segments.emplace_back(doc_segment {
         .start       = start,
         .stop        = stop,
         .trim_stop   = stop,
         .area        = { 0, 0, 0, 0 },
         .descent      = 0,
         .align_width = 0,
         .stream      = m_stream,
         .edit        = m_edit_mode,
         .allow_merge = true
      });
   }

   m_line.index = idx + 1;
}

//********************************************************************************************************************
// This function lays out the document so that it is ready to be drawn.  It calculates the position, pixel length and
// height of each line and rearranges any vectors that are present in the document.

static void layout_doc(extDocument *Self)
{
   kt::Log log(__FUNCTION__);

   if (!Self->UpdatingLayout) return;

   if (Self->Stream.data.empty()) return;

   // Initial height is 1 and not set to the viewport height because we want to accurately report the final height
   // of the page.

   #ifdef DBG_LAYOUT
      log.branch("Area: %gx%g --------------------------------", Self->VPWidth, Self->VPHeight);
   #endif

   padding margins { Self->LeftMargin, Self->TopMargin, Self->RightMargin, Self->BottomMargin };

   //Self->GlyphAdvanceCache.clear(); // Retain the glyph advance cache between passes since fonts survive the object's lifetime.
   Self->LayoutMetrics.reset();

   if ((Self->WidthCacheFontSize != Self->FontSize) or
       (Self->WidthCacheFontFace != Self->FontFace) or
       (Self->WidthCacheFontStyle != Self->FontStyle)) {
      Self->invalidate_text_width_cache();
      Self->WidthCacheFontFace = Self->FontFace;
      Self->WidthCacheFontStyle = Self->FontStyle;
      Self->WidthCacheFontSize = Self->FontSize;
   }

   layout l(Self, &Self->Stream, Self->Page, margins);
   bool repeat = true;
   while (repeat) {
      repeat = false;
      l.m_break_loop--;
      Self->LayoutMetrics.root_passes++;

      double page_width;

      if (Self->PageWidth.Value <= 0) {
         // No preferred page width; maximise the page width to the available viewing area
         page_width = Self->VPWidth;
      }
      else if (!Self->PageWidth.scaled()) page_width = Self->PageWidth.Value;
      else page_width = Self->PageWidth * Self->VPWidth;

      if (page_width < Self->MinPageWidth) page_width = Self->MinPageWidth;

      Self->SortSegments.clear();
      Self->PageProcessed = false;
      Self->Error = ERR::Okay;

      auto font = &glFonts[0];

      double page_height = 1;
      l = layout(Self, &Self->Stream, Self->Page, margins);
      bool vertical_repass = false;
      if (l.do_layout(&font, page_width, page_height, vertical_repass) != ERR::Okay) break;

      // If the resulting page width has increased beyond the available area, increase the MinPageWidth value to reduce
      // the number of passes required for the next time we do a layout.

      if ((page_width > Self->VPWidth) and (Self->MinPageWidth < page_width)) Self->MinPageWidth = page_width;

      Self->PageHeight = page_height;
      //if (Self->PageHeight < Self->AreaHeight) Self->PageHeight = Self->AreaHeight;
      Self->CalcWidth = page_width;
   }

   if (Self->Error IS ERR::Okay) Self->EditCells = l.m_ecells;
   else Self->EditCells.clear();

   if ((Self->Error IS ERR::Okay) and (!l.m_segments.empty())) Self->Segments = l.m_segments;
   else Self->Segments.clear();

   Self->UpdatingLayout = false;

   print_segments(Self);

   DLAYOUT("Layout metrics: root_passes=%d do_layout_calls=%d page_extend_restarts=%d page_extend_requests=%d vertical_repasses=%d table_wrap_restarts=%d row_repasses=%d cell_layouts=%d check_wordwrap_calls=%d wrap_through_clips_calls=%d new_segment_calls=%d segment_peak=%d clip_peak=%d",
      int(Self->LayoutMetrics.root_passes), int(Self->LayoutMetrics.do_layout_calls),
      int(Self->LayoutMetrics.page_extend_restarts), int(Self->LayoutMetrics.page_extend_requests),
      int(Self->LayoutMetrics.vertical_repasses), int(Self->LayoutMetrics.table_wrap_restarts),
      int(Self->LayoutMetrics.row_repasses), int(Self->LayoutMetrics.cell_layouts),
      int(Self->LayoutMetrics.check_wordwrap_calls), int(Self->LayoutMetrics.wrap_through_clips_calls),
      int(Self->LayoutMetrics.new_segment_calls), int(Self->LayoutMetrics.segment_count_peak),
      int(Self->LayoutMetrics.clip_count_peak));

   // If an error occurred during layout processing, unload the document and display an error dialog.  (NB: While it is
   // possible to display a document up to the point at which the error occurred, we want to maintain a strict approach
   // so that human error is considered excusable in document formatting).

   if (Self->Error != ERR::Okay) {
      unload_doc(Self, ULD::REDRAW);

      std::string msg = "A failure occurred during the layout of this document - it cannot be displayed.\n\nDetails: ";
      if (Self->Error IS ERR::Loop) msg.append("This page cannot be rendered correctly in its current form.");
      else msg.append(GetErrorMsg(Self->Error));

      error_dialog("Document Layout Error", msg);
   }
   else {
      acResize(Self->Page, Self->CalcWidth, Self->PageHeight, 0);

      if (l.gen_scene_init(Self->Page) IS ERR::Okay) {
         l.gen_scene_graph(Self->Page, l.m_segments);
      }

      for (auto &trigger : copy_triggers(Self, DRT::AFTER_LAYOUT)) {
         if (trigger.isScript()) {
            sc::Call(trigger, std::to_array<ScriptArg>({
               { "ViewWidth", Self->VPWidth }, { "ViewHeight", Self->VPHeight },
               { "PageWidth", Self->CalcWidth }, { "PageHeight", Self->PageHeight }
            }));
         }
         else if (trigger.isC()) {
            auto routine = (void (*)(APTR, extDocument *, int, int, int, int, APTR))trigger.Routine;
            kt::SwitchContext context(trigger.Context);
            routine(trigger.Context, Self, Self->VPWidth, Self->VPHeight, Self->CalcWidth, Self->PageHeight, trigger.Meta);
         }
      }
   }
}

//********************************************************************************************************************
// Calculate the position, pixel length and height of each element on the page.  Routine will loop if the size of the
// page is too small and requires expansion.  Individual table cells are treated as miniature pages, resulting in a
// recursive call.
//
// TODO: Consider prioritising the layout of table cells first, possibly using concurrent threads.
//
// Offset/End: Start and end points within the stream for layout processing.
// Width:      Minimum width of the page/section.  Can be increased if insufficient space is available.  Includes the
//             left and right margins in the resulting calculation.
// Height:     Minimum height of the page/section.  Will be increased to match the number of lines in the layout.
// Margins:    Margins within the page area.  These are inclusive to the resulting page width/height.  If in a cell,
//             margins reflect cell padding values.

ERR layout::do_layout(font_entry **Font, double &Width, double &Height, bool &VerticalRepass)
{
   kt::Log log(__FUNCTION__);
   Self->LayoutMetrics.do_layout_calls++;

   if ((m_stream->data.empty()) or (!Font) or (!Font[0])) {
      return log.traceWarning(ERR::NoData);
   }

   if (m_depth >= MAX_DEPTH) return log.traceWarning(ERR::Recursion);

   auto page_height = Height;
   m_page_width = Width;

   if (m_margins.left + m_margins.right > m_page_width) {
      m_page_width = m_margins.left + m_margins.right;
   }

   #ifdef DBG_LAYOUT
   log.branch("Dimensions: %gx%g (edge %g), LM %g RM %g TM %g BM %g",
      m_page_width, page_height, m_page_width - m_margins.right,
      m_margins.left, m_margins.right, m_margins.top, m_margins.bottom);
   #endif

   layout_checkpoint tablestate, rowstate, liststate;
   bc_table *table;
   table_layout_values table_values;
   double last_height;
   int edit_segment;
   bool check_wrap;

extend_page:
   if (m_page_width > WIDTH_LIMIT) {
      DLAYOUT("Restricting page width from %g to %d", m_page_width, WIDTH_LIMIT);
      m_page_width = WIDTH_LIMIT;
      if (m_break_loop > 4) m_break_loop = 4; // Very large page widths normally means that there's a parsing problem
   }

   if (Self->Error != ERR::Okay) {
      return Self->Error;
   }
   else if (m_break_loop <= 0) {
      Self->Error = ERR::Loop;
      return Self->Error;
   }
   m_break_loop--;

   reset();

   last_height  = page_height;
   table        = nullptr;
   edit_segment = 0;
   check_wrap   = false;  // true if a wordwrap or collision check is required

   m_left_margin    = m_margins.left;  // Retain the margin in an adjustable variable, in case we adjust the margin
   m_align_edge     = wrap_edge();
   m_cursor_x       = m_margins.left;
   m_cursor_y       = m_margins.top;
   m_line_seg_start = m_segments.size();
   m_font           = *Font;
   m_space_width    = m_font->space_width;
   m_line_count     = 0;

   m_word_index.reset();
   m_line.index.set(0);
   m_line.full_reset(m_margins.left);

   for (idx = 0; (idx < INDEX(m_stream->size())) and (Self->Error IS ERR::Okay); idx++) {
      if ((m_cursor_x >= MAX_PAGE_WIDTH) or (m_cursor_y >= MAX_PAGE_HEIGHT)) {
         log.warning("Invalid cursor position reached @ %gx%g", m_cursor_x, m_cursor_y);
         Self->Error = ERR::InvalidDimension;
         break;
      }

      if (m_line.index.index < idx) {
         // Some byte codes can force a segment definition to be defined now, e.g. because they might otherwise
         // mess up the region size.

         bool set_segment_now = false;
         if ((m_stream[0][idx].code IS SCODE::ADVANCE) or (m_stream[0][idx].code IS SCODE::TABLE_START)) {
            set_segment_now = true;
         }
         else if (m_stream[0][idx].code IS SCODE::INDEX_START) {
            auto &index = m_stream->lookup<bc_index>(idx);
            if (!index.visible) set_segment_now = true;
         }

         if (set_segment_now) {
            DLAYOUT("Setting line at code '%s', index %d, line.x: %g, m_word_width: %d",
               std::string(strCodes[int(m_stream[0][idx].code)]).c_str(), m_line.index.index, m_line.x, m_word_width);
            finish_segment();
            new_segment(m_line.index, stream_char(idx), m_cursor_y, m_cursor_x - m_line.x, m_align_edge - m_line.x);
            reset_broken_segment();
         }
      }

      // Any escape code for an inline element that forces a word-break will initiate a wrapping check.

      if (table) m_align_edge = wrap_edge();
      else switch (m_stream[0][idx].code) {
         case SCODE::TABLE_END: {
            auto &table = m_stream->lookup<bc_table>(idx);
            auto wrap_result = check_wordwrap(m_word_index.index, m_cursor_x, m_cursor_y, m_word_width, (m_line.height < 1) ? 1 : m_line.height, table.floating_x());
           if (wrap_result IS WRAP::EXTEND_PAGE) {
               DLAYOUT("Expanding page width on wordwrap request.");
               Self->LayoutMetrics.page_extend_restarts++;
               goto extend_page;
            }
            break;
         }

         case SCODE::ADVANCE: {
            auto wrap_result = check_wordwrap(m_word_index.index, m_cursor_x, m_cursor_y, m_word_width, (m_line.height < 1) ? 1 : m_line.height);
           if (wrap_result IS WRAP::EXTEND_PAGE) {
               DLAYOUT("Expanding page width on wordwrap request.");
               Self->LayoutMetrics.page_extend_restarts++;
               goto extend_page;
            }
            break;
         }

         default:
            m_align_edge = wrap_edge();
            break;
      }

      if (idx >= INDEX(m_stream->size())) break;

#ifdef DBG_LAYOUT_ESCAPE
      DLAYOUT("ESC_%s Indexes: %d-%d-%d, WordWidth: %d",
         BC_NAME(m_stream[0], idx).c_str(), m_line.index.index, idx, m_word_index.index, m_word_width);
#endif

      switch (m_stream[0][idx].code) {
         case SCODE::TEXT: {
            auto wrap_result = lay_text();
            if (wrap_result IS WRAP::EXTEND_PAGE) { // A word in the text string is too big for the available space.
               DLAYOUT("Expanding page width on wordwrap request.");
               Self->LayoutMetrics.page_extend_restarts++;
               goto extend_page;
            }
            else if (wrap_result IS WRAP::WRAPPED) { // A wrap occurred during text processing.
               // The presence of the line-break must be ignored, due to word-wrap having already made the new line for us
               auto &text = m_stream->lookup<bc_text>(idx);
               if (text.text[0] IS '\n') {
                  if (text.text.size() > 0) m_line.index.offset = 1;
               }
            }
            break;
         }

         case SCODE::ADVANCE: {
            auto adv = &m_stream->lookup<bc_advance>(idx);
            m_cursor_x += adv->x.px(*this);
            m_cursor_y += adv->y.px(*this);
            if (adv->x.value) reset_broken_segment();
            break;
         }

         case SCODE::FONT:        lay_font(); break;
         case SCODE::FONT_END:    lay_font_end(); break;
         case SCODE::INDEX_START: lay_index(); break;

         case SCODE::LINK: {
            auto &link = m_stream->lookup<bc_link>(idx);

            m_stack_font.push(&link.font);
            link.font.layout_font(*this);

            if (link.path.empty()) {
               // This 'invisible' viewport will be used to receive user input
               link.path.set(objVectorPath::create::global({
                  fl::Owner(m_viewport->UID),
                  fl::Name("link_vp"),
                  fl::Cursor(PTC::HAND)
               }));

               if (link.path->Scene->SurfaceID) {
                  link.path->subscribeInput(JTYPE::BUTTON|JTYPE::CROSSING, C_FUNCTION(link_callback));
               }
            }
            break;
         }

         case SCODE::LINK_END:
            m_stack_font.pop();
            if (!m_stack_font.empty()) m_font = m_stack_font.top()->layout_font(*this);
            break;

         case SCODE::PARAGRAPH_START: lay_paragraph(); break;
         case SCODE::PARAGRAPH_END:   lay_paragraph_end(); break;

         case SCODE::LIST_START:
            // This is the start of a list.  Each item in the list will be identified by SCODE::PARAGRAPH codes.  The
            // cursor position is advanced by the size of the item graphics element.

            liststate = capture_checkpoint();
            m_stack_list.push(&m_stream->lookup<bc_list>(idx));
            m_stack_list.top()->repass = false;
            break;

         case SCODE::LIST_END:
            if (lay_list_end()) restore_checkpoint(liststate);
            break;

         case SCODE::USE: {
            auto &use = m_stream->lookup<bc_use>(idx);
            if (!use.processed) {
               Self->SVG->parseSymbol(use.id.c_str(), m_viewport);
               use.processed = true;
            }
            break;
         }

         case SCODE::BUTTON: {
            auto &button = m_stream->lookup<bc_button>(idx);
            auto ww = lay_button(button);
            if (ww IS WRAP::EXTEND_PAGE) {
               Self->LayoutMetrics.page_extend_restarts++;
               goto extend_page;
            }
            break;
         }

         case SCODE::CHECKBOX: {
            auto &checkbox = m_stream->lookup<bc_checkbox>(idx);
            size_widget(checkbox, false);
            auto ww = place_widget(checkbox);
            if (ww IS WRAP::EXTEND_PAGE) {
               Self->LayoutMetrics.page_extend_restarts++;
               goto extend_page;
            }
            break;
         }

         case SCODE::COMBOBOX: {
            auto &combobox = m_stream->lookup<bc_combobox>(idx);
            size_widget(combobox, true);
            auto ww = place_widget(combobox);
            if (ww IS WRAP::EXTEND_PAGE) {
               Self->LayoutMetrics.page_extend_restarts++;
               goto extend_page;
            }
            break;
         }

         case SCODE::IMAGE: {
            auto &image = m_stream->lookup<bc_image>(idx);
            size_widget(image, false);
            auto ww = place_widget(image);
            if (ww IS WRAP::EXTEND_PAGE) {
               Self->LayoutMetrics.page_extend_restarts++;
               goto extend_page;
            }
            break;
         }

         case SCODE::INPUT: {
            auto &input = m_stream->lookup<bc_input>(idx);
            size_widget(input, true);
            auto ww = place_widget(input);
            if (ww IS WRAP::EXTEND_PAGE) {
               Self->LayoutMetrics.page_extend_restarts++;
               goto extend_page;
            }
            break;
         }

         case SCODE::TABLE_START: {
            // Table layout steps are as follows:
            //
            // 1. Copy prefixed/default widths and heights to all cells in the table.
            // 2. Calculate the size of each cell with respect to its content.  This can be left-to-right or
            //    top-to-bottom, it makes no difference.
            // 3. During the cell-layout process, keep track of the maximum width/height for the relevant
            //    row/column.  If either increases, make a second pass so that relevant cells are resized
            //    correctly.
            // 4. If the width of the rows is less than the requested table width (e.g. table width = 100%) then
            //    expand the cells to meet the requested width.
            // 5. Restart the page layout using the correct width and height settings for the cells.

            tablestate = capture_checkpoint();

            table = &m_stream->lookup<bc_table>(idx);
            table->reset_row_height = true; // All rows start with a height of min_height up until TABLE_END in the first pass
            table->compute_columns = 1;
            table->width = -1;
            table_values = resolve_table_layout(*this, *table);

            for (unsigned j=0; j < table->columns.size(); j++) table->columns[j].min_width = 0;

            if (table->viewport.empty()) {
               table->viewport.set(objVectorViewport::create::global({
                  fl::Name("table_viewport"),
                  fl::Owner(m_viewport->UID),
                  fl::X(0), fl::Y(0),
                  fl::Width(1), fl::Height(1)
               }));
            }

            if (table->path.empty()) {
               table->path.set(objVectorPath::create::global({
                  fl::Name("table_path"), fl::Owner(table->viewport->UID)
               }));

               if (!table->fill.empty()) table->path->set(FID_Fill, table->fill);

               if (!table->stroke.empty()) {
                  table->path->setFields(fl::Stroke(table->stroke), fl::StrokeWidth(table_values.stroke_width));
               }
            }

wrap_table_start:
            // Calculate starting table width, ensuring that the table meets the minimum width according to the cell
            // spacing and padding values.

            {
               double width;
               if (table->min_width.type IS DU::SCALED) {
                  width = (Width - m_cursor_x - m_margins.right) * table->min_width.value;
               }
               else width = table_values.min_width;

               if (width < 0) width = 0;

               {
                  double min = (table_values.stroke_width * 2) +
                     (table_values.cell_h_spacing * (std::ssize(table->columns)-1)) +
                     (table_values.cell_padding_width * table->columns.size());

                  if (table->collapsed) min -= table_values.cell_h_spacing * 2; // Thin tables do not have spacing on the left and right borders
                  if (width < min) width = min;
               }

               if (width > WIDTH_LIMIT - m_cursor_x - m_margins.right) {
                  log.traceWarning("Table width in excess of allowable limits.");
                  width = WIDTH_LIMIT - m_cursor_x - m_margins.right;
                  if (m_break_loop > 4) m_break_loop = 4;
               }

               if ((table->compute_columns) and (table->width >= width)) table->compute_columns = 0;

               table->width = width;
            }

wrap_table_end:
wrap_table_cell:
            m_break_loop--;
            table->cursor_x    = m_cursor_x;
            table->cursor_y    = m_cursor_y;
            table->x           = m_cursor_x;
            table->y           = m_cursor_y;
            table->row_index   = 0;
            table->total_clips = m_clips.size();
            table->height      = table_values.stroke_width;

            DLAYOUT("(i%d) Laying out table of %dx%d, coords %gx%g,%gx%g, page width %g.",
               idx, int(table->columns.size()), table->rows, table->x, table->y,
               table->width, table_values.min_height, Width);

            table->computeColumns();

            DLAYOUT("Checking for table collisions before layout (%gx%g).  reset_row_height: %d",
               table->x, table->y, table->reset_row_height);

            auto ww = check_wordwrap(idx, table->x, table->y, table->width, table->height, table->floating_x());
            if (ww IS WRAP::EXTEND_PAGE) {
               DLAYOUT("Expanding page width due to table size.");
               Self->LayoutMetrics.page_extend_restarts++;
               goto extend_page;
            }
            else if (ww IS WRAP::WRAPPED) {
               // The width of the table and positioning information needs to be recalculated in the event of a
               // table wrap.

               DLAYOUT("Restarting table calculation due to page wrap to position %gx%g.", m_cursor_x, m_cursor_y);
               table->compute_columns = 1;
               Self->LayoutMetrics.table_wrap_restarts++;
               goto wrap_table_start;
            }

            m_cursor_x = table->x; // Configure cursor location to help with the layout of rows and cells.
            m_cursor_y = table->y + table_values.stroke_width + table_values.cell_v_spacing;
            new_code_segment();
            break;
         }

         case SCODE::TABLE_END: {
            auto action = lay_table_end(*table, m_margins.top, m_margins.bottom, Height, Width);
            if (action != TE::NIL) {
               auto req_width = m_page_width;
               restore_checkpoint(tablestate);
               m_page_width = req_width;
               if (action IS TE::WRAP_TABLE) {
                  Self->LayoutMetrics.table_wrap_restarts++;
                  goto wrap_table_end;
               }
               else if (action IS TE::REPASS_ROW_HEIGHT) {
                  Self->LayoutMetrics.row_repasses++;
                  goto repass_row_height;
               }
               else if (action IS TE::EXTEND_PAGE) {
                  Self->LayoutMetrics.page_extend_restarts++;
                  goto extend_page;
               }
            }
            break;
         }

         case SCODE::ROW:
            m_row = &m_stream->lookup<bc_row>(idx);
            rowstate = capture_checkpoint();

            if (table->reset_row_height) m_row->row_height = m_row->min_height;

            if (!m_row->fill.empty()) {
               if (m_row->rect_fill.empty()) {
                  m_row->rect_fill.set(objVectorRectangle::create::global({
                     fl::Name("row_fill"),
                     fl::Owner(table->viewport->UID),
                     fl::Fill(m_row->fill)
                  }));
               }
            }

repass_row_height:
            m_row->vertical_repass = false;
            m_row->y = m_cursor_y;
            table->row_width = (table_values.stroke_width * 2) + table_values.cell_h_spacing;

            new_code_segment();
            break;

         case SCODE::ROW_END:
            lay_row_end(table);
            break;

         case SCODE::CELL: {
            switch(lay_cell(table)) {
               case CELL::ABORT:
                  goto exit;

               case CELL::WRAP_TABLE_CELL:
                  restore_checkpoint(tablestate);
                  Self->LayoutMetrics.table_wrap_restarts++;
                  goto wrap_table_cell;

               case CELL::REPASS_ROW_HEIGHT:
                  restore_checkpoint(rowstate);
                  Self->LayoutMetrics.row_repasses++;
                  goto repass_row_height;

               default:
                  break;
            }
            break;
         }

         default: break;
      }
   }

   // Check if the cursor + any remaining text requires closure

   if ((m_cursor_x + m_word_width > m_left_margin) or (m_word_index.valid())) {
      end_line(NL::NONE, stream_char(idx));
   }

exit:

   page_height = calc_page_height();

   if (page_height > MAX_PAGE_HEIGHT) {
      log.warning("Calculated page_height of %g is invalid.", page_height);
      Self->Error = ERR::InvalidDimension;
      return Self->Error;
   }

   // Force a second pass if the page height has increased and there are vectors in the page (the vectors may need
   // to know the page height - e.g. if there is a gradient filling the background).
   //
   // This requirement is also handled in SCODE::CELL, so we only perform it here if processing is occurring within the
   // root page area.

   if ((!m_depth) and (VerticalRepass) and (last_height < page_height)) {
      DLAYOUT("============================================================");
      DLAYOUT("SECOND PASS: Root page height increased from %g to %g", last_height, page_height);
      Self->LayoutMetrics.vertical_repasses++;
      Self->LayoutMetrics.page_extend_restarts++;
      goto extend_page;
   }

   *Font = m_font;

   if (m_page_width > Width) Width = m_page_width;
   if (page_height > Height) Height = page_height;

   return Self->Error;
}

//********************************************************************************************************************
// This function is called only when a paragraph or explicit line-break (\n) is encountered.

void layout::end_line(NL NewLine, stream_char Next, size_t ClipLimit)
{
   kt::Log log(__FUNCTION__);

   if ((!m_line.height) and (m_word_width)) {
      // If this is a one-word line, the line height will not have been defined yet
      m_line.height  = m_font->metrics.LineSpacing;
      m_line.descent = m_font->descent();
   }

   m_line.apply_word_height();

#ifdef DBG_LAYOUT
   log.branch("CursorX/Y: %g/%g, ParaEnd: %d, Line Height: %g, Span: %d:%d - %d:%d",
      m_cursor_x, m_cursor_y, m_paragraph_bottom, m_line.height,
      m_line.index.index, int(m_line.index.offset), Next.index, int(Next.offset));
#endif

   auto clip_count = clip_limit_count(ClipLimit);
   for (size_t i=0; i < clip_count; i++) {
      auto &clip = m_clips[i];
      if (clip.transparent) continue;
      if ((m_cursor_y + m_line.height >= clip.top) and (m_cursor_y < clip.bottom)) {
         if (m_cursor_x + m_word_width < clip.left) {
            if (clip.left < m_align_edge) m_align_edge = clip.left;
         }
      }
   }

   if (idx > m_line.index.index) {
      new_segment(m_line.index, stream_char(idx), m_cursor_y, m_cursor_x + m_word_width - m_line.x, m_align_edge - m_line.x);
   }

   if (NewLine != NL::NONE) {
      // Determine the new vertical position of the cursor.  This subroutine takes into account multiple line-breaks, so that
      // the overall amount of whitespace is no more than the biggest line-break specified in a line-break sequence.

      auto bottom_line = m_cursor_y + m_line.height;
      if (m_paragraph_bottom > bottom_line) bottom_line = m_paragraph_bottom;

      if (!m_line.height) {
         // The line is devoid of content, e.g. in the case of "<p>...<p>...</p></p>" the "</p></p>" is empty.
         // The m_cursor_y position will not be advanced in this case.
      }
      else {
         // Paragraph spacing is managed in the following paragraph, if any.  Otherwise content
         // is positioned flush against the bottom of the paragraph.

         auto advance_to = bottom_line;
         if (advance_to > m_cursor_y) m_cursor_y = advance_to;
      }
   }

   // Reset line management variables for a new line starting from the left margin.

   sanitise_line_height();

   m_line.full_reset(m_left_margin);
   m_line.index       = Next;
   m_cursor_x         = m_left_margin;
   m_line_seg_start   = m_segments.size();
   m_word_index       = m_line.index;
   m_kernchar         = 0;
   m_word_width       = 0;
   m_paragraph_bottom = 0;
}

//********************************************************************************************************************
// This function will check the need for word wrapping of an element marked by the area (x, y, width, height).  The
// (x, y) position will be updated if the element is wrapped.  If clipping boundaries are present on the page,
// horizontal advancement across the line may occur.  Some layout state variables are also updated if a wrap occurs,
// e.g. the cursor position.
//
// Wrapping can be checked even if there is no 'active word' because we need to be able to wrap empty lines (e.g.
// solo <br/> tags).

WRAP layout::check_wordwrap(stream_char Cursor, double &X, double &Y, double Width, double Height, bool Floating,
   size_t ClipLimit)
{
   kt::Log log(__FUNCTION__);
   Self->LayoutMetrics.check_wordwrap_calls++;

   if (m_break_loop <= 0) return WRAP::DO_NOTHING;
   if (Width < 1) Width = 1;

   if ((X > MAX_PAGE_WIDTH) or (Y > MAX_PAGE_HEIGHT) or (m_page_width > MAX_PAGE_WIDTH)) {
      log.warning("Invalid element position of %gx%g in page of %g", X, Y, m_page_width);
      Self->Error = ERR::InvalidDimension;
      return WRAP::DO_NOTHING;
   }

#ifdef DBG_WORDWRAP
   log.branch("Index: %d/%d, %s: %dx%d,%dx%d, LineHeight: %d, Cursor: %gx%g, PageWidth: %d, Edge: %d",
      idx, Cursor.index, type.c_str(), x, y, width, height, m_line.height, m_cursor_x, m_cursor_y, width, m_wrap_edge);
#endif

   auto result = WRAP::DO_NOTHING;
   int breakloop;
   for (breakloop = MAXLOOP; breakloop > 0; breakloop--) {
      m_align_edge = wrap_edge();

      auto clip_count = clip_limit_count(ClipLimit);
      if (clip_count > 0) {
         // If clips are registered then we need to check them for collisions.  Updates m_align_edge if necessary

         auto wrap_clip = WTC::DO_NOTHING;
         do {
            double adv_x = 0;
            wrap_clip = wrap_through_clips(X, Y, Width, Height, adv_x, ClipLimit);

            if (wrap_clip IS WTC::WRAP_OVER) {
               // Set the line segment up to the encountered boundary and continue checking the vector position against the
               // clipping boundaries.

               if (m_line.index < Cursor) {
                  if (!m_line.height) new_segment(m_line.index, Cursor, Y, X - m_line.x, X - m_line.x);
                  else new_segment(m_line.index, Cursor, Y, X - m_line.x, m_align_edge - m_line.x);
               }

               X = adv_x;
               m_line.index = Cursor;
               m_line.x = X;

               if (X + Width > wrap_edge()) wrap_clip = WTC::WRAP_LINE;
            }
         } while (wrap_clip IS WTC::WRAP_OVER);
      }

      if (X + Width <= wrap_edge()) break;

      if ((Floating) or (X IS m_left_margin) or (m_no_wrap)) {
         // Force an extension of the page width and recalculate from scratch.
         // NB: Floating vectors are permitted to wrap when colliding with other clip regions.  In all other cases a width increase is required.
         double min_width = X + Width + m_margins.right;
         if (min_width > m_page_width) {
            m_page_width = min_width;
            DWRAP("Forcing an extension of the page width to %g", min_width);
         }
         else m_page_width += 1;
         Self->LayoutMetrics.page_extend_requests++;
         return WRAP::EXTEND_PAGE;
      }

      if (!m_line.height) {
         m_line.height = 1;
         m_line.descent = 0;
      }

      // Set the line segment up to the cursor.  The line.index is updated so that this process only occurs
      // in the first iteration.

      if (m_line.index < Cursor) {
         new_segment(m_line.index, Cursor, Y, X - m_line.x, m_align_edge - m_line.x);
         m_line.index = Cursor;
      }

      // Reset the line management variables so that the next line starts at the left margin.

      sanitise_line_height();

      X = m_left_margin;
      if (m_stack_para.empty()) Y += m_line.height;
      else Y += m_stack_para.top()->line_height.px(*this);

      m_cursor_x = X;
      m_cursor_y = Y;
      m_kernchar = 0;
      m_line_seg_start = m_segments.size();

      m_line.reset(m_left_margin);

      result = WRAP::WRAPPED;

      // Typically we will loop only once after a wrap, but multiple loops may occur if there are clip regions
      // present and/or the word is very long.
   } // while()

   if (breakloop < 0) {
      log.traceWarning("Infinite loop detected.");
      Self->Error = ERR::Loop;
   }

   #ifdef DBG_WORDWRAP
      if (result IS WRAP::WRAPPED) log.msg("A wrap to Y coordinate %g has occurred.", m_cursor_y);
   #endif

   return result;
}

//********************************************************************************************************************
// Compare a given area against clip regions and move the x,y position when there's a collision.  There are three
// possible outcomes when making these checks:
//
// 1. There is no collision
// 2. A collision occurs and the word can be advanced to white space that is available past the obstacle.
// 3. A collision occurs and there is no further room available on this line (not handled by this routine).

WTC layout::wrap_through_clips(double X, double Y, double Width, double Height, double &AdvanceTo, size_t ClipLimit)
{
   Self->LayoutMetrics.wrap_through_clips_calls++;

   auto clip_count = clip_limit_count(ClipLimit);
   for (size_t i=0; i < clip_count; i++) {
      auto &clip = m_clips[i];
      if (clip.transparent) continue;
      if ((Y + Height < clip.top) or (Y >= clip.bottom)) continue; // Ignore clips above or below the line.
      if ((X >= clip.right) or (X + Width < clip.left)) continue; // Ignore clips to the left or right

      if ((clip.right > X) and (clip.left < m_align_edge)) m_align_edge = clip.left; // Clips can reduce the available space for alignment

      AdvanceTo = clip.right;
      return WTC::WRAP_OVER;
   }

   return WTC::DO_NOTHING;
}

//********************************************************************************************************************
// Calculate the page height, which is either going to be the coordinate of the bottom-most line, or one of the
// clipping regions if one of them extends further than the bottom-most line.

double layout::calc_page_height()
{
   kt::Log log(__FUNCTION__);

   if (m_segments.empty()) return 0;

   // Find the last segment to express a height, use it to determine the bottom of the page

   double page_height = 0;
   for (SEGINDEX last = m_segments.size() - 1; last >= 0; last--) {
      if (m_segments[last].area.Height > 0) {
         page_height = m_segments[last].area.Height + m_segments[last].area.Y;
         break;
      }
   }

   // Extend the height if a clipping region passes the last line of text.

   for (auto &clip : m_clips) {
      if (clip.transparent) continue;
      if (clip.bottom > page_height) page_height = clip.bottom;
   }

   page_height += m_margins.bottom;

   log.trace("Page Height: %g + %g -> %g, Bottom: %g",
      m_segments.back().area.Y, m_segments.back().area.Height, page_height, m_margins.bottom);

   return page_height;
}

//********************************************************************************************************************
// Fonts are shared in glFonts, note that multiple documents all have access to the cache.

font_entry * bc_font::layout_font(layout &Layout)
{
   kt::Log log(__FUNCTION__);

   {
      std::lock_guard lk(glFontsMutex);
      if ((font_index < std::ssize(glFonts)) and (font_index >= 0)) return &glFonts[font_index];
   }

   // Sanity check the face and point values

   if (face.empty()) face = Layout.Self->FontFace;

   if ((req_size.type IS DU::PIXEL) and (req_size.value < 3)) {
      req_size.value = Layout.Self->FontSize;
      if (req_size.value < 3) req_size = DUNIT(DEFAULT_FONTSIZE, DU::PIXEL);
   }

   pixel_size = req_size.px(Layout);

   // Check the cache for this font

   CSTRING resolved_face;
   if (fnt::ResolveFamilyName(face.c_str(), &resolved_face) IS ERR::Okay) {
      face.assign(resolved_face);
   }

   auto cache_key = font_cache_key { face, style, pixel_size };
   {
      std::lock_guard lk(glFontsMutex);
      if (auto it = glFontIndexCache.find(cache_key); it != glFontIndexCache.end()) {
         font_index = it->second;
         if ((font_index < std::ssize(glFonts)) and (font_index >= 0)) return &glFonts[font_index];
      }
   }

   APTR new_handle = nullptr;
   if (vec::GetFontHandle(face.c_str(), style.c_str(), 400, pixel_size, &new_handle) IS ERR::Okay) {
      std::lock_guard lk(glFontsMutex);

      if (auto it = glFontIndexCache.find(cache_key); it != glFontIndexCache.end()) {
         font_index = it->second;
      }
      else {
         log.branch("Index: %d, %s, %s, %d", int(std::ssize(glFonts)), face.c_str(), style.c_str(), pixel_size);

         font_index = std::ssize(glFonts);
         glFonts.emplace_back(new_handle, face, style, pixel_size);
         glFontIndexCache.try_emplace(cache_key, font_index);
      }

      if ((font_index < std::ssize(glFonts)) and (font_index >= 0)) return &glFonts[font_index];
   }
   else {
      std::lock_guard lk(glFontsMutex);
      if (!glFonts.empty()) {
         font_index = 0;
         glFontIndexCache.try_emplace(cache_key, font_index);
         return &glFonts[0];
      }
   }

   log.warning("Failed to create font %s:%d", face.c_str(), pixel_size);

   {
      std::lock_guard lk(glFontsMutex);
      if (!glFonts.empty()) {
         font_index = 0;
         glFontIndexCache.try_emplace(cache_key, font_index);
         return &glFonts[0];
      }
      else return nullptr;
   }
}


static const int MAXLOOP = 1000;

static const char glDefaultStyles[] =
"<template name=\"h1\"><p leading=\"1.5\" font-size=\"2em\" font-style=\"bold\"><inject/></p></>\n\
<template name=\"h2\"><p leading=\"1.25\" font-size=\"1.8em\" font-style=\"bold\"><inject/></p></>\n\
<template name=\"h3\"><p leading=\"1.25\" font-size=\"1.6em\" font-style=\"bold\"><inject/></p></>\n\
<template name=\"h4\"><p leading=\"1.25\" font-size=\"1.4em\"><inject/></p></>\n\
<template name=\"h5\"><p leading=\"1.0\"  font-size=\"1.2em\"><inject/></p></>\n\
<template name=\"h6\"><p leading=\"1.0\"  font-size=\"1em\"><inject/></p></>\n";

//********************************************************************************************************************

constexpr static double fast_hypot(double Width, double Height)
{
   if (Width > Height) std::swap(Width, Height);
   if ((Height / Width) <= 1.5) return 5.0 * (Width + Height) / 7.0; // Fast hypot calculation accurate to within 1% for specific use cases.
   else return std::sqrt((Width * Width) + (Height * Height));
}

//********************************************************************************************************************
// Extract all printable text between start and End.

static std::string stream_to_string(RSTREAM &Stream, stream_char Start, stream_char End)
{
   if (End < Start) std::swap(Start, End);

   std::ostringstream str;
   auto cs = Start;
   for (; (cs.index <= End.index) and (cs.index < INDEX(Stream.size())); cs.next_code()) {
      if (Stream[cs.index].code IS SCODE::TEXT) {
         auto &text = Stream.lookup<bc_text>(cs);
         if (cs.index < End.index) {
            str << text.text.substr(cs.offset, text.text.size() - cs.offset);
         }
         else str << text.text.substr(cs.offset, End.offset - cs.offset);
      }
   }
   return str.str();
}

//********************************************************************************************************************

static void apply_border_to_path(CB Border, std::vector<PathCommand> &Seq, FloatRect Area)
{
   if (Border IS CB::ALL) {
      Seq.push_back({ .Type = PE::Move, .X = Area.X, .Y = Area.Y });
      Seq.push_back({ .Type = PE::HLineRel, .X = Area.Width });
      Seq.push_back({ .Type = PE::VLineRel, .Y = Area.Height });
      Seq.push_back({ .Type = PE::HLineRel, .X = -Area.Width });
      Seq.push_back({ .Type = PE::ClosePath });
   }
   else {
      if ((Border & CB::LEFT) != CB::NIL) {
         Seq.push_back({ .Type = PE::Move, .X = Area.X, .Y = Area.Y });
         Seq.push_back({ .Type = PE::VLineRel, .Y = Area.Height });
         Seq.push_back({ .Type = PE::ClosePath });
      }

      if ((Border & CB::TOP) != CB::NIL) {
         Seq.push_back({ .Type = PE::Move, .X = Area.X, .Y = Area.Y });
         Seq.push_back({ .Type = PE::HLineRel, .X = Area.Width });
         Seq.push_back({ .Type = PE::ClosePath });
      }

      if ((Border & CB::RIGHT) != CB::NIL) {
         Seq.push_back({ .Type = PE::Move, .X = Area.X + Area.Width, .Y = Area.Y });
         Seq.push_back({ .Type = PE::VLineRel, .Y = Area.Height });
         Seq.push_back({ .Type = PE::ClosePath });
      }

      if ((Border & CB::BOTTOM) != CB::NIL) {
         Seq.push_back({ .Type = PE::Move, .X = Area.X, .Y = Area.Y + Area.Height });
         Seq.push_back({ .Type = PE::HLineRel, .X = Area.Width });
         Seq.push_back({ .Type = PE::ClosePath });
      }
   }
}

//********************************************************************************************************************
// Designed for reading unit values such as '50%' and '6px'.  The returned value is scaled to pixels.

static std::string_view read_unit(std::string_view Input, double &Output, bool &Scaled)
{
   auto value = Input;
   while ((not value.empty()) and (unsigned(value.front()) <= 0x20)) value.remove_prefix(1);

   Scaled = false;
   if (value.empty()) {
      Output = 0;
      return value;
   }

   size_t pos = 0;
   bool negative = false;
   if ((value[pos] IS '-') or (value[pos] IS '+')) {
      negative = value[pos] IS '-';
      pos++;
   }

   auto number_start = pos;
   while ((pos < value.size()) and (value[pos] >= '0') and (value[pos] <= '9')) pos++;

   bool has_digits = pos > number_start;
   if ((pos < value.size()) and (value[pos] IS '.')) {
      pos++;
      auto fraction_start = pos;
      while ((pos < value.size()) and (value[pos] >= '0') and (value[pos] <= '9')) pos++;
      has_digits = has_digits or (pos > fraction_start);
   }

   if (not has_digits) {
      Output = 0;
      return value.substr(number_start);
   }

   std::string number_text;
   number_text.reserve((negative ? 1 : 0) + (pos - number_start) + ((value[number_start] IS '.') ? 1 : 0));
   if (negative) number_text += '-';
   if (value[number_start] IS '.') number_text += '0';
   number_text.append(value.data() + number_start, pos - number_start);

   auto numeric_value = 0.0;
   auto [ptr, error] = std::from_chars(number_text.data(), number_text.data() + number_text.size(), numeric_value);
   if (error != std::errc()) {
      Output = 0;
      return value.substr(pos);
   }

   double multiplier = 1.0;
   static constexpr double dpi = 96.0;
   auto suffix = value.substr(pos);

   if (suffix.starts_with('%')) {
      Scaled = true;
      multiplier = 0.01;
      suffix.remove_prefix(1);
   }
   else if (suffix.starts_with("px")) suffix.remove_prefix(2); // Pixel.  This is the default type
   else if (suffix.starts_with("em")) { suffix.remove_prefix(2); multiplier = 12.0 * (4.0 / 3.0); } // Current font-size
   else if (suffix.starts_with("ex")) { suffix.remove_prefix(2); multiplier = 6.0 * (4.0 / 3.0); } // Current font-size, reduced to the height of the 'x' character.
   else if (suffix.starts_with("in")) { suffix.remove_prefix(2); multiplier = dpi; } // Inches
   else if (suffix.starts_with("cm")) { suffix.remove_prefix(2); multiplier = (1.0 / 2.56) * dpi; } // Centimetres
   else if (suffix.starts_with("mm")) { suffix.remove_prefix(2); multiplier = (1.0 / 20.56) * dpi; } // Millimetres
   else if (suffix.starts_with("pt")) { suffix.remove_prefix(2); multiplier = (4.0 / 3.0); } // Points.  A point is 4/3 of a pixel
   else if (suffix.starts_with("pc")) { suffix.remove_prefix(2); multiplier = (4.0 / 3.0) * 12.0; } // Pica.  1 Pica is equal to 12 Points

   Output = numeric_value * multiplier;
   return suffix;
}

//********************************************************************************************************************
// Checks if the file path is safe, i.e. does not refer to an absolute file location.

static int safe_file_path(extDocument *Self, std::string_view Path)
{
   if ((Self->Flags & DCF::UNRESTRICTED) != DCF::NIL) return true;





   return false;
}

//********************************************************************************************************************
// Process an XML tree by setting correct style information and then calling parse_tags().

static ERR insert_xml(extDocument *Self, RSTREAM *Stream, objXML *XML, const objXML::TAGS &Tag, INDEX TargetIndex,
   STYLE StyleFlags, IPF Options)
{
   kt::Log log(__FUNCTION__);

   if (TargetIndex < 0) TargetIndex = Stream->size();

   log.traceBranch("Index: %d, Flags: $%.2x, Tag: %s", TargetIndex, int(StyleFlags), Tag[0].Attribs[0].Name.c_str());

   if ((StyleFlags & STYLE::INHERIT_STYLE) != STYLE::NIL) { // Do nothing to change the style
      parser parse(Self, XML, Stream);
      parse.m_index = stream_char(TargetIndex);

      if (Stream->data.empty()) {
         parse.parse_tags(Tag, Options);
      }
      else {
         // Override the paragraph-content sanity check when inserting content in an existing document
         parse.m_paragraph_depth++;
         parse.parse_tags(Tag, Options);
         parse.m_paragraph_depth--;
      }
   }
   else {
      bc_font style;
      style.fill     = Self->FontFill;
      style.face     = Self->FontFace;
      style.req_size = DUNIT(Self->FontSize, DU::PIXEL);
      style.style    = Self->FontStyle;
      style.pixel_size = Self->FontSize;

      if ((StyleFlags & STYLE::RESET_STYLE) != STYLE::NIL) {
         // Do not search for the most recent font style (force a reset)
      }
      else {
         for (auto i = TargetIndex - 1; i >= 0; i--) {
            if (Stream[0][i].code IS SCODE::FONT) {
               style = Stream->lookup<bc_font>(i);
               break;
            }
            else if (Stream[0][i].code IS SCODE::PARAGRAPH_START) {
               style = Stream->lookup<bc_paragraph>(i).font;
               break;
            }
            else if (Stream[0][i].code IS SCODE::LINK) {
               style = Stream->lookup<bc_link>(i).font;
               break;
            }
         }
      }

      parser parse(Self, XML, Stream);
      parse.m_index = stream_char(TargetIndex);

      if (Stream->data.empty()) {
         parse.parse_tags_with_style(Tag, style, Options);
      }
      else {
         parse.m_paragraph_depth++;
         parse.parse_tags_with_style(Tag, style, Options);
         parse.m_paragraph_depth--;
      }
   }

   // Check that the FocusIndex is valid (there's a slim possibility that it may not be if AC::Focus has been
   // incorrectly used).

   if (Self->FocusIndex >= std::ssize(Self->Tabs)) Self->FocusIndex = -1;

   if (Self->Error IS ERR::Okay) return ERR::Okay;
   else return Self->Error;
}

//********************************************************************************************************************
// This is the principal function for adding/inserting text into the document stream, whether that be in the parse
// phase or from user editing.
//
// Preformat must be set to true if all consecutive whitespace characters in Text are to be inserted.

static constexpr size_t MAX_BC_TEXT_BYTES = 0xffff;

static inline size_t utf8_split_boundary(std::string_view Text, size_t Limit)
{
   if (Text.size() <= Limit) return Text.size();
   if (!Limit) return 0;

   auto split = Limit;
   while ((split > 0) and ((uint8_t(Text[split]) & 0xc0) IS 0x80)) split--;
   if (!split) return Limit;
   return split;
}

static void emit_bc_text(RSTREAM *Stream, stream_char &Index, std::string_view Text, bool Formatted)
{
   if (Text.empty()) return;

   for (size_t offset = 0; offset < Text.size(); ) {
      auto split = utf8_split_boundary(Text.substr(offset), MAX_BC_TEXT_BYTES);
      if (!split) split = std::min(MAX_BC_TEXT_BYTES, Text.size() - offset);

      bc_text et(Text.substr(offset, split), Formatted);
      Stream->emplace<bc_text>(Index, et);
      offset += split;
   }
}

static ERR insert_text(extDocument *Self, RSTREAM *Stream, stream_char &Index, const std::string_view Text, bool Preformat)
{
   // Check if there is content to be processed

   if ((!Preformat) and (Self->NoWhitespace)) {
      unsigned i;
      for (i=0; i < Text.size(); i++) if (unsigned(Text[i]) > 0x20) break;
      if (i IS Text.size()) return ERR::Okay;
   }

   if (Preformat) {
      emit_bc_text(Stream, Index, Text, true);
   }
   else {
      std::string normalised_text;
      normalised_text.reserve(Text.size());
      auto ws = Self->NoWhitespace; // Retrieve previous whitespace state
      for (unsigned i=0; i < Text.size(); ) {
         if (unsigned(Text[i]) <= 0x20) { // Whitespace encountered
            for (++i; (i < Text.size()) and (unsigned(Text[i]) <= 0x20); i++);
            if (!ws) normalised_text += ' ';
            ws = true;
         }
         else {
            normalised_text += Text[i++];
            ws = false;
         }
      }
      Self->NoWhitespace = ws;
      emit_bc_text(Stream, Index, normalised_text, false);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR load_doc(extDocument *Self, std::string Path, bool Unload, ULD UnloadFlags)
{
   kt::Log log(__FUNCTION__);

   log.branch("Loading file '%s', page '%s'", Path.c_str(), Self->PageName.c_str());

   if (Unload) unload_doc(Self, UnloadFlags);

   process_parameters(Self, Path);

   // Generate a path without parameter values.

   auto i = Path.find_first_of("&#?");
   if (i != std::string::npos) Path.erase(i);

   if (AnalysePath(Path.c_str(), nullptr) IS ERR::Okay) {
      auto task = CurrentTask();
      task->setPath(Path);

      auto xml = objXML::create {
         fl::Flags(XMF::INCLUDE_WHITESPACE|XMF::PARSE_HTML|XMF::STRIP_HEADERS|XMF::WELL_FORMED),
         fl::Path(Path),
         fl::ReadOnly(true)
      };

      if (xml.ok()) {
         #ifndef RETAIN_LOG_LEVEL
         kt::LogLevel level(3);
         #endif
         parser parse(Self, &Self->Stream);

         if (Self->PretextXML) {
            parse.process_page(Self->PretextXML);
         }

         parse.process_page(*xml);

         auto process_error = Self->Error;

         if ((Self->initialised()) and (process_error IS ERR::Okay)) {
            Self->UpdatingLayout = true;
            redraw(Self, true);
         }

         #ifdef DBG_STREAM
            print_stream(Self->Stream);
         #endif
         if (process_error != ERR::Okay) return process_error;
         else return Self->Error;
      }
      else {
         error_dialog("Document Load Error", std::string("Failed to load document file '") + Path + "'");
         return log.warning(ERR::OpenFile);
      }
   }
   else return log.warning(ERR::FileNotFound);
}

//********************************************************************************************************************
// This function removes all allocations that were made in displaying the current page, and resets a number of
// variables that they are at the default settings for the next page.
//
// Set ULD::TERMINATE if the document object is being destroyed.
//
// The PageName is not freed because the desired page must not be dropped during refresh of manually loaded XML for
// example.

static ERR unload_doc(extDocument *Self, ULD Flags)
{
   kt::Log log(__FUNCTION__);

   log.branch("Flags: $%.2x", int(Flags));

   #ifdef DBG_STREAM
      print_stream(Self->Stream);
   #endif

   Self->Highlight = glHighlight;

   Self->CursorStroke   = "rgb(102 102 204 / 1)";
   Self->FontFill       = "rgb(0 0 0)";
   Self->LinkFill       = "rgb(0 0 255)";
   Self->LinkSelectFill = "rgb(255 0 0)";
   Self->Background     = "rgb(255 255 255 / 1)";

   Self->LeftMargin    = 10;
   Self->RightMargin   = 10;
   Self->TopMargin     = 10;
   Self->BottomMargin  = 10;
   Self->XPosition     = 0;
   Self->YPosition     = 0;
   //Self->ScrollVisible = false;
   Self->PageHeight    = 0;
   Self->Invisible     = 0;
   Self->PageWidth     = 0;
   Self->CalcWidth     = 0;
   Self->MinPageWidth  = MIN_PAGE_WIDTH;
   Self->DefaultScript = nullptr;
   Self->FocusIndex    = -1;
   Self->PageProcessed = false;
   Self->RefreshTemplates = true;
   Self->MouseOverSegment = -1;
   Self->ActiveEditCellID = 0;
   Self->ActiveEditDef    = nullptr;
   Self->SelectIndex.reset();
   Self->CursorIndex.reset();

   if (Self->ActiveEditDef) deactivate_edit(Self, false);

   Self->Links.clear();

   Self->FontFace  = DEFAULT_FONTFACE;
   Self->FontSize  = DEFAULT_FONTSIZE;
   Self->FontStyle = DEFAULT_FONTSTYLE;
   Self->PageTag   = nullptr;

   Self->EditCells.clear();
   Self->Stream.clear();
   Self->SortSegments.clear();
   Self->Segments.clear();
   Self->Params.clear();
   Self->MouseOverChain.clear();
   Self->Tabs.clear();

   unsubscribe_script_listeners(Self);

   for (auto &t : Self->Triggers) t.clear();

   if ((Flags & ULD::TERMINATE) != ULD::NIL) Self->Vars.clear();

   if (Self->SVG)         { FreeResource(Self->SVG); Self->SVG = nullptr; }
   if (Self->Keywords)    { FreeResource(Self->Keywords); Self->Keywords = nullptr; }
   if (Self->Author)      { FreeResource(Self->Author); Self->Author = nullptr; }
   if (Self->Copyright)   { FreeResource(Self->Copyright); Self->Copyright = nullptr; }
   if (Self->Description) { FreeResource(Self->Description); Self->Description = nullptr; }
   if (Self->Title)       { FreeResource(Self->Title); Self->Title = nullptr; }

   // Free templates only if they have been modified (no longer at the default settings).

   if (Self->Templates) {
      if (Self->TemplatesModified != Self->Templates->Modified) {
         FreeResource(Self->Templates);
         Self->Templates = nullptr;
      }
   }

   // Remove all page related resources

   if (!Self->Resources.empty()) {
      kt::Log log(__FUNCTION__);
      log.branch("Freeing page-allocated resources.");

      for (auto it = Self->Resources.begin(); it != Self->Resources.end(); ) {
         if ((ULD::TERMINATE) != ULD::NIL) it->terminate = true;

         if ((it->type IS RTD::PERSISTENT_SCRIPT) or (it->type IS RTD::PERSISTENT_OBJECT)) {
            // Persistent objects and scripts will survive refreshes
            if ((Flags & ULD::REFRESH) != ULD::NIL);
            else { it = Self->Resources.erase(it); continue; }
         }
         else { it = Self->Resources.erase(it); continue; }

         it++;
      }
   }

   if (!Self->Templates) {
      if (!(Self->Templates = objXML::create::local(fl::Name("xmlTemplates"), fl::Statement(glDefaultStyles),
         fl::Flags(XMF::PARSE_HTML|XMF::STRIP_HEADERS)))) return ERR::CreateObject;

      Self->TemplatesModified = Self->Templates->Modified;
      Self->RefreshTemplates = true;
   }

   if (Self->Page) {
      Self->Page->setMask(nullptr); // Reset the clipping mask if it was defined by <body>

      kt::vector<ChildEntry> list;
      if (ListChildren(Self->Page->UID, &list) IS ERR::Okay) {
         for (auto it=list.rbegin(); it != list.rend(); it++) FreeResource(it->ObjectID);
      }
   }

   if ((Self->View) and (Self->Page)) {
      // Client generated objects can appear in the View if <svg placement="background"/> was used.
      kt::vector<ChildEntry> list;
      if (ListChildren(Self->View->UID, &list) IS ERR::Okay) {
         for (auto child=list.rbegin(); child != list.rend(); child++) {
            if (child->ObjectID != Self->Page->UID) FreeResource(child->ObjectID);
         }
      }
   }

   for (auto it=Self->UIObjects.rbegin(); it != Self->UIObjects.rend(); it++) {
      FreeResource(*it);
   }
   Self->UIObjects.clear();

   if (Self->Page) acMoveToPoint(Self->Page, 0, 0, 0, MTF::X|MTF::Y);

   Self->NoWhitespace   = true;
   Self->UpdatingLayout = true;

   if ((Flags & ULD::REDRAW) != ULD::NIL) {
      Self->Viewport->draw();
   }

   return ERR::Okay;
}

//********************************************************************************************************************

#if 0
static int get_line_from_index(extDocument *Self, INDEX index)
{
   int line;
   for (line=1; line < Self->SegCount; line++) {
      if (index < Self->Segments[line].index) {
         return line-1;
      }
   }
   return 0;
}
#endif

//********************************************************************************************************************
//Checks if an object reference is a valid member of the document.

static bool valid_objectid(extDocument *Self, OBJECTID ObjectID)
{
   if ((Self->Flags & DCF::UNRESTRICTED) != DCF::NIL) return true;

   while (ObjectID) {
      ObjectID = GetOwnerID(ObjectID);
      if (ObjectID IS Self->UID) return true;
   }
   return false;
}

//********************************************************************************************************************

static int getutf8(CSTRING Value, int *Unicode)
{
   int i, len, code;

   if ((*Value & 0x80) != 0x80) {
      if (Unicode) *Unicode = *Value;
      return 1;
   }
   else if ((*Value & 0xe0) IS 0xc0) {
      len  = 2;
      code = *Value & 0x1f;
   }
   else if ((*Value & 0xf0) IS 0xe0) {
      len  = 3;
      code = *Value & 0x0f;
   }
   else if ((*Value & 0xf8) IS 0xf0) {
      len  = 4;
      code = *Value & 0x07;
   }
   else if ((*Value & 0xfc) IS 0xf8) {
      len  = 5;
      code = *Value & 0x03;
   }
   else if ((*Value & 0xfc) IS 0xfc) {
      len  = 6;
      code = *Value & 0x01;
   }
   else {
      // Unprintable character
      if (Unicode) *Unicode = 0;
      return 1;
   }

   for (i=1; i < len; ++i) {
      if ((Value[i] & 0xc0) != 0x80) code = -1;
      code <<= 6;
      code |= Value[i] & 0x3f;
   }

   if (code IS -1) {
      if (Unicode) *Unicode = 0;
      return 1;
   }
   else {
      if (Unicode) *Unicode = code;
      return len;
   }
}

//********************************************************************************************************************
// Build the token list for a bc_text from its text string.  Tokens represent word, whitespace, and newline boundaries.
// This avoids repeated byte-by-byte scanning during layout restarts.

void bc_text::tokenise()
{
   tokens.clear();
   tokens_dirty = false;

   auto &str = text;
   if (str.empty()) return;

   // Reserve a reasonable estimate to reduce allocations (average ~5 chars per token)

   tokens.reserve(str.size() / 5 + 1);

   for (unsigned i = 0; i < str.size(); ) {
      if (str[i] IS '\n') {
         tokens.push_back({ uint16_t(i), uint16_t(i + 1), -1, 0, text_token_kind::NEWLINE });
         i++;
      }
      else if (unsigned(str[i]) <= 0x20) {
         auto start = i;
         i++;
         tokens.push_back({ uint16_t(start), uint16_t(i), -1, 0, text_token_kind::WHITESPACE });
      }
      else {
         auto start = i;
         while ((i < str.size()) and (unsigned(str[i]) > 0x20) and (str[i] != '\n')) i++;
         tokens.push_back({ uint16_t(start), uint16_t(i), -1, 0, text_token_kind::WORD });
      }
   }

   widths_dirty = true;
}

//********************************************************************************************************************
// Find the nearest font style that will represent Char

static bc_font * find_style(RSTREAM &Stream, stream_char &Char)
{
   bc_font *style = nullptr;

   for (INDEX fi = Char.index; fi >= 0; fi--) {
      if (Stream[fi].code IS SCODE::FONT) {
         style = &Stream.lookup<bc_font>(fi);
         break;
      }
      else if (Stream[fi].code IS SCODE::PARAGRAPH_START) {
         style = &Stream.lookup<bc_paragraph>(fi).font;
         break;
      }
      else if (Stream[fi].code IS SCODE::LINK) {
         style = &Stream.lookup<bc_link>(fi).font;
         break;
      }
   }

   return style;
}

//********************************************************************************************************************
// For a given line segment, convert a horizontal coordinate to the corresponding character index and its coordinate.
/*
static ERR resolve_font_pos(doc_segment &Segment, double X, double &CharX, stream_char &Char)
{
   kt::Log log(__FUNCTION__);

   bc_font *style = find_style(Segment.stream[0], Char);
   auto font = style ? style->get_font() : glFonts[0].font;
   if (!font) return ERR::Search;

   for (INDEX i = Segment.start.index; i < Segment.stop.index; i++) {
      if (Segment.stream[0][i].code IS SCODE::TEXT) {
         auto &str = Segment.stream->lookup<bc_text>(i).text;
         int offset, cx;
         if (!fntConvertCoords(font, str.c_str(), X - Segment.area.X, 0, NULL, NULL, NULL, &offset, &cx)) {
            CharX = cx;
            Char.set(Segment.start.index, offset);
            return ERR::Okay;
         }
         else break;
      }
   }

  log.trace("Failed to convert coordinate %d to a font-relative cursor position.", X);
   return ERR::Failed;
}
*/
//********************************************************************************************************************
// For a given character in the stream, find its representative line segment.

static SEGINDEX find_segment(std::vector<doc_segment> &Segments, stream_char Char, bool InclusiveStop)
{
   if (InclusiveStop) {
      for (SEGINDEX segment=0; segment < SEGINDEX(Segments.size()); segment++) {
         if ((Char >= Segments[segment].start) and (Char <= Segments[segment].stop)) {
            if ((Char IS Segments[segment].stop) and (Char.get_prev_char(Segments[segment].stream[0]) IS '\n'));
            else return segment;
         }
      }
   }
   else {
      for (SEGINDEX segment=0; segment < SEGINDEX(Segments.size()); segment++) {
         if ((Char >= Segments[segment].start) and (Char < Segments[segment].stop)) {
            return segment;
         }
      }
   }

   return -1;
}

//********************************************************************************************************************
// scheme://domain.com/path?param1=value&param2=value#fragment:bookmark

static void process_parameters(extDocument *Self, const std::string_view String)
{
   kt::Log log(__FUNCTION__);

   log.branch();

   Self->Params.clear();
   Self->PageName.clear();
   Self->Bookmark.clear();

   std::string arg, value;
   arg.reserve(64);
   value.reserve(0xffff);

   bool pagename_processed = false;
   for (unsigned pos=0; pos < String.size(); pos++) {
      if ((String[pos] IS '#') and (!pagename_processed)) {
         // Reference is '#fragment:bookmark' where 'fragment' refers to a page in the loaded XML file and 'bookmark'
         // is an optional bookmark reference within that page.

         pagename_processed = true;

         if (auto ind = String.find(":", pos+1); ind != std::string::npos) {
            ind -= pos + 1;
            Self->PageName.assign(String, pos + 1);
            if (Self->PageName[ind] IS ':') { // Check for bookmark separator
               Self->Bookmark.assign(Self->PageName, ind + 1);
               if (auto query = Self->Bookmark.find('?'); query != std::string::npos) {
                  Self->Bookmark.resize(query);
               }
               Self->PageName.resize(ind);
            }
         }
         else Self->PageName.assign(String, pos + 1);

         break;
      }
      else if (String[pos] IS '?') {
         // Arguments follow, separated by & characters for separation
         // Please note that it is okay to set zero-length parameter values

         pos++;

         auto uri_char = [&](std::string &Output) {
            if ((String[pos] IS '%') and (pos + 2 < String.size()) and
                 (((String[pos+1] >= '0') and (String[pos+1] <= '9')) or
                  ((String[pos+1] >= 'A') and (String[pos+1] <= 'F')) or
                  ((String[pos+1] >= 'a') and (String[pos+1] <= 'f'))) and
                (((String[pos+2] >= '0') and (String[pos+2] <= '9')) or
                 ((String[pos+2] >= 'A') and (String[pos+2] <= 'F')) or
                 ((String[pos+2] >= 'a') and (String[pos+2] <= 'f')))) {

               int num;
               auto [ v, error ] = std::from_chars(String.data() + pos + 1, String.data() + pos + 3, num, 16);
               if (error IS std::errc()) Output += num;
               pos += 3;
            }
            else Output += String[pos++];
         };

         while (pos < String.size()) {
            arg.clear();

            // Extract the parameter name

            while ((pos < String.size()) and (String[pos] != '#') and (String[pos] != '&') and (String[pos] != ';') and (String[pos] != '=')) {
               uri_char(arg);
            }

            if ((pos < String.size()) and (String[pos] IS '=')) { // Extract the parameter value
               value.clear();
               pos++;
               while ((pos < String.size()) and (String[pos] != '#') and (String[pos] != '&') and (String[pos] != ';')) {
                  uri_char(value);
               }
               Self->Params.emplace(arg, value);
            }
            else Self->Params.emplace(arg, "1");

            while ((pos < String.size()) and (String[pos]) and (String[pos] != '#') and (String[pos] != '&') and (String[pos] != ';')) pos++;
            if (pos >= String.size()) break;
            if ((String[pos] != '&') and (String[pos] != ';')) break;
            pos++;
         }
      }
   }

   log.msg("Reset page name to '%s', bookmark '%s'", Self->PageName.c_str(), Self->Bookmark.c_str());
}

//********************************************************************************************************************
// Resolves function references.
// E.g. "script.function(Args...)"; "function(Args...)"; "function()", "function", "script.function"

static ERR extract_script(extDocument *Self, std::string_view Link, objScript **Script, std::string &Function, std::string &Args)
{
   kt::Log log(__FUNCTION__);

   if (Script) {
      if (!(*Script = Self->DefaultScript)) {
         if (!(*Script = Self->ClientScript)) {
            log.warning("Cannot call function '%.*s', no default script in document.", int(Link.size()), Link.data());
            return ERR::Search;
         }
      }
   }

   auto pos = std::string_view::npos;
   auto dot = Link.find('.');
   auto open_bracket = Link.find('(');

   if (dot != std::string_view::npos) { // A script name is referenced
      pos = dot + 1;
      if (Script) {
         auto script_name = Link.substr(0, dot);
         auto script_name_text = std::string(script_name);
         OBJECTID id;
         if (FindObject(script_name_text.c_str(), CLASSID::SCRIPT, FOF::NIL, &id) IS ERR::Okay) {
            // Security checks
            *Script = (objScript *)GetObjectPtr(id);
            if ((Script[0]->Owner != Self) and ((Self->Flags & DCF::UNRESTRICTED) IS DCF::NIL)) {
               log.warning("Script '%s' does not belong to this document.  Request ignored due to security restrictions.", script_name_text.c_str());
               return ERR::NoPermission;
            }
         }
         else {
            log.warning("Unable to find '%s'", script_name_text.c_str());
            return ERR::Search;
         }
      }
   }
   else pos = 0;

   if ((dot != std::string_view::npos) and (open_bracket != std::string_view::npos) and (open_bracket < dot)) {
      log.warning("Malformed function reference: %.*s", int(Link.size()), Link.data());
      return ERR::InvalidData;
   }

   if (open_bracket != std::string_view::npos) {
      Function.assign(Link.substr(pos, open_bracket - pos));
      if (auto end_bracket = Link.find_last_of(')'); end_bracket != std::string_view::npos) {
         Args.assign(Link.substr(open_bracket + 1, end_bracket - open_bracket - 1));
      }
      else log.warning("Malformed function args: %.*s", int(Link.size()), Link.data());
   }
   else Function.assign(Link.substr(pos));

   return ERR::Okay;
}

//********************************************************************************************************************

void ui_link::exec(extDocument *Self)
{
   objScript *script;
   CLASSID class_id, subclass_id;

   kt::Log log(__FUNCTION__);

   log.branch();

   Self->Processing++;

   if ((Self->EventMask & DEF::LINK_ACTIVATED) != DEF::NIL) {
      KEYVALUE params;

      if (origin.type IS LINK::FUNCTION) {
         std::string function_name, args;
         if (extract_script(Self, origin.ref, nullptr, function_name, args) IS ERR::Okay) {
            params.emplace("on-click", function_name);
         }
      }
      else if (origin.type IS LINK::HREF) {
         params.emplace("href", origin.ref);
      }

      for (const auto & [key, value] : origin.args) {
         auto ksv = std::string_view(key);
         if (ksv.starts_with('@') or ksv.starts_with('$')) ksv.remove_prefix(1);
         params.emplace(ksv, value);
      }

      ERR result = report_event(Self, DEF::LINK_ACTIVATED, &origin, &params);
      if (result IS ERR::Skip) goto end;
   }

   if (origin.type IS LINK::FUNCTION) { // function is in the format 'function()' or 'script.function()'
      std::string function_name, fargs;
      if (extract_script(Self, origin.ref, &script, function_name, fargs) IS ERR::Okay) {
         std::vector<ScriptArg> sa;

         if (!origin.args.empty()) {
            for (const auto& [key, value] : origin.args) {
               if (key.starts_with('_')) { // Global variable setting
                  acSetKey(script, key.c_str()+1, value.c_str());
               }
               else sa.emplace_back("", value.data());
            }
         }

         script->exec(function_name.c_str(), sa.data(), sa.size());
      }
   }
   else if (origin.type IS LINK::HREF) {
      if (origin.ref[0] IS ':') {
         Self->Bookmark = origin.ref.substr(1);
         show_bookmark(Self, Self->Bookmark);
      }
      else {
         if ((origin.ref[0] IS '#') or (origin.ref[0] IS '?')) {
            log.trace("Switching to page '%s'", origin.ref.c_str());

            if (!Self->Path.empty()) {
               int end;
               for (end=0; Self->Path[end]; end++) {
                  if ((Self->Path[end] IS '&') or (Self->Path[end] IS '#') or (Self->Path[end] IS '?')) break;
               }
               auto path = std::string(Self->Path, end) + origin.ref;
               Self->set(FID_Path, path);
            }
            else Self->set(FID_Path, origin.ref);

            if (!Self->Bookmark.empty()) show_bookmark(Self, Self->Bookmark);
         }
         else {
            log.trace("Link is a file reference.");

            std::string path;

            if (!Self->Path.empty()) {
               auto j = origin.ref.find_first_of("/\\:");
               if ((j IS std::string::npos) or (origin.ref[j] != ':')) { // Path is relative
                  auto end = Self->Path.find_first_of("&#?");
                  if (end IS std::string::npos) {
                     path.assign(Self->Path, 0, Self->Path.find_last_of("/\\") + 1);
                  }
                  else path.assign(Self->Path, 0, Self->Path.find_last_of("/\\", end) + 1);
               }
            }

            auto lk = path + origin.ref;
            auto end = lk.find_first_of("?#&");
            auto identify_path = std::string_view(lk);
            if (end != std::string::npos) identify_path = identify_path.substr(0, end);

            auto identify_text = std::string(identify_path);
            if (IdentifyFile(identify_text.c_str(), CLASSID::NIL, &class_id, &subclass_id) IS ERR::Okay) {
               if (class_id IS CLASSID::DOCUMENT) {
                  Self->set(FID_Path, lk);

                  if (!Self->Bookmark.empty()) show_bookmark(Self, Self->Bookmark);
                  else log.msg("No bookmark was preset.");
               }
            }
            else {
               auto msg = std::string("It is not possible to follow this link because the type of file is not recognised.  The referenced link is:\n\n") + lk;
               error_dialog("Action Cancelled", msg);
            }
         }
      }
   }

end:
   Self->Processing--;
}

//********************************************************************************************************************

static void show_bookmark(extDocument *Self, std::string_view Bookmark)
{
   kt::Log log(__FUNCTION__);

   log.branch("%.*s", int(Bookmark.size()), Bookmark.data());

   // Find the indexes for the bookmark name

   auto bookmark_text = std::string(Bookmark);
   int start, end;
   if (Self->findIndex(bookmark_text.c_str(), &start, &end) IS ERR::Okay) {
      // Get the vertical position of the index and scroll to it

      auto &esc_index = Self->Stream.lookup<bc_index>(start);

      Self->XPosition = 0;
      Self->YPosition = -(esc_index.y - 4);

      if (-Self->YPosition > Self->PageHeight - Self->VPHeight) {
         Self->YPosition = -(Self->PageHeight - Self->VPHeight);
      }

      if (Self->YPosition > 0) Self->YPosition = 0;

      acMoveToPoint(Self->Page, 0, Self->YPosition, 0, MTF::X|MTF::Y);
   }
   else log.warning("Failed to find bookmark '%s'", bookmark_text.c_str());
}

//********************************************************************************************************************
// Generic function for reporting events that relate to entities.

static ERR report_event(extDocument *Self, DEF Event, entity *Entity, KEYVALUE *EventData)
{
   kt::Log log(__FUNCTION__);
   ERR result = ERR::Okay;

   if ((Event & Self->EventMask) != DEF::NIL) {
      auto entity_id = Entity ? Entity->uid : 0;
      log.traceBranch("Event $%x -> Entity %d", int(Event), entity_id);

      if (Self->EventCallback.isC()) {
         auto routine = (ERR (*)(extDocument *, DEF, KEYVALUE *, entity *, APTR))Self->EventCallback.Routine;
         kt::SwitchContext context(Self->EventCallback.Context);
         result = routine(Self, Event, EventData, Entity, Self->EventCallback.Meta);
      }
      else if (Self->EventCallback.isScript()) {
         if (EventData) {
            sc::Call(Self->EventCallback, std::to_array<ScriptArg>({
               { "Document", Self, FD_OBJECTPTR },
               { "EventMask", int(Event) },
               { "KeyValue:Parameters", EventData, FD_STRUCT },
               { "Entity", entity_id }
            }), result);
         }
         else {
            sc::Call(Self->EventCallback, std::to_array<ScriptArg>({
               { "Document",  Self, FD_OBJECTPTR },
               { "EventMask", int(Event) },
               { "KeyValue",  int(0) },
               { "Entity",    entity_id }
            }), result);
         }
      }
   }
   else log.trace("No subscriber for event $%.8x", (int)Event);

   return result;
}

//********************************************************************************************************************
// Set padding values in clockwise order.  For percentages, the final value is calculated from the diagonal of the
// parent.

void padding::parse(std::string_view Value)
{
   auto str = Value;
   str = read_unit(str, left, left_scl);

   if (not str.empty()) str = read_unit(str, top, top_scl);
   else { top = left; top_scl = left_scl; }

   if (not str.empty()) str = read_unit(str, right, right_scl);
   else { right = top; right_scl = top_scl; }

   if (not str.empty()) str = read_unit(str, bottom, bottom_scl);
   else { bottom = right; bottom_scl = right_scl; }

   if (left < 0)   left   = 0;
   if (top < 0)    top    = 0;
   if (right < 0)  right  = 0;
   if (bottom < 0) bottom = 0;

   configured = true;
}

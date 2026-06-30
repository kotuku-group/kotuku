
//********************************************************************************************************************
// Widget notification for user entry in inputbox, combobox etc

static void notify_input_onchange(objVectorText *Vector)
{
   auto Self = (extDocument *)CurrentContext();
   std::string_view str;
   Vector->getString(str);

   Self->Vars[Vector->Name].assign(str);

   if ((Self->EventMask & DEF::WIDGET_STATE) != DEF::NIL) {
      if (auto vp = (Object *)Vector->CreatorMeta) {
         KEYVALUE keys = { { "name", Vector->Name }, { "value", std::string(str) } };
         auto ent = std::get<bc_input *>(Self->VPToEntity[vp->UID].widget);
         report_event(Self, DEF::WIDGET_STATE, ent, &keys);
      }
   }
}

static void notify_combo_onchange(objVectorText *Vector)
{
   auto Self = (extDocument *)CurrentContext();
   std::string_view str;
   Vector->getString(str);

   Self->Vars[Vector->Name].assign(str);

   if ((Self->EventMask & DEF::WIDGET_STATE) != DEF::NIL) {
      if (auto vp = (Object *)Vector->CreatorMeta) {
         KEYVALUE keys = { { "name", Vector->Name }, { "value", std::string(str) } };
         auto ent = std::get<bc_combobox *>(Self->VPToEntity[vp->UID].widget);
         report_event(Self, DEF::WIDGET_STATE, ent, &keys);
      }
   }
}

//********************************************************************************************************************
// Feedback events for the combobox viewport.  Note that the viewport retains focus when the drop-down list is
// presented.

static ERR combo_feedback(objVectorViewport *Viewport, FM Event, OBJECTPTR EventObject, APTR Meta)
{
   auto Self = (extDocument *)CurrentContext();

   auto combo = std::get<bc_combobox *>(Self->VPToEntity[Viewport->UID].widget);

   if (Event IS FM::LOST_FOCUS) {
      if (gfx::GetUserFocus() IS combo->menu.m_surface.id) {
         // The drop-down surface has been given the focus - so don't hide it.
      }
      else {
         combo->menu.hide();
      }
   }
   else if ((Event IS FM::HAS_FOCUS) or (Event IS FM::CHILD_HAS_FOCUS)) {
      std::string_view input_string;
      combo->input->getString(input_string);
      combo->last_good_input = input_string;
      if (!combo->name.empty()) {
         Self->Vars[combo->name] = combo->last_good_input;
         if ((Self->EventMask & DEF::WIDGET_STATE) != DEF::NIL) {
            KEYVALUE keys = { { "name", combo->name }, { "value", combo->last_good_input } };
            report_event(Self, DEF::WIDGET_STATE, combo, &keys);
         }
      }
   }

   Viewport->draw();
   return ERR::Okay;
}

//********************************************************************************************************************
// Callback for the user selecting an item from the drop-down list.

void bc_combobox::callback(struct doc_menu &Menu, struct dropdown_item &Item)
{
   auto Self = (extDocument *)CurrentContext();
   auto combo = std::get<bc_combobox *>(Menu.m_ref);
   if (combo) {
      CSTRING value;
      if (!Item.value.empty()) {
         value = Item.value.c_str();
      }
      else value = Item.content.c_str();

      combo->input->setString(value);
      if (!combo->name.empty()) Self->Vars[combo->name] = value;
      combo->viewport->draw();

      if ((Self->EventMask & DEF::WIDGET_STATE) != DEF::NIL) {
         KEYVALUE keys = { { "name", combo->name }, { "value", combo->last_good_input } };
         report_event(Self, DEF::WIDGET_STATE, combo, &keys);
      }
   }
}

//********************************************************************************************************************

static bool delete_selected(extDocument *Self)
{
   if ((Self->SelectIndex.valid()) and (Self->SelectIndex != Self->CursorIndex)) {
      auto start = Self->SelectIndex;
      auto end = Self->CursorIndex;
      if (start > end) std::swap(start, end);

      if (start.offset > 0) {
         if (Self->Stream[start.index].code IS SCODE::TEXT) {
            auto &text = Self->Stream.lookup<bc_text>(start);
            if (start.index IS end.index) text.text.erase(start.offset, end.offset - start.offset);
            else text.text.erase(start.offset, text.text.size() - start.offset);
            text.invalidate_tokens();
         }
         start.index++;
         start.offset = 0;
      }

      if (start.index < end.index) {
         Self->Stream.data.erase(Self->Stream.data.begin() + start.index, Self->Stream.data.begin() + end.index);
         end.index -= (end.index - start.index);

         if ((end.offset > 0) and (Self->Stream[end.index].code IS SCODE::TEXT)) {
            auto &text = Self->Stream.lookup<bc_text>(end);
            text.text.erase(0, end.offset);
            text.invalidate_tokens();
         }
      }

      Self->invalidate_text_width_cache();
      Self->CursorIndex = Self->SelectIndex;
      Self->SelectIndex.reset();
      return true;
   }
   return false;
}

//********************************************************************************************************************

static ERR key_event(objVectorViewport *Viewport, KQ Flags, KEY Value, int Unicode)
{
   kt::Log log(__FUNCTION__);

   if ((Flags & KQ::PRESSED) IS KQ::NIL) return ERR::Okay;

   auto Self = (extDocument *)CurrentContext();

   log.function("Value: %d, Flags: $%.8x, ActiveEdit: %p", int(Value), int(Flags), Self->ActiveEditDef);

   if ((Self->ActiveEditDef) and ((Self->Page->Flags & VF::HAS_FOCUS) IS VF::NIL)) {
      deactivate_edit(Self, true);
   }

   if (Self->ActiveEditDef) {
      reset_cursor(Self);

      if (Unicode) {
         delete_selected(Self);

         // Output the character

         char string[12];
         UTF8WriteValue(Unicode, string, sizeof(string));
         Self->insertText(string, Self->CursorIndex.index, Self->CursorIndex.offset, true); // Will set UpdatingLayout to true
         Self->CursorIndex += std::string_view(string).size(); // Reposition the cursor

         layout_doc_fast(Self);

         resolve_fontx_by_index(Self, Self->CursorIndex, Self->CursorCharX);

         Self->Viewport->draw();
         return ERR::Okay;
      }

      switch(Value) {
         case KEY::TAB: {
            log.branch("Key: Tab");
            if (Self->TabFocusID) {
               kt::ScopedObjectLock tab(Self->TabFocusID);
               if (tab.granted()) acFocus(*tab);
            }
            else if ((Flags & KQ::SHIFT) != KQ::NIL) advance_tabfocus(Self, -1);
            else advance_tabfocus(Self, 1);
            break;
         }

         case KEY::ENTER: {
            delete_selected(Self);

            insert_text(Self, &Self->Stream, Self->CursorIndex, "\n", true);
            Self->CursorIndex.next_char(Self->Stream);

            layout_doc_fast(Self);
            resolve_fontx_by_index(Self, Self->CursorIndex, Self->CursorCharX);
            Self->Viewport->draw();
            break;
         }

         case KEY::LEFT: {
            Self->SelectIndex.reset();
            if (Self->Stream[Self->CursorIndex.index].code IS SCODE::CELL) {
               // Cursor cannot be moved any further left.  The cursor index should never end up here, but
               // better to be safe than sorry.

            }
            else {
               for (auto index = Self->CursorIndex; index.index > 0; ) {
                  index.prev_char(Self->Stream);

                  auto code = Self->Stream[index.index].code;
                  if (code IS SCODE::CELL) {
                     auto &cell = Self->Stream.lookup<bc_cell>(index);
                     if (cell.cell_id IS Self->ActiveEditCellID) break;
                  }
                  else if (code IS SCODE::IMAGE); // Inline images count as a character
                  else if (code != SCODE::TEXT) continue;

                  if (!resolve_fontx_by_index(Self, index, Self->CursorCharX)) {
                     Self->CursorIndex = index;
                     Self->Viewport->draw();
                     log.warning("LeftCursor: %d, X: %g", Self->CursorIndex.index, Self->CursorCharX);
                  }
                  break;
               }
            }
            break;
         }

         case KEY::RIGHT: {
            Self->SelectIndex.reset();

            auto index = Self->CursorIndex;
            while (index < INDEX(Self->Stream.size())) {
               auto code = Self->Stream[index.index].code;
               if (code IS SCODE::IMAGE); // Inline images are treated as content, so do nothing special for these and drop through to next section
               else {
                  index.next_char(Self->Stream);
                  continue;
               }

               // The current index references a content character or object.  Advance the cursor to the next index.

               index.next_char(Self->Stream);
               if (!resolve_fontx_by_index(Self, index, Self->CursorCharX)) {
                  Self->CursorIndex = index;
                  Self->Viewport->draw();
                  log.warning("RightCursor: %d, X: %g", Self->CursorIndex.index, Self->CursorCharX);
               }
               break;
            }
            break;
         }

         case KEY::HOME: {
            break;
         }

         case KEY::END: {
            break;
         }

         case KEY::UP:
            break;

         case KEY::DOWN:
            break;

         case KEY::BACKSPACE: {
            if (Self->Stream[Self->CursorIndex.index].code IS SCODE::CELL) {
               // Cursor cannot be moved any further left
            }
            else {
               auto index = Self->CursorIndex;
               index.prev_char(Self->Stream);

               if (Self->Stream[index.index].code IS SCODE::CELL);
               else {
                  if (!delete_selected(Self)) {
                     // Delete the character/escape code
                     Self->CursorIndex = index;
                     Self->CursorIndex.erase_char(Self->Stream);
                     Self->invalidate_text_width_cache();
                  }

                  Self->UpdatingLayout = true;
                  layout_doc_fast(Self);
                  resolve_fontx_by_index(Self, Self->CursorIndex, Self->CursorCharX);
                  Self->Viewport->draw();
               }
            }
            break;
         }

         case KEY::DELETE: {
            if (!delete_selected(Self)) {
               Self->CursorIndex.erase_char(Self->Stream);
               Self->invalidate_text_width_cache();
            }
            Self->UpdatingLayout = true;
            layout_doc_fast(Self);
            resolve_fontx_by_index(Self, Self->CursorIndex, Self->CursorCharX);
            Self->Viewport->draw();
            break;
         }

         default: break; // Ignore unhandled codes
      }
   }
   else switch (Value) {
      // NB: When not in edit mode, only the navigation keys are enabled
      case KEY::TAB:
         log.branch("Key: Tab");
         if (Self->TabFocusID) {
            kt::ScopedObjectLock tab(Self->TabFocusID);
            if (tab.granted()) acFocus(*tab);
         }
         else if ((Flags & KQ::SHIFT) != KQ::NIL) advance_tabfocus(Self, -1);
         else advance_tabfocus(Self, 1);
         break;

      case KEY::ENTER: {
         auto tab = Self->FocusIndex;
         if ((tab >= 0) and (tab < std::ssize(Self->Tabs))) {
            log.branch("Key: Enter, Tab: %d/%d, Type: %d", tab, int(Self->Tabs.size()), int(Self->Tabs[tab].type));

            if ((Self->Tabs[tab].type IS TT::LINK) and (Self->Tabs[tab].active)) {
               for (auto &link : Self->Links) {
                  if (link.origin.uid IS std::get<BYTECODE>(Self->Tabs[tab].ref)) {
                     link.exec(Self);
                     break;
                  }
               }
            }
         }
         break;
      }

      case KEY::PAGE_DOWN:
         Self->YPosition -= Self->VPHeight;
         if (-Self->YPosition > Self->PageHeight - Self->VPHeight) {
            Self->YPosition = -(Self->PageHeight - Self->VPHeight);
         }
         if (Self->YPosition > 0) Self->YPosition = 0;
         acMoveToPoint(Self->Page, 0, Self->YPosition, 0, MTF::Y);
         break;

      case KEY::PAGE_UP:
         Self->YPosition += Self->VPHeight;
         if (-Self->YPosition > Self->PageHeight - Self->VPHeight) {
            Self->YPosition = -(Self->PageHeight - Self->VPHeight);
         }
         if (Self->YPosition > 0) Self->YPosition = 0;
         acMoveToPoint(Self->Page, 0, Self->YPosition, 0, MTF::Y);
         break;

      case KEY::LEFT:
         Self->XPosition -= 10;
         if (-Self->XPosition > Self->PageWidth - Self->VPWidth) {
            Self->XPosition = -(Self->PageWidth - Self->VPWidth);
         }
         if (Self->XPosition > 0) Self->XPosition = 0;
         acMoveToPoint(Self->Page, Self->XPosition, 0, 0, MTF::X);
         break;

      case KEY::RIGHT:
         Self->XPosition += 10;
         if (-Self->XPosition > Self->PageWidth - Self->VPWidth) {
            Self->XPosition = -(Self->PageWidth - Self->VPWidth);
         }
         if (Self->XPosition > 0) Self->XPosition = 0;
         acMoveToPoint(Self->Page, Self->XPosition, 0, 0, MTF::X);
         break;

      case KEY::DOWN:
         Self->YPosition -= 10;
         if (-Self->YPosition > Self->PageHeight - Self->VPHeight) {
            Self->YPosition = -(Self->PageHeight - Self->VPHeight);
         }
         if (Self->YPosition > 0) Self->YPosition = 0;
         acMoveToPoint(Self->Page, 0, Self->YPosition, 0, MTF::Y);
         break;

      case KEY::UP:
         Self->YPosition += 10;
         if (-Self->YPosition > Self->PageHeight - Self->VPHeight) {
            Self->YPosition = -(Self->PageHeight - Self->VPHeight);
         }
         if (Self->YPosition > 0) Self->YPosition = 0;
         acMoveToPoint(Self->Page, 0, Self->YPosition, 0, MTF::Y);
         break;

      default: break; // Ignore unhandled codes
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static void error_dialog(const std::string_view Title, const std::string_view Message)
{
   kt::Log log(__FUNCTION__);

   log.warning("%.*s", int(Message.size()), Message.data());

#if !(defined(DBG_LAYOUT) || defined(DBG_STREAM) || defined(DBG_SEGMENTS))
   static bool detect_recursive_dialog = false;
   static OBJECTID dialog_id = 0;
   static std::mutex dialog_mutex;

   {
      std::lock_guard lk(dialog_mutex);
      if ((dialog_id) and (CheckResourceExists(dialog_id) IS ERR::True)) return;
      if (detect_recursive_dialog) return;
      detect_recursive_dialog = true;
   }

   objScript *dialog;
   OBJECTID new_dialog_id = 0;
   if (!NewObject(CLASSID::TIRI, &dialog)) {
      dialog->setName("scDialog");
      dialog->setOwner(CurrentTaskID());
      dialog->setPath("scripts:gui/dialog.tiri");

      acSetKey(dialog, "modal", "1");
      acSetKey(dialog, "title", Title);
      acSetKey(dialog, "options", "okay");
      acSetKey(dialog, "type", "error");
      acSetKey(dialog, "message", Message);

      if ((!InitObject(dialog)) and (!acActivate(dialog))) {
         std::span<std::string> results;
         if ((!dialog->getResults(results)) and (not results.empty())) {
            new_dialog_id = strtol(results[0].c_str(), nullptr, 0);
         }
      }
   }

   {
      std::lock_guard lk(dialog_mutex);
      if (new_dialog_id) dialog_id = new_dialog_id;
      detect_recursive_dialog = false;
   }
#endif
}

static void error_dialog(std::string_view Title, ERR Error)
{
   if (auto errstr = GetErrorMsg(Error)) {
      std::string buffer("Error: ");
      buffer.append(errstr);
      error_dialog(Title, buffer);
   }
}

//********************************************************************************************************************

static ERR activate_cell_edit(extDocument *Self, INDEX CellIndex, stream_char CursorIndex)
{
   kt::Log log(__FUNCTION__);

   if ((CellIndex < 0) or (CellIndex >= INDEX(Self->Stream.size()))) return log.warning(ERR::OutOfRange);

   log.branch("Cell Index: %d, Cursor Index: %d", CellIndex, CursorIndex.index);

   if (Self->Stream[CellIndex].code != SCODE::CELL) { // Sanity check
      return log.warning(ERR::Failed);
   }

   auto &cell = Self->Stream.lookup<bc_cell>(CellIndex);
   auto &stream = Self->Stream;

   if ((not CursorIndex.valid()) or (CursorIndex.index <= CellIndex)) { // Go to the start of the cell content
      CursorIndex.set(CellIndex + 1);
   }

   if (CursorIndex.index >= INDEX(stream.size())) return log.warning(ERR::OutOfRange);

   if (stream.data[CursorIndex.index].code != SCODE::TEXT) {
      // Skip ahead to the first relevant control code - it's always best to place the cursor ahead of things like
      // font styles, paragraph formatting etc.

      CursorIndex.offset = 0;
      while (CursorIndex.index < INDEX(stream.size())) {
         std::array<SCODE, 5> content = {
            SCODE::TABLE_START, SCODE::LINK_END, SCODE::IMAGE, SCODE::PARAGRAPH_END, SCODE::TEXT
         };
         if (std::find(std::begin(content), std::end(content), stream.data[CursorIndex.index].code) != std::end(content)) break;
         CursorIndex.next_code();
      }
   }

   auto it = Self->EditDefs.find(cell.edit_def);
   if (it IS Self->EditDefs.end()) return log.warning(ERR::Search);

   deactivate_edit(Self, false);

   auto &edit = it->second;
   cell.modified = false;

   Self->ActiveEditCellID = cell.cell_id;
   Self->ActiveEditDef    = &edit;
   Self->CursorIndex      = CursorIndex;
   Self->SelectIndex.reset();

   log.msg("Activated cell %d, cursor index %d, EditDef: %p, CRC: $%.8x",
      Self->ActiveEditCellID, Self->CursorIndex.index, Self->ActiveEditDef, Self->ActiveEditCRC);

   // Set the focus index to the relevant TT::EDIT entry

   for (unsigned tab=0; tab < Self->Tabs.size(); tab++) {
      if ((Self->Tabs[tab].type IS TT::EDIT) and (std::get<CELL_ID>(Self->Tabs[tab].ref) IS cell.cell_id)) {
         Self->FocusIndex = tab;
         break;
      }
   }

   resolve_fontx_by_index(Self, Self->CursorIndex, Self->CursorCharX);

   reset_cursor(Self); // Reset cursor flashing

   // User callbacks

   if (!edit.on_enter.empty()) {
      objScript *script;
      std::string function_name, argstring;

      log.msg("Calling on-enter callback function.");

      if (!extract_script(Self, edit.on_enter, &script, function_name, argstring)) {
         ScriptArg args[] = { { "ID", edit.name } };
         script->exec(function_name.c_str(), args, std::ssize(args));
      }
   }

   Self->Viewport->draw();
   return ERR::Okay;
}

//********************************************************************************************************************

static void deactivate_edit(extDocument *Self, bool Redraw)
{
   kt::Log log(__FUNCTION__);

   if (!Self->ActiveEditDef) return;

   log.branch("Redraw: %d, CellID: %d", Redraw, Self->ActiveEditCellID);

   if (Self->FlashTimer) {
      UpdateTimer(Self->FlashTimer, 0); // Turn off the timer
      Self->FlashTimer = 0;
   }

   // The edit tag needs to be found so that we can determine if on_exit needs to be called or not.

   auto edit = Self->ActiveEditDef;
   int cell_index = Self->Stream.find_cell(Self->ActiveEditCellID);

   Self->ActiveEditCellID = 0;
   Self->ActiveEditDef = nullptr;
   Self->CursorIndex.reset();
   Self->SelectIndex.reset();

   if (Redraw) Self->Viewport->draw();

   if (cell_index >= 0) {
      if (!edit->on_change.empty()) {
         bc_cell &cell = Self->Stream.lookup<bc_cell>(cell_index);

         if (cell.modified) {
            log.trace("Change detected in editable cell %d", cell.cell_id);

            objScript *script;
            std::string function_name, argstring;
            if (!extract_script(Self, edit->on_change, &script, function_name, argstring)) {
               auto cell_content = cell_index;
               cell_content++;

               std::vector<ScriptArg> args = {
                  ScriptArg("CellID", edit->name)
                  //ScriptArg("Start", cell_content),
                  //ScriptArg("End", i)
               };

               for (const auto& [key, value] : cell.args) args.emplace_back("", value);

               script->exec(function_name.c_str(), args.data(), args.size());
            }
         }
      }

      if (!edit->on_exit.empty()) {



      }
   }
   else log.warning("Failed to find cell ID %d", Self->ActiveEditCellID);
}

//********************************************************************************************************************
// TODO: This code needs to utilise cell viewports for managing UI interactivity.
#if 0
static void check_mouse_click(extDocument *Self, double X, double Y)
{
   kt::Log log(__FUNCTION__);

   Self->ClickX = X;
   Self->ClickY = Y;
   Self->ClickSegment = Self->MouseOverSegment;

   auto segment = Self->MouseOverSegment;

   if (segment IS -1) {
      // The mouse is not positioned over a segment.  Check if the mosue is positioned within
      // an editing cell.  If it is, we need to find the segment nearest to the mouse pointer
      // and position the cursor at the end of that segment.

      unsigned i;
      for (i=0; i < Self->EditCells.size(); i++) {
         if ((X >= Self->EditCells[i].x) and (X < Self->EditCells[i].x + Self->EditCells[i].width) and
             (Y >= Self->EditCells[i].y) and (Y < Self->EditCells[i].y + Self->EditCells[i].height)) {
            break;
         }
      }
      /*
      if (i < Self->EditCells.size()) {
         // Mouse is within an editable segment.

         INDEX cell_start = Self->Stream.find_cell(Self->EditCells[i].cell_id);

         SEGINDEX last_segment = -1;
         auto &ss = Self->get_sorted_segments();
         for (unsigned sortseg=0; sortseg < ss.size(); sortseg++) {
            SEGINDEX seg = ss[sortseg].segment;
            if ((Self->Segments[seg].start.index >= cell_start) and (Self->Segments[seg].stop.index <= cell_end)) {
               last_segment = seg;
               // Segment found.  Break if the segment's vertical position is past the mouse pointer
               if (Y < Self->Segments[seg].area.Y) break;
               if ((Y >= Self->Segments[seg].area.Y) and (X < Self->Segments[seg].area.X)) break;
            }
         }

         if (last_segment != -1) {
            // Set the cursor to the end of the nearest segment
            log.warning("Last seg: %d", last_segment);
            Self->CursorCharX = Self->Segments[last_segment].area.X + Self->Segments[last_segment].area.Width;
            Self->SelectCharX = Self->CursorCharX;

            // A click results in the deselection of existing text

            if (Self->CursorIndex.valid()) deselect_text(Self);

            Self->CursorIndex = Self->Segments[last_segment].stop;
            Self->SelectIndex.reset(); //Self->Segments[last_segment].stop;

            activate_cell_edit(Self, cell_start, Self->CursorIndex);
         }

         return;
      }
      else log.warning("Mouse not within an editable cell.");
      */
   }

   if (segment != -1) {
      stream_char sc;
      if (!resolve_font_pos(Self->Segments[segment], X, Self->CursorCharX, sc)) {
         if (Self->CursorIndex.valid()) deselect_text(Self); // A click results in the deselection of existing text

         if (!Self->Segments[segment].edit) deactivate_edit(Self, true);

         // Set the new cursor information

         Self->CursorIndex = sc;
         Self->SelectIndex.reset(); //sc; // SelectIndex is for text selections where the user holds the LMB and drags the mouse
         Self->SelectCharX = Self->CursorCharX;

         log.msg("User clicked on point %gx%g in segment %d, cursor index: %d, char x: %g", X, Y, segment, Self->CursorIndex.index, Self->CursorCharX);

         if (Self->Segments[segment].edit) {
            // If the segment is editable, we'll have to turn on edit mode so
            // that the cursor flashes.  Work backwards to find the edit cell.

            for (auto cellindex = Self->Segments[segment].start; cellindex.valid(); cellindex.prev_code()) {
               if (Self->Stream[cellindex.index].code IS SCODE::CELL) {
                  auto &cell = Self->Stream.lookup<bc_cell>(cellindex);
                  if (!cell.edit_def.empty()) {
                     activate_cell_edit(Self, cellindex.index, Self->CursorIndex);
                     break;
                  }
               }
            }
         }
      }
   }
   else if (Self->CursorIndex.valid()) {
      deselect_text(Self);
      deactivate_edit(Self, true);
   }
}
#endif
//********************************************************************************************************************
#if 0
static void check_mouse_pos(extDocument *Self, double X, double Y)
{
   Self->MouseOverSegment = -1;
   Self->PointerX = X;
   Self->PointerY = Y;

   check_pointer_exit(Self, X, Y); // For function callbacks

   if (Self->MouseInPage) {
      unsigned row;
      auto &ss = Self->get_sorted_segments();

      for (row=0; (row < ss.size()) and (Y < ss[row].y); row++);

      for (; row < ss.size(); row++) {
         if ((Y >= ss[row].y) and (Y < ss[row].y + Self->Segments[ss[row].segment].area.Height)) {
            if ((X >= Self->Segments[ss[row].segment].area.X) and (X < Self->Segments[ss[row].segment].area.X + Self->Segments[ss[row].segment].area.Width)) {
               Self->MouseOverSegment = ss[row].segment;
               break;
            }
         }
      }
   }

   // If the user is holding the mouse button and moving it around, we need to highlight the selected text.

   if ((Self->LMB) and (Self->CursorIndex.valid())) {
      if (!Self->SelectIndex.valid()) Self->SelectIndex = Self->CursorIndex;

      if (Self->MouseOverSegment != -1) {
         double cursor_x;
         stream_char cursor_index;
         if (!resolve_font_pos(Self->Segments[Self->MouseOverSegment], X, cursor_x, cursor_index)) {
            if (Self->ActiveEditDef) {
               // For select-dragging, we must check that the selection is within the bounds of the editing area.

               if (INDEX cell_index = Self->Stream.find_cell(Self->ActiveEditCellID); cell_index >= 0) {
                  INDEX i = cell_index++;
                  if (cursor_index.index < i) {
                     // If the cursor index precedes the start of the editing area, reset it

                     cursor_index.set(i);
                     if (!resolve_fontx_by_index(Self, cursor_index, cursor_x)) {

                     }
                  }
                  else {
                     // If the cursor index is past the end of the editing area, reset it
                     /*
                     while (i < INDEX(Self->Stream.size())) {
                        if (Self->Stream[i].code IS SCODE::CELL_END) {
                           auto &cell_end = Self->Stream.lookup<bc_cell_end>(i);
                           if (cell_end.cell_id IS Self->ActiveEditCellID) {
                              stream_char sc(i, 0);
                              if (auto seg = find_segment(Self->Segments, sc, false); seg > 0) {
                                 seg--;
                                 sc = Self->Segments[seg].stop;
                                 if (cursor_index > sc) {
                                    if (!resolve_fontx_by_index(Self, sc, cursor_x)) {
                                       cursor_index = sc;
                                    }
                                 }
                              }
                              break;
                           }
                        }
                        i++;
                     }
                     */
                  }

                  Self->CursorIndex = cursor_index;
                  Self->CursorCharX = cursor_x;
               }
               else deactivate_edit(Self, false);
            }
            else {
               Self->CursorIndex = cursor_index;
               Self->CursorCharX = cursor_x;
            }

            Self->Viewport->draw();
         }
      }
   }
}
#endif
//********************************************************************************************************************
// The text will be deselected, but the cursor and editing area will remain active.
#if 0
static void deselect_text(extDocument *Self)
{
   if (Self->CursorIndex IS Self->SelectIndex) return; // Nothing to deselect
   Self->SelectIndex.reset();
   Self->Viewport->draw();
}
#endif
//********************************************************************************************************************

static int find_tabfocus(extDocument *Self, TT Type, BYTECODE Reference)
{
   for (unsigned i=0; i < Self->Tabs.size(); i++) {
      if ((Self->Tabs[i].type IS Type) and (Reference IS std::get<BYTECODE>(Self->Tabs[i].ref))) return i;
   }
   return -1;
}

//********************************************************************************************************************
// This function is used in tags.c by the link and object insertion code.

static int add_tabfocus(extDocument *Self, TT Type, BYTECODE Reference)
{
   kt::Log log(__FUNCTION__);

   //log.branch("Type: %d, Ref: %d", LONG(Type), Reference);

   if (Type IS TT::LINK) { // For TT::LINK types, check that the link isn't already registered
      for (unsigned i=0; i < Self->Tabs.size(); i++) {
         if ((Self->Tabs[i].type IS TT::LINK) and (std::get<BYTECODE>(Self->Tabs[i].ref) IS Reference)) {
            return i;
         }
      }
   }

   auto index = std::ssize(Self->Tabs);
   Self->Tabs.emplace_back(Type, Reference, Self->Invisible ^ 1);
   return index;
}

//********************************************************************************************************************
// Hook for receiving input events from hyperlink regions

static ERR link_callback(objVector *Vector, InputEvent *Event)
{
   kt::Log log(__FUNCTION__);

   auto Self = (extDocument *)CurrentContext();

   ui_link *link = nullptr;
   for (unsigned i=0; i < Self->Links.size(); i++) {
      if (*Self->Links[i].origin.path IS Vector) {
         link = &Self->Links[i];
         break;
      }
   }

   if (!link) {
      log.warning("Failed to relate vector #%d to a hyperlink.", Vector->UID);
      return ERR::Okay;
   }

   objScript *script;
   std::string argstring, func_name;

   if ((Event->Flags & JTYPE::MOVEMENT) != JTYPE::NIL) {
      if (!link->origin.hooks.on_motion.empty()) {
         if (!extract_script(Self, link->origin.hooks.on_motion, &script, func_name, argstring)) {
            const ScriptArg args[] = {
               { "Element", link->origin.uid },
               { "X", Event->X },
               { "Y", Event->Y },
               { "Args", argstring }
            };
            script->exec(func_name.c_str(), args, std::ssize(args));
         }
      }
   }
   else if (Event->Type IS JET::CROSSED_IN) {
      link->hover = true;
      if (!link->origin.hooks.on_crossing.empty()) {
         if (!extract_script(Self, link->origin.hooks.on_crossing, &script, func_name, argstring)) {
            const ScriptArg args[] = {
               { "Element", link->origin.uid },
               { "X", Event->X },
               { "Y", Event->Y },
               { "Args", argstring }
            };
            script->exec(func_name.c_str(), args, std::ssize(args));
         }
      }

      // Change the fill of link text.  TODO: This should also be applied to SVG icons or anything
      // that inherits fill styles

      for (auto cursor=link->cursor_start; cursor < link->cursor_end; cursor.next_code()) {
         if (link->stream[0][cursor.index].code IS SCODE::TEXT) {
            auto &txt = link->stream[0].lookup<bc_text>(cursor);
            for (auto vt : txt.vector_text) vt->setFill(Self->LinkSelectFill);
         }
      }

      Self->Viewport->draw();
   }
   else if (Event->Type IS JET::CROSSED_OUT) {
      link->hover = false;
      if (!link->origin.hooks.on_crossing.empty()) {
         if (!extract_script(Self, link->origin.hooks.on_crossing, &script, func_name, argstring)) {
            const ScriptArg args[] = {
               { "Element", link->origin.uid },
               { "Args", argstring } };
            script->exec(func_name.c_str(), args, std::ssize(args));
         }
      }

      // Change the fill of link text.  TODO: This should also be applied to SVG icons or anything
      // that inherits fill styles

      for (auto cursor=link->cursor_start; cursor < link->cursor_end; cursor.next_code()) {
         if (link->stream[0][cursor.index].code IS SCODE::TEXT) {
            auto &txt = link->stream[0].lookup<bc_text>(cursor);
            for (auto vt : txt.vector_text) vt->setFill(link->origin.fill);
         }
      }

      Self->Viewport->draw();
   }
   else if ((Event->Flags & JTYPE::BUTTON) != JTYPE::NIL) {
      if (Event->Value IS 0) link->exec(Self);
   }
   else log.warning("Unknown event type %d for input vector %d", int(Event->Type), Vector->UID);

   return ERR::Okay;
}

//********************************************************************************************************************
// Changes the focus to an object or link in the document.  The new index is stored in the FocusIndex field.  If the
// Index is set to -1, set_focus() will focus on the first element, but only if it is an object.

static void set_focus(extDocument *Self, INDEX Index, CSTRING Caller)
{
   kt::Log log(__FUNCTION__);

   if (Self->Tabs.empty()) return;

   if ((Index < -1) or (Index >= std::ssize(Self->Tabs))) {
      log.traceWarning("Index %d out of bounds.", Index);
      return;
   }

   log.branch("Index: %d/%d, Type: %d, Ref: %d, HaveFocus: %d, Caller: %s",
      Index, int(std::ssize(Self->Tabs)), (Index != -1) ? int(Self->Tabs[Index].type) : -1,
      (Index != -1) ? std::get<uint32_t>(Self->Tabs[Index].ref) : -1, Self->HasFocus, Caller);

   if (Self->ActiveEditDef) deactivate_edit(Self, true);

   if (Index IS -1) {
      Index = 0;
      Self->FocusIndex = 0;
      if (Self->Tabs[0].type IS TT::LINK) {
         log.msg("First focusable element is a link - focus unchanged.");
         return;
      }
   }

   if (!Self->Tabs[Index].active) {
      log.warning("Tab marker %d is not active.", Index);
      return;
   }

   Self->FocusIndex = Index;

   if (Self->Tabs[Index].type IS TT::EDIT) {
      acFocus(Self->Page);

      if (auto cell_index = Self->Stream.find_cell(std::get<CELL_ID>(Self->Tabs[Self->FocusIndex].ref)); cell_index >= 0) {
         activate_cell_edit(Self, cell_index, stream_char());
      }
   }
   else if (Self->Tabs[Index].type IS TT::VECTOR) {
      if (Self->HasFocus) {
         CLASSID class_id = GetClassID(std::get<OBJECTID>(Self->Tabs[Index].ref));
         if (class_id IS CLASSID::VECTORTEXT) {
            kt::ScopedObjectLock focus(std::get<OBJECTID>(Self->Tabs[Index].ref));
            if (focus.granted()) {
               acFocus(*focus);
               //if ((!input->get(FID_UserInput, text)) and (text)) {
               //   txtSelectArea(text, 0,0, 200000, 200000);
               //}
            }
         }
         else {
            ScopedObjectLock focus(std::get<OBJECTID>(Self->Tabs[Index].ref));
            if (focus.granted()) acFocus(*focus);
         }
      }
   }
   else if (Self->Tabs[Index].type IS TT::LINK) {
      if (Self->HasFocus) { // Scroll to the link if it is out of view, or redraw the display if it is not.
         for (unsigned i=0; i < Self->Links.size(); i++) {
            auto &link = Self->Links[i];
            if (link.origin.uid IS std::get<BYTECODE>(Self->Tabs[Index].ref)) {
               double link_x = 0, link_y = 0, link_width = 0, link_height = 0;
               for (++i; i < Self->Links.size(); i++) {
                  if (link.origin.uid IS std::get<BYTECODE>(Self->Tabs[Index].ref)) {
                     link.origin.path->getBoundary(VBF::NIL, &link_x, &link_y, &link_width, &link_height);
                  }
               }

               view_area(Self, link_x, link_y, link_x + link_width, link_y + link_height);
               break;
            }
         }

         Self->Viewport->draw();
         acFocus(Self->Page);
      }
   }
}

//********************************************************************************************************************
// Scrolls any given area of the document into view.

static bool view_area(extDocument *Self, double Left, double Top, double Right, double Bottom)
{
   kt::Log log(__FUNCTION__);

   double hgap = Self->VPWidth * 0.1, vgap = Self->VPHeight * 0.1;
   double view_x = -Self->XPosition, view_y = -Self->YPosition;
   double view_height = Self->VPHeight, view_width  = Self->VPWidth;

   log.trace("View: %dx%d,%dx%d Link: %gx%g,%gx%g", view_x, view_y, view_width, view_height, Left, Top, Right, Bottom);

   // Vertical

   if (Self->PageHeight > Self->VPHeight) {
      if (Top < view_y + vgap) {
         view_y = Top - vgap;
         if (view_y < view_height * 0.25) view_y = 0;

         if ((Bottom < view_height - vgap) and (-Self->YPosition > view_height)) {
            view_y = 0;
         }
      }
      else if (Bottom > view_y + view_height - vgap) {
         view_y = Bottom + vgap - view_height;
         if (view_y > Self->PageHeight - view_height - (view_height * 0.25)) view_y = Self->PageHeight - view_height;
      }
   }
   else view_y = 0;

   // Horizontal

   if (Self->CalcWidth > Self->VPWidth) {
      if (Left < view_x + hgap) {
         view_x = Left - hgap;
         if (view_x < 0) view_x = 0;
      }
      else if (Right > view_x + view_width - hgap) {
         view_x = Right + hgap - view_width;
         if (view_x > Self->CalcWidth - view_width) view_x = Self->CalcWidth - view_width;
      }
   }
   else view_x = 0;

   if ((-view_x != Self->XPosition) or (-view_y != Self->YPosition)) {
      Self->XPosition = -view_x;
      Self->YPosition = -view_y;
      acMoveToPoint(Self->Page, -view_x, -view_y, 0, MTF::X|MTF::Y);
      return true;
   }
   else return false;
}

//********************************************************************************************************************

static void advance_tabfocus(extDocument *Self, int8_t Direction)
{
   kt::Log log(__FUNCTION__);

   if (Self->Tabs.empty()) return;

   // Check that the FocusIndex is accurate (it may have changed if the user clicked on a gadget).

   OBJECTID currentfocus = gfx::GetUserFocus();
   for (unsigned i=0; i < Self->Tabs.size(); i++) {
      if (std::get<OBJECTID>(Self->Tabs[i].ref) IS currentfocus) {
         Self->FocusIndex = i;
         break;
      }
   }

   log.function("Direction: %d, Current Index: %d", Direction, Self->FocusIndex);

   if (Self->FocusIndex < 0) {
      // FocusIndex may be -1 to indicate nothing is selected, so we'll have to start from the first focusable index in that case.

      if (Direction IS -1) Self->FocusIndex = 1; // Future --
      else Self->FocusIndex = -1; // Future ++
   }

   // Advance the focus index.  Operates as a loop so that disabled surfaces can be skipped.

   auto i = std::ssize(Self->Tabs); // This while loop is designed to stop if no tab indexes are found to be active
   while (i > 0) {
      i--;

      if (Direction IS -1) {
         Self->FocusIndex--;
         if (Self->FocusIndex < 0) Self->FocusIndex = Self->Tabs.size() - 1;
      }
      else {
         Self->FocusIndex++;
         if (Self->FocusIndex >= int(Self->Tabs.size())) Self->FocusIndex = 0;
      }

      if (!Self->Tabs[Self->FocusIndex].active) continue;

      if (Self->Tabs[Self->FocusIndex].type IS TT::VECTOR) {
         /*
         SURFACEINFO *info;
         if (!gfxGetSurfaceInfo(Self->Tabs[Self->FocusIndex].ref, &info)) {
            if ((info->Flags & RNF::DISABLED) != RNF::NIL) continue;
         }
         */
      }
      break;
   }

   if (i >= 0) set_focus(Self, Self->FocusIndex, "adv_tabfocus");
}

//********************************************************************************************************************

static ERR flash_cursor(extDocument *Self, int64_t TimeElapsed, int64_t CurrentTime)
{
   Self->CursorState ^= 1;

   Self->Viewport->draw();
   return ERR::Okay;
}

//********************************************************************************************************************

static void reset_cursor(extDocument *Self)
{
   kt::Log log(__FUNCTION__);

   log.function();

   Self->CursorState = 1;
   if (Self->FlashTimer) UpdateTimer(Self->FlashTimer, 0.5);
   else SubscribeTimer(0.5, C_FUNCTION(flash_cursor), &Self->FlashTimer);
}

//********************************************************************************************************************
// Generic input handler for all widgets

static void handle_widget_event(extDocument *Self, widget_mgr &Widget, const InputEvent *Event)
{
   for (; Event; Event = Event->Next) {
      if (Event->Type IS JET::CROSSED_IN) {
         //Widget.hover = true;
         //Self->Viewport->draw();
      }
      else if (Event->Type IS JET::CROSSED_OUT) {
         //Widget.hover = false;
         //Self->Viewport->draw();
      }
   }
}

//********************************************************************************************************************
// Incoming events from cell viewports

static ERR inputevent_cell(objVectorViewport *Viewport, const InputEvent *Event)
{
   auto Self = (extDocument *)CurrentContext();

   if (!Self->VPToEntity.contains(Viewport->UID)) return ERR::Terminate;

   auto cell = std::get<bc_cell *>(Self->VPToEntity[Viewport->UID].widget);

   std::string func_name, s_args;
   objScript *script;
   for (; Event; Event = Event->Next) {
      if ((Event->Flags & JTYPE::BUTTON) != JTYPE::NIL) {
         if ((Self->EventMask & DEF::ON_CLICK) != DEF::NIL) {
            if (report_event(Self, DEF::ON_CLICK, cell, &cell->args) IS ERR::Skip) continue;
         }

         if (!cell->hooks.on_click.empty()) {
            if (!extract_script(Self, cell->hooks.on_click, &script, func_name, s_args)) {
               const ScriptArg args[] = {
                  { "Entity", cell->uid },
                  { "Button", int(Event->Type) }, // JET::LMB etc
                  { "State", Event->Value }, // 1 = Pressed
                  { "X", Event->X },
                  { "Y", Event->Y },
                  { "Args", s_args }
               };
               script->exec(func_name.c_str(), args, std::ssize(args));
            }
         }
      }
      else if ((Event->Flags & JTYPE::MOVEMENT) != JTYPE::NIL) {
         if ((Self->EventMask & DEF::ON_MOTION) != DEF::NIL) {
            if (report_event(Self, DEF::ON_MOTION, cell, &cell->args) IS ERR::Skip) continue;

            if (!cell->hooks.on_motion.empty()) {
               if (!extract_script(Self, cell->hooks.on_motion, &script, func_name, s_args)) {
                  const ScriptArg args[] = {
                     { "Entity", cell->uid },
                     { "X", Event->X },
                     { "Y", Event->Y },
                     { "Args", s_args }
                  };
                  script->exec(func_name.c_str(), args, std::ssize(args));
               }
            }
         }
      }
      else if (Event->Type IS JET::CROSSED_IN) {
         if ((Self->EventMask & DEF::ON_CROSSING_IN) != DEF::NIL) {
            if (report_event(Self, DEF::ON_CROSSING_IN, cell, &cell->args) IS ERR::Skip) continue;

            if (!cell->hooks.on_crossing.empty()) {
               if (!extract_script(Self, cell->hooks.on_crossing, &script, func_name, s_args)) {
                  const ScriptArg args[] = {
                     { "Entity", cell->uid },
                     { "X", Event->X },
                     { "Y", Event->Y },
                     { "Args", s_args }
                  };
                  script->exec(func_name.c_str(), args, std::ssize(args));
               }
            }
         }
      }
      else if (Event->Type IS JET::CROSSED_OUT) {
         if ((Self->EventMask & DEF::ON_CROSSING_OUT) != DEF::NIL) {
            if (report_event(Self, DEF::ON_CROSSING_OUT, cell, &cell->args) IS ERR::Skip) continue;

            if (!cell->hooks.on_crossing.empty()) {
               if (!extract_script(Self, cell->hooks.on_crossing, &script, func_name, s_args)) {
                  const ScriptArg args[] = {
                     { "Entity", cell->uid },
                     { "X", Event->X },
                     { "Y", Event->Y },
                     { "Args", s_args }
                  };
                  script->exec(func_name.c_str(), args, std::ssize(args));
               }
            }
         }
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Incoming events from the button viewport

static ERR inputevent_button(objVectorViewport *Viewport, const InputEvent *Event)
{
   auto Self = (extDocument *)CurrentContext();

   if (!Self->VPToEntity.contains(Viewport->UID)) return ERR::Terminate;

   vp_to_entity &widget = Self->VPToEntity[Viewport->UID];
   auto button = std::get<bc_button *>(widget.widget);

   handle_widget_event(Self, *button, Event);

   for (; Event; Event = Event->Next) {
      if ((Event->Flags & JTYPE::BUTTON) != JTYPE::NIL) {
         if (Event->Type IS JET::LMB) {
            if (Event->Value IS 1) button->alt_state = true;
            else button->alt_state = false;
         }

         if (button->alt_state) { // Button is active
            if (!button->alt_fill.empty()) button->viewport->setFill(button->alt_fill);

            // Scale the button viewport down a little bit to indicate that it's been clicked.

            if (!button->viewport->Matrices) {
               VectorMatrix *matrix;
               button->viewport->newMatrix(&matrix, false);
            }

            Unit width;
            button->viewport->getWidth(width);
            Unit height;
            button->viewport->getHeight(height);

            if (auto m = button->viewport->Matrices) {
               const auto SCALE = 0.95;
               m->TranslateX -= double(width) * 0.5;
               m->TranslateY -= double(height) * 0.5;
               vec::Scale(m, SCALE, SCALE);
               m->TranslateX += double(width) * 0.5;
               m->TranslateY += double(height) * 0.5;
               vec::FlushMatrix(button->viewport->Matrices);
            }
         }
         else {
            if (!button->alt_fill.empty()) button->viewport->setFill(button->fill);

            if (button->viewport->Matrices) {
               vec::ResetMatrix(button->viewport->Matrices);
            }
         }

         Self->Viewport->draw();
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR inputevent_dropdown(objVectorViewport *Viewport, const InputEvent *Event)
{
   auto Self = (extDocument *)CurrentContext();

   if (!Self->VPToEntity.contains(Viewport->UID)) return ERR::Terminate;

   auto &combo = *std::get<bc_combobox *>(Self->VPToEntity[Viewport->UID].widget);

   handle_widget_event(Self, combo, Event);

   for (; Event; Event = Event->Next) {
      if ((Event->Flags & JTYPE::BUTTON) != JTYPE::NIL) {
         combo.menu.create(combo.final_width);

         if (Event->Type IS JET::LMB) {
            if (Event->Value) combo.menu.toggle(*combo.viewport);
         }

         Self->Viewport->draw();
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Incoming events from the checkbox viewport

static ERR inputevent_checkbox(objVectorViewport *Viewport, const InputEvent *Event)
{
   auto Self = (extDocument *)CurrentContext();

   if (!Self->VPToEntity.contains(Viewport->UID)) return ERR::Terminate;

   auto checkbox = std::get<bc_checkbox *>(Self->VPToEntity[Viewport->UID].widget);

   handle_widget_event(Self, *checkbox, Event);

   for (; Event; Event = Event->Next) {
      if ((Event->Flags & JTYPE::BUTTON) != JTYPE::NIL) {
         if (Event->Type IS JET::LMB) {
            if (Event->Value IS 1) {
               checkbox->alt_state ^= 1;
               if (!checkbox->name.empty()) {
                  Self->Vars[checkbox->name] = checkbox->alt_state ? "1" : "0";

                  if ((Self->EventMask & DEF::WIDGET_STATE) != DEF::NIL) {
                     KEYVALUE keys = { { "name", checkbox->name }, { "value", checkbox->alt_state ? "1" : "0" } };
                     report_event(Self, DEF::WIDGET_STATE, checkbox, &keys);
                  }
               }
            }
         }

         if (checkbox->alt_state) checkbox->viewport->setFill(checkbox->alt_fill);
         else checkbox->viewport->setFill(checkbox->fill);

         Self->Viewport->draw();
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Using only a stream index, this function will determine the x coordinate of the character at that index.  This is
// slower than resolve_font_pos(), because the segment has to be resolved by this function.

static ERR resolve_fontx_by_index(extDocument *Self, stream_char Char, double &CharX)
{
   kt::Log log("resolve_fontx");

   log.branch("Index: %d", Char.index);

   bc_font *style = find_style(Self->Stream, Char);
   auto font = style ? style->get_font() : &glFonts[0];
   if (!font) return log.warning(ERR::Search);

   // Find the segment linked to this character.  This is so that we can derive an x coordinate for the character
   // string.

   if (SEGINDEX segment = find_segment(Self->Segments, Char, true); segment >= 0) {
      auto i = Self->Segments[segment].start;
      while ((i <= Self->Segments[segment].stop) and (i < Char)) {
         if (Self->Stream[i.index].code IS SCODE::TEXT) {
            CharX = vec::StringWidth(font->handle, Self->Stream.lookup<bc_text>(i).text, -1);
            return ERR::Okay;
         }
         i.next_code();
      }
   }

   log.warning("Failed to find a segment for index %d.", Char.index);
   return ERR::Search;
}

//********************************************************************************************************************
// This is the old version of page input management and not currently in use.  Instead we rely on the VectorScene
// performing input management for us.
/*
static ERR consume_input_events(objVector *Vector, const InputEvent *Events)
{
   auto Self = (extDocument *)CurrentContext();

   for (auto input=Events; input; input=input->Next) {
      if ((input->Flags & JTYPE::MOVEMENT) != JTYPE::NIL) {
         for (auto scan=input->Next; (scan) and ((scan->Flags & JTYPE::MOVEMENT) != JTYPE::NIL); scan=scan->Next) {
            input = scan;
         }

         if (input->OverID IS Self->Page->UID) Self->MouseInPage = true;
         else Self->MouseInPage = false;


         check_mouse_pos(Self, input->X, input->Y);

         // Note that this code has to 'drop through' due to the movement consolidation loop earlier in this subroutine.
      }

      if (input->Type IS JET::LMB) {
         if (input->Value > 0) {
            Self->LMB = true;
            check_mouse_click(Self, input->X, input->Y);
         }
         else {
            Self->LMB = false;
            check_mouse_release(Self, input->X, input->Y);
         }
      }
   }

   return ERR::Okay;
}
*/

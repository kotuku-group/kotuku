
//********************************************************************************************************************
// If the layout needs to be recalculated, set the UpdatingLayout field before calling this function.

static void redraw(extDocument *Self, bool Focus)
{
   pf::Log log(__FUNCTION__);

   log.traceBranch();

   {
      #ifndef RETAIN_LOG_LEVEL
      pf::LogLevel level(3);
      #endif

      layout_doc(Self); // Does nothing if UpdatingLayout is false
   }

   if (Self->Viewport->Scene->SurfaceID) Self->Viewport->draw();

   if ((Focus) and (Self->FocusIndex != -1)) set_focus(Self, -1, "redraw()");
}

//********************************************************************************************************************
// Determine the position of the widget and return the coordinates in (X, Y)
//
// Use AsViewport if the Widget.viewport represents the widget in the UI.  A viewport will be created automatically
// if the client has not already done so, and it will be assumed that patterns defined in alt_state/alt_fill are to be
// used as fills.
//
// Alternatively, a VectorRectangle will be created automatically and reference the patterns defined in
// alt_state/alt_fill.  This is the fastest means of rendering widget graphics at the cost of additional bitmap
// caching.
//
// Irrespective of the drawing method, the X/Y/W/H dimensions of the widget are updated before returning.

ERR layout::position_widget(widget_mgr &Widget, doc_segment &Segment, objVectorViewport *ParentVP, bc_font *Style,
   double &XAdvance, double LabelWidth, bool AsViewport, double &X, double &Y)
{
   if (Widget.floating_x()) {
      // If the widget is floating then the X coordinate will be pre-calculated during layout
      X = Widget.x + Widget.final_pad.left;
   }
   else {
      // For inline widgets, alignment is calculated from the active style.
      if ((Style->options & FSO::ALIGN_CENTER) != FSO::NIL) X = XAdvance + ((Segment.align_width - Segment.area.Width) * 0.5);
      else if ((Style->options & FSO::ALIGN_RIGHT) != FSO::NIL) X = XAdvance + Segment.align_width - Segment.area.Width;
      else X = XAdvance;
   }

   // TODO: Is the use of floating_x() correct?
   if (Widget.floating_x()) Y = Segment.area.Y + Widget.final_pad.top;
   else {
      if ((Style->valign & ALIGN::TOP) != ALIGN::NIL) {
         Y = Widget.final_pad.top;
      }
      else if ((Style->valign & ALIGN::VERTICAL) != ALIGN::NIL) {
         Y = (Segment.area.Height - Widget.full_height()) * 0.5;
      }
      else {
         // Bottom alignment.  Aligning to the base-line is preferable, but if the widget is tall then we take up the descent space too.
         auto h = Widget.final_height - Widget.final_pad.bottom;
         if (h > Segment.area.Height - Segment.descent) {
            if (Widget.align_to_text) Y = Segment.area.Height - h + Segment.descent;
            else Y = Segment.area.Height - h;
         }
         else Y = Segment.area.Height - Segment.descent - h;
      }

      Y += Segment.area.Y;
   }

   const double width = Widget.final_width + LabelWidth;

   if (AsViewport) {
      // Using a viewport means that the vector paths will be recomputed on each draw cycle.
      if (Widget.viewport.empty()) {
         auto vp = objVectorViewport::create::global({
            fl::Name("vp_widget"),
            fl::Owner(ParentVP->UID),
            fl::Fill(Widget.alt_state ? Widget.alt_fill : Widget.fill)
         });

         if (!vp) return ERR::CreateObject;
         else Widget.viewport.set(vp);
      }

      Widget.viewport->setFields(fl::X(int(X)), fl::Y(int(Y)), fl::Width(width), fl::Height(Widget.final_height));
   }
   else {
      // Using a rectangle with a pattern reference will keep the pattern bitmap cached.
      if (Widget.rect.empty()) {
         auto rect = objVectorRectangle::create::global({
            fl::Name("rect_widget"),
            fl::Owner(ParentVP->UID),
            fl::Fill(Widget.alt_state ? Widget.alt_fill : Widget.fill)
         });

         if (!rect) return ERR::CreateObject;
         else Widget.rect = rect;
      }

      Widget.rect->setFields(fl::X(int(X)), fl::Y(int(Y)), fl::Width(width), fl::Height(Widget.final_height));
   }

   if (!Widget.floating_x()) XAdvance += Widget.final_pad.left + Widget.final_pad.right + width;

   return ERR::Okay;
}

//********************************************************************************************************************
// Convert the layout information to a vector scene.  This is the final step in the layout process. The advantage in
// performing this step separately to the layout process is that the graphics resources are managed last, which is
// sensible for keeping them out of the layout loop.
//
// It is intended that the layout process generates the document's entire scene graph every time.  Optimisations
// relating to things like the obscuration of graphics elements are considered to be the job of the VectorScene's
// drawing functionality.

ERR layout::gen_scene_init(objVectorViewport *Viewport)
{
   pf::Log log(__FUNCTION__);

   log.branch();

   // Remove former objects from the viewport

   for (auto it=Self->UIObjects.rbegin(); it != Self->UIObjects.rend(); it++) {
      FreeResource(*it);
   }
   Self->UIObjects.clear();

   m_cursor_drawn = false;

   Self->Links.clear();
   Self->VPToEntity.clear(); // NB: Widgets are cleared and re-added because they use direct pointers to the std::vector stream

   if (Self->UpdatingLayout) return ERR::NothingDone; // Drawing is disabled if the layout is being updated

   if (glFonts.empty()) { // Sanity check
      log.traceWarning("Failed to load a default font.");
      return ERR::Failed;
   }

   #ifdef GUIDELINES // Make clip regions visible
      for (unsigned i=0; i < m_clips.size(); i++) {
         auto rect = objVectorRectangle::create::global({
               fl::Owner(Viewport->UID),
               fl::X(m_clips[i].Clip.left), fl::Y(m_clips[i].Clip.top),
               fl::Width(m_clips[i].Clip.right - m_clips[i].Clip.left),
               fl::Height(m_clips[i].Clip.bottom - m_clips[i].Clip.top),
               fl::Fill("rgb(255,200,200,64)") });
      }
   #endif

   // Body background fill is initialised now, if specified.

   if ((!Self->Background.empty()) and (!iequals("none", Self->Background))) {
      if ((Self->Bkgd = objVectorRectangle::create::global({
            fl::Name("doc_body_fill"),
            fl::Owner(Self->Page->UID),
            fl::X(0), fl::Y(0),
            fl::Width(Self->CalcWidth),
            fl::Height(Self->PageHeight < Self->VPHeight ? Self->VPHeight : Self->PageHeight),
            fl::Fill(Self->Background)
         }))) {

         Self->UIObjects.push_back(Self->Bkgd->UID);

         // Move-to-back required because vector objects are created within the page during the layout process.

         acMoveToBack(Self->Bkgd);
      }
   }
   else Self->Bkgd = nullptr;

   return ERR::Okay;
}

void layout::gen_scene_graph(objVectorViewport *Viewport, std::vector<doc_segment> &Segments)
{
   pf::Log log(__FUNCTION__);

   std::stack<bc_list *>      stack_list;
   std::stack<bc_row *>       stack_row;
   std::stack<bc_paragraph *> stack_para;
   std::stack<bc_table *>     stack_table;
   std::stack<ui_link>        stack_ui_link;
   std::stack<bc_font *>      stack_style;
   std::stack<objVectorViewport *> stack_vp;

#ifndef RETAIN_LOG_LEVEL
   pf::LogLevel level(2);
#endif

/*
   stream_char select_start, select_end;
   double select_start_x, select_end_x;

   if ((Self->ActiveEditDef) and (!Self->SelectIndex.valid())) {
      select_start   = Self->CursorIndex;
      select_end     = Self->CursorIndex;
      select_start_x = Self->CursorCharX;
      select_end_x   = Self->CursorCharX;
   }
   else if ((Self->CursorIndex.valid()) and (Self->SelectIndex.valid())) {
      if (Self->SelectIndex < Self->CursorIndex) {
         select_start   = Self->SelectIndex;
         select_end     = Self->CursorIndex;
         select_start_x = Self->SelectCharX;
         select_end_x   = Self->CursorCharX;
      }
      else {
         select_start   = Self->CursorIndex;
         select_end     = Self->SelectIndex;
         select_start_x = Self->CursorCharX;
         select_end_x   = Self->SelectCharX;
      }
   }
*/
   for (SEGINDEX seg=0; seg < SEGINDEX(Segments.size()); seg++) {
      auto &segment = Segments[seg];

      if (!stack_ui_link.empty()) {
         stack_ui_link.top().area = segment.area;
      }

      // Highlighting of selected text
      /*
      if ((select_start <= segment.stop) and (select_end > segment.start)) {
         if (select_start != select_end) {
            alpha = 80.0/255.0;
            if ((select_start > segment.start) and (select_start < segment.stop)) {
               if (select_end < segment.stop) {
                  gfxDrawRectangle(Bitmap, segment.x + select_start_x, segment.y,
                     select_end_x - select_start_x, segment.height, Bitmap->packPixel(0, 128, 0), BAF::FILL);
               }
               else {
                  gfxDrawRectangle(Bitmap, segment.x + select_start_x, segment.y,
                     segment.width - select_start_x, segment.height, Bitmap->packPixel(0, 128, 0), BAF::FILL);
               }
            }
            else if (select_end < segment.stop) {
               gfxDrawRectangle(Bitmap, segment.x, segment.y, select_end_x, segment.height,
                  Bitmap->packPixel(0, 128, 0), BAF::FILL);
            }
            else {
               gfxDrawRectangle(Bitmap, segment.x, segment.y, segment.width, segment.height,
                  Bitmap->packPixel(0, 128, 0), BAF::FILL);
            }
            alpha = 1.0;
         }
      }
      */
      if ((Self->ActiveEditDef) and (Self->CursorState) and (!m_cursor_drawn)) {
         if ((Self->CursorIndex >= segment.start) and (Self->CursorIndex <= segment.stop)) {
            if ((Self->CursorIndex IS segment.stop) and
                (Self->CursorIndex.get_prev_char_or_inline(segment.stream[0]) IS '\n'));
            else if ((Self->Page->Flags & VF::HAS_FOCUS) != VF::NIL) { // Standard text cursor
               std::vector<PathCommand> seq = {
                  { .Type = PE::Move, .X = segment.area.X + Self->CursorCharX, .Y = segment.area.Y },
                  { .Type = PE::VLineRel, .Y = segment.area.Height - segment.descent }
               };

               auto vp = objVectorPath::create::global({
                  fl::Owner(Viewport->UID), fl::Stroke("rgb(255,0,0,255)"), fl::StrokeWidth(2)
               });

               vp->setCommand(seq.size(), seq.data(), seq.size() * sizeof(PathCommand));
               m_cursor_drawn = true;
            }
         }
      }

      auto x_advance = segment.area.X;
      for (auto cursor = segment.start; cursor < segment.stop; cursor.next_code()) {
         switch (segment.stream[0][cursor.index].code) {
            case SCODE::FONT:
               stack_style.push(&segment.stream->lookup<bc_font>(cursor));
               break;

            case SCODE::FONT_END:
               stack_style.pop();
               break;

            case SCODE::LIST_START:
               stack_list.push(&segment.stream->lookup<bc_list>(cursor));
               break;

            case SCODE::LIST_END:
               stack_list.pop();
               break;

            case SCODE::PARAGRAPH_START: {
               auto &para = segment.stream->lookup<bc_paragraph>(cursor);
               stack_para.push(&para);

               stack_style.push(&segment.stream->lookup<bc_paragraph>(cursor).font);
               auto font = stack_style.top()->get_font();

               double x = segment.x(x_advance, stack_style.top()->options);
               double y = segment.y(stack_style.top()->valign, font);

               if ((!stack_list.empty()) and (para.list_item)) {
                  // Handling for paragraphs that form part of a list

                  if ((stack_list.top()->type IS bc_list::CUSTOM) or
                      (stack_list.top()->type IS bc_list::ORDERED)) {
                     if (!para.icon.empty()) {
                        para.icon->setFields(
                           fl::X(int(x - para.item_indent.px(*this))),
                           fl::Y(int(y))
                        );
                     }
                  }
                  else if (stack_list.top()->type IS bc_list::BULLET) {
                     if (!para.icon.empty()) {
                        const double radius = segment.area.Height * 0.2;
                        const double cy = y - (font->metrics.Ascent * 0.5);

                        para.icon->setFields(
                           fl::CenterX(x - para.item_indent.px(*this) + radius),
                           fl::CenterY(cy),
                           fl::Radius(radius));
                     }
                  }
               }
               break;
            }

            case SCODE::PARAGRAPH_END:
               stack_style.pop();
               stack_para.pop();
               break;

            case SCODE::TABLE_START: {
               stack_table.push(&segment.stream->lookup<bc_table>(cursor));
               auto table = stack_table.top();

               if (table->floating_x()); // If floating, X coordinate is calculated during layout
               else {
                  // Otherwise the X coordinate is dependent on the style's alignment
                  // NB: Currently the TABLE code is defined as non-graphical and positioning is declared in the
                  // table structure, not the segment.area.
                  if ((stack_style.top()->options & FSO::ALIGN_CENTER) != FSO::NIL) table->x = table->x + ((segment.align_width - segment.area.Width) * 0.5);
                  else if ((stack_style.top()->options & FSO::ALIGN_RIGHT) != FSO::NIL) table->x = table->x + segment.align_width - segment.area.Width;
               }

               stack_vp.push(Viewport);
               if (!(Viewport = *table->viewport)) return;

               Viewport->setFields(fl::X(table->x), fl::Y(table->y), fl::Width(table->width), fl::Height(table->height));

               // To build sophisticated table grids, we allocate a single VectorPath that
               // the table, rows and cells contribute to.  This ensures efficiency and consistency
               // in the final result.

               if ((!table->fill.empty()) or (!table->stroke.empty())) {
                  table->seq.push_back({ .Type = PE::Move, .X = 0, .Y = 0 });
                  table->seq.push_back({ .Type = PE::HLineRel, .X = table->width, });
                  table->seq.push_back({ .Type = PE::VLineRel, .Y = table->height });
                  table->seq.push_back({ .Type = PE::HLineRel, .X = -table->width, });
                  table->seq.push_back({ .Type = PE::ClosePath });
               }
               break;
            }

            case SCODE::TABLE_END: {
               auto &table = stack_table.top();
               table->path->setCommand(table->seq.size(), table->seq.data(),
                  table->seq.size() * sizeof(PathCommand));
               table->seq.clear();

               Viewport = stack_vp.top();
               stack_vp.pop();

               stack_table.pop();
               break;
            }

            case SCODE::ROW: {
               stack_row.push(&segment.stream->lookup<bc_row>(cursor));
               auto row = stack_row.top();
               if (!row->rect_fill.empty()) {
                  row->rect_fill->setFields(fl::X(0),
                     fl::Y(row->y - stack_table.top()->y),
                     fl::Width(stack_table.top()->width),
                     fl::Height(row->row_height));
               }
               break;
            }

            case SCODE::ROW_END:
               stack_row.pop();
               break;

            case SCODE::CELL: {
               // If a cell defines fill/stroke values then it gets an independent rectangle to achieve that.
               //
               // If it defines a border and the stroke is undefined, we can use the table's VectorPath object, which is
               // more efficient and creates consistent looking output.

               auto &cell = segment.stream->lookup<bc_cell>(cursor);
               auto table = stack_table.top();

               if ((cell.width >= 1) and (cell.height >= 1)) {
                  cell.viewport->setFields(fl::X(cell.x), fl::Y(cell.y),
                     fl::Width(cell.width), fl::Height(cell.height));

                  if (!cell.rect_fill.empty()) {
                     if (!cell.fill.empty()) cell.rect_fill->setFields(fl::Fill(cell.fill));
                     else cell.rect_fill->setFields(fl::Fill(nullptr));
                  }

                  if (!cell.stroke.empty()) {
                     cell.border_path->setFields(fl::Stroke(cell.stroke), fl::StrokeWidth(cell.stroke_width.px(*this)));
                  }

                  if (cell.border != CB::NIL) {
                     if (cell.stroke.empty()) {
                        // When a cell defines a border value without a stroke, it piggy-backs the table's stroke definition
                        apply_border_to_path(cell.border, table->seq, FloatRect { cell.x, cell.y, cell.width, cell.height });
                     }
                     else {
                        std::vector<PathCommand> seq;
                        apply_border_to_path(cell.border, seq, FloatRect { 0, 0, cell.width, cell.height });
                        cell.border_path->setCommand(seq.size(), seq.data(), seq.size() * sizeof(PathCommand));
                     }
                  }

                  gen_scene_graph(*cell.viewport, cell.segments);

                  Self->VPToEntity.emplace(cell.viewport.id, vp_to_entity { &cell });
               }
               else if (!cell.rect_fill.empty()) cell.rect_fill->setFields(fl::Stroke(nullptr), fl::Fill(nullptr));

               break;
            }

            case SCODE::LINK: {
               auto link = &segment.stream->lookup<bc_link>(cursor);

               stack_ui_link.push(ui_link {
                  .origin = *link,
                  .area   = { x_advance, segment.area.Y, segment.area.Width - x_advance, segment.area.Height },
                  .cursor_start = cursor,
                  .stream = segment.stream
               });

               // Font management

               link->font.fill = link->fill; // Reset the fill instruction to the default
               stack_style.push(&link->font);
               if (Self->HasFocus) {
                  // Override the default link colour if the link has the tab key's focus
                  if ((Self->Tabs[Self->FocusIndex].type IS TT::LINK) and
                        (std::get<BYTECODE>(Self->Tabs[Self->FocusIndex].ref) IS link->uid) and
                        (Self->Tabs[Self->FocusIndex].active)) {
                     link->font.fill = Self->LinkSelectFill;
                  }
                  else if (stack_ui_link.top().hover) {
                     link->font.fill = Self->LinkSelectFill;
                  }
               }

               break;
            }

            case SCODE::LINK_END: {
               auto &ui_link = stack_ui_link.top();
               ui_link.cursor_end = cursor;
               ui_link.area.Width = x_advance - ui_link.area.X;
               if (ui_link.area.Width >= 1) ui_link.append_link();

               // Define the path that represents the clickable area.

               ui_link.origin.path->setCommand(ui_link.path.size(), ui_link.path.data(),
                  ui_link.path.size() * sizeof(PathCommand));

               acMoveToFront(*ui_link.origin.path);

               ui_link.path.clear(); // Return the memory

               Self->Links.emplace_back(ui_link);

               stack_ui_link.pop();
               stack_style.pop();
               break;
            }

            case SCODE::BUTTON: {
               auto &button = segment.stream->lookup<bc_button>(cursor);

               gen_scene_graph(*button.viewport, button.segments);

               double wx, wy;
               if (position_widget(button, segment, Viewport, stack_style.top(), x_advance, 0, true, wx, wy) IS ERR::Okay) {
                  Self->VPToEntity.emplace(button.viewport.id, vp_to_entity { &button });
               }
               break;
            }

            case SCODE::CHECKBOX: {
               auto &checkbox = segment.stream->lookup<bc_checkbox>(cursor);

               double wx, wy;
               if (!checkbox.label.empty()) {
                  if (checkbox.label_pos) {
                     // Right-sided labels can be integrated with the widget so that clicking affects state.
                     if (position_widget(checkbox, segment, Viewport, stack_style.top(), x_advance, checkbox.label_width + checkbox.label_pad.px(*this), true, wx, wy) IS ERR::Okay) {
                        double x, y;
                        auto font = stack_style.top()->get_font();
                        const double avail_space = checkbox.final_height - font->metrics.Descent;
                        y = avail_space - ((avail_space - font->metrics.Height) * 0.5);

                        x = checkbox.final_width + checkbox.label_pad.px(*this);

                        if (checkbox.label_text.empty()) {
                           checkbox.label_text.set(objVectorText::create::global({
                              fl::Name("checkbox_label"),
                              fl::Owner(checkbox.viewport->UID),
                              fl::String(checkbox.label),
                              fl::Face(font->face),
                              fl::FontSize(font->font_size),
                              fl::FontStyle(font->style),
                              fl::Fill(stack_style.top()->fill)
                           }));
                        }

                        if (!checkbox.label_text.empty()) {
                           checkbox.label_text->setFields(fl::X(int(x)), fl::Y(int(y)));
                        }
                     }
                  }
                  else {
                     // Left-sided labels aren't included in the scope of the widget's viewport
                     // TODO: Interactivity is feasible but we'll need to add an input feedback mechanism for that
                     auto font = stack_style.top()->get_font();
                     auto x_label = x_advance;
                     x_advance += checkbox.label_width + checkbox.label_pos;
                     if (position_widget(checkbox, segment, Viewport, stack_style.top(), x_advance, 0, true, wx, wy) IS ERR::Okay) {
                        const double avail_space = checkbox.final_height - font->metrics.Descent;
                        double y = wy + avail_space - ((avail_space - font->metrics.Height) * 0.5);

                        if (checkbox.label_text.empty()) {
                           checkbox.label_text.set(objVectorText::create::global({
                              fl::Name("checkbox_label"),
                              fl::Owner(Viewport->UID),
                              fl::String(checkbox.label),
                              fl::Face(font->face),
                              fl::FontSize(font->font_size),
                              fl::FontStyle(font->style),
                              fl::Fill(stack_style.top()->fill)
                           }));
                        }

                        if (!checkbox.label_text.empty()) {
                           checkbox.label_text->setFields(fl::X(int(x_label)), fl::Y(int(y)));
                        }
                     }
                  }
               }
               else position_widget(checkbox, segment, Viewport, stack_style.top(), x_advance, 0, true, wx, wy);

               if (!checkbox.processed) {
                  checkbox.processed = true;
                  if ((!checkbox.viewport.empty()) and (checkbox.viewport->Scene->SurfaceID)) {
                     checkbox.viewport->subscribeInput(JTYPE::BUTTON|JTYPE::CROSSING, C_FUNCTION(inputevent_checkbox));
                  }
               }

               if (!checkbox.viewport.empty()) Self->VPToEntity.emplace(checkbox.viewport.id, vp_to_entity { &checkbox });
               break;
            }

            case SCODE::COMBOBOX: {
               auto &combo = segment.stream->lookup<bc_combobox>(cursor);
               auto font = stack_style.top()->get_font();

               double wx, wy;
               const double avail_space = combo.final_height - font->metrics.Descent;

               if (!combo.label.empty()) {
                  if (combo.label_pos) {
                     position_widget(combo, segment, Viewport, stack_style.top(), x_advance, 0, true, wx, wy);

                     double y = wy + avail_space - ((avail_space - font->metrics.Height) * 0.5);

                     if (combo.label_text.empty()) {
                        combo.label_text = objVectorText::create::global({
                           fl::Name("combo_label"),
                           fl::Owner(Viewport->UID),
                           fl::String(combo.label),
                           fl::Face(font->face),
                           fl::FontSize(font->font_size),
                           fl::FontStyle(font->style),
                           fl::Fill(stack_style.top()->fill)
                        });
                     }

                     combo.label_text->setFields(fl::X(int(x_advance + combo.label_pad.px(*this))), fl::Y(int(y)));

                     x_advance += combo.label_width + combo.label_pad.px(*this);
                  }
                  else {
                     auto x_label = x_advance;
                     x_advance += combo.label_pad.px(*this) + combo.label_width;

                     position_widget(combo, segment, Viewport, stack_style.top(), x_advance, 0, true, wx, wy);

                     double y = wy + avail_space - ((avail_space - font->metrics.Height) * 0.5);

                     if (combo.label_text.empty()) {
                        combo.label_text = objVectorText::create::global({
                           fl::Name("combo_label"),
                           fl::Owner(Viewport->UID),
                           fl::String(combo.label),
                           fl::Face(font->face),
                           fl::FontSize(font->font_size),
                           fl::FontStyle(font->style),
                           fl::Fill(stack_style.top()->fill)
                        });
                     }

                     combo.label_text->setFields(fl::X(int(x_label)), fl::Y(int(y)));
                  }
               }
               else position_widget(combo, segment, Viewport, stack_style.top(), x_advance, 0, true, wx, wy);

               if (combo.clip_vp.empty()) {
                  // Create the button that will control the drop-down list

                  objVectorViewport::create::global({
                     fl::Name("vp_combo_button"),
                     fl::Owner(combo.viewport->UID),
                     fl::Y(0), fl::XOffset(0), fl::YOffset(0),
                     fl::Width(combo.full_height()) // Button width matches the widget height
                  });

                  combo.viewport->subscribeInput(JTYPE::BUTTON|JTYPE::CROSSING, C_FUNCTION(inputevent_dropdown));
                  combo.viewport->subscribeFeedback(FM::HAS_FOCUS|FM::CHILD_HAS_FOCUS|FM::LOST_FOCUS, C_FUNCTION(combo_feedback));

                  combo.clip_vp = objVectorViewport::create::global({
                     fl::Name("vp_clip_combo"),
                     fl::Owner(combo.viewport->UID),
                     fl::Overflow(VOF::HIDDEN)
                  });

                  double y = avail_space - ((avail_space - font->metrics.Height) * 0.5);

                  combo.input = objVectorText::create::global({
                     fl::Name(combo.name.empty() ? "combo_input" : combo.name), // Required for notify_combo_onchange()
                     fl::Owner(combo.clip_vp->UID),
                     fl::X(0), fl::Y(int(y)),
                     fl::String(combo.value),
                     fl::Cursor(PTC::TEXT),
                     fl::Face(font->face),
                     fl::FontSize(font->font_size),
                     fl::FontStyle(font->style),
                     fl::Fill(combo.font_fill),
                     fl::LineLimit(1),
                     fl::TextFlags(VTXF::EDITABLE),
                     fl::OnChange(C_FUNCTION(notify_combo_onchange))
                  });

                  combo.input->CreatorMeta = *combo.viewport; // Required for notify_combo_onchange()
               }

               if (!combo.clip_vp.empty()) {
                  combo.clip_vp->setFields(fl::X(combo.label_pad.px(*this) * 0.75), fl::Y(0),
                     fl::XOffset(combo.label_pad.px(*this) + (combo.final_height * 0.75)), fl::YOffset(0));
               }

               combo.menu.define_font(font);
               combo.menu.m_ref   = &combo;
               combo.menu.m_style = combo.style;

               Self->VPToEntity.emplace(combo.viewport.id, vp_to_entity { &combo });
               break;
            }

            case SCODE::IMAGE: {
               auto &img = segment.stream->lookup<bc_image>(cursor);
               double wx, wy;
               position_widget(img, segment, Viewport, stack_style.top(), x_advance, 0, false, wx, wy);
               break;
            }

            case SCODE::INPUT: {
               auto &input = segment.stream->lookup<bc_input>(cursor);
               auto font = stack_style.top()->get_font();

               double wx, wy;
               const double avail_space = input.final_height - font->metrics.Descent;

               if (!input.label.empty()) {
                  if (input.label_pos) {
                     position_widget(input, segment, Viewport, stack_style.top(), x_advance, 0, true, wx, wy);

                     double y = wy + avail_space - ((avail_space - font->metrics.Height) * 0.5);

                     if (input.label_text.empty()) {
                        input.label_text = objVectorText::create::global({
                           fl::Name("input_label"),
                           fl::Owner(Viewport->UID),
                           fl::String(input.label),
                           fl::Face(font->face),
                           fl::FontSize(font->font_size),
                           fl::FontStyle(font->style),
                           fl::Fill(stack_style.top()->fill)
                        });
                     }

                     input.label_text->setFields(fl::X(int(x_advance + input.label_pad.px(*this))), fl::Y(int(y)));

                     x_advance += input.label_width + input.label_pad.px(*this);
                  }
                  else {
                     auto x_label = x_advance;
                     x_advance += input.label_pad.px(*this) + input.label_width;

                     position_widget(input, segment, Viewport, stack_style.top(), x_advance, 0, true, wx, wy);

                     double y = wy + avail_space - ((avail_space - font->metrics.Height) * 0.5);

                     if (input.label_text.empty()) {
                        input.label_text = objVectorText::create::global({
                           fl::Name("input_label"),
                           fl::Owner(Viewport->UID),
                           fl::String(input.label),
                           fl::Face(font->face),
                           fl::FontSize(font->font_size),
                           fl::FontStyle(font->style),
                           fl::Fill(stack_style.top()->fill)
                        });
                     }

                     input.label_text->setFields(fl::X(int(x_label)), fl::Y(int(y)));
                  }
               }
               else position_widget(input, segment, Viewport, stack_style.top(), x_advance, 0, true, wx, wy);

               if (input.clip_vp.empty()) {
                  input.clip_vp = objVectorViewport::create::global({
                     fl::Name("vp_clip_input"),
                     fl::Owner(input.viewport->UID),
                     fl::Overflow(VOF::HIDDEN)
                  });

                  auto flags = VTXF::EDITABLE;
                  if (input.secret) flags |= VTXF::SECRET;

                  double y = avail_space - ((avail_space - font->metrics.Height) * 0.5);

                  auto vt = objVectorText::create::global({
                     fl::Name(input.name.empty() ? "input_text" : input.name), // Required for notify_input_onchange()
                     fl::Owner(input.clip_vp->UID),
                     fl::X(0), fl::Y(int(y)),
                     fl::String(input.value),
                     fl::Cursor(PTC::TEXT),
                     fl::Face(font->face),
                     fl::FontSize(font->font_size),
                     fl::FontStyle(font->style),
                     fl::Fill(input.font_fill),
                     fl::LineLimit(1),
                     fl::TextFlags(flags),
                     fl::OnChange(C_FUNCTION(notify_input_onchange))
                  });

                  vt->CreatorMeta = *input.viewport; // Required for notify_input_onchange()
               }

               if (!input.clip_vp.empty()) {
                  input.clip_vp->setFields(fl::X(input.label_pad.px(*this)), fl::Y(0), fl::XOffset(input.label_pad.px(*this)), fl::YOffset(0));
               }

               Self->VPToEntity.emplace(input.viewport.id, vp_to_entity { &input });
               break;
            }

            case SCODE::TEXT: {
               auto &txt = segment.stream->lookup<bc_text>(cursor);
               auto font = stack_style.top()->get_font();

               std::string str;
               if (cursor.index < segment.trim_stop.index) str.append(txt.text, cursor.offset, std::string::npos);
               else str.append(txt.text, cursor.offset, segment.trim_stop.offset - cursor.offset);

               if (!str.empty()) {
                  double x = segment.x(x_advance, stack_style.top()->options);
                  double y = segment.y(stack_style.top()->valign, font);

                  if (auto vt = objVectorText::create::global({
                        fl::Name("doc_text"),
                        fl::Owner(Viewport->UID),
                        fl::X(int(x)), fl::Y(int(y)),
                        fl::String(str),
                        fl::Cursor(PTC::TEXT),
                        fl::Face(font->face),
                        fl::FontSize(font->font_size),
                        fl::FontStyle(font->style),
                        fl::Fill(stack_style.top()->fill),
                        fl::TextFlags(((stack_style.top()->options & FSO::UNDERLINE) != FSO::NIL) ? VTXF::UNDERLINE : VTXF::NIL)
                     })) {

                     Self->UIObjects.push_back(vt->UID);

                     txt.vector_text.push_back(vt);

                     x_advance += vt->get<double>(FID_TextWidth);
                  }
               }
               break;
            }

            default: break;
         } // switch()
      } // for cursor

      if (!stack_ui_link.empty()) {
         auto &link = stack_ui_link.top();
         link.area.Width = x_advance - link.area.X;
         if (link.area.Width >= 1) link.append_link();
      }

   } // for segment
}

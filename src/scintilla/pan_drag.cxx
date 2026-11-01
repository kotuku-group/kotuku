// Drag payloads use dedicated Clipboard objects and never replace the ordinary clipboard.

static std::string receipt_text(std::string_view Text)
{
   std::string result;
   for (auto ch : Text) {
      if (ch IS '&') result += "&amp;";
      else if (ch IS '<') result += "&lt;";
      else if (ch IS '>') result += "&gt;";
      else if (ch IS '\r') result += "&#13;";
      else result += ch;
   }
   return result;
}

static ERR request_editor_drag(objClipboard *Clipboard, OBJECTPTR Requester, int Item, char *Types, APTR)
{
   auto Self = (extScintilla *)CurrentContext();
   if ((not Self->API) or (not Self->API->panDragging()) or (Clipboard != Self->DragClipboard) or (Item != 1))
      return ERR::Cancelled;
   for (int i = 0; i < 4 and Types[i]; i++) {
      if (Types[i] IS char(DATA::TEXT)) {
         std::string_view text(Self->DragText);
         auto receipt = "<receipt id=\"" + std::to_string(Item) + "\"><text>" + receipt_text(text) +
            "</text></receipt>";
         return acDataFeed(Requester, Clipboard, DATA::RECEIPT,
            std::span<const int8_t>((const int8_t *)receipt.data(), receipt.size()));
      }
   }
   return ERR::NoSupport;
}

static void end_editor_drag(objClipboard *, int, ERR, APTR)
{
   auto Self = (extScintilla *)CurrentContext();
   if (Self->API) Self->API->panEndDrag();
}

void ScintillaKTK::StartDrag()
{
   if ((not drag.s) or (drag.len <= 1) or scintilla->KeyAlt) { panEndDrag(); return; }
   if (not scintilla->DragClipboard) {
      kt::SwitchContext context(scintilla);
      objClipboard::create clipboard = { fl::Owner(scintilla->UID), fl::Flags(CPF::DRAG_DROP) };
      if (not clipboard.ok()) { panEndDrag(); return; }
      clipboard->setRequestHandler(C_FUNCTION(request_editor_drag));
      clipboard->setDragEndHandler(C_FUNCTION(end_editor_drag));
      scintilla->DragClipboard = clipboard.detach();
   }
   scintilla->DragText.assign(drag.s, drag.len - 1);
   drag_selection = sel;
   drag_revision = scintilla->DocumentRevision;
   const char datatype = char(DATA::TEXT);
   if (gfx::StartCursorDrag(scintilla->DragClipboard->UID, 1, std::string_view(&datatype, 1), 0) != ERR::Okay) {
      panEndDrag();
      return;
   }
   drag_active = true;
   inDragDrop = ddDragging;
   SetMouseCapture(false);
}

void ScintillaKTK::panEndDrag()
{
   drag_active = false;
   inDragDrop = ddNone;
   SetDragPosition(Scintilla::SelectionPosition());
   drag.Free();
   scintilla->DragText.clear();
   drag_selection.Clear();
   SetMouseCapture(false);
   Redraw();
}

int ScintillaKTK::panDropPosition(double X, double Y)
{
   return SPositionFromLocation(Scintilla::Point(int(std::floor(X)), int(std::floor(Y))),
      false, false, false).Position();
}

ERR ScintillaKTK::panDropText(int Position, std::string_view Text, extScintilla *Source)
{
   if (((scintilla->Flags & (SCIF::EDIT | SCIF::DISABLED)) != SCIF::EDIT) or pdoc->IsReadOnly()) {
      pdoc->CheckReadOnly();
      return ERR::ReadOnly;
   }
   if ((Position < 0) or (Position > pdoc->Length()) or Text.empty()) return ERR::NoData;
   if (Text.find('\0') != std::string_view::npos) return ERR::InvalidData;

   auto source = Source ? Source->API : nullptr;
   if (source and ((not source->drag_active) or (Source->DocumentRevision != source->drag_revision))) {
      return ERR::Cancelled;
   }
   // Modifiers are tracked by the source while the pointer may be over an unfocused destination.
   const bool copy = source and (Source->KeyCtrl or source->pdoc->IsReadOnly() or
      ((Source->Flags & SCIF::EDIT) IS SCIF::NIL));
   if ((source and Source->KeyAlt) or (not source and scintilla->KeyAlt)) return ERR::Cancelled;
   if (source and not copy) {
      for (auto &range : source->drag_selection.RangesCopy()) {
         if (source->RangeContainsProtected(range.Start().Position(), range.End().Position())) return ERR::ReadOnly;
      }
   }

   int length = 0;
   std::unique_ptr<char[]> content(Scintilla::Document::TransformLineEnds(&length, Text.data(), Text.size(),
      pdoc->eolMode));
   const bool rectangular = source and source->drag.rectangular;
   const auto revision = scintilla->DocumentRevision;
   {
      Scintilla::UndoGroup undo(pdoc);
      if (source IS this) {
         sel = drag_selection;
         if ((not rectangular) and (sel.Count() > 1)) {
            int removed_before = 0;
            for (auto &range : sel.RangesCopy()) {
               if ((Position > range.Start().Position()) and (Position < range.End().Position()))
                  return ERR::NothingDone;
               if ((not copy) and (Position >= range.Start().Position()) and (Position <= range.End().Position()))
                  return ERR::NothingDone;
               if (Position >= range.End().Position()) removed_before += range.Length();
            }
            if (not copy) { ClearSelection(true); Position -= removed_before; }
            if (pdoc->InsertString(Position, content.get(), length)) SetSelection(Position + length, Position);
         }
         else DropAt(Scintilla::SelectionPosition(Position), content.get(), not copy, rectangular);
      }
      else {
         if (rectangular) PasteRectangular(Scintilla::SelectionPosition(Position), content.get(), length);
         else if (pdoc->InsertString(Position, content.get(), length)) SetSelection(Position + length, Position);
      }
   }
   if (scintilla->DocumentRevision IS revision) return ERR::NothingDone;
   if (source and (source != this) and not copy) {
      // The destination has committed.  Only now remove the frozen source selection, in one source undo action.
      Scintilla::UndoGroup undo(source->pdoc);
      source->sel = source->drag_selection;
      source->ClearSelection(true);
      calc_longest_line(Source);
      source->Redraw();
   }
   calc_longest_line(scintilla);
   Redraw();
   return ERR::Okay;
}

void ScintillaKTK::panPrimaryPaste(double X, double Y)
{
   if (((scintilla->Flags & (SCIF::EDIT | SCIF::DISABLED)) != SCIF::EDIT) or pdoc->IsReadOnly()) return;
   // Negative items distinguish primary reads from ordinary drop item numbers.
   if (scintilla->PrimaryItem IS INT_MIN) scintilla->PrimaryItem = 0;
   scintilla->PrimaryDrop = { 0, --scintilla->PrimaryItem, panDropPosition(X, Y), scintilla->DocumentRevision, true };
   if (gfx::ReadPrimarySelection(scintilla->UID, scintilla->PrimaryDrop.item) != ERR::Okay)
      scintilla->PrimaryDrop.active = false;
}

static ERR receive_drop(extScintilla *Self, struct acDataFeed *Args)
{
   auto &operation = Args->Object ? Self->Drop : Self->PrimaryDrop;
   if ((not Self->API) or (not operation.active)) return ERR::Cancelled;
   if ((Args->Object ? Args->Object->UID : 0) != operation.source) return ERR::Cancelled;
   const auto pending = operation;
   std::vector<std::string> paths;
   std::string text;
   {
      objXML::create xml = { fl::Statement(
         std::string_view((const char *)Args->Buffer.data(), Args->Buffer.size())) };
      if (not xml.ok()) { operation.active = false; return ERR::InvalidData; }
      if ((xml->Tags.size() != 1) or not iequals(xml->Tags[0].name(), "receipt")) {
         operation.active = false;
         return ERR::InvalidData;
      }
      auto &root = xml->Tags[0];
      bool matched = false;
      for (auto &attribute : root.Attribs) {
         if (iequals(attribute.Name, "id")) matched = attribute.Value IS std::to_string(pending.item);
      }
      if (not matched) return ERR::Cancelled;
      operation.active = false; // Consume before calling client code, including rejected or malformed receipts.
      if ((pending.revision != Self->DocumentRevision) or ((Self->Flags & SCIF::DISABLED) != SCIF::NIL)) {
         return ERR::Cancelled;
      }

      for (auto &tag : root.Children) {
         if (iequals(tag.name(), "file")) {
            for (auto &attribute : tag.Attribs) {
               if (iequals(attribute.Name, "path") and not attribute.Value.empty()) paths.push_back(attribute.Value);
            }
         }
         else if (iequals(tag.name(), "text")) {
            for (auto &child : tag.Children) if (child.isContent()) text += child.Attribs[0].Value;
         }
      }
   } // Release the parser before a callback can destroy its owner.
   if (not paths.empty()) {
      auto callback = Self->FileDrop;
      if (not callback.defined() or callback.stale()) return ERR::Cancelled;
      callback.pin();
      const auto unpin = kt::Defer([&]() { callback.unpin(); });
      const auto editor_id = Self->UID;
      for (auto &path : paths) {
         if (callback.stale()) break;
         if (auto pointer = gfx::AccessPointer()) { pointer->setDragFeedback(1); ReleaseObject(pointer); }
         if (callback.isC()) {
            kt::SwitchContext ctx(callback.Context);
            auto routine = (void (*)(extScintilla *, CSTRING, APTR))callback.Routine;
            routine(Self, path.c_str(), callback.Meta);
         }
         else {
            ERR result;
            if (sc::Call(callback, std::to_array<ScriptArg>({
                  { "Scintilla", Self, FD_OBJECTPTR }, { "Path", path.c_str() }
               }), result) != ERR::Okay) break;
         }
         if (CheckResourceExists(editor_id) != ERR::True) return ERR::Okay;
      }
      return ERR::Okay;
   }

   extScintilla *source = nullptr;
   if (Args->Object and (Args->Object->classID() IS CLASSID::CLIPBOARD)) {
      auto owner = Args->Object->Owner;
      if (owner and (owner->classID() IS CLASSID::SCINTILLA)) {
         auto editor = (extScintilla *)owner;
         if (editor->DragClipboard IS Args->Object) source = editor;
      }
   }
   auto result = Self->API->panDropText(pending.position, text, source);
   if ((result IS ERR::Okay) and Self->Focus) acFocus(Self->Focus);
   if (pending.source) {
      if (auto pointer = gfx::AccessPointer()) {
         pointer->setDragFeedback(result IS ERR::Okay ? 1 : 0);
         ReleaseObject(pointer);
      }
   }
   return result;
}

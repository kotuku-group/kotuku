//********************************************************************************************************************
// For a given index in the stream, return the element code.  Index MUST be a valid reference to a byte code sequence.

template <class T> T & RSTREAM::lookup(const stream_char Index) {
   return std::get<T>(codes[data[Index.index].uid]);
}

template <class T> T & RSTREAM::lookup(const INDEX Index) {
   return std::get<T>(codes[data[Index].uid]);
}

//********************************************************************************************************************
// Inserts a byte code sequence into the text stream.  Makes a copy of the incoming Code object.

template <class T> T & RSTREAM::insert(stream_char &Cursor, T &Code)
{
   // All byte codes are saved to a global container.

   if (codes.contains(Code.uid)) {
      // Sanity check - is the UID unique?  The caller probably needs to utilise glByteCodeID++
      // TODO: At some point the re-use of codes should be allowed, e.g. bc_font reversions would benefit from this.
      kt::Log log(__FUNCTION__);
      log.warning("Code #%d is already registered.", Code.uid);
   }
   else codes[Code.uid] = Code; // Intentional copy

   if (Cursor.index IS INDEX(data.size())) {
      data.emplace_back(Code.code, Code.uid);
   }
   else {
      const stream_code insert(Code.code, Code.uid);
      data.insert(data.begin() + Cursor.index, insert);
   }
   Cursor.next_code();
   return std::get<T>(codes[Code.uid]);
}

// Identical to insert() but utilises std::move as an optimisation

template <class T> T & RSTREAM::emplace(stream_char &Cursor, T &Code)
{
   // All byte codes are saved to a global container.

   auto uid     = Code.uid;
   auto code_no = Code.code;

   if (codes.contains(uid)) {
      kt::Log log(__FUNCTION__);
      log.warning("Code #%d is already registered.", uid);
   }
   else codes[uid] = std::move(Code);

   if (Cursor.index IS INDEX(data.size())) {
      data.emplace_back(code_no, uid);
   }
   else {
      const stream_code insert(code_no, uid);
      data.insert(data.begin() + Cursor.index, insert);
   }

   Cursor.next_code();
   return std::get<T>(codes[uid]);
}

// Optimal construction of new stream codes in-place.

template <class T> T & RSTREAM::emplace(stream_char &Cursor)
{
   auto key = glByteCodeID.fetch_add(1, std::memory_order_relaxed);
   glReservedByteCodeID = key;

   auto it = codes.emplace(key, std::in_place_type<T>); // Construct the desired code in place
   if (glReservedByteCodeID IS key) glReservedByteCodeID = 0;
   auto &result = std::get<T>(it.first->second);

   if (Cursor.index IS INDEX(data.size())) {
      data.emplace_back(result.code, result.uid);
   }
   else {
      const stream_code insert(result.code, result.uid);
      data.insert(data.begin() + Cursor.index, insert);
   }
   Cursor.next_code();
   return result;
}

//********************************************************************************************************************

void stream_char::erase_char(RSTREAM &Stream) // Erase a character OR an escape code.
{
   if (Stream[index].code IS SCODE::TEXT) {
      auto &text = Stream.lookup<bc_text>(index);
      if (offset < text.text.size()) {
         text.text.erase(offset, 1);
         text.invalidate_tokens();
      }
      if (offset > text.text.size()) offset = text.text.size();
   }
   else Stream.data.erase(Stream.data.begin() + index);
}

//********************************************************************************************************************
// Retrieve the first available character.  Assumes that the position is valid.  Does not support unicode!

uint8_t stream_char::get_char(RSTREAM &Stream)
{
   auto idx = index;
   auto seek = offset;
   while (size_t(idx) < Stream.data.size()) {
      if (Stream.data[idx].code IS SCODE::TEXT) {
         auto &text = Stream.lookup<bc_text>(idx);
         if (seek < text.text.size()) return text.text[seek];
         else seek = 0; // The current character offset isn't valid, reset it.
      }
      idx++;
   }
   return 0;
}

//********************************************************************************************************************
// Retrieve the first character after seeking past N viable characters (forward only).  Does not support unicode!

uint8_t stream_char::get_char(RSTREAM &Stream, INDEX Seek)
{
   auto idx = index;
   auto off = offset;

   while (idx < std::ssize(Stream)) {
      if (Stream[idx].code IS SCODE::TEXT) {
         auto &text = Stream.lookup<bc_text>(idx);
         if (off + Seek < text.text.size()) return text.text[off + Seek];
         Seek -= text.text.size() - off;
         off = 0;
      }
      idx++;
   }

   return 0;
}

void stream_char::next_char(RSTREAM &Stream)
{
   if (Stream[index].code IS SCODE::TEXT) {
      auto &text = Stream.lookup<bc_text>(index);
      if (++offset >= text.text.size()) {
         index++;
         offset = 0;
      }
   }
   else if (index < INDEX(Stream.size())) index++;
}

//********************************************************************************************************************
// Move the cursor to the previous character OR code.

void stream_char::prev_char(RSTREAM &Stream)
{
   if (offset > 0) { // If the offset is defined then the indexed code is TEXT
      offset--;
      return;
   }

   if (index > 0) {
      index--;
      if (Stream[index].code IS SCODE::TEXT) {
         offset = Stream.lookup<bc_text>(index).text.size()-1;
      }
      else offset = 0;
   }
   else offset = 0;
}

//********************************************************************************************************************
// Return the previous printable character for a given position.  Does not support unicode!  Non-text codes are
// completely ignored.

uint8_t stream_char::get_prev_char(RSTREAM &Stream)
{
   if ((offset > 0) and (Stream[index].code IS SCODE::TEXT)) {
      return Stream.lookup<bc_text>(index).text[offset-1];
   }

   for (auto i=index-1; i >= 0; i--) {
      if (Stream[i].code IS SCODE::TEXT) {
         return Stream.lookup<bc_text>(i).text.back();
      }
   }

   return 0;
}

//********************************************************************************************************************
// Return the previous printable character for a given position.  Inline graphics are considered characters but will
// be returned as 0xff.

uint8_t stream_char::get_prev_char_or_inline(RSTREAM &Stream)
{
   if ((offset > 0) and (Stream[index].code IS SCODE::TEXT)) {
      return Stream.lookup<bc_text>(index).text[offset-1];
   }

   for (auto i=index-1; i >= 0; i--) {
      if (Stream[i].code IS SCODE::TEXT) {
         return Stream.lookup<bc_text>(i).text.back();
      }
      else if (Stream[i].code IS SCODE::IMAGE) {
         auto &widget = Stream.lookup<bc_image>(i);
         if (!widget.floating_x()) return 0xff;
      }
      else if (Stream[i].code IS SCODE::BUTTON) {
         auto &widget = Stream.lookup<bc_button>(i);
         if (!widget.floating_x()) return 0xff;
      }
      else if (Stream[i].code IS SCODE::CHECKBOX) {
         auto &widget = Stream.lookup<bc_checkbox>(i);
         if (!widget.floating_x()) return 0xff;
      }
      else if (Stream[i].code IS SCODE::COMBOBOX) {
         auto &widget = Stream.lookup<bc_combobox>(i);
         if (!widget.floating_x()) return 0xff;
      }
      else if (Stream[i].code IS SCODE::INPUT) {
         auto &widget = Stream.lookup<bc_input>(i);
         if (!widget.floating_x()) return 0xff;
      }
   }

   return 0;
}

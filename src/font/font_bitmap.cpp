
//********************************************************************************************************************
// Win32 font structures

struct winFont {
   int Offset, Size;
};

struct winmz_header_fields {
   uint16_t magic;
   uint8_t data[29 * 2];
   uint32_t lfanew;
};

struct winne_header_fields {
   uint16_t magic;
   uint8_t data[34];
   uint16_t resource_tab_offset;
   uint16_t rname_tab_offset;
};

PACK(struct winfnt_header_fields {
   uint16_t version;
   uint32_t file_size;
   char copyright[60];
   uint16_t file_type;
   uint16_t nominal_point_size;     // Point size
   uint16_t vertical_resolution;
   uint16_t horizontal_resolution;
   uint16_t ascent;                 // The amount of pixels above the base-line
   uint16_t internal_leading;       // top leading pixels
   uint16_t external_leading;       // gutter
   int8_t   italic;                 // TRUE if font is italic
   int8_t   underline;              // TRUE if font is underlined
   int8_t   strike_out;             // TRUE if font is striked-out
   uint16_t weight;                 // Indicates font boldness
   int8_t   charset;
   uint16_t pixel_width;
   uint16_t pixel_height;
   int8_t   pitch_and_family;
   uint16_t avg_width;
   uint16_t max_width;
   uint8_t  first_char;
   uint8_t  last_char;
   uint8_t  default_char;
   uint8_t  break_char;
   uint16_t bytes_per_row;
   uint32_t device_offset;
   uint32_t face_name_offset;
   uint32_t bits_pointer;
   uint32_t bits_offset;
   int8_t   reserved;
   uint32_t flags;
   uint16_t A_space;
   uint16_t B_space;
   uint16_t C_space;
   uint16_t color_table_offset;
   int8_t   reservedend[4];
});

#define ID_WINMZ  0x5A4D
#define ID_WINNE  0x454E

//********************************************************************************************************************

static ERR validate_winfnt_header(const winfnt_header_fields &Header, std::string_view Path)
{
   kt::Log log(__FUNCTION__);

   // NOTE: 0x100 indicates the Microsoft vector font format, which we do not support.

   if ((Header.version != 0x200) and (Header.version != 0x300)) {
      if (not Path.empty()) {
         log.warning("Font \"%.*s\" is written in unsupported version %d / $%x.", int(Path.size()), Path.data(),
            Header.version, Header.version);
      }
      return ERR::NoSupport;
   }

   if (Header.file_type & 1) {
      if (not Path.empty()) {
         log.warning("Font \"%.*s\" is in the non-supported vector font format.", int(Path.size()), Path.data());
      }
      return ERR::NoSupport;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Reads the Windows .fon resource table and returns each embedded bitmap font entry.

static ERR read_winfont_entries(objFile *File, std::vector<winFont> &Fonts)
{
   if (not File) return ERR::NullArgs;

   Fonts.clear();

   winmz_header_fields mz_header;
   if (File->read(&mz_header, sizeof(mz_header)) != ERR::Okay) return ERR::Read;
   if (mz_header.magic != ID_WINMZ) return ERR::NoSupport;

   File->seekStart(mz_header.lfanew);

   winne_header_fields ne_header;
   if ((File->read(&ne_header, sizeof(ne_header)) != ERR::Okay) or (ne_header.magic != ID_WINNE)) {
      return ERR::NoSupport;
   }

   File->seekStart(mz_header.lfanew + ne_header.resource_tab_offset);

   uint16_t size_shift = 0;
   if (fl::ReadLE(File, &size_shift) != ERR::Okay) return ERR::Read;

   uint16_t font_count = 0;
   int font_offset = 0;
   uint16_t type_id = 0;
   ERR error = fl::ReadLE(File, &type_id);

   while ((error IS ERR::Okay) and (type_id)) {
      uint16_t count = 0;
      if (fl::ReadLE(File, &count) != ERR::Okay) return ERR::Read;

      if (type_id IS 0x8008) {
         font_count = count;
         File->get(FID_Position, font_offset);
         font_offset += 4;
         break;
      }

      File->seekCurrent(4 + count * 12);
      error = fl::ReadLE(File, &type_id);
   }

   if (error != ERR::Okay) return error;
   if ((not font_count) or (not font_offset)) return ERR::NoData;

   File->seekStart(font_offset);
   Fonts.resize(font_count);

   for (int i=0; i < int(font_count); i++) {
      uint16_t offset = 0, size = 0;
      if (fl::ReadLE(File, &offset) != ERR::Okay) return ERR::Read;
      if (fl::ReadLE(File, &size) != ERR::Okay) return ERR::Read;
      Fonts[i].Offset = offset<<size_shift;
      Fonts[i].Size   = size<<size_shift;
      File->seekCurrent(8);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static FTF font_style_flags(const std::string_view Style)
{
   if (iequals("Bold", Style)) return FTF::BOLD;
   else if (iequals("Italic", Style)) return FTF::ITALIC;
   else if (iequals("Bold Italic", Style)) return FTF::BOLD|FTF::ITALIC;
   else return FTF::NIL;
}

//********************************************************************************************************************
// Structure definition for cached bitmap fonts.

class BitmapCache {
private:
   std::vector<uint8_t> mOutline;

public:
   std::vector<uint8_t> mData;
   winfnt_header_fields Header;
   FontCharacter Chars[256];
   std::string Path;
   int16_t OpenCount;
   FTF StyleFlags;
   ERR Result;

   BitmapCache(const winfnt_header_fields &Face, std::string_view RequestedStyle, std::string_view SourcePath,
      objFile *File, const winFont &WinFont) :
      Header(Face),
      Path(SourcePath),
      OpenCount(0),
      StyleFlags(font_style_flags(RequestedStyle)),
      Result(ERR::Okay)
   {
      kt::Log log(__FUNCTION__);

      log.branch("Caching font %.*s : %d : %.*s", int(SourcePath.size()), SourcePath.data(),
         Face.nominal_point_size, int(RequestedStyle.size()), RequestedStyle.data());

      // Read character information from the file

      if (File->seek(WinFont.Offset + 118, SEEK::START) != ERR::Okay) {
         Result = log.warning(ERR::Read);
         return;
      }

      clearmem(Chars, sizeof(Chars));
      if (Face.version IS 0x300) {
         int j = Face.first_char;
         for (int i=0; i < Face.last_char - Face.first_char + 1; i++) {
            uint16_t width;
            uint32_t offset;

            if (fl::ReadLE(File, &width) != ERR::Okay) {
               Result = log.warning(ERR::Read);
               return;
            }
            if (fl::ReadLE(File, &offset) != ERR::Okay) {
               Result = log.warning(ERR::Read);
               return;
            }
            if ((width > 0x7fff) or (offset < Face.bits_offset)) {
               Result = log.warning(ERR::InvalidData);
               return;
            }

            Chars[j].Width   = width;
            Chars[j].Advance = Chars[j].Width;
            Chars[j].Offset  = offset - Face.bits_offset;
            j++;
         }
      }
      else {
         int j = Face.first_char;
         for (int i=0; i < Face.last_char - Face.first_char + 1; i++) {
            uint16_t width, offset;
            if (fl::ReadLE(File, &width) != ERR::Okay) {
               Result = log.warning(ERR::Read);
               return;
            }
            if (fl::ReadLE(File, &offset) != ERR::Okay) {
               Result = log.warning(ERR::Read);
               return;
            }
            if ((width > 0x7fff) or (uint32_t(offset) < Face.bits_offset)) {
               Result = log.warning(ERR::InvalidData);
               return;
            }

            Chars[j].Width   = width;
            Chars[j].Advance = Chars[j].Width;
            Chars[j].Offset  = offset - Face.bits_offset;
            j++;
         }
      }

      if (Face.bits_offset >= Face.file_size) {
         Result = log.warning(ERR::InvalidData);
         return;
      }

      uint32_t data_size = Face.file_size - Face.bits_offset;
      if (data_size > 0x7fffffff) {
         Result = log.warning(ERR::InvalidData);
         return;
      }

      int size = int(data_size);

      if (size > 0) {
         mData.resize(size);
         int result;
         if (File->seek(WinFont.Offset + Face.bits_offset, SEEK::START) != ERR::Okay) {
            Result = log.warning(ERR::Read);
            return;
         }

         if ((File->read(mData.data(), size, &result) IS ERR::Okay) and (result IS size)) {
            for (int16_t i=0; i < 256; i++) {
               if (!Chars[i].Width) continue;

               uint32_t bytewidth = uint32_t((Chars[i].Width+7)>>3);
               uint32_t char_size = bytewidth * Header.pixel_height;

               if ((Chars[i].Offset >= data_size) or (char_size > data_size - Chars[i].Offset)) {
                  Result = log.warning(ERR::InvalidData);
                  return;
               }
            }

            // Convert the graphics format for wide characters from column-first format to row-first format.

            for (int16_t i=0; i < 256; i++) {
               if (!Chars[i].Width) continue;

               int sz = ((Chars[i].Width+7)>>3) * Face.pixel_height;
               if (Chars[i].Width > 8) {
                  auto buffer = std::make_unique<uint8_t[]>(sz);
                  clearmem(buffer.get(), sz);

                  uint8_t *gfx = mData.data() + Chars[i].Offset;
                  int bytewidth = (Chars[i].Width + 7)>>3;
                  int pos = 0;
                  for (int k=0; k < Face.pixel_height; k++) {
                     for (int j=0; j < bytewidth; j++) {
                        buffer[pos++] = gfx[k + (j * Face.pixel_height)];
                     }
                  }

                  copymem(buffer.get(), gfx, pos);
               }
            }
         }
         else {
            Result = log.warning(ERR::Read);
            return;
         }
      }
      else {
         Result = log.warning(ERR::AllocMemory);
         return;
      }

      if (((StyleFlags & FTF::BOLD) != FTF::NIL) and (Header.weight < 600)) {
         log.msg("Converting base font graphics data to bold.");

         int size = 0;
         for (int i=0; i < 256; i++) {
            if (Chars[i].Width) size += Header.pixel_height * ((Chars[i].Width+8)>>3);
         }

         if (size > 0) {
            std::vector<uint8_t> buffer(size);
            int pos = 0;
            for (int i=0; i < 256; i++) {
               if (Chars[i].Width) {
                  uint8_t *gfx = mData.data() + Chars[i].Offset;
                  Chars[i].Offset = pos;

                  // Copy character graphic to the buffer and embolden it

                  int oldwidth = (Chars[i].Width+7)>>3;
                  int newwidth = (Chars[i].Width+8)>>3;
                  for (int y=0; y < Header.pixel_height; y++) {
                     for (int xb=0; xb < oldwidth; xb++) {
                        buffer[pos+xb] |= gfx[xb]|(gfx[xb]>>1);
                        if ((xb + 1 < newwidth) and (gfx[xb] & 0x01)) buffer[pos+xb+1] |= 0x80;
                     }

                     pos += newwidth;
                     gfx += oldwidth;
                  }

                  Chars[i].Width++;
                  Chars[i].Advance++;
               }
            }

            mData = std::move(buffer);
         }
         else {
            Result = log.warning(ERR::AllocMemory);
            return;
         }
      }

      if (((StyleFlags & FTF::ITALIC) != FTF::NIL) and (!Header.italic)) {
         log.msg("Converting base font graphics data to italic.");

         int size = 0;
         int extra = Header.pixel_height>>2;

         for (int i=0; i < 256; i++) {
            if (Chars[i].Width) size += Header.pixel_height * ((Chars[i].Width+7+extra)>>3);
         }

         if (size > 0) {
            std::vector<uint8_t> buffer(size);
            int pos = 0;
            for (int i=0; i < 256; i++) {
               if (Chars[i].Width) {
                  uint8_t *gfx = mData.data() + Chars[i].Offset;
                  Chars[i].Offset = pos;

                  int oldwidth = (Chars[i].Width+7)>>3;
                  int newwidth = (Chars[i].Width+7+extra)>>3;
                  int italic = Header.pixel_height;
                  uint8_t *dest = buffer.data() + pos;
                  for (int y=0; y < Header.pixel_height; y++) {
                     int dx = italic>>2;
                     for (int sx=0; sx < Chars[i].Width; sx++) {
                        if (gfx[sx>>3] & (0x80>>(sx & 0x07))) {
                           dest[dx>>3] |= (0x80>>(dx & 0x07));
                        }
                        dx++;
                     }

                     pos  += newwidth;
                     dest += newwidth;
                     gfx  += oldwidth;
                     italic--;
                  }

                  Chars[i].Width += extra;
               }
            }

            mData = std::move(buffer);
         }
         else {
            Result = log.warning(ERR::AllocMemory);
            return;
         }
      }
   }

   uint8_t * get_outline()
   {
      CACHE_LOCK lock(glCacheMutex);

      if (not mOutline.empty()) return mOutline.data();

      int size = 0;
      for (int16_t i=0; i < 256; i++) {
         if (Chars[i].Width) size += (Header.pixel_height+2) * ((Chars[i].Width+9)>>3);
      }

      if (size <= 0) return nullptr;

      std::vector<uint8_t> buffer(size);

      int pos = 0;
      for (int16_t i=0; i < 256; i++) {
         if (Chars[i].Width) {
            auto gfx = mData.data() + Chars[i].Offset;
            Chars[i].OutlineOffset = pos;

            int oldwidth = (Chars[i].Width+7)>>3;
            int newwidth = (Chars[i].Width+9)>>3;

            auto dest = buffer.data() + pos;

            dest += newwidth; // Start ahead of line 0
            for (int sy=0; sy < Header.pixel_height; sy++) {
               int dx = 1;
               for (int sx=0; sx < Chars[i].Width; sx++) {
                  if (gfx[sx>>3] & (0x80>>(sx & 0x07))) {
                     if ((sx >= Chars[i].Width-1) or (!(gfx[(sx+1)>>3] & (0x80>>((sx+1) & 0x07))))) dest[(dx+1)>>3] |= (0x80>>((dx+1) & 0x07));
                     if ((sx IS 0) or (!(gfx[(sx-1)>>3] & (0x80>>((sx-1) & 0x07))))) dest[(dx-1)>>3] |= (0x80>>((dx-1) & 0x07));
                     if ((sy < 1) or (!(gfx[(sx>>3)-oldwidth] & (0x80>>(sx & 0x07))))) dest[(dx>>3)-newwidth] |= (0x80>>(dx & 0x07));
                     if ((sy >= Header.pixel_height-1) or (!(gfx[(sx>>3)+oldwidth] & (0x80>>(sx & 0x07))))) dest[(dx>>3)+newwidth] |= (0x80>>(dx & 0x07));
                  }
                  dx++;
               }

               pos  += newwidth;
               dest += newwidth;
               gfx  += oldwidth;
            }
            pos += newwidth * 2;
         }
      }

      mOutline = std::move(buffer);
      return mOutline.data();
   }

   ~BitmapCache() {
      if (OpenCount) {
         kt::Log log(__FUNCTION__);
         log.warning("Removing \"%s : %d : $%.8x\" with an open count of %d", Path.c_str(), Header.nominal_point_size, int(StyleFlags), OpenCount);
      }
   }
};

static std::list<BitmapCache> glBitmapCache;
static APTR glCacheTimer = nullptr;

//********************************************************************************************************************
// Assumes a cache lock is held on being called.

static BitmapCache * check_bitmap_cache(extFont *Self, FTF Style)
{
   kt::Log log(__FUNCTION__);

   for (auto & cache : glBitmapCache) {
      if (cache.Result != ERR::Okay) continue;

      if (iequals(cache.Path, Self->Path)) {
         if (cache.StyleFlags IS Style) {
            if (Self->Point IS cache.Header.nominal_point_size) {
               log.trace("Exists in cache (count %d) %s : %s", cache.OpenCount, cache.Path.c_str(),
                  Self->Style.c_str());
               return &cache;
            }
            else log.trace("Failed point check %.2f / %d", Self->Point, cache.Header.nominal_point_size);
         }
         else log.trace("Failed style check $%.8x != $%.8x", Style, cache.StyleFlags);
      }
   }

   return nullptr;
}

//********************************************************************************************************************

ERR bitmap_cache_cleaner(OBJECTPTR Subscriber, int64_t Elapsed, int64_t CurrentTime)
{
   kt::Log log(__FUNCTION__);

   log.msg("Checking bitmap font cache for unused fonts...");

   CACHE_LOCK lock(glCacheMutex);
   for (auto it=glBitmapCache.begin(); it != glBitmapCache.end(); ) {
      if (!it->OpenCount) it = glBitmapCache.erase(it);
      else it++;
   }
   glCacheTimer = nullptr;
   return ERR::Terminate;
}

#include "../../defs.h"

namespace display {

static void video_draw_pixel(objBitmap *, int, int, uint32_t) { }
static void video_draw_rgb_pixel(objBitmap *, int, int, RGB8 *) { }
static void video_draw_rgb_index(objBitmap *, uint8_t *, RGB8 *) { }
static uint32_t video_read_pixel(objBitmap *, int, int) { return 0; }
static void video_read_rgb_pixel(objBitmap *, int, int, RGB8 *) { }
static void video_read_rgb_index(objBitmap *, uint8_t *, RGB8 *) { }

void android_install_bitmap_routines(extBitmap *Bitmap)
{
   Bitmap->DrawUCPixel = video_draw_pixel;
   Bitmap->DrawUCRPixel = video_draw_rgb_pixel;
   Bitmap->DrawUCRIndex = video_draw_rgb_index;
   Bitmap->ReadUCPixel = video_read_pixel;
   Bitmap->ReadUCRPixel = video_read_rgb_pixel;
   Bitmap->ReadUCRIndex = video_read_rgb_index;
}

}


#include <mutex>

struct FontCharacter {
   int16_t  Width;
   int16_t  Advance;
   uint32_t Offset;
   uint32_t OutlineOffset;
};

typedef const std::lock_guard<std::recursive_mutex> CACHE_LOCK;
static std::recursive_mutex glCacheMutex; // Protects access to glBitmapCache for multi-threading support

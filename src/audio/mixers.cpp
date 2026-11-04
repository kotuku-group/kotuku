
// Thread-local step value for mixing (temporary solution)
thread_local int glMixStep = 1;

static constexpr double PCM_U8_OFFSET = 128.0;
static constexpr double PCM_U8_SCALE = 1.0 / 128.0;
static constexpr double PCM_S16_SCALE = 1.0 / 32768.0;

// Function to set the mixing step (called from functions.cpp)
void set_mix_step(int Step) {
   glMixStep = Step;
}

template<typename T>
struct SampleTraits;

template<>
struct SampleTraits<uint8_t> {
   static inline double normalize(uint8_t Value) {
      return (double(Value) - PCM_U8_OFFSET) * PCM_U8_SCALE;
   }
};

template<>
struct SampleTraits<int16_t> {
   static inline double normalize(int16_t Value) {
      return double(Value) * PCM_S16_SCALE;
   }
};

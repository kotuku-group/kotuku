// Included by audio.cpp to exercise the module implementation.

#include <chrono>
#include <complex>
#include <limits>

namespace audio_tests_audio_saturator_dsp {

// Configure exactly as activation does: prepare outside the lock, publish, then reset on the render thread.

struct SaturatorFixture {
   extAudioEffect Effect{nullptr, 1};
   SaturatorProcessor Processor;
   ERR Prepared;
   int Channels;

   SaturatorFixture(int Rate, bool Stereo, const SaturatorSettings &Settings) : Processor(&Effect, Settings) {
      Effect.OutputRate = Rate;
      Effect.Stereo = Stereo;
      Effect.ResetPending = false;
      Channels = Stereo ? 2 : 1;
      std::unique_ptr<AudioEffectConfiguration> config;
      Prepared = Processor.prepare(Rate, Stereo, config);
      if (config) config->publish();
      Processor.reset();
   }

   // Parameter updates derive their target on the control thread, as SaturatorUpdate does.
   void change(const SaturatorSettings &Settings) {
      SaturatorTarget target;
      const bool valid = saturator_target(Settings, Processor.SampleRate, target);
      Processor.update(Settings, valid ? &target : nullptr);
   }

   void run(std::vector<float> &Buffer, int Block = 0) {
      const int frames = int(Buffer.size()) / Channels;
      if (Block <= 0) Block = frames;
      for (int i = 0; i < frames; i += Block) {
         Processor.process(Buffer.data() + size_t(i) * Channels, std::min(Block, frames - i));
      }
   }

   void run_range(std::vector<float> &Buffer, int Start, int End) {
      if (End > Start) Processor.process(Buffer.data() + size_t(Start) * Channels, End - Start);
   }
};

static SaturatorSettings settings(double Drive, int Shape, double Gain = 0, double Mix = 100)
{
   return SaturatorSettings { .Drive = Drive, .Shape = Shape, .Gain = Gain, .Mix = Mix };
}

// Hard clipping is exactly linear below unity, so at 0 dB drive and moderate levels the processor is a linear filter.

static SaturatorSettings linear_settings(double Mix = 100)
{
   return settings(0, SATURATE_HARD, 0, Mix);
}

static double curve(double Z, int Shape)
{
   return (Shape IS SATURATE_HARD) ? std::clamp(Z, -1.0, 1.0) : std::tanh(Z);
}

static double noise(uint32_t &Seed)
{
   Seed = Seed * 1664525u + 1013904223u;
   return double(Seed >> 8) / double(1u << 23) - 1.0;
}

static std::vector<float> noise_buffer(int Frames, int Channels, uint32_t Seed, double Level = 0.5)
{
   std::vector<float> buffer(size_t(Frames) * Channels);
   for (auto &sample : buffer) sample = float(noise(Seed) * Level);
   return buffer;
}

static double peak(const std::vector<float> &Buffer)
{
   double result = 0;
   for (auto sample : Buffer) result = std::max(result, double(std::abs(sample)));
   return result;
}

//********************************************************************************************************************
// Zero-phase amplitude response of the symmetric prototype at Frequency, expressed as a fraction of the base rate.

static double prototype_response(double Frequency)
{
   const auto &g = saturator_filter().Prototype;
   constexpr int middle = (SATURATOR_TAPS - 1) / 2;
   const double omega = 2.0 * std::numbers::pi * Frequency / double(SATURATOR_FACTOR);
   double sum = g[middle];
   for (int k = 1; k <= middle; k++) sum += 2.0 * g[middle + k] * std::cos(omega * double(k));
   return sum;
}

//********************************************************************************************************************
// An independent model of the published signal path, evaluated by direct convolution at the oversampled rate with
// unbounded history.  It shares only the prototype coefficients with the processor.

static std::vector<double> reference(const std::vector<float> &Input, int Channels, const SaturatorSettings &Settings)
{
   const auto &g = saturator_filter().Prototype;
   const int frames = int(Input.size()) / Channels;
   const double drive = std::pow(10.0, Settings.Drive / 20.0);
   const double trim = std::pow(10.0, Settings.Gain / 20.0);
   const double m = Settings.Mix / 100.0;

   auto input = [&](int Frame, int Channel) {
      if (Frame < 0) return 0.0;
      const float x = Input[size_t(Frame) * Channels + Channel];
      return std::isfinite(x) ? double(x) : 0.0;
   };

   std::vector<double> output(Input.size(), 0.0);
   std::vector<double> shaped(size_t(frames) * SATURATOR_FACTOR);
   for (int c = 0; c < Channels; c++) {
      for (int i = 0; i < frames * SATURATOR_FACTOR; i++) {
         double up = 0;
         for (int k = 0; k < SATURATOR_TAPS; k++) {
            const int j = i - k;
            if ((j >= 0) and (j % SATURATOR_FACTOR IS 0)) up += 4.0 * g[k] * input(j / SATURATOR_FACTOR, c);
         }
         const double z = up * drive;
         shaped[i] = curve(z, Settings.Shape);
      }

      for (int n = 0; n < frames; n++) {
         double wet = 0;
         for (int k = 0; k < SATURATOR_TAPS; k++) {
            const int j = n * SATURATOR_FACTOR - k;
            if (j >= 0) wet += g[k] * shaped[j];
         }
         output[size_t(n) * Channels + c] = (1.0 - m) * input(n - SATURATOR_LATENCY, c) + m * trim * wet;
      }
   }
   return output;
}

static double max_error(const std::vector<float> &Actual, const std::vector<double> &Expected)
{
   double error = 0;
   for (size_t i = 0; i < Actual.size(); i++) {
      error = std::max(error, std::abs(double(Actual[i]) - Expected[i]) / std::max(1.0, std::abs(Expected[i])));
   }
   return error;
}

//********************************************************************************************************************

static void test_configuration(AudioTestContext &Test)
{
   SaturatorFixture low(7999, false, SaturatorSettings());
   AUDIO_CHECK(low.Prepared IS ERR::NoSupport);
   SaturatorFixture high(192001, true, SaturatorSettings());
   AUDIO_CHECK(high.Prepared IS ERR::NoSupport);

   SaturatorFixture slow(8000, false, SaturatorSettings()), fast(192000, true, SaturatorSettings());
   AUDIO_CHECK(slow.Prepared IS ERR::Okay and fast.Prepared IS ERR::Okay);
   AUDIO_CHECK(slow.Processor.storage_samples() IS size_t(130 + 520 + 64));
   AUDIO_CHECK(fast.Processor.storage_samples() IS size_t(2 * (130 + 520 + 64)));

   // An unconfigured processor passes audio through and reports no latency or tail.
   SaturatorFixture idle(0, true, SaturatorSettings());
   AUDIO_CHECK(idle.Prepared IS ERR::Okay);
   float samples[] = { 0.25f, -0.5f, 0.75f, 1.0f };
   idle.Processor.process(samples, 2);
   AUDIO_CHECK(samples[0] IS 0.25f and samples[1] IS -0.5f and samples[2] IS 0.75f and samples[3] IS 1.0f);
   AUDIO_CHECK((not idle.Processor.pending()) and (idle.Processor.tail_frames() IS 0));
   AUDIO_CHECK(idle.Processor.latency() IS 0 and idle.Processor.decay_estimate() IS 0);

   // Fixed latency and support at every supported rate, independent of the settings.
   for (int rate : { 8000, 44100, 48000, 96000, 192000 }) {
      for (int shape : { SATURATE_SOFT, SATURATE_HARD }) {
         SaturatorFixture fixture(rate, true, settings(36, shape, 12, 0));
         AUDIO_CHECK(fixture.Processor.latency() IS 64 and fixture.Processor.tail() IS AudioTail::FINITE);
         AUDIO_CHECK(fixture.Processor.tail_frames() IS 129 and fixture.Processor.decay_estimate() IS 65);
      }
   }

   // Defaults are applied without a transition.
   SaturatorFixture typical(48000, true, SaturatorSettings());
   AUDIO_CHECK(typical.Processor.drive_level() IS 6.0 and typical.Processor.trim_level() IS -6.0);
   AUDIO_CHECK(std::abs(typical.Processor.drive_linear() - std::pow(10.0, 0.3)) < 1e-15);
   AUDIO_CHECK(typical.Processor.hard_weight() IS 0 and typical.Processor.wet_mix() IS 1.0);
   AUDIO_CHECK(not typical.Processor.ramping());
}

//********************************************************************************************************************
// The prototype and its polyphase decomposition: symmetry, exact phase DC gains and the measured response against the
// acceptance targets of 0.1 dB combined passband ripple and 90 dB stopband attenuation per filter.

static void test_filter(AudioTestContext &Test)
{
   const auto &filter = saturator_filter();
   const auto &g = filter.Prototype;

   bool symmetric = true;
   for (int k = 0; k < SATURATOR_TAPS; k++) symmetric &= g[k] IS g[SATURATOR_TAPS - 1 - k];
   AUDIO_CHECK(symmetric);

   double total = 0;
   for (auto c : g) total += c;
   AUDIO_CHECK(std::abs(total - 1.0) < 1e-14);

   for (int p = 0; p < SATURATOR_FACTOR; p++) {
      double phase = 0, interpolation = 0;
      for (int n = p; n < SATURATOR_TAPS; n += SATURATOR_FACTOR) phase += g[n];
      for (auto c : filter.Phases[p]) interpolation += c;
      AUDIO_CHECK(std::abs(phase - 0.25) < 1e-15);
      AUDIO_CHECK(std::abs(interpolation - 1.0) < 1e-14);
   }
   AUDIO_CHECK(filter.Phases[0][64] IS 4.0 * g[0] and filter.Phases[0][0] IS 4.0 * g[256]);
   AUDIO_CHECK(filter.Phases[1][0] IS 0 and filter.Phases[1][64] IS 4.0 * g[1] and filter.Phases[3][1] IS 4.0 * g[255]);

   double pass_min = 1e9, pass_max = -1e9;
   for (int i = 0; i <= 800; i++) {
      const double gain = prototype_response(0.4 * double(i) / 800.0);
      const double combined = 20.0 * std::log10(gain * gain);
      pass_min = std::min(pass_min, combined);
      pass_max = std::max(pass_max, combined);
   }

   double stop = 0;
   for (int i = 0; i <= 6000; i++) {
      stop = std::max(stop, std::abs(prototype_response(0.5 + 1.5 * double(i) / 6000.0)));
   }
   const double attenuation = -20.0 * std::log10(stop);
   const double transition = 20.0 * std::log10(std::abs(prototype_response(0.45)));
   const double edge = 20.0 * std::log10(std::abs(prototype_response(0.4)));

   AUDIO_CHECK(pass_max - pass_min <= 0.1 and pass_max - pass_min < 0.001);
   AUDIO_CHECK(attenuation >= 90.0 and attenuation > 99.0);
   AUDIO_CHECK(std::abs(transition + 6.02) < 0.01);
   kt::Log("AudioTests").msg("Saturator filter: combined passband ripple %.5f dB (%.5f to %.5f dB) to 0.4 fs, "
      "stopband %.2f dB from 0.5 fs, %.4f dB per filter at 0.4 fs, %.2f dB at 0.45 fs", pass_max - pass_min,
      pass_min, pass_max, attenuation, edge, transition);
}

//********************************************************************************************************************
// The linear path delays an impulse by exactly 64 frames with the exact combined filter response, influences exactly
// 129 output frames, and the dry branch is delayed by the same amount.

static void test_alignment(AudioTestContext &Test)
{
   const auto &g = saturator_filter().Prototype;

   SaturatorFixture fixture(48000, false, linear_settings());
   std::vector<float> output(400, 0.0f);
   output[0] = 0.5f;
   fixture.run(output, 1);

   // Independent combined response: (4g upsampled) convolved with g, sampled at phase zero.
   int peak_frame = -1;
   double peak_value = 0, response_sum = 0, error = 0;
   for (int n = 0; n < 400; n++) {
      double expected = 0;
      for (int k = 0; k < SATURATOR_TAPS; k++) {
         const int j = n * SATURATOR_FACTOR - k;
         if ((j >= 0) and (j < SATURATOR_TAPS)) expected += g[k] * 4.0 * g[j] * 0.5;
      }
      error = std::max(error, std::abs(double(output[n]) - expected));
      response_sum += output[n];
      if (std::abs(output[n]) > peak_value) { peak_value = std::abs(output[n]); peak_frame = n; }
   }
   AUDIO_CHECK(error < 1e-8);
   AUDIO_CHECK(peak_frame IS SATURATOR_LATENCY);
   AUDIO_CHECK(std::abs(response_sum - 0.5) < 1e-6);

   bool symmetric = true;
   for (int k = 1; k <= 64; k++) symmetric &= std::abs(output[64 - k] - output[64 + k]) < 1e-9;
   AUDIO_CHECK(symmetric);

   // The final influenced frame is 128 frames after the input; everything later is exactly zero.
   bool silent = true;
   for (int n = 129; n < 400; n++) silent &= output[n] IS 0;
   AUDIO_CHECK(output[128] != 0 and silent);

   // Mix zero is the input delayed by exactly 64 frames, bit for bit, including headroom above unity.
   for (int channels = 1; channels <= 2; channels++) {
      SaturatorFixture dry(44100, channels IS 2, settings(36, SATURATE_HARD, 12, 0));
      const auto input = noise_buffer(2000, channels, 3, 4.0);
      auto result = input;
      dry.run(result, 17);
      bool delayed = true;
      for (int n = 0; n < 2000; n++) {
         for (int c = 0; c < channels; c++) {
            const float expected = (n < 64) ? 0.0f : input[size_t(n - 64) * channels + c];
            delayed &= result[size_t(n) * channels + c] IS expected;
         }
      }
      AUDIO_CHECK(delayed and dry.Processor.pending());
   }

   // A partial mix is the blend of the delayed dry signal and an independently rendered fully wet branch.
   {
      const auto input = noise_buffer(3000, 2, 5, 0.8);
      auto wet = input, blended = input;
      SaturatorFixture wet_only(48000, true, settings(18, SATURATE_SOFT, -3, 100));
      SaturatorFixture partial(48000, true, settings(18, SATURATE_SOFT, -3, 30));
      wet_only.run(wet, 256);
      partial.run(blended, 256);
      double blend_error = 0;
      for (int n = 0; n < 3000; n++) {
         for (int c = 0; c < 2; c++) {
            const size_t i = size_t(n) * 2 + c;
            const double dry_value = (n < 64) ? 0.0 : double(input[i - 128]);
            blend_error = std::max(blend_error, std::abs(blended[i] - (0.7 * dry_value + 0.3 * double(wet[i]))));
         }
      }
      AUDIO_CHECK(blend_error < 1e-6);
   }
}

//********************************************************************************************************************
// Transfer behaviour against the independent reference and the defining properties of both curves.

static void test_transfer(AudioTestContext &Test)
{
   // Both shapes, several drives and trims, mono and stereo, at several rates.
   const SaturatorSettings cases[] = {
      settings(0, SATURATE_SOFT), settings(6, SATURATE_SOFT, -6), settings(36, SATURATE_SOFT, -36, 100),
      settings(12, SATURATE_HARD, 12, 100), settings(36, SATURATE_HARD, -12, 60), settings(20, SATURATE_SOFT, 3, 25)
   };
   for (int rate : { 8000, 44100, 96000 }) {
      for (int channels = 1; channels <= 2; channels++) {
         for (const auto &config : cases) {
            SaturatorFixture fixture(rate, channels IS 2, config);
            auto buffer = noise_buffer(600, channels, 9, 0.9);
            const auto expected = reference(buffer, channels, config);
            fixture.run(buffer, 64);
            AUDIO_CHECK(max_error(buffer, expected) < 1e-6);
         }
      }
   }

   // Silence in, silence out, with no pending state.
   {
      SaturatorFixture fixture(48000, true, settings(36, SATURATE_SOFT, 12));
      std::vector<float> silence(2000, 0.0f);
      fixture.run(silence, 100);
      bool zero = true;
      for (auto sample : silence) zero &= sample IS 0;
      AUDIO_CHECK(zero and (not fixture.Processor.pending()));
   }

   // Odd symmetry is exact: negating the input negates the output, for both curves.
   for (int shape : { SATURATE_SOFT, SATURATE_HARD }) {
      SaturatorFixture a(48000, false, settings(24, shape)), b(48000, false, settings(24, shape));
      auto positive = noise_buffer(1000, 1, 13, 0.7);
      auto negative = positive;
      for (auto &sample : negative) sample = -sample;
      a.run(positive, 50);
      b.run(negative, 50);
      bool odd = true;
      for (size_t i = 0; i < positive.size(); i++) odd &= positive[i] IS -negative[i];
      AUDIO_CHECK(odd);
   }

   // Small signals see a gain equal to the drive times the trim: no drive-dependent normalisation.
   {
      SaturatorFixture unity(48000, false, settings(0, SATURATE_SOFT));
      SaturatorFixture driven(48000, false, settings(12, SATURATE_SOFT));
      auto a = noise_buffer(1000, 1, 17, 1e-6), b = a;
      unity.run(a);
      driven.run(b);
      double ratio_error = 0;
      const double drive = std::pow(10.0, 12.0 / 20.0);
      for (size_t i = 200; i < a.size(); i++) {
         ratio_error = std::max(ratio_error, std::abs(double(b[i]) - drive * double(a[i])) / 1e-6);
      }
      AUDIO_CHECK(ratio_error < 1e-5);
   }

   // DC settles on the static curve value: trim * curve(drive * level).
   for (int shape : { SATURATE_SOFT, SATURATE_HARD }) {
      SaturatorFixture fixture(48000, false, settings(6, shape, -6));
      std::vector<float> dc(600, 0.5f);
      fixture.run(dc, 7);
      const double drive = std::pow(10.0, 6.0 / 20.0), trim = std::pow(10.0, -6.0 / 20.0);
      const double expected = trim * curve(drive * 0.5, shape);
      AUDIO_CHECK(std::abs(dc[599] - expected) < 1e-6 and std::abs(dc[200] - expected) < 1e-6);
   }

   // Drive and trim endpoints: 36 dB of drive hard-clips a -30 dBFS DC level at full scale; -36 dB trim scales it.
   {
      SaturatorFixture fixture(48000, false, settings(36, SATURATE_HARD, -36));
      std::vector<float> dc(600, 0.0316f);
      fixture.run(dc);
      AUDIO_CHECK(std::abs(dc[599] - std::pow(10.0, -36.0 / 20.0)) < 1e-7);

      SaturatorFixture boost(48000, false, settings(0, SATURATE_HARD, 12));
      std::vector<float> level(600, 0.25f);
      boost.run(level);
      AUDIO_CHECK(std::abs(level[599] - 0.25 * std::pow(10.0, 12.0 / 20.0)) < 1e-6);
   }

   // Very large finite input saturates without overflow, within the bound set by the decimation filter's absolute sum
   // and the trim; non-finite input is silence.
   double absolute_sum = 0;
   for (auto c : saturator_filter().Prototype) absolute_sum += std::abs(c);
   const double bound = absolute_sum * std::pow(10.0, 12.0 / 20.0);
   for (int shape : { SATURATE_SOFT, SATURATE_HARD }) {
      SaturatorFixture fixture(48000, true, settings(36, shape, 12));
      std::vector<float> buffer(2000);
      for (size_t i = 0; i < buffer.size(); i++) buffer[i] = (i % 7 < 3) ? 3.0e38f : -3.0e38f;
      fixture.run(buffer, 33);
      bool finite = true;
      for (auto sample : buffer) finite &= std::isfinite(sample);
      AUDIO_CHECK(finite and (peak(buffer) <= bound * (1.0 + 1e-6)) and (peak(buffer) > 3.0));
   }
   {
      SaturatorFixture fixture(48000, true, settings(24, SATURATE_SOFT, 0, 50));
      std::vector<float> buffer(1000);
      for (size_t i = 0; i < buffer.size(); i++) {
         buffer[i] = (i % 3 IS 0) ? std::numeric_limits<float>::quiet_NaN() :
            ((i % 3 IS 1) ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity());
      }
      fixture.run(buffer, 10);
      bool silent = true;
      for (auto sample : buffer) silent &= sample IS 0;
      AUDIO_CHECK(silent and (not fixture.Processor.pending()));
   }
}

//********************************************************************************************************************
// Spectral measurement of coherent tones.  The band-limited reference is the curve applied to the analytic input at
// Oversample times the base rate, so that it is independent of the processor's filters.  The processed output is
// compared with the reference after removing the 64-frame latency and applying the measured linear response of the
// decimation filter, so the residual in the passband (0 to 0.4 fs) is non-linear error: aliasing and the error of
// shaping an interpolated rather than an analytic signal.

constexpr int SPECTRUM_FRAMES = 4096;

struct Tone {
   int Bin;
   double Level;
};

struct SpectrumCase {
   const char *Name;
   std::vector<Tone> Tones;
   double Drive;
   int Shape;
};

struct SpectrumResult {
   double Plain = 0;       // Error energy of unoversampled processing, dB relative to the reference band energy
   double Oversampled = 0; // Error energy of the saturator, dB relative to the reference band energy
   double Harmonics = 0;   // Energy of the reference outside the input tones, dB relative to the reference
   double Rolloff = 0;     // Energy removed by the linear response of the decimation filter, dB relative
   double Convergence = 0; // Change in the reference between two oversampling factors, dB relative
};

static void fft(std::vector<std::complex<double>> &Data)
{
   const size_t n = Data.size();
   for (size_t i = 1, j = 0; i < n; i++) {
      size_t bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) std::swap(Data[i], Data[j]);
   }
   for (size_t length = 2; length <= n; length <<= 1) {
      const double angle = -2.0 * std::numbers::pi / double(length);
      const std::complex<double> unit(std::cos(angle), std::sin(angle));
      for (size_t start = 0; start < n; start += length) {
         std::complex<double> w(1.0, 0.0);
         for (size_t k = 0; k < length / 2; k++) {
            const auto a = Data[start + k], b = Data[start + k + length / 2] * w;
            Data[start + k] = a + b;
            Data[start + k + length / 2] = a - b;
            w *= unit;
         }
      }
   }
}

static double tone_value(const std::vector<Tone> &Tones, double Frame)
{
   double sum = 0;
   for (const auto &tone : Tones) {
      sum += tone.Level * std::sin(2.0 * std::numbers::pi * double(tone.Bin) * Frame / double(SPECTRUM_FRAMES));
   }
   return sum;
}

// Bins 0 to SPECTRUM_FRAMES / 2 of the analytic shaped signal, scaled to match an FFT at the base rate.

static std::vector<std::complex<double>> reference_spectrum(const SpectrumCase &Case, int Oversample)
{
   const size_t points = size_t(SPECTRUM_FRAMES) * Oversample;
   const double drive = std::pow(10.0, Case.Drive / 20.0);
   std::vector<std::complex<double>> data(points);
   for (size_t i = 0; i < points; i++) {
      data[i] = curve(drive * tone_value(Case.Tones, double(i) / double(Oversample)), Case.Shape);
   }
   fft(data);
   data.resize(SPECTRUM_FRAMES / 2 + 1);
   for (auto &bin : data) bin /= double(Oversample);
   return data;
}

static std::vector<std::complex<double>> spectrum(const std::vector<float> &Signal, int Start)
{
   std::vector<std::complex<double>> data(SPECTRUM_FRAMES);
   for (int i = 0; i < SPECTRUM_FRAMES; i++) data[i] = double(Signal[size_t(Start + i)]);
   fft(data);
   data.resize(SPECTRUM_FRAMES / 2 + 1);
   return data;
}

static SpectrumResult measure_spectrum(const SpectrumCase &Case, int Oversample, int Convergence = 0)
{
   constexpr int rate = 48000, warm = 512;
   const int frames = warm + SPECTRUM_FRAMES + SATURATOR_LATENCY;

   // The analysis window starts at the zero phase of every tone.
   std::vector<float> input(frames);
   for (int n = 0; n < frames; n++) input[n] = float(tone_value(Case.Tones, double(n - warm)));

   auto processed = input;
   SaturatorFixture fixture(rate, false, settings(Case.Drive, Case.Shape, 0, 100));
   fixture.run(processed, 256);

   const double drive = std::pow(10.0, Case.Drive / 20.0);
   std::vector<float> plain(frames);
   for (int n = 0; n < frames; n++) plain[n] = float(curve(drive * double(input[n]), Case.Shape));

   const auto ideal = reference_spectrum(Case, Oversample);
   const auto actual = spectrum(processed, warm + SATURATOR_LATENCY);
   const auto direct = spectrum(plain, warm);

   double band = 0, plain_error = 0, error = 0, harmonics = 0, rolloff = 0;
   for (int b = 1; b <= int(0.4 * SPECTRUM_FRAMES); b++) {
      const double response = prototype_response(double(b) / double(SPECTRUM_FRAMES));
      bool input_bin = false;
      for (const auto &tone : Case.Tones) input_bin |= tone.Bin IS b;

      band += std::norm(ideal[b]);
      if (not input_bin) harmonics += std::norm(ideal[b]);
      rolloff += std::norm(ideal[b] * (1.0 - response));
      error += std::norm(actual[b] - ideal[b] * response);
      plain_error += std::norm(direct[b] - ideal[b]);
   }

   SpectrumResult result;
   result.Plain = 10.0 * std::log10(plain_error / band);
   result.Oversampled = 10.0 * std::log10(error / band);
   result.Harmonics = 10.0 * std::log10(harmonics / band);
   result.Rolloff = 10.0 * std::log10(std::max(rolloff, 1e-300) / band);
   if (Convergence > 0) {
      const auto finer = reference_spectrum(Case, Convergence);
      double change = 0;
      for (int b = 1; b <= int(0.4 * SPECTRUM_FRAMES); b++) change += std::norm(finer[b] - ideal[b]);
      result.Convergence = 10.0 * std::log10(std::max(change, 1e-300) / band);
   }
   return result;
}

// Gate cases, fixed before measurement: low and mid-band tones at -6 dBFS whose unoversampled processing aliases
// measurably.  Bins are odd, so no aliased harmonic can coincide with a direct harmonic.  The saturator must reduce
// the passband error energy by at least 20 dB relative to unoversampled processing.

static const SpectrumCase glGateCases[] = {
   { "soft 24 dB, 996 Hz", { { 85, 0.5 } }, 24, SATURATE_SOFT },
   { "soft 12 dB, 2988 Hz", { { 255, 0.5 } }, 12, SATURATE_SOFT },
   { "hard 12 dB, 996 Hz", { { 85, 0.5 } }, 12, SATURATE_HARD },
   { "hard 24 dB, 2988 Hz", { { 255, 0.5 } }, 24, SATURATE_HARD }
};

// Limitation cases quantify the residual aliasing of high-frequency, high-drive and dense material.  They carry no
// improvement threshold.

static const SpectrumCase glLimitCases[] = {
   { "soft 36 dB, 14988 Hz", { { 1279, 0.5 } }, 36, SATURATE_SOFT },
   { "hard 36 dB, 14988 Hz", { { 1279, 0.5 } }, 36, SATURATE_HARD },
   { "soft 24 dB, 18996 Hz", { { 1621, 0.5 } }, 24, SATURATE_SOFT },
   { "hard 24 dB, 18996 Hz", { { 1621, 0.5 } }, 24, SATURATE_HARD },
   { "soft 24 dB, multitone", { { 85, 0.2 }, { 255, 0.2 }, { 511, 0.2 } }, 24, SATURATE_SOFT },
   { "hard 24 dB, multitone", { { 85, 0.2 }, { 255, 0.2 }, { 511, 0.2 } }, 24, SATURATE_HARD }
};

static void log_spectrum(const SpectrumCase &Case, const SpectrumResult &Result)
{
   kt::Log("AudioTests").msg("Saturator %-22s 1x %7.1f dB, 4x %7.1f dB, improvement %5.1f dB, harmonics %6.1f dB, "
      "roll-off %7.1f dB, reference change %7.1f dB", Case.Name, Result.Plain, Result.Oversampled,
      Result.Plain - Result.Oversampled, Result.Harmonics, Result.Rolloff, Result.Convergence);
}

static void test_aliasing(AudioTestContext &Test)
{
   for (const auto &test_case : glGateCases) {
      const auto result = measure_spectrum(test_case, 64);
      log_spectrum(test_case, result);
      AUDIO_CHECK(result.Plain > -70.0);                       // The baseline aliases measurably
      AUDIO_CHECK(result.Plain - result.Oversampled >= 20.0);
   }

   // A near-Nyquist limitation case still improves, by a smaller margin.
   const auto result = measure_spectrum(glLimitCases[3], 64);
   log_spectrum(glLimitCases[3], result);
   AUDIO_CHECK(result.Oversampled < result.Plain - 6.0);
}

// The complete spectral report, with reference convergence, is run on request through the `saturator-spectra`
// test option.

static void spectra(AudioTestContext &Test)
{
   const std::span<const SpectrumCase> lists[] = { glGateCases, glLimitCases };
   for (const auto &list : lists) {
      for (const auto &test_case : list) {
         const auto result = measure_spectrum(test_case, 64, 128);
         log_spectrum(test_case, result);
         AUDIO_CHECK(result.Convergence < result.Oversampled - 20.0);
      }
   }
}

//********************************************************************************************************************
// Pending state follows the input history exactly: 128 frames after the last non-zero input, regardless of the mix,
// ramps and edits.

static void test_tail(AudioTestContext &Test)
{
   // Pending ends exactly when the last influenced frame has been produced.
   {
      SaturatorFixture fixture(48000, false, linear_settings());
      std::vector<float> frame(1, 0.5f);
      fixture.Processor.process(frame.data(), 1);
      bool held = fixture.Processor.pending();
      for (int n = 1; n < 128; n++) {
         frame[0] = 0;
         fixture.Processor.process(frame.data(), 1);
         held &= fixture.Processor.pending();
      }
      frame[0] = 0;
      fixture.Processor.process(frame.data(), 1);
      AUDIO_CHECK(held and (frame[0] != 0) and (not fixture.Processor.pending()));
   }

   // Silent ramps and shape transitions never make silence pending.
   {
      SaturatorFixture fixture(48000, true, SaturatorSettings());
      std::vector<float> silence(2000, 0.0f);
      fixture.change(settings(30, SATURATE_HARD, 12, 50));
      fixture.run_range(silence, 0, 100);
      fixture.change(settings(0, SATURATE_SOFT, -36, 100));
      fixture.run_range(silence, 100, 1000);
      bool zero = true;
      for (auto sample : silence) zero &= sample IS 0;
      AUDIO_CHECK(zero and (not fixture.Processor.pending()));
   }

   // Shape and drive edits during a pending tail do not extend it.
   {
      SaturatorFixture fixture(48000, false, settings(24, SATURATE_SOFT));
      std::vector<float> buffer(300, 0.0f);
      buffer[0] = 0.9f;
      fixture.run_range(buffer, 0, 50);
      fixture.change(settings(36, SATURATE_HARD, 6));
      fixture.run_range(buffer, 50, 128);
      const bool before = fixture.Processor.pending();
      fixture.run_range(buffer, 128, 129);
      bool silent = true;
      for (int n = 129; n < 300; n++) silent &= buffer[n] IS 0;
      fixture.run_range(buffer, 129, 300);
      for (int n = 129; n < 300; n++) silent &= buffer[n] IS 0;
      AUDIO_CHECK(before and (not fixture.Processor.pending()) and silent);
   }

   // Mix zero retains the wet history: raising the mix reveals it with the 10 ms ramp applied.
   {
      SaturatorFixture wet_fixture(48000, false, linear_settings());
      SaturatorFixture fixture(48000, false, linear_settings(0));
      std::vector<float> wet(300, 0.0f), buffer(300, 0.0f);
      wet[0] = buffer[0] = 0.5f;
      wet_fixture.run(wet);
      fixture.run_range(buffer, 0, 10);
      fixture.change(linear_settings(100));
      fixture.run_range(buffer, 10, 300);
      double error = 0;
      for (int n = 65; n < 300; n++) {
         error = std::max(error, std::abs(double(buffer[n]) - double(n - 10) / 480.0 * double(wet[n])));
      }
      AUDIO_CHECK(error < 1e-7 and buffer[100] != 0);
   }

   // A partial final block: frames are counted, not blocks.
   {
      SaturatorFixture a(44100, true, SaturatorSettings()), b(44100, true, SaturatorSettings());
      auto first = noise_buffer(10, 2, 1);
      auto second = first;
      a.run(first, 3);
      b.run(second, 10);
      std::vector<float> rest_a(256, 0.0f), rest_b(256, 0.0f);
      a.run_range(rest_a, 0, 127);
      b.run_range(rest_b, 0, 127);
      AUDIO_CHECK(a.Processor.pending() and b.Processor.pending());
      a.run_range(rest_a, 127, 128);
      b.run_range(rest_b, 127, 128);
      AUDIO_CHECK((not a.Processor.pending()) and (not b.Processor.pending()));
   }
}

//********************************************************************************************************************

static void test_lifecycle(AudioTestContext &Test)
{
   SaturatorFixture fixture(48000, true, SaturatorSettings());
   auto buffer = noise_buffer(500, 2, 4);
   fixture.run(buffer, 100);
   fixture.change(settings(30, SATURATE_HARD, -12, 40));
   AUDIO_CHECK(fixture.Processor.pending() and fixture.Processor.ramping());

   // Reset discards every history and applies the committed settings immediately.
   fixture.Processor.reset();
   AUDIO_CHECK((not fixture.Processor.pending()) and (not fixture.Processor.ramping()));
   AUDIO_CHECK(fixture.Processor.drive_level() IS 30.0 and fixture.Processor.hard_weight() IS 1.0);
   AUDIO_CHECK(fixture.Processor.trim_level() IS -12.0 and fixture.Processor.wet_mix() IS 0.4);
   std::vector<float> after(512, 0.0f);
   fixture.run(after, 64);
   bool silent = true;
   for (auto sample : after) silent &= sample IS 0;
   AUDIO_CHECK(silent);

   // After reset the output matches a fresh processor exactly.
   {
      auto input = noise_buffer(800, 2, 8);
      auto fresh = input;
      SaturatorFixture reference_fixture(48000, true, settings(30, SATURATE_HARD, -12, 40));
      fixture.run(input, 17);
      reference_fixture.run(fresh, 256);
      bool same = true;
      for (size_t i = 0; i < input.size(); i++) same &= input[i] IS fresh[i];
      AUDIO_CHECK(same);
   }

   // An edit while a reset is pending, or with an unsupported target, records the settings without a transition.
   fixture.Effect.ResetPending = true;
   fixture.change(settings(12, SATURATE_SOFT, 0, 100));
   AUDIO_CHECK(fixture.Processor.Settings.Drive IS 12 and fixture.Processor.drive_level() IS 30.0);
   AUDIO_CHECK(not fixture.Processor.ramping());
   fixture.Effect.ResetPending = false;
   fixture.Processor.update(settings(18, SATURATE_SOFT, 0, 100), nullptr);
   AUDIO_CHECK(fixture.Processor.Settings.Drive IS 18 and (not fixture.Processor.ramping()));
   fixture.Processor.reset();
   AUDIO_CHECK(fixture.Processor.drive_level() IS 18.0 and fixture.Processor.hard_weight() IS 0);

   // Re-preparing for another rate and layout publishes new storage, keeps the latency and starts silent.
   std::unique_ptr<AudioEffectConfiguration> config;
   AUDIO_CHECK(fixture.Processor.prepare(96000, false, config) IS ERR::Okay);
   AUDIO_CHECK(config->latency() IS 64);
   config->publish();
   fixture.Effect.OutputRate = 96000;
   fixture.Effect.Stereo = false;
   fixture.Processor.reset();
   AUDIO_CHECK(fixture.Processor.storage_samples() IS size_t(130 + 520 + 64));
   AUDIO_CHECK(fixture.Processor.latency() IS 64 and fixture.Processor.tail_frames() IS 129);

   // A rate that the storage was not prepared for leaves the processor inactive.
   fixture.Effect.OutputRate = 44100;
   fixture.Processor.reset();
   AUDIO_CHECK(fixture.Processor.tail_frames() IS 0 and (not fixture.Processor.pending()));
}

//********************************************************************************************************************
// Ramps run over 10 ms: drive and trim in dB, mix and shape linearly, retargeted from their current values, with the
// oversampled and base-rate ramps ending at the same output frame.

static void test_ramps(AudioTestContext &Test)
{
   SaturatorFixture fixture(48000, false, settings(0, SATURATE_SOFT, 0, 100));
   std::vector<float> buffer(2000, 0.0f);
   fixture.change(settings(24, SATURATE_HARD, -12, 50));
   fixture.run_range(buffer, 0, 240);

   AUDIO_CHECK(std::abs(fixture.Processor.drive_level() - 12.0) < 1e-9);
   AUDIO_CHECK(std::abs(fixture.Processor.drive_linear() - std::pow(10.0, 12.0 / 20.0)) < 1e-9);
   AUDIO_CHECK(std::abs(fixture.Processor.hard_weight() - 0.5) < 1e-12);
   AUDIO_CHECK(std::abs(fixture.Processor.trim_level() + 6.0) < 1e-9);
   AUDIO_CHECK(std::abs(fixture.Processor.trim_linear() - std::pow(10.0, -6.0 / 20.0)) < 1e-9);
   AUDIO_CHECK(std::abs(fixture.Processor.wet_mix() - 0.75) < 1e-12);

   // Retarget midway: the new ramp starts from the current values and lasts a full 10 ms.
   fixture.change(settings(0, SATURATE_SOFT, 0, 100));
   fixture.run_range(buffer, 240, 480);
   AUDIO_CHECK(std::abs(fixture.Processor.drive_level() - 6.0) < 1e-9);
   AUDIO_CHECK(std::abs(fixture.Processor.hard_weight() - 0.25) < 1e-12);
   AUDIO_CHECK(std::abs(fixture.Processor.wet_mix() - 0.875) < 1e-12);
   fixture.run_range(buffer, 480, 719);
   AUDIO_CHECK(fixture.Processor.ramping());
   fixture.run_range(buffer, 719, 720);
   AUDIO_CHECK(not fixture.Processor.ramping());
   AUDIO_CHECK(fixture.Processor.drive_level() IS 0 and fixture.Processor.drive_linear() IS 1.0);
   AUDIO_CHECK(fixture.Processor.hard_weight() IS 0 and fixture.Processor.wet_mix() IS 1.0);
   AUDIO_CHECK(fixture.Processor.trim_level() IS 0 and fixture.Processor.trim_linear() IS 1.0);

   // A shape crossfade evaluates the blend of the two curves: with a DC input, the output moves from the soft value
   // to the hard value along the oversampled blend, and settles exactly.
   {
      SaturatorFixture shape(48000, false, settings(12, SATURATE_SOFT, 0, 100));
      std::vector<float> dc(2000, 0.3f);
      shape.run_range(dc, 0, 1000);
      shape.change(settings(12, SATURATE_HARD, 0, 100));
      shape.run_range(dc, 1000, 2000);
      const double soft = std::tanh(std::pow(10.0, 0.6) * 0.3);
      // The transition is monotonic apart from the filter's ringing at the corners of the ramp, which is below 2e-5.
      bool smooth = true;
      for (int n = 1001; n < 2000; n++) smooth &= dc[n] >= dc[n - 1] - 2e-5;
      AUDIO_CHECK(std::abs(dc[1000] - soft) < 1e-6 and std::abs(dc[1999] - 1.0) < 1e-6 and smooth);

      // A symmetric filter passes a linear ramp unchanged apart from its delay, so output frame n reflects the blend
      // at oversampled frame 4n - 128.  The blend is 0.5 at oversampled frame 4000 + 960.
      AUDIO_CHECK(std::abs(dc[1272] - (soft + 0.5 * (1.0 - soft))) < 1e-6);
      AUDIO_CHECK(std::abs(dc[1544] - 1.0) < 1e-6);
   }
}

//********************************************************************************************************************
// Output is independent of block partitioning, with edits at identical sample offsets.

static void test_blocks(AudioTestContext &Test)
{
   struct Edit {
      int Frame;
      SaturatorSettings Settings;
   };
   const Edit edits[] = {
      { 700, settings(30, SATURATE_HARD, -6, 100) },
      { 905, settings(12, SATURATE_SOFT, 3, 60) },
      { 1800, settings(36, SATURATE_HARD, -20, 0) },
      { 1850, settings(6, SATURATE_SOFT, -6, 100) }
   };

   for (int rate : { 44100, 48000, 96000 }) {
      const auto input = noise_buffer(3000, 2, 23, 0.8);
      std::vector<std::vector<float>> results;
      for (int partition = 0; partition < 4; partition++) {
         SaturatorFixture fixture(rate, true, SaturatorSettings());
         auto buffer = input;
         uint32_t seed = 99;
         int frame = 0;
         size_t next_edit = 0;
         while (frame < 3000) {
            int block;
            switch (partition) {
               case 0: block = 1; break;
               case 1: block = 17; break;
               case 2: block = 256; break;
               default: block = 1 + int((noise(seed) + 1.0) * 150.0); break;
            }
            int end = std::min(3000, frame + block);
            if ((next_edit < std::size(edits)) and (edits[next_edit].Frame < end)) end = edits[next_edit].Frame;
            if ((next_edit < std::size(edits)) and (edits[next_edit].Frame IS frame)) {
               fixture.change(edits[next_edit++].Settings);
               continue;
            }
            fixture.run_range(buffer, frame, end);
            frame = end;
         }
         results.push_back(std::move(buffer));
      }

      bool same = true;
      for (size_t r = 1; r < results.size(); r++) {
         for (size_t i = 0; i < input.size(); i++) same &= results[r][i] IS results[0][i];
      }
      AUDIO_CHECK(same);
   }
}

//********************************************************************************************************************
// Storage never reallocates after preparation.  The worst block time is measured at 192 kHz stereo with a shape
// crossfade and a drive ramp in progress throughout.

static void test_resources(AudioTestContext &Test)
{
   SaturatorFixture fixture(192000, true, SaturatorSettings());
   const double *storage = fixture.Processor.shaped_storage();
   const size_t samples = fixture.Processor.storage_samples();

   uint32_t seed = 31;
   std::vector<float> buffer(size_t(1024) * 2);
   double worst_block = 0, total = 0;
   for (int block = 0; block < 200; block++) {
      fixture.change(settings((block & 1) ? 36 : 0, (block & 1) ? SATURATE_HARD : SATURATE_SOFT, -6, 70));
      for (auto &sample : buffer) sample = float(noise(seed));
      const auto start = std::chrono::steady_clock::now();
      fixture.Processor.process(buffer.data(), 1024);
      const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start);
      worst_block = std::max(worst_block, elapsed.count());
      total += elapsed.count();
   }

   const auto start = std::chrono::steady_clock::now();
   fixture.Processor.reset();
   const auto reset = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start);

   AUDIO_CHECK(fixture.Processor.shaped_storage() IS storage and fixture.Processor.storage_samples() IS samples);
   kt::Log("AudioTests").msg("Saturator at 192 kHz stereo: %.1f KB history, 1024-frame block (5.33 ms) worst %.1f us, "
      "mean %.1f us, reset %.2f us", double(fixture.Processor.storage_bytes()) / 1024.0,
      worst_block, total / 200.0, reset.count());
}

//********************************************************************************************************************

static void run(AudioTestContext &Test)
{
   test_configuration(Test);
   test_filter(Test);
   test_alignment(Test);
   test_transfer(Test);
   test_aliasing(Test);
   test_tail(Test);
   test_lifecycle(Test);
   test_ramps(Test);
   test_blocks(Test);
   test_resources(Test);
}

} // namespace audio_tests_audio_saturator_dsp

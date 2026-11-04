// Included by audio.cpp to exercise the module implementation.

#include <chrono>
#include <complex>

namespace audio_tests_audio_pitchshift_dsp {

using cplx = std::complex<double>;

// Configure exactly as activation does: prepare outside the lock, publish, then reset on the render thread.

struct PitchFixture {
   extAudioEffect Effect{nullptr, 1};
   PitchShiftProcessor Processor;
   ERR Prepared;
   int Channels;

   PitchFixture(int Rate, bool Stereo, const PitchShiftSettings &Settings) : Processor(&Effect, Settings) {
      Effect.OutputRate = Rate;
      Effect.Stereo = Stereo;
      Effect.ResetPending = false;
      Channels = Stereo ? 2 : 1;
      std::unique_ptr<AudioEffectConfiguration> config;
      Prepared = Processor.prepare(Rate, Stereo, config);
      if (config) config->publish();
      Processor.reset();
   }

   // Parameter updates derive their target on the control thread, as PitchShiftUpdate does.
   void change(const PitchShiftSettings &Settings) {
      PitchShiftTarget target;
      const bool valid = pitch_target(Settings, Processor.SampleRate, target);
      Processor.update(Settings, valid ? &target : nullptr);
   }

   void run(std::vector<float> &Buffer, int Block = 0) {
      const int frames = int(Buffer.size()) / Channels;
      if (Block <= 0) Block = frames;
      for (int i = 0; i < frames; i += Block) {
         Processor.process(Buffer.data() + size_t(i) * Channels, std::min(Block, frames - i));
      }
   }

   // Frames of silence processed until the processor stops reporting pending, or Limit.  The output is appended to
   // Output if provided.
   int drain(int Limit, std::vector<float> *Output = nullptr) {
      std::vector<float> silence(size_t(Channels), 0.0f);
      int frames = 0;
      while (Processor.pending() and (frames < Limit)) {
         std::fill(silence.begin(), silence.end(), 0.0f);
         Processor.process(silence.data(), 1);
         if (Output) Output->insert(Output->end(), silence.begin(), silence.end());
         frames++;
      }
      return frames;
   }
};

static PitchShiftSettings settings(double Semitones, double Mix = 100, double Gain = 0)
{
   return PitchShiftSettings { .Semitones = Semitones, .Mix = Mix, .Gain = Gain };
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

// A sum of Partials harmonics of Frequency with 1/h amplitudes, identical in every channel.

static std::vector<float> harmonics(int Frames, int Channels, int Rate, double Frequency, int Partials, double Level)
{
   std::vector<float> buffer(size_t(Frames) * Channels);
   for (int i = 0; i < Frames; i++) {
      double s = 0;
      for (int h = 1; h <= Partials; h++) {
         s += Level / h * std::sin(2.0 * std::numbers::pi * Frequency * h * i / Rate + 0.7 * h);
      }
      for (int c = 0; c < Channels; c++) buffer[size_t(i) * Channels + c] = float(s);
   }
   return buffer;
}

static std::vector<double> channel(const std::vector<float> &Buffer, int Channels, int Channel)
{
   std::vector<double> result(Buffer.size() / Channels);
   for (size_t i = 0; i < result.size(); i++) result[i] = Buffer[i * Channels + Channel];
   return result;
}

static double peak(const std::vector<float> &Buffer, size_t Start = 0)
{
   double result = 0;
   for (size_t i = Start; i < Buffer.size(); i++) result = std::max(result, std::abs(double(Buffer[i])));
   return result;
}

static double rms(const std::vector<double> &Signal, size_t Start, size_t End)
{
   double sum = 0;
   for (size_t i = Start; i < End; i++) sum += Signal[i] * Signal[i];
   return std::sqrt(sum / double(End - Start));
}

// Blackman-Harris spectrum of Count frames from Start.  Count must be a power of two.

static std::vector<double> spectrum_power(const std::vector<double> &Signal, size_t Start, size_t Count)
{
   std::vector<cplx> data(Count);
   for (size_t i = 0; i < Count; i++) {
      const double a = 2.0 * std::numbers::pi * double(i) / double(Count);
      const double w = 0.35875 - 0.48829 * std::cos(a) + 0.14128 * std::cos(2 * a) - 0.01168 * std::cos(3 * a);
      data[i] = Signal[Start + i] * w;
   }
   analyser_fft(data);
   std::vector<double> power(Count / 2);
   for (size_t k = 0; k < power.size(); k++) power[k] = std::norm(data[k]);
   return power;
}

// Interpolated frequency of the strongest component, and the energy outside it relative to the total, in dB.

static void tone_metrics(const std::vector<double> &Signal, size_t Start, int Rate, double &Frequency, double &Spurious)
{
   constexpr size_t count = 32768;
   const auto power = spectrum_power(Signal, Start, count);
   size_t top = 1;
   for (size_t k = 1; k + 1 < power.size(); k++) if (power[k] > power[top]) top = k;
   const double a = std::log(power[top - 1]), b = std::log(power[top]), c = std::log(power[top + 1]);
   const double offset = 0.5 * (a - c) / (a - 2 * b + c);
   Frequency = (double(top) + offset) * Rate / double(count);

   double inside = 0, total = 0;
   for (size_t k = 1; k < power.size(); k++) {
      total += power[k];
      if (std::abs(int(k) - int(top)) <= 6) inside += power[k];
   }
   Spurious = 10 * std::log10(std::max(total - inside, 1e-300) / total);
}

// Energy away from every harmonic of Fundamental (more than 6 Hz from a multiple), relative to the total, in dB.

static double between_partials(const std::vector<double> &Signal, size_t Start, int Rate, double Fundamental)
{
   constexpr size_t count = 32768;
   const auto power = spectrum_power(Signal, Start, count);
   double on = 0, total = 0;
   for (size_t k = 1; k < power.size(); k++) {
      const double f = double(k) * Rate / double(count), h = std::round(f / Fundamental);
      total += power[k];
      if ((h >= 1) and (std::abs(f - h * Fundamental) < 6)) on += power[k];
   }
   return 10 * std::log10(std::max(total - on, 1e-300) / total);
}

//********************************************************************************************************************
// An independent model of the published algorithm for a fixed ratio.  Each channel has its own full complex FFT and
// Hermitian synthesis spectrum, rather than the processor's packed pair; the output is accumulated offline in input
// time and every frame is evaluated, including silent ones.

static std::vector<double> reference(const std::vector<float> &Input, int Channels, int Rate,
   const PitchShiftSettings &Settings)
{
   const int n = pitch_window(Rate), r = n / 8, k_max = n / 2;
   const double ratio = std::pow(2.0, Settings.Semitones / 12.0), mix = Settings.Mix / 100.0;
   const double trim = std::pow(10.0, Settings.Gain / 20.0);
   const int64_t frames = int64_t(Input.size()) / Channels;
   const double tau = 2.0 * std::numbers::pi;

   std::vector<double> window(n);
   double energy = 0;
   for (int i = 0; i < n; i++) {
      window[i] = 0.5 * (1.0 - std::cos(tau * i / n));
      energy += window[i] * window[i];
   }

   auto at = [&](int Channel, int64_t Time) -> double {
      return ((Time < 0) or (Time >= frames)) ? 0.0 : double(Input[size_t(Time) * Channels + Channel]);
   };

   const PitchTables tables(n); // Only the makeup table is used; test_makeup() verifies it independently
   std::vector<std::vector<double>> wet(Channels, std::vector<double>(size_t(frames + n), 0.0));
   std::vector<std::vector<cplx>> previous(Channels, std::vector<cplx>(k_max + 1));
   std::vector<double> rotation(k_max + 1, 0.0);

   for (int64_t end = r - 1; end < frames; end += r) {
      std::vector<std::vector<cplx>> x(Channels, std::vector<cplx>(n));
      for (int c = 0; c < Channels; c++) {
         for (int i = 0; i < n; i++) x[c][(i + n / 2) % n] = at(c, end - n + 1 + i) * window[i];
         analyser_fft(x[c]);
      }

      std::vector<double> power(k_max + 1, 0.0);
      double top = 0;
      for (int k = 0; k <= k_max; k++) {
         for (int c = 0; c < Channels; c++) power[k] += std::norm(x[c][k]);
         top = std::max(top, power[k]);
      }
      std::vector<int> peaks;
      if ((power[0] > top * 1e-14) and (power[0] >= power[1]) and (power[0] >= power[2])) peaks.push_back(0);
      for (int k = 1; k < k_max; k++) {
         bool is_peak = (power[k] > top * 1e-14) and (power[k] > power[k - 1]) and (power[k] >= power[k + 1]);
         if (is_peak and (k >= 2)) is_peak = power[k] > power[k - 2];
         if (is_peak and (k + 2 <= k_max)) is_peak = power[k] >= power[k + 2];
         if (is_peak) peaks.push_back(k);
      }
      if ((power[k_max] > top * 1e-14) and (power[k_max] > power[k_max - 1]) and
          (power[k_max] > power[k_max - 2])) peaks.push_back(k_max);

      std::vector<std::vector<cplx>> y(Channels, std::vector<cplx>(k_max + 1));
      std::vector<double> next(k_max + 1, 0.0);
      int start = 0;
      for (size_t i = 0; i < peaks.size(); i++) {
         const int p = peaks[i];
         int last = k_max;
         if (i + 1 < peaks.size()) {
            last = p + 1;
            for (int k = p + 1; k < peaks[i + 1]; k++) if (power[k] < power[last]) last = k;
         }
         cplx cross = 0;
         for (int c = 0; c < Channels; c++) cross += x[c][p] * std::conj(previous[c][p]);
         const double omega = tau * p / n;   // Bin centre, radians per frame
         double deviation = (std::abs(cross) > 0) ? std::arg(cross) - omega * r : 0.0;
         deviation -= tau * std::round(deviation / tau);
         const double frequency = omega + deviation / r;
         const double theta = rotation[p] + (ratio - 1.0) * frequency * r;
         const double move = (ratio - 1.0) * frequency * n / tau;
         const int shift = int(std::lround(move));
         const double gain = tables.makeup(move - shift);
         for (int k = start; k <= last; k++) {
            next[k] = theta;
            if ((k + shift < 0) or (k + shift > k_max)) continue;
            for (int c = 0; c < Channels; c++) y[c][k + shift] += x[c][k] * std::polar(gain, theta);
         }
         start = last + 1;
      }
      rotation = next;

      for (int c = 0; c < Channels; c++) {
         for (int k = 0; k <= k_max; k++) previous[c][k] = x[c][k];
         std::vector<cplx> full(n);
         for (int k = 0; k <= k_max; k++) full[k] = std::conj(y[c][k]);
         full[0] = y[c][0].real();
         full[k_max] = y[c][k_max].real();
         for (int k = 1; k < k_max; k++) full[n - k] = y[c][k];
         analyser_fft(full); // The conjugated forward transform is the conjugated inverse; the result is real.
         for (int i = 0; i < n; i++) {
            const int64_t time = end - n + 1 + i;
            if (time + n < 0) continue;
            wet[c][size_t(time + n)] += full[(i + n / 2) % n].real() / n * window[i] * double(r) / energy;
         }
      }
   }

   std::vector<double> output(Input.size());
   for (int64_t t = 0; t < frames; t++) {
      for (int c = 0; c < Channels; c++) {
         const double delayed = at(c, t - n);
         output[size_t(t) * Channels + c] = trim * ((1.0 - mix) * delayed + mix * wet[c][size_t(t)]);
      }
   }
   return output;
}

//********************************************************************************************************************

static void test_configuration(AudioTestContext &Test)
{
   PitchFixture low(7999, true, PitchShiftSettings());
   AUDIO_CHECK(low.Prepared IS ERR::NoSupport);
   PitchFixture high(192001, true, PitchShiftSettings());
   AUDIO_CHECK(high.Prepared IS ERR::NoSupport);

   // Window length and latency at representative rates: 32 to 47 ms.
   const std::pair<int, int> windows[] = {
      { 8000, 256 }, { 11025, 512 }, { 16000, 512 }, { 22050, 1024 }, { 32000, 1024 }, { 44100, 2048 },
      { 48000, 2048 }, { 88200, 4096 }, { 96000, 4096 }, { 176400, 8192 }, { 192000, 8192 }
   };
   for (auto [rate, size] : windows) {
      AUDIO_CHECK(pitch_window(rate) IS size);
      const double ms = 1000.0 * size / rate;
      AUDIO_CHECK((ms >= 32.0) and (ms <= 47.0));
   }

   PitchFixture slow(8000, false, PitchShiftSettings()), fast(192000, true, PitchShiftSettings());
   AUDIO_CHECK(slow.Prepared IS ERR::Okay and fast.Prepared IS ERR::Okay);
   AUDIO_CHECK(slow.Processor.latency() IS 256 and fast.Processor.latency() IS 8192);
   AUDIO_CHECK(slow.Processor.window_size() IS 256 and slow.Processor.hop_size() IS 32);
   AUDIO_CHECK(slow.Processor.tail() IS AudioTail::FINITE);
   AUDIO_CHECK(fast.Processor.tail_frames() IS 16384 and fast.Processor.decay_estimate() IS 6144);
   AUDIO_CHECK(fast.Processor.pending() IS false);

   // The configuration reports the same latency before it is published.
   PitchShiftProcessor loose(&fast.Effect, PitchShiftSettings());
   std::unique_ptr<AudioEffectConfiguration> config;
   AUDIO_CHECK(loose.prepare(44100, true, config) IS ERR::Okay and config->latency() IS 2048);

   // A zero rate leaves the processor inactive: audio passes untouched and there is no latency.
   PitchFixture idle(0, true, settings(7, 50, -12));
   AUDIO_CHECK(idle.Prepared IS ERR::Okay and idle.Processor.latency() IS 0);
   std::vector<float> buffer = { 0.25f, -0.5f, 0.75f, 0.125f };
   const auto original = buffer;
   idle.run(buffer);
   AUDIO_CHECK(buffer IS original);

   AUDIO_CHECK(pitch_ratio(12) IS 2.0 and pitch_ratio(-12) IS 0.5 and pitch_ratio(0) IS 1.0);
}

//********************************************************************************************************************
// At zero semitones the output is the input delayed by exactly one window, at every mix.  Zero mix takes the dry path
// alone, which is bit-identical.

static void test_identity(AudioTestContext &Test)
{
   for (int rate : { 8000, 44100, 192000 }) {
      for (bool stereo : { false, true }) {
         const int channels = stereo ? 2 : 1, n = pitch_window(rate);
         for (double mix : { 100.0, 37.0, 0.0 }) {
            PitchFixture fixture(rate, stereo, settings(0, mix));
            const auto input = noise_buffer(4 * n, channels, 3);
            auto buffer = input;
            fixture.run(buffer, 97);
            double error = 0;
            bool exact = true;
            for (size_t i = 0; i < buffer.size(); i++) {
               const size_t lag = size_t(n) * channels;
               const double expected = (i >= lag) ? double(input[i - lag]) : 0.0;
               error = std::max(error, std::abs(double(buffer[i]) - expected));
               exact = exact and (buffer[i] IS float(expected));
            }
            AUDIO_CHECK(error < 1e-6);
            if (mix IS 0.0) AUDIO_CHECK(exact);
            AUDIO_CHECK(fixture.Processor.realigned());
         }
      }
   }

   // Edge-bin and flat spectra still need a peak-region owner.  At unity ratio, constant, Nyquist and impulse inputs
   // are transparent rather than disappearing from the wet path.
   const int rate = 48000, n = pitch_window(rate), frames = 4 * n;
   std::vector<std::vector<float>> edge_cases;
   edge_cases.emplace_back(size_t(frames), 0.25f);
   edge_cases.emplace_back(size_t(frames));
   for (int i = 0; i < frames; i++) edge_cases.back()[size_t(i)] = (i & 1) ? -0.25f : 0.25f;
   edge_cases.emplace_back(size_t(frames));
   edge_cases.back()[size_t(n + 17)] = 0.75f;

   for (const auto &input : edge_cases) {
      PitchFixture fixture(rate, false, settings(0));
      auto output = input;
      fixture.run(output, 97);
      double error = 0;
      for (int i = 0; i < frames; i++) {
         const double expected = (i >= n) ? input[size_t(i - n)] : 0.0;
         error = std::max(error, std::abs(double(output[size_t(i)]) - expected));
      }
      AUDIO_CHECK(error < 1e-6);
   }

   auto impulse = edge_cases.back();
   PitchFixture shifted(rate, false, settings(7));
   shifted.run(impulse, 97);
   bool finite = true;
   for (auto sample : impulse) finite = finite and std::isfinite(sample);
   AUDIO_CHECK(finite and (peak(impulse) > 1e-3));
}

//********************************************************************************************************************
// The processor against the reference model, mono and stereo, at three rates, with fixed settings and block sizes.

static void test_model(AudioTestContext &Test)
{
   // The highest rate is limited to one case because the model is slow in debug builds.
   const PitchShiftSettings cases[] = { settings(7), settings(-12, 60, -3), settings(12), settings(-0.37, 80, 4) };
   for (int rate : { 8000, 44100, 192000 }) {
      for (bool stereo : { false, true }) {
         const int channels = stereo ? 2 : 1, n = pitch_window(rate);
         for (auto &setting : cases) {
            if ((rate IS 192000) and ((not stereo) or (&setting != &cases[1]))) continue;
            // A tone plus noise, with different content in each channel.
            auto input = harmonics(5 * n, channels, rate, 0.013 * rate, 6, 0.2);
            uint32_t seed = 11;
            for (size_t i = 0; i < input.size(); i++) input[i] += float(noise(seed) * ((i % 2) ? 0.05 : 0.1));
            const auto expected = reference(input, channels, rate, setting);
            PitchFixture fixture(rate, stereo, setting);
            auto buffer = input;
            fixture.run(buffer, 333);
            double error = 0;
            for (size_t i = 0; i < buffer.size(); i++) error = std::max(error, std::abs(buffer[i] - expected[i]));
            AUDIO_CHECK(error < 1e-6);
         }
      }
   }

   // Bursts separated by silence long enough for the processor to go idle and skip frames.  The model evaluates every
   // frame, so any output lost or altered by idling shows as a difference.
   for (int rate : { 8000, 44100 }) {
      const int n = pitch_window(rate);
      std::vector<float> input(size_t(14 * n) * 2, 0.0f);
      uint32_t seed = 21;
      for (int burst = 0; burst < 3; burst++) {
         const int at = burst * 4 * n + 7;
         for (int i = 0; i < n; i++) {
            const double envelope = std::sin(std::numbers::pi * i / n);
            input[size_t(at + i) * 2] = float(noise(seed) * 0.4 * envelope);
            input[size_t(at + i) * 2 + 1] = float(noise(seed) * 0.2 * envelope);
         }
      }
      for (auto &setting : { settings(5), settings(-9, 70) }) {
         const auto expected = reference(input, 2, rate, setting);
         PitchFixture fixture(rate, true, setting);
         auto buffer = input;
         fixture.run(buffer, 100);
         double error = 0;
         for (size_t i = 0; i < buffer.size(); i++) error = std::max(error, std::abs(buffer[i] - expected[i]));
         AUDIO_CHECK(error < 1e-6 and peak(buffer, buffer.size() - size_t(2 * n)) IS 0.0);
      }
   }
}

//********************************************************************************************************************
// Sustained tones leave at the transposed frequency with little spurious energy and an unchanged level.

static void test_tones(AudioTestContext &Test)
{
   // Analysis starts two windows in, once the output is in its steady state.
   for (int rate : { 44100, 48000, 96000 }) {
      for (double semitones : { -12.0, -7.0, -1.0, 0.5, 1.0, 4.0, 7.0, 12.0 }) {
         const int start = 2 * pitch_window(rate), frames = start + 32768;
         std::vector<float> buffer(frames, 0.0f);
         for (int i = 0; i < frames; i++) {
            buffer[size_t(i)] = float(0.5 * std::sin(2.0 * std::numbers::pi * 440 * i / rate));
         }
         PitchFixture fixture(rate, false, settings(semitones));
         fixture.run(buffer, 512);
         const auto output = channel(buffer, 1, 0);
         double frequency, spurious;
         tone_metrics(output, size_t(start), rate, frequency, spurious);
         const double cents = 1200.0 * std::log2(frequency / (440.0 * std::pow(2.0, semitones / 12.0)));
         AUDIO_CHECK(std::abs(cents) < 0.1);
         AUDIO_CHECK(spurious < -70);
         const double level = 20 * std::log10(rms(output, size_t(start), size_t(frames)) / (0.5 / std::sqrt(2.0)));
         AUDIO_CHECK(std::abs(level) < 0.05);
      }
   }

   // A 110 Hz harmonic tone keeps its partials distinct: the octave down places them 55 Hz apart.
   for (double semitones : { -12.0, -5.0, 5.0, 12.0 }) {
      const int rate = 48000, start = 2 * pitch_window(rate);
      auto buffer = harmonics(start + 32768, 1, rate, 110, 20, 0.1);
      PitchFixture fixture(rate, false, settings(semitones));
      fixture.run(buffer, 256);
      const double between = between_partials(channel(buffer, 1, 0), size_t(start), rate,
         110 * std::pow(2.0, semitones / 12.0));
      AUDIO_CHECK(between < -60);
   }
}

//********************************************************************************************************************
// The makeup table against a direct evaluation of the overlapping frames, at and between its nodes, for every window.
// The makeup is unity for an exact frequency and grows towards the half-bin error.

static void test_makeup(AudioTestContext &Test)
{
   for (int n : { 256, 2048, 8192 }) {
      const PitchTables tables(n);
      const int r = n / 8;
      double previous = 0;
      for (double e : { 0.0, 0.05, 0.1234, 0.25, 0.3001, 0.4, 0.49, 0.5 }) {
         double sum = 0, energy = 0;
         for (int i = 0; i < n; i++) energy += std::pow(0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / n), 2);
         for (int t = 0; t < r; t++) {
            cplx total = 0;
            for (int m = 0; m < 8; m++) {
               const int i = t + m * r;
               const double w = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / n);
               total += w * w * std::polar(1.0, 2 * std::numbers::pi * e * (i - n / 2) / n);
            }
            sum += std::abs(total) * r / energy;
         }
         const double expected = r / sum;
         AUDIO_CHECK(std::abs(tables.makeup(e) - expected) < 1e-5 and tables.makeup(-e) IS tables.makeup(e));
         AUDIO_CHECK(tables.makeup(e) >= previous);
         previous = tables.makeup(e);
      }
      AUDIO_CHECK(tables.makeup(0) IS 1.0 and tables.makeup(0.5) > 1.05 and tables.makeup(0.5) < 1.15);
   }

   // Noise-like content is decorrelated between overlapping frames and loses a little level, which grows with the
   // shift.  Low-passed noise keeps its content below the Nyquist frequency at an octave up.
   const int rate = 48000;
   for (double semitones : { -12.0, -7.0, -3.0, 3.0, 7.0, 12.0 }) {
      uint32_t seed = 1;
      std::vector<float> buffer(size_t(rate) * 2);
      double a = 0, b = 0;
      for (auto &sample : buffer) {
         a += 0.3 * (noise(seed) * 0.2 - a);
         b += 0.3 * (a - b);
         sample = float(b);
      }
      const auto input = channel(buffer, 1, 0);
      PitchFixture fixture(rate, false, settings(semitones));
      fixture.run(buffer, 512);
      const auto output = channel(buffer, 1, 0);
      const int n = pitch_window(rate);
      const double change = 20 * std::log10(rms(output, size_t(rate / 2 + n), output.size()) /
         rms(input, size_t(rate / 2), output.size() - n));
      AUDIO_CHECK((change < 0) and (change > -1.6));
   }
}

//********************************************************************************************************************
// Stereo processing is linked: the level ratio and time offset between channels follow the source, a channel that is
// silent stays silent, and a mono layout matches the left channel of a stereo pair with a silent right.

static void test_stereo(AudioTestContext &Test)
{
   const int rate = 48000, frames = rate * 2, offset = 12;
   const auto source = harmonics(frames + offset, 1, rate, 196, 12, 0.08);
   uint32_t seed = 5;
   std::vector<float> stereo(size_t(frames) * 2);
   for (int i = 0; i < frames; i++) {
      const float s = source[size_t(i + offset)] + float(noise(seed) * 0.02);
      stereo[size_t(i) * 2] = s;
      stereo[size_t(i) * 2 + 1] = 0.0f;
   }
   for (int i = offset; i < frames; i++) stereo[size_t(i) * 2 + 1] = 0.5f * stereo[size_t(i - offset) * 2];

   for (double semitones : { -5.0, 7.0 }) {
      auto buffer = stereo;
      PitchFixture fixture(rate, true, settings(semitones));
      fixture.run(buffer, 480);
      const auto left = channel(buffer, 2, 0), right = channel(buffer, 2, 1);
      const size_t a = size_t(rate / 2), b = size_t(frames - rate / 4);
      const double ild = 20 * std::log10(rms(right, a, b) / rms(left, a, b));
      AUDIO_CHECK(std::abs(ild + 6.0206) < 0.05);

      // The offset scales with the period of the shifted content.
      double best = -1;
      int lag = 0;
      const double el = rms(left, a, b), er = rms(right, a, b);
      for (int d = -40; d <= 40; d++) {
         double sum = 0;
         for (size_t i = a; i < b; i++) sum += left[i] * right[i + d];
         const double correlation = sum / (double(b - a) * el * er);
         if (correlation > best) { best = correlation; lag = d; }
      }
      AUDIO_CHECK(lag IS int(std::lround(offset / std::pow(2.0, semitones / 12.0))));
      AUDIO_CHECK(best > 0.995);
   }

   // Channel isolation and mono equivalence.
   auto pair = noise_buffer(rate, 2, 23);
   for (size_t i = 1; i < pair.size(); i += 2) pair[i] = 0.0f;
   std::vector<float> mono(pair.size() / 2);
   for (size_t i = 0; i < mono.size(); i++) mono[i] = pair[i * 2];
   PitchFixture both(rate, true, settings(5)), single(rate, false, settings(5));
   both.run(pair, 1000);
   single.run(mono, 1000);
   double leak = 0, difference = 0;
   for (size_t i = 0; i < mono.size(); i++) {
      leak = std::max(leak, std::abs(double(pair[i * 2 + 1])));
      difference = std::max(difference, std::abs(double(pair[i * 2]) - double(mono[i])));
   }
   AUDIO_CHECK(leak < 1e-6 and difference < 1e-6 and peak(mono) > 0.1);
}

//********************************************************************************************************************
// A short burst arrives one latency later, is mostly contained within 5 ms of that position and never precedes the
// input.  Output after the last input ends within the published tail, which is exact: once pending stops the output is
// silent.

static void test_timing(AudioTestContext &Test)
{
   for (int rate : { 8000, 48000, 96000 }) {
      for (double semitones : { -7.0, 7.0 }) {
         const int n = pitch_window(rate), at = 3 * n + 17, length = rate / 500;
         std::vector<float> buffer(size_t(8 * n), 0.0f);
         uint32_t seed = 9;
         for (int i = 0; i < length; i++) {
            buffer[size_t(at + i)] = float(noise(seed) * std::sin(std::numbers::pi * i / length));
         }
         PitchFixture fixture(rate, false, settings(semitones));
         fixture.run(buffer, 128);
         std::vector<float> tail;
         const int drained = fixture.drain(4 * n, &tail);
         buffer.insert(buffer.end(), tail.begin(), tail.end());

         // Causality, then containment around the expected position.
         bool causal = true;
         for (int i = 0; i <= at; i++) causal = causal and (buffer[size_t(i)] IS 0.0f);
         AUDIO_CHECK(causal);
         const double centre = at + length / 2.0 + n, tolerance = rate * 0.005;
         double inside = 0, total = 0, weighted = 0;
         int last = 0;
         for (size_t i = 0; i < buffer.size(); i++) {
            const double e = double(buffer[i]) * double(buffer[i]);
            total += e;
            weighted += e * double(i);
            if (std::abs(double(i) - centre) <= tolerance) inside += e;
            if (buffer[i] != 0.0f) last = int(i);
         }
         AUDIO_CHECK(10 * std::log10((total - inside) / total) < -20);
         AUDIO_CHECK(std::abs(weighted / total - centre) < rate * 0.001);
         AUDIO_CHECK(last - (at + length - 1) <= 2 * n - 1);
         AUDIO_CHECK(drained <= int(fixture.Processor.tail_frames()));

         // The decay estimate covers a 60 dB decay of the burst beyond the latency.
         double top = 0;
         for (auto sample : buffer) top = std::max(top, std::abs(double(sample)));
         int decayed = at + length;
         for (size_t i = 0; i < buffer.size(); i++) if (std::abs(double(buffer[i])) > top * 1e-3) decayed = int(i);
         AUDIO_CHECK(decayed - (at + length + n) <= int(fixture.Processor.decay_estimate()));
      }
   }

   // Once the drain completes, further silence produces silence and no work.
   PitchFixture quiet(48000, true, settings(7));
   auto burst = noise_buffer(4800, 2, 31);
   quiet.run(burst);
   quiet.drain(1 << 20);
   AUDIO_CHECK(not quiet.Processor.pending());
   std::vector<float> zeros(size_t(48000) * 2, 0.0f);
   quiet.run(zeros, 77);
   AUDIO_CHECK(peak(zeros) IS 0.0);
}

//********************************************************************************************************************
// Live edits: a semitone change takes effect within one window, mix and gain ramp over exactly 10 ms, and a return
// to zero semitones realigns the wet path with the input.

static void test_edits(AudioTestContext &Test)
{
   const int rate = 48000, n = pitch_window(rate);
   const int frames = rate * 2;
   std::vector<float> tone(frames, 0.0f);
   for (int i = 0; i < frames; i++) tone[size_t(i)] = float(0.5 * std::sin(2.0 * std::numbers::pi * 440 * i / rate));

   PitchFixture fixture(rate, false, settings(0));
   auto &p = fixture.Processor;
   std::vector<float> first(tone.begin(), tone.begin() + rate / 2), second(tone.begin() + rate / 2, tone.end());
   fixture.run(first);
   fixture.change(settings(7, 40, -6));
   AUDIO_CHECK(p.frequency_ratio() IS std::pow(2.0, 7.0 / 12.0) and p.ramping());
   std::vector<float> ramp(second.begin(), second.begin() + rate / 100 - 1);
   fixture.run(ramp);
   AUDIO_CHECK(p.ramping() and std::abs(p.wet_mix() - 0.4) > 1e-9);
   std::vector<float> one(second.begin() + rate / 100 - 1, second.begin() + rate / 100);
   fixture.run(one);
   AUDIO_CHECK((not p.ramping()) and (p.wet_mix() IS 0.4) and (p.trim_gain() IS std::pow(10.0, -6.0 / 20.0)));

   std::vector<float> rest(second.begin() + rate / 100, second.end());
   fixture.run(rest);
   const auto output = channel(rest, 1, 0);
   const auto power = spectrum_power(output, size_t(2 * n), 32768);
   auto level = [&](double Frequency) {
      const size_t k = size_t(std::lround(Frequency * 32768 / rate));
      double sum = 0;
      for (size_t i = k - 4; i <= k + 4; i++) sum += power[i];
      return sum;
   };
   // Dry 440 Hz at 60% and the fifth at 40%.
   const double ratio = std::sqrt(level(440 * std::pow(2.0, 7.0 / 12.0)) / level(440));
   AUDIO_CHECK(std::abs(20 * std::log10(ratio) - 20 * std::log10(0.4 / 0.6)) < 0.5);

   // Return to zero semitones at full mix: the rotations step back to zero and the output becomes the delayed input.
   fixture.change(settings(0));
   auto noise_in = noise_buffer(6 * n, 1, 19);
   auto out = noise_in;
   fixture.run(out, 64);
   AUDIO_CHECK(p.realigned());
   double error = 0;
   for (int i = 4 * n; i < 6 * n; i++) {
      error = std::max(error, std::abs(double(out[size_t(i)]) - double(noise_in[size_t(i - n)])));
   }
   AUDIO_CHECK(error < 1e-6);
}

//********************************************************************************************************************
// Reset discards history and applies the latest settings; updates while inactive are kept for reset.

static void test_lifecycle(AudioTestContext &Test)
{
   PitchFixture fixture(48000, true, PitchShiftSettings());
   auto &p = fixture.Processor;
   auto burst = noise_buffer(4800, 2, 31);
   fixture.run(burst);
   fixture.change(settings(5, 30, 6));
   AUDIO_CHECK(p.pending() and p.ramping());

   p.reset();
   AUDIO_CHECK((not p.pending()) and (not p.ramping()) and p.realigned());
   AUDIO_CHECK(p.wet_mix() IS 0.3 and p.frequency_ratio() IS std::pow(2.0, 5.0 / 12.0));

   // ResetPending: the update is stored and reset() applies it without a ramp.
   fixture.Effect.ResetPending = true;
   fixture.change(settings(-3, 90));
   AUDIO_CHECK(p.wet_mix() IS 0.3);
   fixture.Effect.ResetPending = false;
   p.reset();
   AUDIO_CHECK(p.wet_mix() IS 0.9 and p.frequency_ratio() IS std::pow(2.0, -3.0 / 12.0));

   // A rate change through prepare and publish changes the window and latency.
   fixture.Effect.OutputRate = 96000;
   std::unique_ptr<AudioEffectConfiguration> config;
   AUDIO_CHECK(p.prepare(96000, true, config) IS ERR::Okay);
   config->publish();
   p.reset();
   AUDIO_CHECK(p.SampleRate IS 96000 and p.window_size() IS 4096 and p.latency() IS 4096);

   // An update derived for another rate is ignored until reset.
   PitchShiftTarget stale;
   const auto next = settings(2, 10);
   pitch_target(next, 48000, stale);
   p.update(next, &stale);
   AUDIO_CHECK(p.wet_mix() IS 0.9);
   p.reset();
   AUDIO_CHECK(p.wet_mix() IS 0.1);

   // A layout wider than stereo leaves the processor inactive.
   fixture.Effect.Layout = { 0, 1, 2 };
   p.reset();
   std::vector<float> wide = { 0.5f, 0.25f };
   const auto untouched = wide;
   p.process(wide.data(), 1);
   AUDIO_CHECK(wide IS untouched and (not p.pending()) and p.tail_frames() IS 0);
   fixture.Effect.Layout.clear();

   // Non-finite input is silence and does not poison the history.
   PitchFixture hostile(48000, true, settings(7));
   std::vector<float> bad = noise_buffer(4096, 2, 7);
   bad[10] = std::numeric_limits<float>::quiet_NaN();
   bad[11] = std::numeric_limits<float>::infinity();
   bad[300] = -std::numeric_limits<float>::infinity();
   hostile.run(bad);
   std::vector<float> after;
   hostile.drain(1 << 20, &after);
   bool finite = true;
   for (auto sample : bad) finite = finite and std::isfinite(sample);
   for (auto sample : after) finite = finite and std::isfinite(sample);
   AUDIO_CHECK(finite and (not hostile.Processor.pending()));
}

//********************************************************************************************************************
// Output does not depend on how the frames are divided into blocks, including edits at fixed frame positions.

static void test_blocks(AudioTestContext &Test)
{
   const int rate = 44100, quarter = 6000;
   auto input = noise_buffer(4 * quarter, 2, 37);
   // A silent gap long enough to reach the idle state.
   std::fill(input.begin() + 2 * quarter * 2, input.begin() + 3 * quarter * 2, 0.0f);
   std::vector<std::vector<float>> results;
   for (int block : { 4 * quarter, 1024, 333, 64, 1 }) {
      PitchFixture fixture(rate, true, settings(3));
      std::vector<float> output;
      for (int part = 0; part < 4; part++) {
         if (part IS 1) fixture.change(settings(-11.5, 70, -4));
         if (part IS 2) fixture.change(settings(0, 100, 2));
         if (part IS 3) fixture.change(settings(9, 20, 0));
         std::vector<float> chunk(input.begin() + part * quarter * 2, input.begin() + (part + 1) * quarter * 2);
         fixture.run(chunk, block);
         output.insert(output.end(), chunk.begin(), chunk.end());
      }
      results.push_back(std::move(output));
   }
   for (size_t i = 1; i < results.size(); i++) AUDIO_CHECK(results[i] IS results[0]);
}

//********************************************************************************************************************
// The worst-case render cost at 192 kHz stereo and the throughput at 48 kHz are logged.

static void test_resources(AudioTestContext &Test)
{
   PitchFixture fixture(192000, true, settings(7));
   uint32_t seed = 41;
   std::vector<float> buffer(size_t(1024) * 2);
   double worst_block = 0;
   for (int block = 0; block < 200; block++) {
      fixture.change(settings((block % 25) - 12.0, block % 101, (block % 5) - 2.0));
      for (auto &sample : buffer) sample = float(noise(seed) * 0.5);
      const auto start = std::chrono::steady_clock::now();
      fixture.Processor.process(buffer.data(), 1024);
      const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start);
      worst_block = std::max(worst_block, elapsed.count());
   }
   AUDIO_CHECK(peak(buffer) < 4.0);

   PitchFixture typical(48000, true, settings(7));
   auto music = noise_buffer(48000 * 4, 2, 43, 0.3);
   const auto start = std::chrono::steady_clock::now();
   typical.run(music, 512);
   const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
   AUDIO_CHECK(typical.Processor.storage_bytes() < 320000);
   kt::Log("AudioTests").msg("Pitch shift: worst 1024-frame block at 192 kHz stereo %.1f us; %.0fx real time at "
      "48 kHz stereo; storage %d bytes at 48 kHz stereo", worst_block, 4.0 / seconds,
      int(typical.Processor.storage_bytes()));
}

//********************************************************************************************************************

static void run(AudioTestContext &Test)
{
   test_configuration(Test);
   test_identity(Test);
   test_model(Test);
   test_tones(Test);
   test_makeup(Test);
   test_stereo(Test);
   test_timing(Test);
   test_edits(Test);
   test_lifecycle(Test);
   test_blocks(Test);
   test_resources(Test);
}

} // namespace audio_tests_audio_pitchshift_dsp

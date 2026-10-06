// Spectrum, waveform and level measurement for the AudioAnalyser class.
//
// The render thread writes the measured channel, or the mean of every channel, into a ring buffer and leaves the audio
// unchanged.  Analysis runs on the client thread from a copy of the ring, so the render cost is one sum per sample and
// the mixer lock is only held for the copy.  RMS and stereo correlation are accumulated over each meter interval.

#pragma once

#include <array>
#include <bit>
#include <complex>
#include <numbers>

constexpr int ANALYSER_MIN_RATE     = 8000;
constexpr int ANALYSER_MAX_RATE     = 192000;
constexpr int ANALYSER_MAX_WINDOW   = 8192;   // Largest FFT size, reached at 192 kHz
constexpr int ANALYSER_MAX_WAVEFORM = 16384;  // Largest GetWaveform() request, in frames
constexpr int ANALYSER_MAX_BANDS    = 1024;
constexpr double ANALYSER_HISTORY   = 1.0;    // Seconds of device queue that the ring can compensate for
constexpr double ANALYSER_FLOOR_DB  = -120;
constexpr double ANALYSER_FLOOR_POWER = 1e-12; // ANALYSER_FLOOR_DB as a mean square

// Processor meter value indexes

enum { ANALYSER_RMS = 0, ANALYSER_CORRELATION };

//********************************************************************************************************************
// FFT size in frames: the largest power of two not exceeding Rate/16, e.g. 2048 at 44.1 and 48 kHz.  The window spans
// between 32 and 47 ms at every supported rate.

inline int analyser_window(int Rate)
{
   int size = 256;
   while ((size < ANALYSER_MAX_WINDOW) and (size * 2 <= Rate / 16)) size <<= 1;
   return size;
}

// Ring capacity in frames, which is a power of two that covers the device queue and the largest read.

inline size_t analyser_capacity(int Rate)
{
   return std::bit_ceil(size_t(Rate * ANALYSER_HISTORY) + size_t(ANALYSER_MAX_WAVEFORM));
}

//********************************************************************************************************************
// In-place iterative radix-2 FFT.  The size of Data must be a power of two.

inline void analyser_fft(std::span<std::complex<double>> Data)
{
   const size_t n = Data.size();
   for (size_t i = 1, j = 0; i < n; i++) {
      size_t bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) std::swap(Data[i], Data[j]);
   }

   for (size_t len = 2; len <= n; len <<= 1) {
      const double angle = -2.0 * std::numbers::pi / double(len);
      const std::complex<double> step(std::cos(angle), std::sin(angle));
      const size_t half = len >> 1;
      for (size_t i = 0; i < n; i += len) {
         std::complex<double> w(1.0, 0.0);
         for (size_t k = 0; k < half; k++) {
            const auto u = Data[i + k];
            const auto v = Data[i + k + half] * w;
            Data[i + k] = u + v;
            Data[i + k + half] = u - v;
            w *= step;
         }
      }
   }
}

//********************************************************************************************************************
// Hann-windowed power spectrum of Samples, with Samples.size() / 2 + 1 bins.  Each bin holds the power of a sinusoid,
// such that the bins of a sinusoid with amplitude A sum to A².  The size of Samples must be a power of two.

inline void analyser_power(std::span<const float> Samples, std::vector<double> &Power)
{
   const size_t n = Samples.size();
   std::vector<std::complex<double>> data(n);
   double energy = 0;
   for (size_t i = 0; i < n; i++) {
      const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / double(n)); // Periodic Hann
      energy += w * w;
      data[i] = { double(Samples[i]) * w, 0 };
   }

   analyser_fft(data);

   // By Parseval's theorem the positive frequencies of a sinusoid hold N·A²·energy/4.  DC and Nyquist are not
   // mirrored, so they take half of the scale.

   const double scale = 4.0 / (double(n) * energy);
   Power.resize(n / 2 + 1);
   for (size_t k = 0; k <= n / 2; k++) Power[k] = std::norm(data[k]) * scale;
   Power[0] *= 0.5;
   Power[n / 2] *= 0.5;
}

//********************************************************************************************************************
// Level in dBFS of each band between consecutive Edges, from a power spectrum with bins BinWidth hertz apart.  A bin
// belongs to the band that contains its centre frequency, and the final band includes its upper edge.  A band that
// contains no bin centre takes the power interpolated at its geometric centre, scaled by the band's share of a bin,
// so that narrow low-frequency bands follow their neighbours instead of reading silence.

inline void analyser_bands(std::span<const double> Power, double BinWidth, std::span<const double> Edges,
   std::span<double> Levels)
{
   const double floor_power = std::pow(10.0, ANALYSER_FLOOR_DB / 10.0);
   const size_t bins = Power.size();

   for (size_t b = 0; b + 1 < Edges.size(); b++) {
      const double low = Edges[b] / BinWidth, high = Edges[b + 1] / BinWidth;
      const size_t first = std::min(bins, size_t(std::ceil(low)));
      const bool last = b + 2 IS Edges.size();
      const size_t end = std::min(bins, last ? size_t(std::floor(high)) + 1 : size_t(std::ceil(high)));

      double sum = 0;
      if (first < end) {
         for (size_t k = first; k < end; k++) sum += Power[k];
      }
      else if (bins > 1) {
         const double centre = std::clamp((low > 0) ? std::sqrt(low * high) : (high * 0.5), 0.0, double(bins - 1));
         const size_t k = std::min(bins - 2, size_t(centre));
         const double t = centre - double(k);
         sum = (Power[k] + t * (Power[k + 1] - Power[k])) * (high - low);
      }

      Levels[b] = 10.0 * std::log10(std::max(sum, floor_power));
   }
}

//********************************************************************************************************************

class AnalyserProcessor final : public AudioEffectProcessor {
public:
   extAudioEffect *Owner;
   std::vector<float> History; // Capture ring; the size is a power of two, or zero until configured
   uint64_t Written = 0;       // Captured and silent frames since History was configured
   uint64_t Origin = 0;        // Frames before this position predate the current channel selection
   int64_t RenderTime = 0;     // PreciseTime() for the most recent captured or skipped block, or zero
   int Rate = 0;               // Sample rate of History

private:
   int channels = 0;
   int selection = -1;         // Captured channel index, or -1 for the mean of every channel
   bool active = false;
   bool resume = true;

   // Sums over the current meter interval, restarted by meter_reset().

   std::array<double, MAX_FORMAT_CHANNELS> squares {};
   double cross = 0;           // Sum of the products of the first two channels
   uint64_t measured = 0;

   void measure(const float *Buffer, int Frames) {
      const int count = std::min(channels, MAX_FORMAT_CHANNELS);
      if (count < 1) return;
      for (int frame = 0; frame < Frames; frame++) {
         const float *in = Buffer + size_t(frame) * channels;
         for (int c = 0; c < count; c++) squares[c] += double(in[c]) * double(in[c]);
      }
      if (channels IS 2) {
         for (int frame = 0; frame < Frames; frame++) {
            cross += double(Buffer[size_t(frame) * 2]) * double(Buffer[size_t(frame) * 2 + 1]);
         }
      }
      measured += uint64_t(Frames);
   }

   // Silent spans are bounded by the ring capacity even if the device has been idle for hours.

   void silence(size_t Frames) {
      const size_t count = std::min(Frames, History.size());
      const size_t start = size_t(Written) & (History.size() - 1);
      const size_t first = std::min(count, History.size() - start);
      std::fill_n(History.data() + start, first, 0.0f);
      std::fill_n(History.data(), count - first, 0.0f);
      Written += Frames;
   }

   // Explicit skip notifications cover gaps while the device renders.  On resuming, also account for time during
   // which the device rendered nothing.  Ordinary consecutive captures use frame counts, independent of jitter.

   void resume_at(int64_t Now, int Frames) {
      if (resume and RenderTime and (Now > RenderTime)) {
         const double elapsed = double(Now - RenderTime) * double(Rate) / 1000000.0;
         const double gap = std::clamp(elapsed - double(Frames), 0.0, double(History.size()));
         silence(size_t(std::llround(gap)));
      }
   }

public:
   // Storage is allocated on the control thread.  Publication swaps it in under the mixer lock, and the retired
   // storage is released with the configuration after unlocking.  History is retained if the rate is unchanged, in
   // which case the prepared storage is released unused.

   class Configuration final : public AudioEffectConfiguration {
   public:
      AnalyserProcessor *Processor;
      std::vector<float> Storage;
      int Rate;

      Configuration(AnalyserProcessor *Target, int PreparedRate) : Processor(Target), Rate(PreparedRate) {
         if (Rate > 0) Storage.assign(analyser_capacity(Rate), 0.0f);
      }

      void publish() override {
         auto &p = *Processor;
         if (Rate IS p.Rate) return;
         p.History.swap(Storage);
         p.Rate       = Rate;
         p.Written    = 0;
         p.Origin     = 0;
         p.RenderTime = 0;
         p.resume     = true;
      }

      int64_t latency() const override { return 0; }
   };

   explicit AnalyserProcessor(extAudioEffect *Effect, int Selection = -1) : Owner(Effect), selection(Selection) { }

   ERR prepare(int PrepareRate, bool Stereo, std::unique_ptr<AudioEffectConfiguration> &Result) override {
      if ((PrepareRate > 0) and ((PrepareRate < ANALYSER_MIN_RATE) or (PrepareRate > ANALYSER_MAX_RATE))) {
         return ERR::NoSupport;
      }
      Result = std::make_unique<Configuration>(this, PrepareRate);
      return ERR::Okay;
   }

   // History is not discarded, because frames that are still queued at the device have yet to be heard.

   void reset() override {
      channels = int(Owner->Layout.size());
      active = (Rate > 0) and (Owner->OutputRate IS Rate) and (channels > 0) and (not History.empty());
      resume = true;
      meter_reset();
   }

   void meter_reset() override {
      squares.fill(0);
      cross = 0;
      measured = 0;
   }

   // RMS is the level of the mean square over the interval, so a full-scale sinusoid reads -3.01 dBFS.  Correlation
   // is zero if either channel is at the floor, because the phase of silence is undefined.

   double meter_value(int Value, int Channel, bool &Floor) const override {
      if (Value IS ANALYSER_RMS) {
         const double power = ((measured > 0) and (Channel >= 0) and (Channel < MAX_FORMAT_CHANNELS)) ?
            squares[Channel] / double(measured) : 0;
         if (not (power > ANALYSER_FLOOR_POWER)) { // Also catches NaN
            Floor = true;
            return ANALYSER_FLOOR_DB;
         }
         return 10.0 * std::log10(power);
      }
      else if (Value IS ANALYSER_CORRELATION) {
         if ((channels != 2) or (measured < 1)) return 0;
         const double floor = ANALYSER_FLOOR_POWER * double(measured);
         if (not ((squares[0] > floor) and (squares[1] > floor))) return 0;
         const double result = cross / std::sqrt(squares[0] * squares[1]);
         return std::isfinite(result) ? std::clamp(result, -1.0, 1.0) : 0;
      }
      return 0;
   }

   // Selects the captured channel.  History captured from the previous selection is discarded, so that a read never
   // mixes two selections.  Called under the mixer lock.

   void select(int Selection) {
      if (Selection IS selection) return;
      selection = Selection;
      Origin = Written;
   }

   int selected() const { return selection; }

   void process(float *Buffer, int Frames) override {
      process(Buffer, Frames, PreciseTime());
   }

   void process(float *Buffer, int Frames, int64_t Now) {
      if (Frames <= 0) return;
      measure(Buffer, Frames);
      if (not active) return;
      resume_at(Now, Frames);

      const size_t mask = History.size() - 1;
      if (selection < 0) {
         const float scale = 1.0f / float(channels);
         for (int frame = 0; frame < Frames; frame++) {
            const float *in = Buffer + size_t(frame) * channels;
            float sum = 0;
            for (int c = 0; c < channels; c++) sum += in[c];
            History[size_t(Written + frame) & mask] = sum * scale;
         }
      }
      else if (selection < channels) {
         for (int frame = 0; frame < Frames; frame++) {
            History[size_t(Written + frame) & mask] = Buffer[size_t(frame) * channels + size_t(selection)];
         }
      }
      else { // The layout has narrowed since selection; reads fail with ERR::OutOfRange
         for (int frame = 0; frame < Frames; frame++) History[size_t(Written + frame) & mask] = 0.0f;
      }
      Written += Frames;
      RenderTime = Now;
      resume = false;
   }

   void skip(int Frames) override {
      skip(Frames, PreciseTime());
   }

   void skip(int Frames, int64_t Now) {
      if ((Rate <= 0) or History.empty() or (Frames <= 0)) return;
      resume_at(Now, Frames);
      silence(size_t(Frames));
      RenderTime = Now;
      resume = true;
   }

   // The frame position that is audible at Now, where Lag is the device queue in seconds immediately after each
   // submission.  Positions beyond Written have not been rendered because the source has stopped, so they are silent.

   int64_t audible(int64_t Now, double Lag) const {
      if ((not RenderTime) or (Rate <= 0)) return 0;
      const double elapsed = std::max(0.0, double(Now - RenderTime) / 1000000.0);
      const double lag = std::clamp(Lag, 0.0, ANALYSER_HISTORY);
      return int64_t(Written) - std::llround(lag * Rate) + std::llround(elapsed * Rate);
   }

   // Copies Output.size() frames that end at End, oldest first.  Frames that were never written, have since been
   // overwritten or predate the channel selection are silent.  The caller holds the mixer lock.

   void copy(int64_t End, std::span<float> Output) const {
      const int64_t count = std::ssize(Output);
      const int64_t written = int64_t(Written);
      const int64_t oldest = std::max(written - std::ssize(History), int64_t(Origin));
      const size_t mask = History.empty() ? 0 : History.size() - 1;
      for (int64_t i = 0; i < count; i++) {
         const int64_t position = End - count + i;
         const bool stored = (not History.empty()) and (position >= 0) and (position >= oldest) and
            (position < written);
         Output[size_t(i)] = stored ? History[size_t(position) & mask] : 0.0f;
      }
   }
};

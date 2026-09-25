#pragma once

// RBJ biquad sections for the AudioEqualiser processor.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <span>
#include <vector>

struct EQSection {
   AudioEQBand Band;
   double B0 = 1, B1 = 0, B2 = 0, A1 = 0, A2 = 0;
   std::array<double, 2> Z1 = {}, Z2 = {};
};

class EqualiserProcessor final : public AudioEffectProcessor {
public:
   extAudioEffect *Owner;
   std::vector<EQSection> Sections;
   double Trim = 1;
   int Rate = 0;
   int Channels = 2;

private:
   std::vector<EQSection> previous, pending;
   std::vector<int> pending_origins;
   double previous_trim = 1, pending_trim = 1;
   int transition_left = 0, transition_frames = 1;
   bool has_pending = false;

   static double filter(std::vector<EQSection> &Sections, double Sample, int Channel) {
      for (auto &section : Sections) {
         const double output = section.B0 * Sample + section.Z1[Channel];
         section.Z1[Channel] = section.B1 * Sample - section.A1 * output + section.Z2[Channel];
         section.Z2[Channel] = section.B2 * Sample - section.A2 * output;
         Sample = output;
      }
      return Sample;
   }

   void start_transition(std::vector<EQSection> &Next, const std::vector<int> &Origins, double NextTrim) {
      for (size_t i = 0; i < Next.size(); i++) {
         const auto origin = Origins[i];
         if ((origin >= 0) and (size_t(origin) < Sections.size())) {
            Next[i].Z1 = Sections[origin].Z1;
            Next[i].Z2 = Sections[origin].Z2;
         }
      }
      previous.swap(Sections);
      Sections.swap(Next);
      previous_trim = Trim;
      Trim = NextTrim;
      transition_frames = std::max(1, Rate / 100); // 10 ms, independent of mixer block size.
      transition_left = transition_frames;
   }

public:
   // Caller holds the mixer lock.  Next and Origins are prepared beforehand and retire replaced storage on return.
   // Keep an in-flight fade intact; rapid edits coalesce into one queued target with composed entry origins.

   void update(std::vector<EQSection> &Next, std::vector<int> &Origins, double NextTrim) {
      if ((Rate < 2) or Owner->ResetPending) {
         Sections.swap(Next);
         Trim = NextTrim;
         transition_left = 0;
         has_pending = false;
      }
      else if (transition_left or has_pending) {
         if (has_pending) {
            for (auto &origin : Origins) {
               origin = ((origin >= 0) and (size_t(origin) < pending_origins.size())) ?
                  pending_origins[origin] : -1;
            }
         }

         pending.swap(Next);
         pending_origins.swap(Origins);
         pending_trim = NextTrim;
         has_pending = true;
      }
      else start_transition(Next, Origins, NextTrim);
   }

   EqualiserProcessor(extAudioEffect *Effect, const std::vector<AudioEQBand> &Bands, double Gain)
      : Owner(Effect) {
      Sections.reserve(Bands.size());
      for (const auto &band : Bands) Sections.push_back({ band });
      Trim = std::pow(10.0, Gain / 20.0);
   }

   void compute(EQSection &Section) {
      const auto &band = Section.Band;
      if (Rate < 2) return;
      const double frequency = std::min(band.Frequency, double(Rate) * 0.499);
      const double omega = 2.0 * std::numbers::pi * frequency / double(Rate);
      const double cosine = std::cos(omega);
      const double sine = std::sin(omega);
      const double amplitude = std::pow(10.0, band.Gain / 40.0);
      const double alpha = (band.Type IS EQB::LOW_SHELF or band.Type IS EQB::HIGH_SHELF) ?
         sine * 0.5 * std::sqrt((amplitude + 1.0 / amplitude) * (1.0 / band.Q - 1.0) + 2.0) :
         sine / (2.0 * band.Q);
      const double root = 2.0 * std::sqrt(amplitude) * alpha;
      double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

      switch (band.Type) {
         case EQB::PEAK:
            b0 = 1 + alpha * amplitude; b1 = -2 * cosine; b2 = 1 - alpha * amplitude;
            a0 = 1 + alpha / amplitude; a1 = -2 * cosine; a2 = 1 - alpha / amplitude;
            break;
         case EQB::LOW_SHELF:
            b0 = amplitude * ((amplitude + 1) - (amplitude - 1) * cosine + root);
            b1 = 2 * amplitude * ((amplitude - 1) - (amplitude + 1) * cosine);
            b2 = amplitude * ((amplitude + 1) - (amplitude - 1) * cosine - root);
            a0 = (amplitude + 1) + (amplitude - 1) * cosine + root;
            a1 = -2 * ((amplitude - 1) + (amplitude + 1) * cosine);
            a2 = (amplitude + 1) + (amplitude - 1) * cosine - root;
            break;
         case EQB::HIGH_SHELF:
            b0 = amplitude * ((amplitude + 1) + (amplitude - 1) * cosine + root);
            b1 = -2 * amplitude * ((amplitude - 1) + (amplitude + 1) * cosine);
            b2 = amplitude * ((amplitude + 1) + (amplitude - 1) * cosine - root);
            a0 = (amplitude + 1) - (amplitude - 1) * cosine + root;
            a1 = 2 * ((amplitude - 1) - (amplitude + 1) * cosine);
            a2 = (amplitude + 1) - (amplitude - 1) * cosine - root;
            break;
         case EQB::LOW_PASS:
            b0 = (1 - cosine) * 0.5; b1 = 1 - cosine; b2 = b0;
            a0 = 1 + alpha; a1 = -2 * cosine; a2 = 1 - alpha;
            break;
         case EQB::HIGH_PASS:
            b0 = (1 + cosine) * 0.5; b1 = -(1 + cosine); b2 = b0;
            a0 = 1 + alpha; a1 = -2 * cosine; a2 = 1 - alpha;
            break;
         default: return;
      }
      Section.B0 = b0 / a0; Section.B1 = b1 / a0; Section.B2 = b2 / a0;
      Section.A1 = a1 / a0; Section.A2 = a2 / a0;
   }

   void reset() override {
      if (has_pending) {
         Sections.swap(pending);
         Trim = pending_trim;
         has_pending = false;
      }

      transition_left = 0;
      Rate = Owner->OutputRate;
      Channels = Owner->Stereo ? 2 : 1;
      for (auto &section : Sections) {
         section.Z1 = {}; section.Z2 = {};
         compute(section);
      }
   }

   void process(float *Buffer, int Frames) override {
      for (int frame = 0; frame < Frames; ++frame) {
         if ((not transition_left) and has_pending) {
            start_transition(pending, pending_origins, pending_trim);
            has_pending = false;
         }

         const double blend = 1.0 - double(transition_left) / double(transition_frames);
         for (int channel = 0; channel < Channels; ++channel) {
            const double sample = Buffer[frame * Channels + channel];
            double output = filter(Sections, sample, channel) * Trim;
            if (transition_left) {
               const double old_output = filter(previous, sample, channel) * previous_trim;
               output = old_output + (output - old_output) * blend;
            }

            Buffer[frame * Channels + channel] = float(output);
         }

         if (transition_left) --transition_left;
      }
   }
};

//********************************************************************************************************************
// Evaluate the combined magnitude response of every section and the trim, in decibels.  Coefficients must already be
// computed for Model.Rate.  The result is floored at -300 dB so that a response null never produces -inf.

inline void equaliser_magnitudes(const EqualiserProcessor &Model, std::span<const double> Frequencies,
   std::span<double> Magnitudes)
{
   for (size_t i = 0; i < Frequencies.size(); i++) {
      const double omega = 2.0 * std::numbers::pi * Frequencies[i] / double(Model.Rate);
      const auto z1 = std::polar(1.0, -omega);
      const auto z2 = z1 * z1;
      double magnitude = Model.Trim;
      for (const auto &section : Model.Sections) {
         const auto numerator = section.B0 + section.B1 * z1 + section.B2 * z2;
         const auto denominator = 1.0 + section.A1 * z1 + section.A2 * z2;
         magnitude *= std::abs(numerator) / std::abs(denominator);
      }

      Magnitudes[i] = 20.0 * std::log10(std::max(magnitude, 1e-15));
   }
}

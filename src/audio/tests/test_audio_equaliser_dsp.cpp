#include <kotuku/main.h>
#include <kotuku/modules/audio.h>
#include <kotuku/modules/script.h>
#include <kotuku/modules/filesystem.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include "audio.h"
#include "audio_effect.cpp"
#include "audio_equaliser_dsp.h"

// Prepare exactly as the control thread does; update() alone runs under the mixer lock.
static void change(EqualiserProcessor &Processor, const std::vector<AudioEQBand> &Bands,
   std::vector<int> Origins, double Gain)
{
   EqualiserProcessor prepared(Processor.Owner, Bands, Gain);
   prepared.Rate = Processor.Rate;
   for (auto &section : prepared.Sections) prepared.compute(section);
   Processor.update(prepared.Sections, Origins, prepared.Trim);
}

static void test_transitions()
{
   extAudioEffect effect(nullptr, 1);
   effect.OutputRate = 48000;
   effect.Stereo = 1;
   effect.ResetPending = false;
   EqualiserProcessor ramp(&effect, {}, 0);
   ramp.reset();
   change(ramp, {}, {}, 6.020599913279624);
   float samples[962];
   std::fill(std::begin(samples), std::end(samples), 1.0f);
   ramp.process(samples, 481);
   assert(samples[0] IS 1.0f and samples[960] IS 2.0f);
   for (int i = 0; i < 481; i++) {
      assert(std::abs(samples[i * 2] - (1.0 + double(i) / 480.0)) < 0.000001);
      assert(samples[i * 2] IS samples[i * 2 + 1]);
   }

   // A rapid edit must not interrupt the current ramp; only the latest queued target is heard next.
   change(ramp, {}, {}, 0);
   float first[240];
   std::fill(std::begin(first), std::end(first), 1.0f);
   ramp.process(first, 120);
   change(ramp, {}, {}, 12.041199826559248);
   change(ramp, {}, {}, -6.020599913279624);
   float rest[1682];
   std::fill(std::begin(rest), std::end(rest), 1.0f);
   ramp.process(rest, 841);
   assert(std::abs(rest[0] - 1.75f) < 0.000001);
   assert(rest[720] IS 1.0f and rest[1680] IS 0.5f);
   for (int i = 1; i < 841; i++) {
      assert(rest[i * 2] <= rest[(i - 1) * 2]);
      assert(std::abs(rest[i * 2] - rest[(i - 1) * 2]) < 0.003);
   }

   // Reset during a fade must adopt the queued target and the new output layout immediately.
   change(ramp, {}, {}, 0);
   change(ramp, {}, {}, 6.020599913279624);
   effect.OutputRate = 24000;
   effect.Stereo = 0;
   ramp.reset();
   float one[] = { 1 };
   ramp.process(one, 1);
   assert(one[0] IS 2.0f and ramp.Rate IS 24000 and ramp.Channels IS 1);

   // A filter topology change starts with the old output and converges to the new transfer function.
   effect.OutputRate = 48000;
   const AudioEQBand low { EQB::LOW_PASS, 1000, 0, 0.707 };
   const AudioEQBand high { EQB::HIGH_PASS, 1000, 0, 0.707 };
   EqualiserProcessor topology(&effect, { low }, 0);
   topology.reset();
   float steady[2048];
   std::fill(std::begin(steady), std::end(steady), 1.0f);
   topology.process(steady, 2048);
   const float old_output = steady[2047];
   change(topology, { high }, { 0 }, 0);
   std::fill(std::begin(steady), std::end(steady), 1.0f);
   topology.process(steady, 2048);
   assert(std::abs(steady[0] - old_output) < 0.000001);
   assert(std::abs(steady[2047]) < 0.000001);
   for (auto sample : steady) assert(std::isfinite(sample));

   // Block boundaries do not change the fade, including queued targets and stereo filter history.
   effect.Stereo = 1;
   EqualiserProcessor whole(&effect, { low }, 0), split(&effect, { low }, 0);
   whole.reset();
   split.reset();
   change(whole, { high }, { 0 }, -3);
   change(split, { high }, { 0 }, -3);
   change(whole, { low, high }, { 0, -1 }, 2);
   change(split, { low, high }, { 0, -1 }, 2);
   // Remove the first pending band: the surviving new band must retain its -1 origin.
   change(whole, { high }, { 1 }, 1);
   change(split, { high }, { 1 }, 1);
   float all[2400], blocks[2400];
   for (int i = 0; i < 1200; i++) {
      all[i * 2] = blocks[i * 2] = float(std::sin(double(i) * 0.13));
      all[i * 2 + 1] = blocks[i * 2 + 1] = 0;
   }
   whole.process(all, 1200);
   for (int i = 0; i < 1200; i += 37) split.process(blocks + i * 2, std::min(37, 1200 - i));
   for (int i = 0; i < 2400; i++) {
      assert(std::isfinite(all[i]) and std::abs(all[i] - blocks[i]) < 0.000001);
      if (i & 1) assert(all[i] IS 0);
   }
}

int main()
{
   test_transitions();
   extAudioEffect effect(nullptr, 1);
   effect.OutputRate = 48000;
   effect.Stereo = 0;

   EqualiserProcessor trim(&effect, {}, 6.020599913279624);
   trim.reset();
   float unity[] = { 0.5f };
   trim.process(unity, 1);
   assert(std::abs(unity[0] - 1.0f) < 0.00001f);

   AudioEQBand low_pass { EQB::LOW_PASS, 1000, 0, 0.707 };
   EqualiserProcessor low(&effect, { low_pass }, 0);
   low.reset();
   float impulse[128] = { 1 };
   low.process(impulse, 128);
   assert(impulse[0] > 0 and impulse[0] < 0.1f);
   assert(impulse[1] > impulse[0]);
   assert(std::abs(impulse[127]) < 0.001f);

   AudioEQBand high_pass { EQB::HIGH_PASS, 1000, 0, 0.707 };
   EqualiserProcessor high(&effect, { high_pass }, 0);
   high.reset();
   float steady[512];
   std::fill(std::begin(steady), std::end(steady), 1.0f);
   high.process(steady, 512);
   assert(std::abs(steady[511]) < 0.001f);

   effect.Stereo = 1;
   low.reset();
   float stereo[256] = { 1 };
   low.process(stereo, 128);
   for (int i = 1; i < 256; i += 2) assert(stereo[i] IS 0);
   const double coefficient_at_48000 = low.Sections[0].B0;
   effect.OutputRate = 24000;
   low.reset();
   assert(std::abs(low.Sections[0].B0 - coefficient_at_48000) > 0.01);

   effect.OutputRate = 48000;
   effect.Stereo = 0;
   AudioEQBand peak { EQB::PEAK, 1000, 6, 1 };
   EqualiserProcessor bell(&effect, { peak }, 0);
   bell.reset();
   float tone[480] = {};
   double input_energy = 0, output_energy = 0;
   for (int i = 0; i < 480; ++i) tone[i] = std::sin(2 * std::numbers::pi * 1000 * i / 48000);
   for (int i = 240; i < 480; ++i) input_energy += tone[i] * tone[i];
   bell.process(tone, 480);
   for (int i = 240; i < 480; ++i) output_energy += tone[i] * tone[i];
   assert(output_energy / input_energy > 3.8 and output_energy / input_energy < 4.1);

   // Magnitude response against analytic values.

   const double frequencies[] = { 20, 1000, 20000 };
   double magnitudes[3];

   bell.Rate = 48000;
   for (auto &section : bell.Sections) bell.compute(section);
   equaliser_magnitudes(bell, frequencies, magnitudes);
   assert(std::abs(magnitudes[1] - 6.0) < 0.001);   // A peak band's gain at its centre frequency
   assert(std::abs(magnitudes[0]) < 0.05);          // Negligible far below the centre

   effect.Stereo = 0;
   low.reset();
   equaliser_magnitudes(low, frequencies, magnitudes);
   assert(std::abs(magnitudes[1] - 20 * std::log10(0.707)) < 0.0001); // Pass filters are 20log10(Q) at the corner
   assert(std::abs(magnitudes[0]) < 0.01);
   assert(magnitudes[2] < -40);

   high.reset();
   equaliser_magnitudes(high, frequencies, magnitudes);
   assert(std::abs(magnitudes[1] - 20 * std::log10(0.707)) < 0.0001);
   assert(std::abs(magnitudes[2]) < 0.01);

   EqualiserProcessor trimmed(&effect, { peak }, 3);  // The trim offsets every point
   trimmed.reset();
   double trimmed_magnitudes[3];
   equaliser_magnitudes(trimmed, frequencies, trimmed_magnitudes);
   bell.reset();
   equaliser_magnitudes(bell, frequencies, magnitudes);
   for (int i = 0; i < 3; i++) assert(std::abs(trimmed_magnitudes[i] - magnitudes[i] - 3.0) < 0.000001);

   trim.reset();
   equaliser_magnitudes(trim, frequencies, magnitudes);
   for (auto magnitude : magnitudes) assert(std::abs(magnitude - 6.0206) < 0.001);
   return 0;
}

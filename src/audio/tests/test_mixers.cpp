//********************************************************************************************************************
// Unit Tests for Audio Mixers - Testing mix_template() function
//********************************************************************************************************************

#define _USE_MATH_DEFINES
#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include <iomanip>
#include <algorithm>
#include <cstdint>
#include <kotuku/main.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "mixers.cpp"

//********************************************************************************************************************
// Test utilities and data generation

class SineWaveGenerator {
public:
   static std::vector<uint8_t> generate8BitMono(int samples, double frequency, double amplitude = 0.8) {
      std::vector<uint8_t> data(samples);
      for (int i = 0; i < samples; i++) {
         double value = amplitude * sin(2.0 * M_PI * frequency * i / samples);
         data[i] = uint8_t(128 + value * 127);
      }
      return data;
   }

   static std::vector<int16_t> generate16BitMono(int samples, double frequency, double amplitude = 0.8) {
      std::vector<int16_t> data(samples);
      for (int i = 0; i < samples; i++) {
         double value = amplitude * sin(2.0 * M_PI * frequency * i / samples);
         data[i] = int16_t(value * 32767);
      }
      return data;
   }

   static std::vector<uint8_t> generate8BitStereo(int samples, double frequency, double amplitude = 0.8) {
      std::vector<uint8_t> data(samples * 2);
      for (int i = 0; i < samples; i++) {
         double leftValue = amplitude * sin(2.0 * M_PI * frequency * i / samples);
         double rightValue = amplitude * sin(2.0 * M_PI * frequency * i / samples + M_PI/4); // Phase shift
         data[i*2] = uint8_t(128 + leftValue * 127);
         data[i*2+1] = uint8_t(128 + rightValue * 127);
      }
      return data;
   }

   static std::vector<int16_t> generate16BitStereo(int samples, double frequency, double amplitude = 0.8) {
      std::vector<int16_t> data(samples * 2);
      for (int i = 0; i < samples; i++) {
         double leftValue = amplitude * sin(2.0 * M_PI * frequency * i / samples);
         double rightValue = amplitude * sin(2.0 * M_PI * frequency * i / samples + M_PI/4); // Phase shift
         data[i*2] = int16_t(leftValue * 32767);
         data[i*2+1] = int16_t(rightValue * 32767);
      }
      return data;
   }
};

class TestResults {
public:
   static bool approximately_equal(double a, double b, double tolerance = 1e-6) {
      return std::abs(a - b) < tolerance;
   }

   static bool approximately_equal(float a, float b, float tolerance = 1e-5f) {
      return std::abs(a - b) < tolerance;
   }

   static void print_buffer(const std::vector<float>& buffer, const std::string& name, int maxSamples = 10) {
      std::cout << name << " (first " << std::min(maxSamples, (int)buffer.size()) << " samples): ";
      for (int i = 0; i < std::min(maxSamples, (int)buffer.size()); i++) {
         std::cout << std::fixed << std::setprecision(6) << buffer[i] << " ";
      }
      std::cout << std::endl;
   }
};

//********************************************************************************************************************
// Test Cases

class MixerTests {
private:
   int testsPassed = 0;
   int testsTotal = 0;

   void test_result(const std::string& testName, bool passed) {
      testsTotal++;
      if (passed) {
         testsPassed++;
         std::cout << "✓ " << testName << " - PASSED" << std::endl;
      } else {
         std::cout << "✗ " << testName << " - FAILED" << std::endl;
      }
   }

public:
   void test_8bit_mono_to_mono_no_interpolation() {
      const int samples = 64;
      auto sineData = SineWaveGenerator::generate8BitMono(samples, 4.0); // 4 cycles in 64 samples

      std::vector<float> outputBuffer(samples, 0.0f);
      float* dest = outputBuffer.data();

      int srcPos = 0;
      mix_template<uint8_t, false, false, false>(
         sineData.data(), srcPos, samples, 1, 1.0f, 1.0f, &dest);

      // Verify some expected values
      bool passed = true;

      // Sample 0 should be around 0 (sine starts at 0)
      if (!TestResults::approximately_equal(outputBuffer[0], 0.0f, 500.0f)) {
         std::cout << "Sample 0 expected ~0, got " << outputBuffer[0] << std::endl;
         passed = false;
      }

      // Sample 16 should be around 0 (start of second cycle, 2π)
      if (!TestResults::approximately_equal(outputBuffer[16], 0.0f, 500.0f)) {
         std::cout << "Sample 16 expected ~0 (2π), got " << outputBuffer[16] << std::endl;
         passed = false;
      }

      // Sample 4 should be positive (quarter wave, π/2)
      if (outputBuffer[4] <= 0) {
         std::cout << "Sample 4 should be positive, got " << outputBuffer[4] << std::endl;
         passed = false;
      }

      // Sample 32 should be around 0 again (half wave)
      if (!TestResults::approximately_equal(outputBuffer[32], 0.0f, 500.0f)) {
         std::cout << "Sample 32 expected ~0, got " << outputBuffer[32] << std::endl;
         passed = false;
      }

      test_result("8-bit mono to mono (no interpolation)", passed);
   }

   void test_16bit_mono_to_stereo_no_interpolation() {
      const int samples = 32;
      auto sineData = SineWaveGenerator::generate16BitMono(samples, 2.0); // 2 cycles

      std::vector<float> outputBuffer(samples * 2, 0.0f);
      float* dest = outputBuffer.data();

      int srcPos = 0;
      mix_template<int16_t, false, true, false>(
         sineData.data(), srcPos, samples, 1, 0.5f, 0.8f, &dest);

      bool passed = true;

      // Check that left and right channels have correct volume scaling
      // Left channel should be 0.5x original, right channel 0.8x original
      for (int i = 0; i < 10; i++) {
         double expected = double(sineData[i]);
         float leftExpected = 0.5f * expected;
         float rightExpected = 0.8f * expected;

         if (!TestResults::approximately_equal(outputBuffer[i*2], leftExpected, 1.0f)) {
            std::cout << "Left channel sample " << i << " expected " << leftExpected
                     << ", got " << outputBuffer[i*2] << std::endl;
            passed = false;
            break;
         }

         if (!TestResults::approximately_equal(outputBuffer[i*2+1], rightExpected, 1.0f)) {
            std::cout << "Right channel sample " << i << " expected " << rightExpected
                     << ", got " << outputBuffer[i*2+1] << std::endl;
            passed = false;
            break;
         }
      }

      test_result("16-bit mono to stereo (no interpolation)", passed);
   }

   void test_8bit_stereo_to_stereo_no_interpolation() {
      const int samples = 32;
      auto sineData = SineWaveGenerator::generate8BitStereo(samples, 2.0);

      std::vector<float> outputBuffer(samples * 2, 0.0f);
      float* dest = outputBuffer.data();

      int srcPos = 0;
      mix_template<uint8_t, true, true, false>(sineData.data(), srcPos, samples, 1, 1.0f, 1.0f, &dest);

      bool passed = true;

      // Verify that stereo input produces stereo output
      // Check first few samples for correct channel separation
      for (int i = 0; i < 5; i++) {
         double expectedLeft = SampleTraits<uint8_t>::normalize(sineData[i*2]);
         double expectedRight = SampleTraits<uint8_t>::normalize(sineData[i*2+1]);

         if (!TestResults::approximately_equal(outputBuffer[i*2], float(expectedLeft), 1.0f)) {
            std::cout << "Stereo left sample " << i << " expected " << expectedLeft
                     << ", got " << outputBuffer[i*2] << std::endl;
            passed = false;
            break;
         }

         if (!TestResults::approximately_equal(outputBuffer[i*2+1], float(expectedRight), 1.0f)) {
            std::cout << "Stereo right sample " << i << " expected " << expectedRight
                     << ", got " << outputBuffer[i*2+1] << std::endl;
            passed = false;
            break;
         }
      }

      test_result("8-bit stereo to stereo (no interpolation)", passed);
   }

   void test_volume_scaling() {
      const int samples = 16;
      auto sineData = SineWaveGenerator::generate16BitMono(samples, 1.0);

      // Test with different volume levels
      std::vector<float> volumes = {0.0f, 0.25f, 0.5f, 1.0f, 2.0f};

      bool passed = true;

      for (float vol : volumes) {
         std::vector<float> outputBuffer(samples, 0.0f);
         float* dest = outputBuffer.data();

         int srcPos = 0;
         mix_template<int16_t, false, false, false>(
            sineData.data(), srcPos, samples, 1, vol, vol, &dest);

         // Check that output is scaled by volume
         for (int i = 0; i < 5; i++) {
            double expected = vol * double(sineData[i]);
            if (!TestResults::approximately_equal(outputBuffer[i], float(expected), 1.0f)) {
               std::cout << "Volume " << vol << " sample " << i << " expected " << expected
                        << ", got " << outputBuffer[i] << std::endl;
               passed = false;
               break;
            }
         }
         if (!passed) break;
      }

      test_result("Volume scaling accuracy", passed);
   }

   void test_interpolation_accuracy() {
      // Create a simple test pattern: [0, 32767, 0, -32767] (16-bit)
      std::vector<int16_t> testData = {0, 32767, 0, -32767, 0, 32767};

      std::vector<float> outputBuffer(4, 0.0f);
      float* dest = outputBuffer.data();

      // Test interpolation at half-sample positions
      MixStep = 32768; // 0.5 in 16.16 fixed point (half-step)
      int srcPos = 32768; // Start at 0.5 sample position

      mix_template<int16_t, false, false, true>(
         testData.data(), srcPos, 4, 1, 1.0f, 1.0f, &dest);

      bool passed = true;

      // At position 0.5, we should get interpolation between samples 0 and 1
      // Expected: (weight0=32768 * 0 + weight1=32768 * 32767) / 65536 = 16383.5
      double expected0 = 16383.5;
      if (!TestResults::approximately_equal(outputBuffer[0], float(expected0), 1.0f)) {
         std::cout << "Interpolation sample 0 expected ~" << expected0
                  << ", got " << outputBuffer[0] << std::endl;
         passed = false;
      }

      // At position 1.0, no interpolation needed - exactly on sample 1
      // Expected: sample[1] = 32767
      double expected1 = 32767.0;
      if (!TestResults::approximately_equal(outputBuffer[1], float(expected1), 1.0f)) {
         std::cout << "Interpolation sample 1 expected ~" << expected1
                  << ", got " << outputBuffer[1] << std::endl;
         passed = false;
      }

      // At position 1.5, interpolation between samples 1 and 2
      // Expected: (weight0=32768 * 32767 + weight1=32768 * 0) / 65536 = 16383.5
      double expected2 = 16383.5;
      if (!TestResults::approximately_equal(outputBuffer[2], float(expected2), 1.0f)) {
         std::cout << "Interpolation sample 2 expected ~" << expected2
                  << ", got " << outputBuffer[2] << std::endl;
         passed = false;
      }

      // At position 2.0, exactly on sample 2
      // Expected: sample[2] = 0
      double expected3 = 0.0;
      if (!TestResults::approximately_equal(outputBuffer[3], float(expected3), 1.0f)) {
         std::cout << "Interpolation sample 3 expected ~" << expected3
                  << ", got " << outputBuffer[3] << std::endl;
         passed = false;
      }

      test_result("Interpolation accuracy", passed);

      // Reset MixStep for other tests
      MixStep = 65536;
   }

   void test_additive_mixing() {
      const int samples = 16;
      auto sineData = SineWaveGenerator::generate16BitMono(samples, 2.0);

      std::vector<float> outputBuffer(samples, 1000.0f); // Pre-fill with non-zero values
      float* dest = outputBuffer.data();

      int srcPos = 0;
      mix_template<int16_t, false, false, false>(
         sineData.data(), srcPos, samples, 1, 1.0f, 1.0f, &dest);

      bool passed = true;

      // Verify that values were added to existing buffer content
      for (int i = 0; i < 5; i++) {
         double expected = 1000.0f + double(sineData[i]);
         if (!TestResults::approximately_equal(outputBuffer[i], float(expected), 1.0f)) {
            std::cout << "Additive sample " << i << " expected " << expected
                     << ", got " << outputBuffer[i] << std::endl;
            passed = false;
            break;
         }
      }

      test_result("Additive mixing behavior", passed);
   }

   void test_sample_position_advancement() {
      const int samples = 8;
      auto sineData = SineWaveGenerator::generate16BitMono(samples, 1.0);

      std::vector<float> outputBuffer(4, 0.0f);
      float* dest = outputBuffer.data();

      int srcPos = 0;
      int finalPos = mix_template<int16_t, false, false, false>(
         sineData.data(), srcPos, 4, 1, 1.0f, 1.0f, &dest);

      // Should advance by 4 samples * MixStep (65536)
      int expectedPos = 4 * 65536;
      bool passed = (finalPos == expectedPos);

      if (!passed) {
         std::cout << "Expected final position " << expectedPos
                  << ", got " << finalPos << std::endl;
      }

      test_result("Sample position advancement", passed);
   }

   void run_all_tests() {
      std::cout << "Running Audio Mixer Unit Tests..." << std::endl;
      std::cout << "=================================" << std::endl;

      set_mix_step(65536); // 1.0 in 16.16 fixed point (no resampling)

      test_8bit_mono_to_mono_no_interpolation();
      test_16bit_mono_to_stereo_no_interpolation();
      test_8bit_stereo_to_stereo_no_interpolation();
      test_volume_scaling();
      test_interpolation_accuracy();
      test_additive_mixing();
      test_sample_position_advancement();

      std::cout << std::endl;
      std::cout << "Test Results: " << testsPassed << "/" << testsTotal << " passed";
      if (testsPassed == testsTotal) {
         std::cout << " ✓ ALL TESTS PASSED!" << std::endl;
      } else {
         std::cout << " ✗ " << (testsTotal - testsPassed) << " tests failed." << std::endl;
      }
   }
};

//********************************************************************************************************************
// Main test runner

int main() {
   MixerTests tests;
   tests.run_all_tests();
   return 0;
}
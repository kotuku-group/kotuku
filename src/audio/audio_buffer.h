#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

constexpr int audio_period_count(int Value) { return std::clamp(Value, 2, 16); }
constexpr int audio_period_frames(int Value) { return std::clamp(Value, 32, 16384); }
constexpr unsigned audio_frame_bytes(int Bits, bool Stereo)
{
   return (Bits IS 8 or Bits IS 16 or Bits IS 32) ? (Bits / 8) * (Stereo ? 2 : 1) : 0;
}

constexpr double audio_latency(uint64_t Frames, unsigned Rate)
{
   return Rate ? double(Frames) / Rate : 0;
}

constexpr bool audio_buffer_valid(uint64_t Period, uint64_t Buffer, unsigned FrameBytes)
{
   return Period > 0 and Period <= 65536 and Buffer >= Period * 2 and
      Buffer <= 1048576 and FrameBytes > 0 and FrameBytes <= 8;
}

constexpr int legacy_period_frames(int Bytes, int Bits, bool Stereo)
{
   const int frame_bytes = std::max(1, Bits / 8) * (Stereo ? 2 : 1);
   return std::clamp(Bytes / frame_bytes, 32, 16384);
}

// A produced period is immutable until every frame has been accepted.  Recovery leaves the suffix intact.
struct RetainedAudioPeriod {
   size_t Remaining = 0;
   size_t Offset = 0;

   void produce(size_t Frames) { Remaining = Frames; Offset = 0; }
   bool accept(size_t Frames) {
      if (Frames > Remaining) return false;
      Remaining -= Frames;
      Offset += Frames;
      return true;
   }
};

struct AudioRingCursor {
   size_t Read = 0;
   size_t Used = 0;

   size_t write_position(size_t Capacity) const { return (Read + Used) % Capacity; }
   bool publish(size_t Bytes, size_t Capacity, size_t FrameBytes) {
      if (!FrameBytes or Bytes % FrameBytes or Used > Capacity or Bytes > Capacity - Used) return false;
      Used += Bytes;
      return true;
   }
   bool consume(size_t Bytes, size_t Capacity, size_t FrameBytes) {
      if (!Capacity or !FrameBytes or Bytes % FrameBytes or Bytes > Used) return false;
      Read = (Read + Bytes) % Capacity;
      Used -= Bytes;
      return true;
   }
};

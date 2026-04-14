#pragma once

#ifdef ALSA_ENABLED
#include <alsa/asoundlib.h>
#include <vector>
#include <string>
#include <functional>
#include <kotuku/main.h>
#include <kotuku/strings.hpp>

// Device information structure
struct ALSADeviceInfo {
   int card_number;
   std::string card_id;
   std::string card_name;
   std::string device_name;  // "hw:X" format
   int16_t mixer_controls;
   bool is_modem;

   ALSADeviceInfo() : card_number(-1), mixer_controls(0), is_modem(false) {}
};

// Device enumeration callback types
using DeviceFilter = std::function<bool(const ALSADeviceInfo&)>;
using DeviceSelector = std::function<bool(const ALSADeviceInfo&, const ALSADeviceInfo&)>; // Returns true if first is better

// Unified device enumeration interface
class ALSADeviceEnumerator {
public:
   // Enumerate all available audio devices
   static std::vector<ALSADeviceInfo> enumerate_devices();

   // Find device by card ID (e.g., "default", "pulse", specific card name)
   static ALSADeviceInfo find_device_by_id(const std::string& device_id);

   // Select best device using custom criteria
   static ALSADeviceInfo select_best_device(DeviceFilter filter = nullptr,
                                          DeviceSelector selector = nullptr);

   // Check if any genuine (non-modem) audio devices are available
   static bool has_genuine_devices();

   // Wait for audio devices to become available (with timeout)
   static ERR wait_for_devices(int timeout_ms);

private:
   // Core enumeration function - populates device info for a single card
   static bool populate_device_info(int card_number, ALSADeviceInfo& info);

   // Default device selector - chooses device with most mixer controls
   static bool default_selector(const ALSADeviceInfo& candidate, const ALSADeviceInfo& current_best);

   // Default filter - excludes modems
   static bool default_filter(const ALSADeviceInfo& device);
};

#endif // ALSA_ENABLED
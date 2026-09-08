#ifdef ALSA_ENABLED

#include "device_enum.h"

// Core enumeration function - populates device info for a single card
bool ALSADeviceEnumerator::populate_device_info(int card_number, ALSADeviceInfo& info)
{
   std::string device_name = "hw:" + std::to_string(card_number);
   snd_ctl_t *ctlhandle = nullptr;
   snd_ctl_card_info_t *card_info = nullptr;
   bool success = false;

   snd_ctl_card_info_alloca(&card_info);

   if (snd_ctl_open(&ctlhandle, device_name.c_str(), 0) >= 0) {
      if (snd_ctl_card_info(ctlhandle, card_info) >= 0) {
         info.card_number = card_number;
         info.card_id = snd_ctl_card_info_get_id(card_info);
         info.card_name = snd_ctl_card_info_get_name(card_info);
         info.device_name = device_name;
         info.is_modem = kt::iequals("modem", info.card_id);

         // Get mixer control count
         info.mixer_controls = 0;
         snd_mixer_t *mixhandle = nullptr;
         if (snd_mixer_open(&mixhandle, 0) >= 0) {
            if (snd_mixer_attach(mixhandle, device_name.c_str()) >= 0) {
               if (snd_mixer_selem_register(mixhandle, nullptr, nullptr) >= 0) {
                  if (snd_mixer_load(mixhandle) >= 0) {
                     snd_mixer_elem_t *elem;
                     for (elem = snd_mixer_first_elem(mixhandle); elem; elem = snd_mixer_elem_next(elem)) {
                        info.mixer_controls++;
                     }
                  }
               }
            }
            snd_mixer_close(mixhandle);
         }

         success = true;
      }
      snd_ctl_close(ctlhandle);
   }

   return success;
}

// Enumerate all available audio devices
std::vector<ALSADeviceInfo> ALSADeviceEnumerator::enumerate_devices()
{
   std::vector<ALSADeviceInfo> devices;
   int card = -1;

   // Iterate through all available cards
   if (snd_card_next(&card) >= 0 and card >= 0) {
      while (card >= 0) {
         ALSADeviceInfo info;
         if (populate_device_info(card, info)) {
            devices.push_back(info);
         }

         if (snd_card_next(&card) < 0) card = -1;
      }
   }

   return devices;
}

// Find device by card ID
ALSADeviceInfo ALSADeviceEnumerator::find_device_by_id(const std::string& device_id)
{
   ALSADeviceInfo result;

   // Handle special case for "default"
   if (kt::iequals("default", device_id)) {
      return select_best_device();
   }

   // Search for device by card ID
   auto devices = enumerate_devices();
   for (const auto& device : devices) {
      if (kt::iequals(device.card_id, device_id)) {
         return device;
      }
   }

   return result; // Empty result if not found
}

// Default filter - excludes modems
bool ALSADeviceEnumerator::default_filter(const ALSADeviceInfo& device)
{
   return !device.is_modem;
}

// Default device selector - chooses device with most mixer controls
bool ALSADeviceEnumerator::default_selector(const ALSADeviceInfo& candidate, const ALSADeviceInfo& current_best)
{
   return candidate.mixer_controls > current_best.mixer_controls;
}

// Select best device using custom criteria
ALSADeviceInfo ALSADeviceEnumerator::select_best_device(DeviceFilter filter, DeviceSelector selector)
{
   auto devices = enumerate_devices();
   ALSADeviceInfo best_device;

   // Use default filter and selector if none provided
   if (!filter) filter = default_filter;
   if (!selector) selector = default_selector;

   for (const auto& device : devices) {
      if (!filter(device)) continue;

      if (best_device.card_number IS -1 or selector(device, best_device)) {
         best_device = device;
      }
   }

   return best_device;
}

// Check if any genuine (non-modem) audio devices are available
bool ALSADeviceEnumerator::has_genuine_devices()
{
   auto devices = enumerate_devices();

   for (const auto& device : devices) {
      if (!device.is_modem) {
         return true;
      }
   }

   return false;
}

// Wait for audio devices to become available (with timeout)
ERR ALSADeviceEnumerator::wait_for_devices(int timeout_ms)
{
   kt::Log log(__FUNCTION__);
   log.branch("Waiting for audio drivers to start...");

   int64_t start_time = PreciseTime();
   int64_t timeout_us = int64_t(timeout_ms) * 1000LL;

   while (PreciseTime() - start_time < timeout_us) {
      if (has_genuine_devices()) {
         log.msg("Genuine audio devices detected.");
         return ERR::Okay;
      }

      WaitTime(-0.1); // Wait 0.1 seconds without processing messages
   }

   log.msg("No sound drivers were started in the allotted time period.");
   return ERR::Timeout;
}

#endif // ALSA_ENABLED
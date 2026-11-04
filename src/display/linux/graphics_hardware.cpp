// See graphics_hardware.h.

#include "graphics_hardware.h"

#include <kotuku/config.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace graphics_hardware {

//********************************************************************************************************************
// Parses a four digit hexadecimal ID that must be followed by whitespace, as used for the IDs in pci.ids.  Returns -1
// if the text does not match.

static int parse_id(std::string_view Text)
{
   if ((Text.size() < 5) or ((Text[4] != ' ') and (Text[4] != '\t'))) return -1;
   int value = 0;
   for (int i=0; i < 4; i++) {
      auto ch = Text[i];
      if ((ch >= '0') and (ch <= '9')) value = (value << 4) | (ch - '0');
      else if ((ch >= 'a') and (ch <= 'f')) value = (value << 4) | (ch - 'a' + 10);
      else if ((ch >= 'A') and (ch <= 'F')) value = (value << 4) | (ch - 'A' + 10);
      else return -1;
   }
   return value;
}

// Returns the name that follows an ID, without surrounding whitespace.

static std::string id_name(std::string_view Text)
{
   Text.remove_prefix(4);
   while ((not Text.empty()) and ((Text.front() IS ' ') or (Text.front() IS '\t'))) Text.remove_prefix(1);
   while ((not Text.empty()) and ((Text.back() IS ' ') or (Text.back() IS '\r'))) Text.remove_suffix(1);
   return std::string(Text);
}

//********************************************************************************************************************
// Vendor lines start in the first column and are followed by their device lines, which are indented by a single tab.
// Subsystem lines are indented by two tabs, and comment lines start with '#'.

Names lookup_pci_ids(std::string_view Database, uint16_t Vendor, uint16_t Device)
{
   Names result;
   bool in_vendor = false;

   for (size_t pos=0; pos < Database.size();) {
      auto end = Database.find('\n', pos);
      if (end IS std::string_view::npos) end = Database.size();
      auto line = Database.substr(pos, end - pos);
      pos = end + 1;

      if (line.empty() or (line[0] IS '#')) continue;

      if (line[0] != '\t') {
         if (in_vendor) break; // The next vendor has been reached
         if (parse_id(line) IS Vendor) {
            in_vendor = true;
            result.Vendor = id_name(line);
         }
      }
      else if (in_vendor and (line.size() > 1) and (line[1] != '\t')) {
         line.remove_prefix(1);
         if (parse_id(line) IS Device) {
            result.Device = id_name(line);
            break;
         }
      }
   }

   return result;
}

//********************************************************************************************************************

static std::string read_text(const std::filesystem::path &Path)
{
   std::ifstream file(Path, std::ios::binary);
   if (not file) return {};
   std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
   while ((not text.empty()) and ((text.back() IS '\n') or (text.back() IS ' '))) text.pop_back();
   return text;
}

static int read_hex_file(const std::filesystem::path &Path)
{
   auto text = read_text(Path);
   if (text.empty()) return -1;
   char *end = nullptr;
   auto value = std::strtol(text.c_str(), &end, 16);
   return ((end IS text.c_str()) or (value < 0) or (value > 0xffff)) ? -1 : int(value);
}

//********************************************************************************************************************

static Names identify_primary_device()
{
   // DRM cards are named card0, card1 and so on.  Connector entries such as card0-DP-1 are excluded.

   std::vector<std::filesystem::path> cards;
   std::error_code error;
   for (auto it = std::filesystem::directory_iterator("/sys/class/drm", error);
         (not error) and (it != std::filesystem::directory_iterator()); it.increment(error)) {
      auto name = it->path().filename().string();
      if ((name.size() > 4) and name.starts_with("card") and
          std::all_of(name.begin() + 4, name.end(), [](char Ch) { return (Ch >= '0') and (Ch <= '9'); })) {
         cards.push_back(it->path());
      }
   }
   std::sort(cards.begin(), cards.end());

   int vendor = -1, device = -1;
   for (auto &card : cards) {
      auto card_vendor = read_hex_file(card / "device/vendor");
      auto card_device = read_hex_file(card / "device/device");
      if ((card_vendor < 0) or (card_device < 0)) continue; // Not a PCI device
      if (vendor < 0) { vendor = card_vendor; device = card_device; }
      if (read_text(card / "device/boot_vga") IS "1") { vendor = card_vendor; device = card_device; break; }
   }

   if (vendor < 0) return {};

   Names names;
   for (auto path : { "/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids", "/usr/share/pci.ids" }) {
      if (auto database = read_text(path); not database.empty()) {
         names = lookup_pci_ids(database, uint16_t(vendor), uint16_t(device));
         break;
      }
   }

   char buffer[32];
   if (names.Vendor.empty()) {
      std::snprintf(buffer, sizeof(buffer), "PCI vendor %04x", vendor);
      names.Vendor = buffer;
   }

   if (names.Device.empty()) {
      std::snprintf(buffer, sizeof(buffer), "PCI device %04x:%04x", vendor, device);
      names.Device = buffer;
   }

   return names;
}

const Names & primary_device()
{
   static const Names names = identify_primary_device();
   return names;
}

} // namespace graphics_hardware

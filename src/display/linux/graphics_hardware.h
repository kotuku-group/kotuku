#pragma once

// Identifies the graphics hardware on Linux from the kernel's DRM device nodes and the PCI ID database.  These
// functions are independent of the Kotuku runtime so that they can be unit tested in isolation.

#include <cstdint>
#include <string>
#include <string_view>

namespace graphics_hardware {

struct Names {
   std::string Vendor; // e.g. "NVIDIA Corporation"
   std::string Device; // e.g. "TU106 [GeForce RTX 2060 Rev. A]"
};

// Looks up a PCI vendor and device in the text of a pci.ids database.  A name is left empty if it is not listed.

Names lookup_pci_ids(std::string_view Database, uint16_t Vendor, uint16_t Device);

// Returns the names of the primary graphics device, being the device that the firmware used for boot output, or else
// the first DRM card.  Hexadecimal IDs are substituted for names that are not in the PCI ID database.  Both names are
// empty if no PCI graphics device could be found.  The result is computed on the first call and then cached.

const Names & primary_device();

} // namespace graphics_hardware

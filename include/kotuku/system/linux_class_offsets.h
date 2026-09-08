#pragma once

//  linux_class_offsets.h
//
//  Platform-specific origins for generated direct class-field access.  These values describe the 64-bit Linux
//  GCC/libstdc++ ABI and are maintained in module sections by the IDL header generator.

#include <cstdint>

inline constexpr int32_t CLASS_OFFSET = 96;

// Display module start
// Display module end

// Image module start
// Image module end

// Network module start
inline constexpr int32_t CLASS_OFFSET_NETSOCKET = 344;
// Network module end

// Processes module start
// Processes module end

// Script module start
// Script module end

// Vector module start
inline constexpr int32_t CLASS_OFFSET_FILTEREFFECT = 224;
inline constexpr int32_t CLASS_OFFSET_GRADIENT = 288;
inline constexpr int32_t CLASS_OFFSET_VECTOR = 984;
// Vector module end

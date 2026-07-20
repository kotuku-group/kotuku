#pragma once

//  windows_class_offsets.h
//
//  Platform-specific origins for generated direct class-field access.  These values describe the 64-bit Windows
//  MSVC/MSVC STL ABI and are maintained in module sections by the IDL header generator.

#include <cstdint>

// Display module start
inline constexpr int32_t CLASS_OFFSET_BITMAP = 96;
inline constexpr int32_t CLASS_OFFSET_DISPLAY = 96;
// Display module end

// Image module start
inline constexpr int32_t CLASS_OFFSET_IMAGE = 96;
// Image module end

// Network module start
inline constexpr int32_t CLASS_OFFSET_NETSERVER = 336;
// Network module end

// Processes module start
inline constexpr int32_t CLASS_OFFSET_TASK = 96;
inline constexpr int32_t CLASS_OFFSET_THREAD = 96;
// Processes module end

// Script module start
inline constexpr int32_t CLASS_OFFSET_SCRIPT = 96;
// Script module end

// Vector module start
inline constexpr int32_t CLASS_OFFSET_BLURFX = 224;
inline constexpr int32_t CLASS_OFFSET_COLOURFX = 224;
inline constexpr int32_t CLASS_OFFSET_COMPOSITEFX = 224;
inline constexpr int32_t CLASS_OFFSET_CONVOLVEFX = 224;
inline constexpr int32_t CLASS_OFFSET_DISPLACEMENTFX = 224;
inline constexpr int32_t CLASS_OFFSET_FILTEREFFECT = 96;
inline constexpr int32_t CLASS_OFFSET_FLOODFX = 224;
inline constexpr int32_t CLASS_OFFSET_GRADIENT = 96;
inline constexpr int32_t CLASS_OFFSET_GRADIENTCONIC = 288;
inline constexpr int32_t CLASS_OFFSET_GRADIENTCONTOUR = 288;
inline constexpr int32_t CLASS_OFFSET_GRADIENTDIAMOND = 288;
inline constexpr int32_t CLASS_OFFSET_GRADIENTDISTAL = 288;
inline constexpr int32_t CLASS_OFFSET_GRADIENTLINEAR = 288;
inline constexpr int32_t CLASS_OFFSET_GRADIENTMESH = 288;
inline constexpr int32_t CLASS_OFFSET_GRADIENTRADIAL = 288;
inline constexpr int32_t CLASS_OFFSET_GRADIENTVORONOI = 288;
inline constexpr int32_t CLASS_OFFSET_IMAGEFX = 224;
inline constexpr int32_t CLASS_OFFSET_LIGHTINGFX = 224;
inline constexpr int32_t CLASS_OFFSET_MORPHOLOGYFX = 224;
inline constexpr int32_t CLASS_OFFSET_OFFSETFX = 224;
inline constexpr int32_t CLASS_OFFSET_SOURCEFX = 224;
inline constexpr int32_t CLASS_OFFSET_TURBULENCEFX = 224;
inline constexpr int32_t CLASS_OFFSET_VECTOR = 96;
inline constexpr int32_t CLASS_OFFSET_VECTORELLIPSE = 976;
inline constexpr int32_t CLASS_OFFSET_VECTORFILTER = 96;
inline constexpr int32_t CLASS_OFFSET_VECTORIMAGE = 96;
inline constexpr int32_t CLASS_OFFSET_VECTORPATTERN = 96;
inline constexpr int32_t CLASS_OFFSET_VECTORSHAPE = 976;
inline constexpr int32_t CLASS_OFFSET_VECTORSPIRAL = 976;
inline constexpr int32_t CLASS_OFFSET_VECTORVIEWPORT = 976;
inline constexpr int32_t CLASS_OFFSET_VECTORWAVE = 976;
inline constexpr int32_t CLASS_OFFSET_WAVEFUNCTIONFX = 224;
// Vector module end

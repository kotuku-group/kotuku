#pragma once

// Name:      xrandr.h
// Copyright: Paul Manias © 2014-2017
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_XRANDR (1)

#ifdef KOTUKU_STATIC
#define JUMPTABLE_XRANDR static struct XRandRBase *XRandRBase;
#else
#define JUMPTABLE_XRANDR struct XRandRBase *XRandRBase;
#endif

struct XRandRBase {
#ifndef KOTUKU_STATIC
   ERR (*_SetDisplayMode)(LONG * Width, LONG * Height);
   LONG (*_Notify)(APTR XEvent);
   void (*_SelectInput)(LONG Window);
   LONG (*_GetDisplayTotal)(void);
   APTR (*_GetDisplayMode)(LONG Index);
#endif // KOTUKU_STATIC
};

#ifndef PRV_XRANDR_MODULE
#ifndef KOTUKU_STATIC
extern struct XRandRBase *XRandRBase;
inline ERR xrSetDisplayMode(LONG * Width, LONG * Height) { return XRandRBase->_SetDisplayMode(Width,Height); }
inline LONG xrNotify(APTR XEvent) { return XRandRBase->_Notify(XEvent); }
inline void xrSelectInput(LONG Window) { return XRandRBase->_SelectInput(Window); }
inline LONG xrGetDisplayTotal(void) { return XRandRBase->_GetDisplayTotal(); }
inline APTR xrGetDisplayMode(LONG Index) { return XRandRBase->_GetDisplayMode(Index); }
#else
extern "C" {
extern ERR xrSetDisplayMode(LONG * Width, LONG * Height);
extern LONG xrNotify(APTR XEvent);
extern void xrSelectInput(LONG Window);
extern LONG xrGetDisplayTotal(void);
extern APTR xrGetDisplayMode(LONG Index);
}
#endif // KOTUKU_STATIC
#endif

struct xrMode {
   LONG Width;    // Horizontal
   LONG Height;   // Vertical
   LONG Depth;    // bit depth
};


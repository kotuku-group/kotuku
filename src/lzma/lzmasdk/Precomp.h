/* Precomp.h -- StdAfx
2013-11-12 : Igor Pavlov : Public domain

Minimal local replacement for the LZMA SDK's Precomp.h.  The upstream file pulls in Compiler.h purely to apply MSVC
warning pragmas; that dependency is omitted here so the vendored decode set remains limited to LzmaDec.c, LzmaDec.h,
7zTypes.h and this stub. */

#ifndef __7Z_PRECOMP_H
#define __7Z_PRECOMP_H

#endif

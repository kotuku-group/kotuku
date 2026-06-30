# Vendored LZMA SDK reference decoder

This directory contains the minimal decode-only subset of Igor Pavlov's **LZMA SDK**, vendored directly into the
Kōtuku `lzma` module and compiled alongside `lzma.cpp` (the same approach `src/mp3` uses for `minimp3`).

## Provenance

- **Upstream:** LZMA SDK by Igor Pavlov (https://www.7-zip.org/sdk.html).
- **Mirror used:** https://github.com/jljusten/LZMA-SDK (`C/` directory).
- **Files imported:** `LzmaDec.c` (2018-02-28), `LzmaDec.h` (2018-04-21), `7zTypes.h` (2017-07-17).
- **Licence:** Public domain (as stated in each file header).

## Local modifications

- `Precomp.h` is a minimal local stub.  The upstream `Precomp.h` only `#include`s `Compiler.h` to apply MSVC warning
  pragmas; that dependency is dropped so the vendored set stays limited to the four files in this directory.
- No edits were made to `LzmaDec.c`, `LzmaDec.h` or `7zTypes.h`.

## Scope

Only the raw LZMA1 decoder is vendored — enough to decode a 5-byte properties header followed by a raw compressed
stream with a caller-known output size (the SWF `ZWS` case).  Encoding, the `.lzma`/`.alone` and `.xz` containers,
and the multi-threaded SDK components are intentionally excluded.

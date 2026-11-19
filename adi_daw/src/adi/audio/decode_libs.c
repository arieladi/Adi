/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The decoders' implementations, compiled once, as C, outside the warning wall
 * (ADR-0156). Each library keeps its own licence: dr_libs are public domain
 * (Unlicense) or MIT-0, stb_vorbis is MIT or public domain; see
 * docs/EXTERNAL-CODE.md. dr_libs use the audited C++ callbacks in decode.cpp.
 * Vorbis uses an already-open native FILE*, opened wide on Windows, so the
 * compressed file streams instead of being retained in memory.
 */
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "dr_flac.h"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "dr_wav.h"

#if defined(_WIN32)
/* Select stb's secure CRT branch for its unused filename convenience API. */
#define __STDC_WANT_SECURE_LIB__ 1
#endif
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

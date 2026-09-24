/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The decoders' implementations, compiled once, as C, outside the warning wall
 * (ADR-0156). Each library keeps its own licence: dr_libs are public domain
 * (Unlicense) or MIT-0, stb_vorbis is MIT or public domain; see
 * docs/EXTERNAL-CODE.md. No stdio: every byte comes through the callbacks in
 * decode.cpp, which read with C++ file streams (wide paths on Windows).
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

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

# SPDX-License-Identifier: GPL-3.0-or-later
#
# Turn docs/format/schema.sql into a C++ header holding it as bytes.
#
# Why generate rather than paste: schema.sql is the normative DDL (SPEC §13) and
# tools/validate_schema.py executes that exact file. If the C++ carried its own
# copy the two would drift — silently, and in the direction where the validator
# keeps passing while the shipped binary creates a different database. There is
# one source of truth and it is the .sql file.
#
# Why a BYTE ARRAY and not a raw string literal: MSVC rejects any single string
# literal over 16380 characters (C2026), and the schema is ~31 KB. Adjacent
# literal concatenation does not help — the limit applies after concatenation.
# A char array has no such limit on any compiler, and sidesteps the other trap
# too: a raw string would break if the SQL ever contained the delimiter.
#
# Run as a script at build time:
#   cmake -DIN=<schema.sql> -DOUT=<schema_sql.hpp> -P embed_schema.cmake

if(NOT DEFINED IN OR NOT DEFINED OUT)
    message(FATAL_ERROR "embed_schema.cmake needs -DIN= and -DOUT=")
endif()

file(READ "${IN}" _hex HEX)
string(LENGTH "${_hex}" _hexlen)
math(EXPR _bytes "${_hexlen} / 2")

if(_bytes LESS 1000)
    message(FATAL_ERROR
        "${IN} is only ${_bytes} bytes — that is not the schema. Refusing to "
        "generate a header that would create an empty database.")
endif()

# "2d2d20" -> "0x2d,0x2d,0x20," with a newline every 16 bytes so the generated
# file stays diffable and does not become one enormous line.
string(REGEX REPLACE "(..)" "0x\\1," _body "${_hex}")
string(REGEX REPLACE "((0x..,){16})" "\\1\n    " _body "${_body}")

get_filename_component(_name "${IN}" NAME)

file(WRITE "${OUT}"
"// GENERATED FILE -- DO NOT EDIT.
// Produced from ${_name} (${_bytes} bytes) by cmake/embed_schema.cmake.
// Edit the .sql file; this header regenerates on build.
#pragma once

#include <string_view>

namespace adi {
namespace detail {

// unsigned char, not char: any byte >= 0x80 does not fit a signed char and
// MSVC rejects the initialiser (C4309). The schema is ASCII today, but a
// comment with a non-ASCII character would otherwise break the build for a
// reason nobody would guess from the error.
inline constexpr unsigned char kSchemaSqlBytes[] = {
    ${_body}
};

}  // namespace detail

/// The normative DDL, byte-for-byte as docs/format/schema.sql holds it.
/// Not constexpr: reinterpret_cast is not allowed in a constant expression.
inline const std::string_view kSchemaSql{
    reinterpret_cast<const char*>(detail::kSchemaSqlBytes),
    sizeof(detail::kSchemaSqlBytes)};

}  // namespace adi
")

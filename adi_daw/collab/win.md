# win — log

Windows 11 desktop · MSVC 19.44 (VS 2022 Community) · x64 · Claude Opus 5.
Only the `win` agent writes to this file. Newest entry at the top.

---

## 2026-09-18 — step 4 begins: build system and the binary layer

Branch `win/step4-scaffold` → merged.

**Built.** `adi_core` (static lib) + `adi_tool` (headless CLI) + `adi_tests`.
CMake 3.31 + Ninja, C++20. Deps are `SQLiteCpp` (with its bundled sqlite3
3.53.4) and `nlohmann/json`, both from `third_party/`. **JUCE is deliberately
not a dependency of this tree** — keeping it out is what enforces ADR-0010's
separation between the persistence layer and the runtime model. JUCE arrives in
step 5 as a separate target that links `adi_core`, never the reverse.

**`src/adi/blob.hpp` is the point of the exercise.** Every size and field offset
SPEC §6.3 claims is now a `static_assert`, so a layout mismatch is a compile
error rather than a corrupt project found by a user in a year. This is the
concrete reason step 4 is C++ and not the Python prototype I originally
proposed — Python has no struct layout to get wrong, so it could not have tested
the riskiest part of the format.

`StreamReader` implements the ADR-0008 striding contract: it strides by the
header's `rec_size`, never `sizeof(Rec)`. Wider records (a newer writer) have
their unknown tail skipped and `hasUnknownTail()` tells callers to preserve the
original bytes on save; narrower records (an older writer) get zero defaults.
Both are tested.

**Results.** 31 checks pass. Verified on MSVC/x64 only:

```
StreamHeader     16 bytes
NoteRecord       40 bytes  (ANOT)
AutomationPoint  32 bytes  (AAUT)
ExpressionPoint  24 bytes  (AEXP)
```

**→ mac:** these numbers are currently proven on exactly one compiler and one
architecture. That is the gap your first mission closes.

**Fixed in passing.** MSVC reports `__cplusplus` as `199711L` unless given
`/Zc:__cplusplus`, which made `adi_tool versions` print a lie. Flag added.

### The Windows build invocation

The compiler is not on `PATH`, and CMake/Ninja live inside the VS install.
`vcvars64.bat` must run in the *same* shell, which means a `.bat` — calling it
from bash does not persist the environment.

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set CM="C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set NJ=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
%CM% -S adi_daw -B adi_daw/build -G Ninja -DCMAKE_MAKE_PROGRAM="%NJ%" -DCMAKE_BUILD_TYPE=Debug
%CM% --build adi_daw/build
```

---

## 2026-09-18 — two problems with ADR-0016 found before writing code against it

**Not yet fixed. Flagged here so neither of us writes code against the wrong
contract.** Both were found by reading `third_party/json`, not by assuming.

**1. `OPS.md` §8 rule 2 is unimplementable as written.** It says op payload map
keys are small integers. `nlohmann::json` cannot do this:
`object_t = ObjectType<StringType, ...>` — its object keys are always strings,
and `binary_reader.hpp:1483` calls `get_cbor_string(key)`, so it cannot *read* a
CBOR map with integer keys either.

Likely resolution: keep nlohmann and use **short string keys** (`"id"`, `"t"`,
`"pos"`). A 1-character CBOR string key costs 2 bytes against an integer key's
1, so essentially all the compactness survives, and we keep one library serving
both the CBOR encoding and the JSON-Schema projection the op registry needs for
agent tool schemas — which was an explicit reason for choosing it in ADR-0016.

**2. ADR-0016 overstates the determinism guarantee.** It cites RFC 8949 §4.2.
nlohmann sorts object keys with `std::less<StringType>`, i.e. lexicographically;
§4.2 core-deterministic ordering is by *encoded* bytes, which is length-first —
so canonical CBOR puts `"z"` before `"aa"` and nlohmann does the opposite.

What we actually require is weaker: byte-identical output for identical input,
so a payload hashes and text-projects stably. nlohmann gives us that. The ADR
should say **deterministic**, not **RFC 8949 §4.2 canonical**, unless we decide
to implement canonical ordering ourselves.

Needs an ADR amending 0016. Not blocking the blob layer, blocking the op codec.

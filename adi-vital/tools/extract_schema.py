#!/usr/bin/env python3
"""
Extract Vital's full parameter schema straight from the C++ source of truth.

Parses src/common/synth_parameters.cpp (the ValueDetails tables) and
src/interface/look_and_feel/synth_strings.h (the enum label tables), replays
the group expansion that ValueDetailsLookup's constructor performs at runtime,
and emits JSON.

Two outputs:
  vital_schema_full.json  -- every parameter, including all 64 mod slots
  vital_schema_llm.json   -- sound-design params only, enums resolved to labels

The second one is the contract for the LoRA fine-tune and the server-side
validator. Regenerate both any time the fork's parameter tables change.
"""

import json
import math
import re
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Mirrors of the C++ constants. If you change these in the fork, change them
# here too -- synth_constants.h is the source of truth.
# ---------------------------------------------------------------------------
NUM_ENVELOPES = 6
NUM_LFOS = 8
NUM_RANDOM_LFOS = 4
NUM_OSCILLATORS = 3
NUM_FILTERS = 2
MAX_MODULATION_CONNECTIONS = 64

ID_DELIMITER = "_"
NAME_DELIMITER = " "

# (array name, id-prefix constant, name-prefix constant, instance count or ids).
# The prefix VALUES are read out of synth_parameters.cpp rather than hardcoded:
# kRandomNamePrefix is "Random LFO", not "Random", and hardcoding it produced
# display_names that silently failed to join against the host's parameter list.
GROUPS = [
    ("env_parameter_list", "kEnvIdPrefix", "kEnvNamePrefix", NUM_ENVELOPES),
    ("lfo_parameter_list", "kLfoIdPrefix", "kLfoNamePrefix", NUM_LFOS),
    ("random_lfo_parameter_list", "kRandomIdPrefix", "kRandomNamePrefix", NUM_RANDOM_LFOS),
    ("osc_parameter_list", "kOscIdPrefix", "kOscNamePrefix", NUM_OSCILLATORS),
    # filters are 1..kNumFilters plus a literal "fx" instance
    ("filter_parameter_list", "kFilterIdPrefix", "kFilterNamePrefix",
     [str(i + 1) for i in range(NUM_FILTERS)] + ["fx"]),
    ("mod_parameter_list", "kModulationIdPrefix", "kModulationNamePrefix",
     MAX_MODULATION_CONNECTIONS),
]

PREFIX_RE = re.compile(r'static\s+const\s+std::string\s+(k\w*Prefix)\s*=\s*"([^"]*)"\s*;')


def parse_prefixes(src):
    """Read the kXxxIdPrefix / kXxxNamePrefix string constants from the source."""
    found = {m.group(1): m.group(2) for m in PREFIX_RE.finditer(src)}
    needed = {c for _a, i, n, _x in GROUPS for c in (i, n)}
    missing = needed - set(found)
    if missing:
        raise SystemExit(f"could not find prefix constants: {sorted(missing)}")
    return found

# Applied after expansion, matching synth_parameters.cpp:586-590
DEFAULT_OVERRIDES = {
    "osc_1_on": 1.0,
    "osc_2_destination": 1.0,
    "osc_3_destination": 3.0,
    "filter_1_osc1_input": 1.0,
    "filter_2_osc2_input": 1.0,
}

SCALE_RE = re.compile(r"ValueDetails::k(\w+)")

# Several ValueDetails fields are C++ constant expressions rather than literals
# ("kMaxPolyphony - 1", "kNumFilterModels - 1.0"). We scan the whole tree for
# constexpr/const definitions and enum members so those can be evaluated.
CONSTEXPR_RE = re.compile(
    r"(?:static\s+)?constexpr\s+\w[\w:]*\s+(k\w+)\s*=\s*([^;]+);"
)
CONST_RE = re.compile(
    r"(?:static\s+)?const\s+(?:int|float|double|mono_float)\s+(k\w+)\s*=\s*([^;]+);"
)
ENUM_RE = re.compile(r"enum\s+(?:class\s+)?(\w+)?\s*(?::\s*\w+\s*)?\{([^{}]*?)\}\s*;", re.S)


def build_constant_table(src_root):
    """Scan the source tree for numeric constants and enum members."""
    raw = {}       # name -> unevaluated expression string
    collisions = set()

    def record(name, expr):
        expr = expr.strip()
        if name in raw and raw[name] != expr:
            collisions.add(name)
        raw.setdefault(name, expr)

    for path in list(src_root.rglob("*.h")) + list(src_root.rglob("*.cpp")):
        text = strip_comments(path.read_text(encoding="utf-8", errors="ignore"))
        for m in CONSTEXPR_RE.finditer(text):
            record(m.group(1), m.group(2))
        for m in CONST_RE.finditer(text):
            record(m.group(1), m.group(2))
        for m in ENUM_RE.finditer(text):
            body = m.group(2)
            ordinal = 0
            for member in split_top_level(body):
                member = member.strip()
                if not member:
                    continue
                if "=" in member:
                    mname, mexpr = member.split("=", 1)
                    mname = mname.strip()
                    if not mname.startswith("k"):
                        continue
                    record(mname, mexpr.strip())
                    val = try_eval(mexpr.strip(), {})
                    ordinal = (int(val) + 1) if val is not None else ordinal + 1
                else:
                    if member.startswith("k"):
                        record(member, str(ordinal))
                    ordinal += 1

    # Iteratively resolve: expressions may reference other constants.
    resolved = {}
    for _ in range(12):
        progress = False
        for name, expr in raw.items():
            if name in resolved:
                continue
            val = try_eval(expr, resolved)
            if val is not None:
                resolved[name] = val
                progress = True
        if not progress:
            break
    return resolved, collisions


def try_eval(expr, consts):
    """Evaluate a C++ constant expression against a table of known constants."""
    e = expr.strip()
    if not e:
        return None
    # Drop namespace qualifiers and float/unsigned literal suffixes.
    e = re.sub(r"\b\w+::", "", e)
    e = re.sub(r"(\d)[fFuUlL]+\b", r"\1", e)
    e = e.replace("static_cast<int>", "int").replace("static_cast<float>", "float")
    if not re.fullmatch(r"[\w\s+\-*/().]+", e):
        return None
    # A couple of tables call constexpr helpers (utils::factorial for the
    # effect-chain permutation count). Expose the ones that actually appear.
    ns = dict(consts)
    ns["factorial"] = lambda n: float(math.factorial(int(n)))
    try:
        val = eval(e, {"__builtins__": {}}, ns)  # noqa: S307
        return float(val) if isinstance(val, (int, float)) and not isinstance(val, bool) else None
    except Exception:
        return None


def split_top_level(text):
    """Split on commas that are not inside quotes or nested braces."""
    parts, depth, in_str, cur = [], 0, False, []
    i = 0
    while i < len(text):
        c = text[i]
        if in_str:
            if c == "\\":
                cur.append(c)
                i += 1
                if i < len(text):
                    cur.append(text[i])
                i += 1
                continue
            if c == '"':
                in_str = False
            cur.append(c)
        elif c == '"':
            in_str = True
            cur.append(c)
        elif c in "{[(":
            depth += 1
            cur.append(c)
        elif c in "}])":
            depth -= 1
            cur.append(c)
        elif c == "," and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    if "".join(cur).strip():
        parts.append("".join(cur).strip())
    return parts


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    return src


def extract_array_body(src, array_name):
    """Return the text between the outer braces of a named ValueDetails array."""
    marker = f"ValueDetailsLookup::{array_name}[] = {{"
    start = src.find(marker)
    if start < 0:
        raise SystemExit(f"could not find array {array_name}")
    i = start + len(marker)
    depth = 1
    body_start = i
    while i < len(src) and depth > 0:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
        i += 1
    return src[body_start:i - 1]


def parse_entries(body):
    """Yield each brace-delimited ValueDetails initializer inside an array body."""
    entries, depth, cur, in_str = [], 0, [], False
    i = 0
    while i < len(body):
        c = body[i]
        if in_str:
            if c == "\\":
                cur.append(c)
                i += 1
                if i < len(body):
                    cur.append(body[i])
                i += 1
                continue
            if c == '"':
                in_str = False
            cur.append(c)
        elif c == '"':
            in_str = True
            cur.append(c)
        elif c == "{":
            depth += 1
            if depth == 1:
                cur = []
            else:
                cur.append(c)
        elif c == "}":
            depth -= 1
            if depth == 0:
                entries.append("".join(cur))
            else:
                cur.append(c)
        elif depth >= 1:
            cur.append(c)
        i += 1
    return entries


def unquote(tok):
    tok = tok.strip()
    if tok.startswith('"') and tok.endswith('"'):
        return tok[1:-1]
    return tok


def num(tok, consts=None):
    tok = tok.strip()
    try:
        if tok.lower().startswith("0x"):
            return int(tok, 16)
        return float(tok)
    except ValueError:
        pass
    # Not a literal -- try it as a C++ constant expression.
    return try_eval(tok, consts or {})


def parse_value_details(entry, consts=None):
    """Field order: name, version_added, min, max, default, post_offset,
    display_multiply, value_scale, display_invert, display_units,
    display_name, string_lookup"""
    f = split_top_level(entry)
    if len(f) < 11:
        return None
    consts = consts or {}
    scale_match = SCALE_RE.search(f[7]) if len(f) > 7 else None
    lookup = f[11].strip() if len(f) > 11 else "nullptr"
    lookup = None if lookup in ("nullptr", "NULL", "") else lookup.split("::")[-1]
    return {
        "name": unquote(f[0]),
        "version_added": int(num(f[1], consts) or 0),
        "min": num(f[2], consts),
        "max": num(f[3], consts),
        "default": num(f[4], consts),
        "post_offset": num(f[5], consts),
        "display_multiply": num(f[6], consts),
        "scale": scale_match.group(1) if scale_match else "Linear",
        "display_invert": f[8].strip() == "true",
        "units": unquote(f[9]),
        "display_name": unquote(f[10]),
        "string_lookup": lookup,
    }


def parse_string_tables(src):
    """Parse `const std::string kFooNames[] = { "a", "b" };` into {name: [labels]}."""
    tables = {}
    for m in re.finditer(
        r"const\s+std::string\s+(k\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\}\s*;", src, re.S
    ):
        name, body = m.group(1), m.group(2)
        labels = [unquote(t) for t in split_top_level(body) if t.strip()]
        tables[name] = labels
    return tables


def main():
    root = Path(__file__).resolve().parent.parent / "vital"
    params_cpp = root / "src" / "common" / "synth_parameters.cpp"
    strings_h = root / "src" / "interface" / "look_and_feel" / "synth_strings.h"

    for p in (params_cpp, strings_h):
        if not p.exists():
            raise SystemExit(f"missing {p}")

    src = strip_comments(params_cpp.read_text(encoding="utf-8", errors="ignore"))
    strings_src = strip_comments(strings_h.read_text(encoding="utf-8", errors="ignore"))
    tables = parse_string_tables(strings_src)

    print("scanning source tree for constants...")
    consts, collisions = build_constant_table(root / "src")
    print(f"  resolved {len(consts)} constants"
          + (f", {len(collisions)} name collisions" if collisions else ""))
    if collisions:
        interesting = sorted(c for c in collisions if c.startswith(("kNum", "kMax", "kMin")))
        if interesting:
            print(f"  colliding names to verify: {interesting[:12]}")
    print()

    schema = {}

    # Ungrouped globals go in verbatim.
    for entry in parse_entries(extract_array_body(src, "parameter_list")):
        d = parse_value_details(entry, consts)
        if d:
            d["group"] = "global"
            schema[d["name"]] = d

    prefixes = parse_prefixes(src)
    print("prefixes read from source:")
    for _a, i, n, _x in GROUPS:
        print(f"  {prefixes[i]:<12} -> {prefixes[n]!r}")
    print()

    # Replay the group expansion.
    for array_name, id_const, name_const, instances in GROUPS:
        id_prefix, name_prefix = prefixes[id_const], prefixes[name_const]
        ids = ([str(i + 1) for i in range(instances)]
               if isinstance(instances, int) else instances)
        entries = parse_entries(extract_array_body(src, array_name))
        for inst in ids:
            for entry in entries:
                d = parse_value_details(entry, consts)
                if not d:
                    continue
                d = dict(d)
                d["name"] = f"{id_prefix}{ID_DELIMITER}{inst}{ID_DELIMITER}{d['name']}"
                d["display_name"] = (
                    f"{name_prefix}{NAME_DELIMITER}{inst}{NAME_DELIMITER}{d['display_name']}"
                )
                d["group"] = id_prefix
                d["instance"] = inst
                schema[d["name"]] = d

    for name, val in DEFAULT_OVERRIDES.items():
        if name in schema:
            schema[name]["default"] = val
        else:
            print(f"  WARNING: override target {name} not in schema", file=sys.stderr)

    # Resolve enum labels for indexed parameters.
    unresolved = set()
    for d in schema.values():
        lut = d.pop("string_lookup", None)
        if lut:
            if lut in tables:
                d["enum"] = tables[lut]
            else:
                unresolved.add(lut)

    out_dir = Path(__file__).resolve().parent / "out"
    out_dir.mkdir(exist_ok=True)

    full = dict(sorted(schema.items()))

    # LLM-facing subset: drop the 64 mod slots (routing lives in "modulations"),
    # and drop phantom parameters -- table entries the engine never registers,
    # so setting them does nothing. See find_phantom_params.py, which derives
    # them from a running plugin; we consume its output rather than hardcoding
    # a list that would rot the moment upstream changes.
    phantom_file = out_dir / "phantom_params.json"
    phantom = set()
    if phantom_file.exists():
        phantom = set(json.loads(phantom_file.read_text(encoding="utf-8"))["phantom"])
        for name in phantom:
            if name in full:
                full[name]["phantom"] = True
    else:
        print("  WARNING: out/phantom_params.json missing -- the LLM subset will")
        print("           include phantom parameters. Regenerate it with:")
        print("             VitalValidator.exe <plugin>.vst3 --dump-params out/host_params.tsv")
        print("             python find_phantom_params.py")
        print()

    # Written after phantom tagging so the full schema carries the flag too.
    (out_dir / "vital_schema_full.json").write_text(
        json.dumps(full, indent=2), encoding="utf-8"
    )

    llm = {
        k: v for k, v in full.items()
        if v.get("group") != "modulation" and k not in phantom
    }
    (out_dir / "vital_schema_llm.json").write_text(
        json.dumps(llm, indent=2), encoding="utf-8"
    )

    # ---- report ----
    by_group = {}
    for v in full.values():
        by_group[v["group"]] = by_group.get(v["group"], 0) + 1
    by_scale = {}
    for v in llm.values():
        by_scale[v["scale"]] = by_scale.get(v["scale"], 0) + 1

    print("parameters by group:")
    for g, n in sorted(by_group.items(), key=lambda x: -x[1]):
        print(f"  {g:12} {n:5}")
    print(f"  {'TOTAL':12} {len(full):5}")
    print()
    print(f"phantom parameters excluded: {len(phantom)}")
    print(f"LLM-facing subset: {len(llm)} parameters")
    print("  by scale:")
    for s, n in sorted(by_scale.items(), key=lambda x: -x[1]):
        print(f"    {s:12} {n:5}")
    print()
    enum_count = sum(1 for v in llm.values() if "enum" in v)
    indexed = sum(1 for v in llm.values() if v["scale"] == "Indexed")
    print(f"  indexed params: {indexed}, of which {enum_count} have resolved labels")
    if unresolved:
        print(f"  unresolved string tables: {sorted(unresolved)}")
    print()
    print(f"wrote {out_dir / 'vital_schema_full.json'}")
    print(f"wrote {out_dir / 'vital_schema_llm.json'}")


if __name__ == "__main__":
    main()

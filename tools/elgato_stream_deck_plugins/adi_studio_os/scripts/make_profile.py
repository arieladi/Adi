#!/usr/bin/env python3
"""Generate the Studio OS profile directly into the Stream Deck profile store.

A `.streamDeckProfile` export is a binary archive that can only be produced by
the Stream Deck app, which is why every legacy plugin here ends up telling the
user to hand-build and export one. But the INSTALLED form is a directory of
plain JSON, so the whole 36-key + 6-dial surface can simply be written — no
dragging one action 42 times, no export step.

This is unofficial: it writes into the app's own store. Therefore it
  * refuses to run while the Stream Deck app is open (it would overwrite us on
    quit, since the app flushes its in-memory state on exit),
  * backs up ProfilesV3 and the preferences plist before touching anything,
  * adds a NEW profile and never edits or deletes an existing one.

Usage:
    python3 scripts/make_profile.py            # create the profile
    python3 scripts/make_profile.py --activate # ...and make it the device default
    python3 scripts/make_profile.py --restore  # undo: put the newest backup back
"""
import json, os, plistlib, shutil, subprocess, sys, time, uuid

SUPPORT = os.path.expanduser("~/Library/Application Support/com.elgato.StreamDeck")
PROFILES = os.path.join(SUPPORT, "ProfilesV3")
PLIST = os.path.expanduser("~/Library/Preferences/com.elgato.StreamDeck.plist")

PLUGIN_UUID = "com.adiariel.studioos"
PLUGIN_NAME = "Adi Studio OS"
PLUGIN_VERSION = "2.0.0.0"
CELL = f"{PLUGIN_UUID}.cell"
DIAL = f"{PLUGIN_UUID}.dial"
PROFILE_NAME = "Studio OS"

COLS, ROWS, DIALS = 9, 4, 6
DEVICE_TYPE = 13   # Stream Deck + XL


def app_running():
    out = subprocess.run(["pgrep", "-f", "Elgato Stream Deck.app/Contents/MacOS/Stream Deck"],
                         capture_output=True, text=True)
    return bool(out.stdout.strip())


def load_plist():
    with open(PLIST, "rb") as fh:
        return plistlib.load(fh)


def find_device(pl):
    """Locate the Stream Deck + XL entry. Returns (plist_key, Model, UUID)."""
    for key, dev in (pl.get("Devices") or {}).items():
        if not key.strip("@").startswith("("):
            continue
        name = dev.get("DeviceName", "")
        if "XL" in name and "+" in name:
            models = dev.get("map_dev_accessories") or []
            model = next((m for m in [key] if False), None)
            return key, models, name
    return None, None, None


def device_model_from_profiles():
    """The Model string lives in existing profile manifests, not the plist."""
    for entry in sorted(os.listdir(PROFILES)):
        man = os.path.join(PROFILES, entry, "manifest.json")
        if not os.path.isfile(man):
            continue
        with open(man) as fh:
            d = json.load(fh)
        dev = d.get("Device") or {}
        if dev.get("Model") and dev.get("UUID"):
            return dev["Model"], dev["UUID"]
    return None, None


def state_block(alignment="middle"):
    return {
        "FontFamily": "", "FontSize": 12, "FontStyle": "", "FontUnderline": False,
        "Image": "", "OutlineThickness": 2, "ShowTitle": False,
        "TitleAlignment": alignment, "TitleColor": "#ffffff",
    }


def action(action_uuid, name, states):
    return {
        "ActionID": str(uuid.uuid4()),
        "LinkedTitle": True,
        "Name": name,
        "Plugin": {"Name": PLUGIN_NAME, "UUID": PLUGIN_UUID, "Version": PLUGIN_VERSION},
        "Resources": None,
        "Settings": {},
        "State": 0,
        "States": states,
        "UUID": action_uuid,
    }


def build_page():
    keypad, encoder = {}, {}
    for row in range(ROWS):
        for col in range(COLS):
            keypad[f"{col},{row}"] = action(CELL, "Studio OS Cell", [state_block()])
    for col in range(DIALS):
        encoder[f"{col},0"] = action(DIAL, "Studio OS Dial", [{}])
    return {
        "Controllers": [
            {"Type": "Keypad", "Actions": keypad},
            {"Type": "Encoder", "Actions": encoder},
        ],
        "Icon": "",
        "Name": PROFILE_NAME,
    }


def backup():
    stamp = time.strftime("%Y%m%d-%H%M%S")
    dst = os.path.join(SUPPORT, f"ProfilesV3.studioos-backup-{stamp}")
    shutil.copytree(PROFILES, dst)
    shutil.copy2(PLIST, PLIST + f".studioos-backup-{stamp}")
    return dst, stamp


def restore():
    backups = sorted(b for b in os.listdir(SUPPORT) if b.startswith("ProfilesV3.studioos-backup-"))
    if not backups:
        sys.exit("no Studio OS backup found")
    if app_running():
        sys.exit("quit the Stream Deck app first")
    newest = backups[-1]
    stamp = newest.split("ProfilesV3.studioos-backup-")[1]
    shutil.rmtree(PROFILES)
    shutil.copytree(os.path.join(SUPPORT, newest), PROFILES)
    pb = PLIST + f".studioos-backup-{stamp}"
    if os.path.exists(pb):
        shutil.copy2(pb, PLIST)
    print(f"restored ProfilesV3 and preferences from {stamp}")


def main():
    activate = "--activate" in sys.argv
    if "--restore" in sys.argv:
        return restore()
    if "--activate-only" in sys.argv:
        # Re-point the device at the existing Studio OS profile without creating
        # another one — the recovery path when activation failed on its own.
        existing = find_profile()
        if not existing:
            sys.exit(f"no '{PROFILE_NAME}' profile found — run without --activate-only first")
        return activate_profile(existing)

    if not os.path.isdir(PROFILES):
        sys.exit(f"no profile store at {PROFILES} — is the Stream Deck app installed?")
    if app_running():
        sys.exit("The Stream Deck app is running. Quit it first — it rewrites this\n"
                 "store from memory when it exits and would discard the new profile.")

    model, dev_uuid = device_model_from_profiles()
    if not model:
        sys.exit("could not determine the device from existing profiles — "
                 "open the Stream Deck app once with the device connected, then retry")

    dst, stamp = backup()
    print(f"backed up profile store -> {os.path.basename(dst)}")

    profile_id = str(uuid.uuid4()).upper()
    page_id = str(uuid.uuid4()).upper()
    default_id = str(uuid.uuid4()).upper()
    bundle = os.path.join(PROFILES, f"{profile_id}.sdProfile")

    # "Default" is a SEPARATE hidden page that is deliberately NOT listed in
    # Pages — that is how the app's own profiles are shaped (verified against
    # "Default Profile Mac", whose Default id appears in no Pages list). Setting
    # Current == Default makes the app repair the profile on launch, which is
    # what happened on the first attempt here.
    os.makedirs(os.path.join(bundle, "Profiles", page_id))
    os.makedirs(os.path.join(bundle, "Profiles", default_id))

    with open(os.path.join(bundle, "Profiles", page_id, "manifest.json"), "w") as fh:
        json.dump(build_page(), fh, indent=1)
    with open(os.path.join(bundle, "Profiles", default_id, "manifest.json"), "w") as fh:
        json.dump({"Controllers": [{"Type": "Keypad", "Actions": {}},
                                   {"Type": "Encoder", "Actions": {}}],
                   "Icon": "", "Name": "Default"}, fh, indent=1)

    # Directory names are uppercase but the Pages references are lowercase, which
    # is what the app itself writes — mirrored exactly rather than normalised.
    with open(os.path.join(bundle, "manifest.json"), "w") as fh:
        json.dump({
            "Device": {"Model": model, "UUID": dev_uuid},
            "Name": PROFILE_NAME,
            "Pages": {"Current": page_id.lower(), "Default": default_id.lower(),
                      "Pages": [page_id.lower()]},
            "Version": "3.0",
        }, fh, indent=1)

    print(f"created profile '{PROFILE_NAME}' ({profile_id})")
    print(f"  36 keys  -> {CELL}")
    print(f"   6 dials -> {DIAL}")

    if activate:
        activate_profile(profile_id)

    print(f"\nundo with:  python3 scripts/make_profile.py --restore   (backup {stamp})")


def activate_profile(profile_id):
    """Point the device's preferred-profile pointer at `profile_id`.

    Devices is NOT a uniform dict of device records — it also carries a
    "PreferredDevice" key whose value is a plain string, so every entry has to be
    type-checked before .get() is called on it.
    """
    if app_running():
        sys.exit("quit the Stream Deck app first")

    # cfprefsd MUST be flushed BEFORE the file is rewritten. It caches the plist
    # and will happily flush its stale copy back over a direct write — which is
    # exactly what silently reverted the first two attempts here: the app then
    # read the old preferred profile and the device stayed on its previous one.
    subprocess.run(["killall", "-u", os.environ.get("USER", ""), "cfprefsd"], capture_output=True)
    time.sleep(1)

    pl = load_plist()
    devices = pl.get("Devices") or {}
    target = next((k for k, v in devices.items()
                   if isinstance(v, dict) and "XL" in (v.get("DeviceName") or "")), None)
    if not target:
        print("! could not find the device in preferences — pick the profile manually")
        return False
    info = devices[target].setdefault("ESDProfilesInfo", {})
    info["ESDProfilesPreferred"] = profile_id.lower()
    with open(PLIST, "wb") as fh:
        plistlib.dump(pl, fh)
    print(f"set profile {profile_id} as default for '{devices[target].get('DeviceName')}'")
    return True


def find_profile(name=PROFILE_NAME):
    """Newest existing profile bundle with this Name, or None."""
    hits = []
    for entry in os.listdir(PROFILES):
        man = os.path.join(PROFILES, entry, "manifest.json")
        if not entry.endswith(".sdProfile") or not os.path.isfile(man):
            continue
        with open(man) as fh:
            d = json.load(fh)
        if d.get("Name") == name:
            hits.append((os.path.getmtime(man), entry[: -len(".sdProfile")]))
    return sorted(hits)[-1][1] if hits else None


if __name__ == "__main__":
    main()

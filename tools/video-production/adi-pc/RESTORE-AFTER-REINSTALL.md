# ADI-PC: bring the video AI back after a Windows reinstall

Written 2026-11-20, before Adi reinstalls ADI-PC with Windows 11 LTSC and Ableton 12.

## Where everything is

**All AI work is on F:, the external Verbatim USB drive (label `verbatim`, 1 TB):**
- `F:\AvasthaAI` holds the models, Deforum, ComfyUI, Python, the venvs, every run and video, the controller and ffmpeg.
- `F:\AvasthaAI\_before-reinstall-20261120` holds the system-side pieces that lived on C::
  - `AvasthaAI-Controller-task.xml`: the scheduled task that runs the Stop/Continue controller
  - `desktop\`: the "Stop video AI" / "Continue video AI" shortcuts, plus the two Cosmic Rain phone videos from the desktop
  - `ssh\`: `sshd_config`, `administrators_authorized_keys` and the public host keys. The private host keys were not copied on purpose, so Windows makes new ones.
  - `drivers\`: NVIDIA Quadro driver installers. **597.16 is the one in use**; 553.50 is older.
  - `system-info.txt`: users, firewall rules, GPU UUIDs, the long-paths setting and the drive list as they were.

**Not needed after the reinstall:** the uv download cache on C: (4.8 GB) and the pip cache. They are only caches; the venvs on F: carry their own packages.

**Everything runs from F: only (since 2026-11-20).** The old `D:\AvasthaAI` shortcut folder was removed. Every script, setting, venv and Python launcher now points at `F:\AvasthaAI` (1,206 text files and 151 launcher .exe files rewritten). Originals are in `_before-reinstall-20261120\d-to-f-*.zip`. Tested with D:\AvasthaAI gone: ComfyUI and Deforum started from F:, and the one-picture style rendered 8 frames matching the approved run. **Do not re-create `D:\AvasthaAI`.**

## Steps

1. **Unplug the F: USB drive before installing Windows.** Then the installer can't touch it. Plug it back in after setup.
2. **Give the drive back the letter F:.** Disk Management → right-click the `verbatim` volume → Change Drive Letter → F.
3. (Nothing to do for D:. The video AI doesn't use it.)
4. **Install the NVIDIA driver** `597.16-quadro-rtx-...exe` from `_before-reinstall-20261120\drivers`. Both GPUs, the Quadro RTX 4000 and the RTX A2000 12GB, must show in `nvidia-smi`. The scripts select the GPUs by UUID; the UUIDs belong to the cards, so they stay the same.
5. **Turn on long paths** (admin PowerShell):
   `Set-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem LongPathsEnabled 1`
6. **Remote access for ORC-VST (Claude):**
   - Create the local admin user `avastha-remote`. Adi chooses the password; nobody else needs it.
   - Settings → Optional features → add **OpenSSH Server**. Set the `sshd` service to Automatic and start it.
   - Copy `ssh\sshd_config` and `ssh\administrators_authorized_keys` to `C:\ProgramData\ssh\`. Then, in admin cmd:
     `icacls C:\ProgramData\ssh\administrators_authorized_keys /inheritance:r /grant "Administrators:F" /grant "SYSTEM:F"`
     Then `Restart-Service sshd`.
   - Firewall: allow inbound TCP 22 from 192.168.1.74 (ORC-VST) only:
     `New-NetFirewallRule -DisplayName "DroneBay SSH (ORC-VST only)" -Direction Inbound -Protocol TCP -LocalPort 22 -RemoteAddress 192.168.1.74 -Action Allow`
7. **Controller task** (admin cmd). Windows asks for the avastha-remote password:
   `schtasks /Create /XML "F:\AvasthaAI\_before-reinstall-20261120\AvasthaAI-Controller-task.xml" /TN "AvasthaAI Controller" /RU avastha-remote /RP`
8. **Desktop buttons:** copy `Stop video AI.lnk` and `Continue video AI.lnk` from `_before-reinstall-20261120\desktop` to the Studio desktop.
9. **Tell Claude it's done.** The PC will have new SSH host keys, so Claude refreshes ORC-VST's `known_hosts`. Claude then checks the services:
   - Deforum on port 7865
   - ComfyUI on port 8192
   - both GPUs
   - a short one-picture-style render

## Nothing else lives on C:

Checked 2026-11-20: no AI videos, models or scripts outside F:, and nothing in F:\AvasthaAI points at D: any more.

# Colour probe

Measures the display path stage by stage, so a colour complaint is settled by numbers. Event and file reference: the header comment of `devices/pvg_colorprobe.h`. Commands: `tools/colorprobe/probe.sh help`.

## What is known

- macOS shows its desktop through a 1920x1080 RGBA16Float surface (fourcc `RGhA`). With a gamma display profile (sRGB, Display P3, Generic RGB) the guest writes **display-encoded** values there: CSS gray 128 arrives as 0.5020. Measured 2026-11-06 on an iPhone 17 Pro, macOS 15.6.1.
- `hv_half_mode` (`devices/pvg_metal.m`) decides how that surface is shown, at all three sites (screen shader, `half_to_byte`, snapshot): `encoded` (default) passes values through; `linear` applies the sRGB encode, the pre-fix behaviour, right only for a linear profile such as ACES CG Linear. The pre-fix default encoded twice, which made the screen pale.
- Black, white and pure primaries survive any transfer function, so judge with mid-range colours.

## Steps

1. **Launch** with `probe.sh launch`. Relaunching ends a running guest abruptly: ask the user to shut macOS down from the Apple menu first. Done when `probe.sh follow` shows `event=probe_start` and `event=color_mode`.
2. **Show known colours.** Serve `tools/colorprobe/testpage/` on the LAN (`python3 -m http.server --bind <LAN ip> --directory tools/colorprobe/testpage`, after the user agrees to the open port) and have the user open it in Safari at 100% zoom. Done when the user has named the display profile in use.
3. **Capture** with `probe.sh capture`. Done when the files are pulled and the log holds a `post_check` line for that capture.
4. **Judge** with `python3 -I tools/colorprobe/swatches.py <pull dir>`. Done when it states what the guest wrote (encoded or linear) and what Henge did (pass-through or encode), and both match the mode in force. A mismatch names the faulty stage; for A/B, `probe.sh mode linear|encoded` switches live.

## Gotchas

- The syslog relay behind `probe.sh watch` can stall silently. `Documents/henge.log` is the authoritative record: `probe.sh follow` or `probe.sh log`.
- The pointer is drawn after the screen shader, so `post_check` reports a pointer-sized box; `verdict=matches_except_pointer_sized_box` is a pass.
- Captures hold whatever the user's screen showed. Analyse numbers; open the PNG previews only when the numbers cannot answer.

# Guest Environment Notes

This directory records guest-side environment fixes that are not part of the
Trierarch source tree but are required for the supported desktop profiles.

- [Hyprland / aquamarine](hyprland-aquamarine.md) — nested Wayland black screen
  caused by the distro aquamarine package being older than the upstream fix.

Add a new note here whenever a guest package needs a version newer than the
distribution repository provides, and include both the verification step and
the command to rebuild it.

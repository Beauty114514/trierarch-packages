#!/usr/bin/env python3
"""Check cross-language runtime paths against contracts/runtime-paths.json.

This is a source-level contract test.  It deliberately checks the actual
constants used by the Rust, C and Kotlin launch paths, so a change in one
component cannot silently drift away from the others.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

PACKAGES = Path(__file__).resolve().parents[1]
APP = PACKAGES.parent / "trierarch-app"
CONTRACT = PACKAGES / "contracts" / "runtime-paths.json"

failures: list[str] = []


def read(relative: str) -> str:
    path = PACKAGES / relative
    if not path.is_file():
        failures.append(f"missing source file: {path}")
        return ""
    return path.read_text(encoding="utf-8")


def expect(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


def rust_const(relative: str, name: str, expected: str) -> None:
    text = read(relative)
    match = re.search(
        rf'const\s+{re.escape(name)}\s*:\s*&str\s*=\s*"([^"]*)"', text
    )
    if not match:
        failures.append(f"{relative}: missing Rust constant {name}")
    elif match.group(1) != expected:
        failures.append(
            f"{relative}: {name} is {match.group(1)!r}, contract says {expected!r}"
        )


def c_define(relative: str, name: str, expected: str) -> None:
    text = read(relative)
    match = re.search(rf'#define\s+{re.escape(name)}\s+"([^"]*)"', text)
    if not match:
        failures.append(f"{relative}: missing C define {name}")
    elif match.group(1) != expected:
        failures.append(
            f"{relative}: {name} is {match.group(1)!r}, contract says {expected!r}"
        )


def source_contains(relative: str, expected: str) -> None:
    if expected not in read(relative):
        failures.append(f"{relative}: missing {expected!r}")


def kotlin_contains(relative: str, expected: str) -> None:
    path = APP / relative
    if not path.is_file():
        print(f"skip (app repository not present): {path}")
        return
    if expected not in path.read_text(encoding="utf-8"):
        failures.append(f"{path}: missing {expected!r}")


def main() -> int:
    contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
    wayland = contract["wayland"]
    dmabuf = wayland["dmabuf_feedback"]
    virgl = contract["virgl"]
    pipewire = contract["pipewire"]
    tvg = contract["tvg"]

    for file in ("native/src/chroot.rs", "native/src/droidspaces.rs"):
        rust_const(file, "GUEST_WAYLAND_HOST_DIRECTORY", wayland["host_directory"])
        rust_const(file, "GUEST_WAYLAND_RUNTIME_DIRECTORY", wayland["runtime_directory"])
    rust_const("native/src/proot/args.rs", "GUEST_WAYLAND_RUNTIME_DIRECTORY", wayland["runtime_directory"])
    rust_const("native/src/proot/args.rs", "GUEST_WAYLAND_IME_DIRECTORY", wayland["ime_directory"])

    rust_const("native/src/chroot.rs", "GUEST_DMABUF_FEEDBACK_SOCKET", dmabuf["chroot_path"])
    rust_const("native/src/droidspaces.rs", "GUEST_DMABUF_FEEDBACK_SOCKET", dmabuf["droidspaces_path"])
    rust_const("native/src/proot/args.rs", "GUEST_DMABUF_FEEDBACK_SOCKET", dmabuf["proot_path"])

    for file in ("native/src/chroot.rs", "native/src/droidspaces.rs", "native/src/proot/args.rs"):
        rust_const(file, "GUEST_VIRGL_RUNTIME_DIRECTORY", virgl["runtime_directory"])
        rust_const(file, "GUEST_PIPEWIRE_RUNTIME_DIRECTORY", pipewire["runtime_directory"])
        rust_const(file, "VIRGL_SOCKET", virgl["socket"])
        rust_const(file, "PIPEWIRE_SOCKET", pipewire["socket"])
    rust_const("native/src/pipewire.rs", "PULSE_SOCKET", pipewire["pulse_socket"])

    c_define("wayland-host/src/dmabuf_feedback_path.c", "FEEDBACK_SOCKET_NAME", dmabuf["socket_name"])
    source_contains("wayland-host/src/compositor.c", f'"{wayland["socket"]}"')

    kotlin_contains(
        "app/src/main/java/app/trierarch/config/ProfileStore.kt",
        f"TRIERARCH_TVG_SOCKET={tvg['socket']}",
    )
    kotlin_contains(
        "app/src/main/java/app/trierarch/config/ProfileStore.kt",
        f"LD_PRELOAD={tvg['gbm_shim']}",
    )

    if failures:
        print("runtime contract check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("runtime path contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

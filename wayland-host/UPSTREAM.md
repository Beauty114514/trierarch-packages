# Upstream sources

The package currently tracks the reference Wayland libraries and protocol
definitions separately from Trierarch-owned compositor code.

| Component | Repository | Pinned revision |
| --- | --- | --- |
| Wayland libraries | `https://gitlab.freedesktop.org/wayland/wayland.git` | `87cc8a8728a923fc57938faa81ba0e74f34ecdc7` (`1.26.0`) |
| Wayland protocols | `https://gitlab.freedesktop.org/wayland/wayland-protocols.git` | `ee78491a237eaff9389a0ccf8680521d074407d3` (`1.49`) |

The revisions correspond to the current stable tags listed by the
freedesktop Wayland mirrors: Wayland `1.26.0` and wayland-protocols `1.49`.
They are fetched by `scripts/fetch.sh`; the resulting source trees are local
build inputs and are not committed.

## Ownership boundary

- Upstream Wayland and protocol XML remain upstream material and retain their
  original licenses.
- Trierarch compositor code will live in this package.
- Generated protocol C files are build outputs unless a later compatibility
  decision requires checked-in generated sources.
- Any modification to upstream code must be represented by a numbered patch
  in `patches/`, with the upstream revision updated in this document.

The old implementation in `trierarch-old/trierarch-wayland` is a migration
reference, not an upstream checkout and not copied wholesale into this
package.

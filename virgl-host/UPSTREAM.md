# Upstream sources

| Component | Source | Pinned version | Integrity |
| --- | --- | --- | --- |
| virglrenderer | `https://gitlab.freedesktop.org/virgl/virglrenderer` | `virglrenderer-1.3.0` | SHA-256 `56170f8caa1bb642a2624b649e3bcca095ec2834814e5c308efc8a85a709e4ce` |
| libepoxy | `https://github.com/anholt/libepoxy` | `1.5.10` | SHA-256 `a7ced37f4102b745ac86d6a70a9da399cc139ff168ba6b8002b4d8d43c900c15` |
| ANGLE Android | Termux package repository | `2.1.24923-f09a19ce-2` aarch64 | SHA-256 `3e421cecefc8cb5ca9c57ffe1eb38f77dc315d88d99f1018ef0fdd2add6ca330` |

The version set and Android patch intent are migrated from the old Trierarch
VirGL builder. The old builder modified sources from inside one large build
script; this package instead stores its changes as a numbered patch series.

## Ownership boundary

- `sources/` and all downloaded archives are reproducible local inputs and
  ignored by Git.
- Every modification to virglrenderer or libepoxy is represented in
  `patches/series`.
- `build/` and `dist/` are local build outputs and ignored by Git.
- This package owns the Android VirGL server; it does not own guest Mesa,
  container lifecycle, or the app's runtime UI.

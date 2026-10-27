#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_DIR="${PIPEWIRE_REFERENCE_SOURCE_DIR:-$ROOT_DIR/sources/pipewire-standalone-aaudio-reference}"
SOURCE_COMMIT="0543368d3262e098b92ff0305e149ff0d1fce3e1"
SOURCE_LIB_DIR="$SOURCE_DIR/assets/libs/arm64-v8a"
OUTPUT_DIR="$ROOT_DIR/dist/android/arm64-v8a"

[[ -d "$SOURCE_DIR/.git" ]] || {
    echo "Run scripts/fetch.sh first" >&2
    exit 1
}
[[ "$(git -C "$SOURCE_DIR" rev-parse HEAD)" == "$SOURCE_COMMIT" ]] || {
    echo "PipeWire/AAudio reference source is not at pinned commit $SOURCE_COMMIT" >&2
    exit 1
}
[[ ! -e "$OUTPUT_DIR" ]] || {
    echo "Output already exists; preserve or move it before packaging: $OUTPUT_DIR" >&2
    exit 1
}

mkdir -p "$OUTPUT_DIR/bin" "$OUTPUT_DIR/lib" \
    "$OUTPUT_DIR/lib/spa-0.2/support" \
    "$OUTPUT_DIR/lib/spa-0.2/audioconvert" \
    "$OUTPUT_DIR/lib/spa-0.2/audiomixer" \
    "$OUTPUT_DIR/share"

for pair in \
    "libpipewire_exec.so:pipewire" \
    "libpipewire_pulse_exec.so:pipewire-pulse" \
    "libwireplumber_exec.so:wireplumber"; do
    source_name="${pair%%:*}"
    output_name="${pair#*:}"
    [[ -f "$SOURCE_LIB_DIR/$source_name" ]] || {
        echo "Missing upstream runtime artifact: $source_name" >&2
        exit 1
    }
    cp "$SOURCE_LIB_DIR/$source_name" "$OUTPUT_DIR/bin/$output_name"
done

mapfile -t sink_artifacts < <(find "$SOURCE_LIB_DIR" -maxdepth 1 -type f \
    -name '*pipewire*aaudio*sink*.so' | sort)
if [[ "${#sink_artifacts[@]}" -ne 1 ]]; then
    echo "Expected exactly one upstream PipeWire AAudio sink artifact; found ${#sink_artifacts[@]}" >&2
    exit 1
fi
cp "${sink_artifacts[0]}" "$OUTPUT_DIR/bin/aaudio-sink"

while IFS= read -r library; do
    case "$(basename "$library")" in
        libpipewire-0.3.so|libpipewire-module-*.so|\
        libwireplumber-0.5.so|libwireplumber-module-*.so|\
        libspa-support.so|libspa-audioconvert.so|libspa-audiomixer.so|\
        libandroid-support.so|libffi.so|libgio-2.0.so|libglib-2.0.so|\
        libgmodule-2.0.so|libgobject-2.0.so|libiconv.so|liblua5.4.so|\
        libpcre2-8.so|libz.so) ;;
        *) continue ;;
    esac
    cp "$library" "$OUTPUT_DIR/lib/"
done < <(find "$SOURCE_LIB_DIR" -maxdepth 1 -type f -name '*.so' | sort)

cp "$SOURCE_LIB_DIR/libspa-support.so" "$OUTPUT_DIR/lib/spa-0.2/support/"
cp "$SOURCE_LIB_DIR/libspa-audioconvert.so" "$OUTPUT_DIR/lib/spa-0.2/audioconvert/"
cp "$SOURCE_LIB_DIR/libspa-audiomixer.so" "$OUTPUT_DIR/lib/spa-0.2/audiomixer/"
cp "$SOURCE_DIR/assets/wireplumber-share.tar" "$OUTPUT_DIR/share/"

cat > "$OUTPUT_DIR/PIPEWIRE_ASSETS_MANIFEST.txt" <<EOF
Trierarch Android PipeWire runtime bundle
Architecture: arm64-v8a
PipeWire: Termux package 1.4.9-1
WirePlumber: Termux package 0.5.13
AAudio sink source revision: $SOURCE_COMMIT
Runtime executables are packaged under bin/; shared libraries and SPA plugins
are packaged under lib/. WirePlumber data is packaged in share/.

The upstream AAudio sink executable is normalized to bin/aaudio-sink.
EOF

echo "Packaged Trierarch Android PipeWire runtime at $OUTPUT_DIR"

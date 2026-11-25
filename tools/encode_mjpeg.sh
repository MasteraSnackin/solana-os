#!/usr/bin/env bash
# Encode a silent MJPEG clip (or JPEG sequence) for the BP26 Video app
# (vidplayer). Target panel is 320×240; frames must be baseline JPEG and
# under 48 KB each. Requires ffmpeg on PATH.
#
# Usage:
#   tools/encode_mjpeg.sh [-o out.mjpeg] [-r fps] [-q quality] [-s WxH] input.mp4
#   tools/encode_mjpeg.sh --sequence [-o walk/] [-r fps] [-q quality] input.mp4
#   tools/encode_mjpeg.sh --demo [-o tools/samples/videos/demo.mjpeg]
#
# Examples:
#   tools/encode_mjpeg.sh clip.mp4
#   tools/encode_mjpeg.sh -o demo.mjpeg -r 10 -q 7 movie.mkv
#   tools/encode_mjpeg.sh --sequence -o walk/ clip.mp4
#   tools/encode_mjpeg.sh --demo
set -euo pipefail

OUT=""
FPS=10
QUALITY=7
SIZE="320x240"
SEQUENCE=0
DEMO=0
INPUT=""

usage() {
  sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage 0 ;;
    -o|--output) OUT="$2"; shift 2 ;;
    -r|--fps) FPS="$2"; shift 2 ;;
    -q|--quality) QUALITY="$2"; shift 2 ;;
    -s|--size) SIZE="$2"; shift 2 ;;
    --sequence) SEQUENCE=1; shift ;;
    --demo) DEMO=1; shift ;;
    --) shift; break ;;
    -*)
      echo "unknown option: $1" >&2
      usage 1
      ;;
    *)
      if [[ -n "$INPUT" ]]; then
        echo "unexpected argument: $1" >&2
        usage 1
      fi
      INPUT="$1"
      shift
      ;;
  esac
done

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffmpeg not found on PATH" >&2
  exit 1
fi

W="${SIZE%x*}"; H="${SIZE#*x}"
if [[ -z "$W" || -z "$H" || "$W" == "$SIZE" ]]; then
  echo "bad --size (want WxH, e.g. 320x240)" >&2
  exit 1
fi

VF="scale=${W}:${H}:force_original_aspect_ratio=decrease,pad=${W}:${H}:(ow-iw)/2:(oh-ih)/2:black"

if [[ "$DEMO" -eq 1 ]]; then
  if [[ -n "$INPUT" ]]; then
    echo "--demo does not take an input file" >&2
    exit 1
  fi
  OUT="${OUT:-tools/samples/videos/demo.mjpeg}"
  mkdir -p "$(dirname "$OUT")"
  # Generated test pattern — no third-party footage, safe to redistribute.
  ffmpeg -y -f lavfi -i "testsrc=size=${W}x${H}:rate=${FPS}" -t 1 \
    -an -pix_fmt yuvj420p -c:v mjpeg -q:v "$QUALITY" "$OUT"
  echo "wrote $OUT (1s lavfi testsrc @ ${W}x${H} ${FPS}fps q=${QUALITY})"
  exit 0
fi

if [[ -z "$INPUT" ]]; then
  echo "missing input video (or use --demo)" >&2
  usage 1
fi
if [[ ! -f "$INPUT" ]]; then
  echo "input not found: $INPUT" >&2
  exit 1
fi

base="$(basename "$INPUT")"
stem="${base%.*}"

if [[ "$SEQUENCE" -eq 1 ]]; then
  OUT="${OUT:-${stem}_frames}"
  mkdir -p "$OUT"
  ffmpeg -y -i "$INPUT" -an -vf "$VF" -r "$FPS" -pix_fmt yuvj420p \
    -q:v "$QUALITY" "${OUT}/%04d.jpg"
  echo "wrote JPEG sequence under $OUT/ (copy the folder to /sdcard/videos/)"
  exit 0
fi

OUT="${OUT:-${stem}.mjpeg}"
mkdir -p "$(dirname "$OUT")"
# Raw MJPEG: concatenated baseline JPEGs (SOI…EOI…). No audio.
ffmpeg -y -i "$INPUT" -an -vf "$VF" -r "$FPS" -pix_fmt yuvj420p \
  -c:v mjpeg -q:v "$QUALITY" "$OUT"
echo "wrote $OUT — copy to /sdcard/videos/ on the microSD card"

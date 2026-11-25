# Sample clips for Video (vidplayer)

Silent MJPEG for the BP26 badge player. Clips belong on the microSD card under
`/sdcard/videos/` (not in flash).

## Bundled demo

`videos/demo.mjpeg` — ~1 s, 320×240, 10 fps, baseline JPEG. Generated with
ffmpeg’s `lavfi` `testsrc` (no third-party footage). Regenerate:

```bash
tools/encode_mjpeg.sh --demo
```

Copy onto the card:

```bash
# card reader
cp tools/samples/videos/demo.mjpeg /path/to/card/videos/

# or serial (if sdrecv / tools/sd_put.py is in your build)
python3 tools/sd_put.py --port /dev/cu.usbserial-110 \
  tools/samples/videos/demo.mjpeg videos/demo.mjpeg
```

## Encode your own

```bash
tools/encode_mjpeg.sh -o demo.mjpeg clip.mp4
tools/encode_mjpeg.sh --sequence -o walk/ clip.mp4
```

Defaults: 320×240, 10 fps, `-q:v 7`, silent. Keep each JPEG frame under
**48 KB**; use `-q 8` or a lower resolution (`-s 160x120`) if needed.

If you prefer not to commit binaries, omit `videos/demo.mjpeg` and always run
`--demo` (or encode any public-domain source) before copying to the card.

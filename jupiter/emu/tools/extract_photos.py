#!/usr/bin/env python3
"""
extract_photos.py -- pull the DP700WD's built-in demo photos out of its ROM.

The Coby DP700WD ships five demo images in flash (the Windows sample-picture
set: butterfly, Grand Teton barn, chrysanthemum, Golden Gate, koala/etc.).
They live at the top of the image as ordinary JFIF/EXIF JPEGs, one 640x360
"slide" per 64 KiB slot, each followed by two thumbnails.

This tool locates each slot's main image, decodes it, and (optionally) renders
it at the DP700WD's 480x234 panel geometry the way the frame's scaler would --
i.e. what you'd actually see on the screen.

Usage:
    python3 extract_photos.py [rom.bin] [-o outdir] [--panel]

Requires Pillow (PIL).
"""
import argparse
import io
import os
import sys

# Built-in slideshow slots (flash offsets), verified against dp700wd.bin.
SLOT_OFFSETS = [0x160000, 0x170000, 0x180000, 0x190000, 0x1A0000]
SLOT_SPAN = 0x10000            # one 64 KiB flash sector per slide

# The boot LOGO: the "LOGO" flash section (table entry at 0xe8) is a 480x270
# JFIF that the firmware decodes and shows at power-on (UTL_ShowLogo). The JPEG
# starts a few bytes into the section payload.
LOGO_OFFSET = 0x104DE0

PANEL_W, PANEL_H = 480, 234    # DP700WD 7" LCD


def decode_slot(rom: bytes, off: int):
    """Decode the main JPEG that begins at a slot offset."""
    if rom[off:off + 3] != b"\xff\xd8\xff":
        return None
    # Hand Pillow the whole sector; it stops at the image's real EOI and
    # ignores the trailing thumbnails / padding.
    im = Image.open(io.BytesIO(rom[off:off + SLOT_SPAN]))
    im.load()
    return im.convert("RGB")


def to_panel(im):
    """Scale an image onto the 480x234 panel, preserving aspect (letterboxed),
    which is how the frame presents a 16:9 photo on its wider LCD."""
    fitted = im.copy()
    fitted.thumbnail((PANEL_W, PANEL_H), Image.LANCZOS)
    canvas = Image.new("RGB", (PANEL_W, PANEL_H), (0, 0, 0))
    canvas.paste(fitted, ((PANEL_W - fitted.width) // 2,
                          (PANEL_H - fitted.height) // 2))
    return canvas


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("rom", nargs="?",
                    default=os.path.join(here, "..", "dp700wd.bin"),
                    help="ROM image (default: ../dp700wd.bin)")
    ap.add_argument("-o", "--outdir", default=".",
                    help="output directory (default: current)")
    ap.add_argument("--panel", action="store_true",
                    help="also render each photo at 480x234 panel geometry")
    args = ap.parse_args()

    rom = open(args.rom, "rb").read()
    os.makedirs(args.outdir, exist_ok=True)

    # boot logo first
    logo = decode_slot(rom, LOGO_OFFSET)
    if logo is not None:
        p = os.path.join(args.outdir, "logo.png")
        logo.save(p)
        print("LOGO  @ %#08x  %dx%d  -> %s" % (LOGO_OFFSET, logo.width,
                                               logo.height, p))
        if args.panel:
            to_panel(logo).save(os.path.join(args.outdir, "logo_panel.png"))

    n = 0
    for i, off in enumerate(SLOT_OFFSETS):
        im = decode_slot(rom, off)
        if im is None:
            print("slot %d @ %#x: no JPEG" % (i, off))
            continue
        p = os.path.join(args.outdir, "demo%d.png" % i)
        im.save(p)
        print("slot %d @ %#08x  %dx%d  -> %s" % (i, off, im.width, im.height, p))
        if args.panel:
            pp = os.path.join(args.outdir, "demo%d_panel.png" % i)
            to_panel(im).save(pp)
            print("                 panel 480x234 -> %s" % pp)
        n += 1
    print("extracted %d/%d demo photos" % (n, len(SLOT_OFFSETS)))


if __name__ == "__main__":
    try:
        from PIL import Image
    except ImportError:
        sys.exit("this tool needs Pillow:  pip install pillow")
    main()

#!/usr/bin/env python3
"""
Build the release manifest the device fetches from GitHub.

The manifest only announces that a release exists; the firmware still reads the
app descriptor out of the image it downloads before installing anything (see
main/app/ota_manager.c). What this script adds is the matching check on the
other side: it reads the version out of the binary it is describing and refuses
to publish a manifest that disagrees with the tag. A release whose asset says
"v1.1.0" while the manifest says "v1.2.0" would leave every device in the fleet
convinced an update is pending and unable to ever apply it, because the image
it keeps downloading is not actually newer.

Usage:
    python tools/make_manifest.py \
        --bin build/luanvan_firmware.bin \
        --tag v1.2.0 \
        --repo owner/name \
        --asset luanvan_firmware.bin \
        --notes "Short one-line note" \
        --out manifest.json
"""

import argparse
import hashlib
import json
import os
import struct
import sys

# esp_app_desc_t lives immediately after the 24-byte image header and the
# 8-byte header of the first segment, i.e. at offset 0x20 of the .bin.
APP_DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432

# Must match OTA_NOTES_MAX in main/app/ota_manager.h: anything longer is
# truncated on the device anyway, so truncate here where it is visible.
NOTES_MAX = 47


def read_app_desc(path):
    """Return (version, project_name, idf_ver) from a built ESP-IDF app image."""
    with open(path, "rb") as f:
        f.seek(APP_DESC_OFFSET)
        blob = f.read(144)
    if len(blob) < 144:
        sys.exit("error: %s is too short to hold an app descriptor" % path)

    magic = struct.unpack("<I", blob[0:4])[0]
    if magic != APP_DESC_MAGIC:
        sys.exit("error: no app descriptor at offset 0x%X in %s (magic 0x%08X)"
                 % (APP_DESC_OFFSET, path, magic))

    def field(start, length):
        return blob[start:start + length].split(b"\x00")[0].decode("utf-8", "replace")

    return field(16, 32), field(48, 32), field(112, 32)


def semver(s):
    """major.minor.patch as a tuple, ignoring a leading 'v' and any suffix."""
    s = s.lstrip("vV")
    parts = []
    for chunk in s.split(".")[:3]:
        digits = ""
        for ch in chunk:
            if not ch.isdigit():
                break
            digits += ch
        if digits == "":
            return None
        parts.append(int(digits))
    if len(parts) < 2:
        return None
    while len(parts) < 3:
        parts.append(0)
    return tuple(parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="path to the built app image")
    ap.add_argument("--tag", required=True, help="release tag, e.g. v1.2.0")
    ap.add_argument("--repo", required=True, help="owner/name")
    ap.add_argument("--asset", default="luanvan_firmware.bin",
                    help="asset filename as it will appear on the release")
    ap.add_argument("--notes", default="", help="one-line release note")
    ap.add_argument("--out", default="manifest.json")
    args = ap.parse_args()

    version, project, idf_ver = read_app_desc(args.bin)

    tag_ver = semver(args.tag)
    img_ver = semver(version)
    if tag_ver is None:
        sys.exit("error: tag %r is not major.minor[.patch]" % args.tag)
    if img_ver is None:
        sys.exit("error: the image reports version %r, which is not a release "
                 "build. The tag was probably not fetched: checkout needs "
                 "fetch-depth: 0 so `git describe` can see it." % version)
    if tag_ver != img_ver:
        sys.exit("error: tag %s and image version %s disagree; refusing to "
                 "publish a manifest the fleet could never satisfy"
                 % (args.tag, version))

    data = open(args.bin, "rb").read()
    manifest = {
        "version": args.tag,
        "url": "https://github.com/%s/releases/download/%s/%s"
               % (args.repo, args.tag, args.asset),
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "notes": args.notes.strip().replace("\n", " ")[:NOTES_MAX],
        "project": project,
        "idf_version": idf_ver,
    }

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    print("wrote %s for %s (%s, %d bytes)"
          % (args.out, args.tag, project, len(data)))
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()

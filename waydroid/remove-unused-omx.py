#!/usr/bin/env python3
"""Remove the stale OMX HAL declaration from Waydroid's mainline vendor image."""

import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def remove_unused_omx(rootfs: Path) -> None:
    manifest = rootfs / "vendor/etc/vintf/manifest.xml"
    software_codec = rootfs / "system/apex/com.android.media.swcodec/bin/mediaswcodec"
    if not manifest.is_file() or not software_codec.is_file():
        return

    # Keep the declaration on images that ship an OMX service. Check both the
    # usual executable names and init interfaces for differently named services.
    for name in ("vendor", "odm", "system", "system_ext", "product"):
        partition = rootfs / name
        for directory in ("bin", "bin/hw"):
            if any((partition / directory).glob("android.hardware.media.omx*")):
                return
        for rc in (partition / "etc/init").glob("*.rc"):
            if "android.hardware.media.omx@" in rc.read_text():
                return

    parser = ET.XMLParser(target=ET.TreeBuilder(insert_comments=True))
    tree = ET.parse(manifest, parser=parser)
    device = tree.getroot()
    removed = False
    for hal in device.findall("hal"):
        if hal.get("format") == "hidl" and hal.findtext("name") == "android.hardware.media.omx":
            device.remove(hal)
            removed = True
    if removed:
        tree.write(manifest, encoding="utf-8", xml_declaration=True)
        print("Removed the unavailable OMX HAL from the vendor manifest")


if __name__ == "__main__":
    remove_unused_omx(Path(sys.argv[1]))

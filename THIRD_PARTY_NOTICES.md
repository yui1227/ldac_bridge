# Third-party notices

ldac_bridge does not include third-party source code in this repository. `setup.sh`
downloads the components below at pinned versions. Release packages contain binaries
built from them, together with the license texts in the `licenses/` folder.

## BTstack

- Source: https://github.com/bluekitchen/btstack (pinned commit in `setup.sh`)
- Copyright (C) 2009 BlueKitchen GmbH
- License: BTstack license (BSD-style, **non-commercial use only**), see `licenses/BTSTACK_LICENSE.txt`
- Modified: `patches/btstack-avrcp-passthrough-release.patch` (AVRCP pass-through release response),
  `patches/btstack-intel-stale-reply.patch` (ignore stale replies during Intel controller startup)

Because ldac_bridge.exe statically links BTstack, the program may only be used,
modified and redistributed for personal benefit, not for any commercial purpose or
monetary gain. Commercial licensing is available from BlueKitchen GmbH.

## libldac

- Source: https://android.googlesource.com/platform/external/libldac (pinned commit in `setup.sh`)
- Copyright Sony Corporation
- License: Apache License 2.0, see `licenses/LIBLDAC_LICENSE.txt` and `licenses/LIBLDAC_NOTICE.txt`

Sony's NOTICE states that taking the LDAC certification process is required to use
LDAC in products. ldac_bridge is a non-commercial hobby project, not a product, and is
not certified or endorsed by Sony.

## Intel Bluetooth firmware

- Source: https://gitlab.com/kernel-firmware/linux-firmware (`intel/ibt-*.sfi`, `intel/ibt-*.ddc`)
- Copyright © 2014, Intel Corporation
- License: redistributable in unmodified binary form, see `firmware/LICENCE.ibt_firmware`

## Protocol references

The Sony headset battery protocol follows the protocol description reverse-engineered by
the [Gadgetbridge](https://codeberg.org/Freeyourgadget/Gadgetbridge) project. No
Gadgetbridge code is included.

## Trademarks

LDAC is a trademark of Sony Corporation. Bluetooth is a registered trademark of
Bluetooth SIG, Inc. Intel is a trademark of Intel Corporation. This project is not
affiliated with or endorsed by any of them.

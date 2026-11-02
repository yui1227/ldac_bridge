# ldac_bridge

**English** | [繁體中文](README.zh-TW.md)

Send Windows audio to Bluetooth headphones over **LDAC** (up to 990 kbps), which Windows does not support natively.

This is not a kernel driver. It is a regular user-mode program that takes over one Bluetooth adapter and runs its own Bluetooth stack:

```
Windows playback → virtual audio device (VB-CABLE) → WASAPI loopback → libldac encoder → BTstack A2DP → Bluetooth adapter (WinUSB) → headphones
```

## Features

- LDAC at 44.1 / 48 / 88.2 / 96 kHz, 990 / 660 / 330 kbps, plus a simple adaptive bitrate mode
- Headphone buttons (play/pause, next, previous) become Windows media keys
- Two-way volume sync between Windows and the headphones (AVRCP absolute volume)
- Left / right / case battery levels on Sony headphones
- Tray app: starts at sign-in, shows a notification with the battery level when the headphones connect, and recovers after sleep or a lost USB connection
- Reconnects the way Windows does: it retries for a few minutes, then waits for the headphones to connect back

## ⚠ Before you start

- **The Bluetooth adapter is taken away from Windows.** ldac_bridge needs the adapter's driver replaced with WinUSB. While that driver is installed, Windows' own Bluetooth (mice, keyboards, game controllers, other headphones) does not work on that adapter. Use a second adapter if you need both. The change can be undone, see [Restore Windows Bluetooth](#restore-windows-bluetooth).
- **Only one adapter has been tested so far.** See [Supported hardware](#supported-hardware).
- **No microphone.** Only A2DP playback is implemented, so there is no headset microphone (HFP) for calls.
- **Latency is roughly 150–200 ms**, most of it inside the headphones. It is fine for music; for video, use your player's audio delay setting (VLC, mpv, MPC-HC, PotPlayer).
- **Non-commercial use only.** See [License](#license).

## Supported hardware

### Bluetooth adapter

The adapter must be supported by BTstack's Windows WinUSB port, and Intel adapters also need their firmware loaded by ldac_bridge.

| Adapter | USB ID | Status |
|---|---|---|
| Intel ThunderPeak, e.g. Wireless-AC 9260 (firmware `ibt-18-16-1`) | `8087:0025` | ✅ Tested |
| Other Intel adapters | `8087:xxxx` | ❓ Need a different firmware file. Not supported yet |
| Realtek, Broadcom, CSR, MediaTek | – | ❓ Not supported yet |

Many desktop motherboards and laptops have an Intel Bluetooth adapter built in. To find yours, open Device Manager → Bluetooth → your adapter → Properties → Details → Hardware Ids, and look for `VID_8087&PID_xxxx`.

### Headphones

Any headphones with LDAC should stream. Tested:

| Headphones | LDAC | Buttons | Volume sync | Battery |
|---|---|---|---|---|
| Sony WF-1000XM6 | ✅ | ✅ | ✅ | ✅ L / R / case |
| EarFun Wave Pro | ✅ | not tested | not tested | – |

Battery levels use Sony's own control protocol, so they are only shown for Sony headphones.

Reports for other adapters and headphones are welcome. Please open an issue.

## Installation

### 1. Download

Download the latest `ldac_bridge-*-win64.zip` from [Releases](../../releases) and extract it to a permanent folder, for example `C:\Program Files\ldac_bridge` or `C:\Tools\ldac_bridge`. Keep the `firmware` folder next to the executables. To build it yourself, see [Building from source](#building-from-source).

### 2. Install a virtual audio device

Windows needs a playback device to send sound to. ldac_bridge records that device and sends it to the headphones.

1. Download **VB-CABLE** from the [VB-Audio website](https://vb-audio.com/Cable/), run `VBCABLE_Setup_x64.exe` as administrator, and restart.
2. In **Settings → System → Sound**, set the output device to **CABLE Input**.
3. Run `ldac_bridge.exe --list-devices` and check that `CABLE Input` is listed.

ldac_bridge records the default playback device, so the volume slider and media keys keep working as usual.

### 3. Give the Bluetooth adapter to WinUSB

1. Download and run [Zadig](https://zadig.akeo.ie).
2. Choose **Options → List All Devices**.
3. Select your Bluetooth adapter, for example **Intel(R) Wireless Bluetooth(R)**. **Check that the USB ID matches the adapter you found above.** Choosing the wrong device can disable your keyboard, mouse or another USB device.
4. Select **WinUSB** as the target driver and click **Replace Driver**.
5. Shut down and start the computer again, so the adapter starts from its bootloader.

### 4. First connection

Put the headphones in pairing mode, then run in a terminal:

```bash
ldac_bridge.exe --name "WF-1000XM6"
```

`--name` picks the first device whose name contains that text. Without it, the first audio device found is used. After pairing, the headphones are remembered (`last_device.txt`), so later starts reconnect without any options.

A successful connection looks like this:

```
Intel controller ready.
BTstack up and running on XX:XX:XX:XX:XX:XX.
A2DP: connected to XX:XX:XX:XX:XX:XX, querying codecs...
  remote codec: SBC
  remote codec: LDAC (seid 2), rates: 44.1k 48k 88.2k 96k
Selecting LDAC 48000 Hz stereo
A2DP: LDAC configured, 48000 Hz
A2DP: streaming LDAC. Play something on Windows. Ctrl-C to quit.
[stats] HQ 990 kbps, 9xx kbps on air, 187 pkt/s, buffer 28/30 ms, drift +12 ppm, ...
Battery: L 80% | R 79% | Case 67%
```

Press Ctrl-C to stop.

### 5. Run in the background and start at sign-in

`ldac_tray.exe` runs `ldac_bridge.exe` hidden and adds a tray icon:

- When the headphones connect, a notification shows their name and battery levels.
- The tray icon's tooltip shows the connection state and battery levels. Click the icon to open the log, restart Bluetooth, or exit.
- It restarts ldac_bridge after a crash, after sleep, or when the adapter stops responding.
- All output goes to `ldac_bridge.log` next to the executables.

To start it when you sign in to Windows (current user only, no administrator rights needed):

```bash
ldac_tray.exe --install     # turn on
ldac_tray.exe --uninstall   # turn off
```

Do not run `ldac_bridge.exe` by hand while `ldac_tray.exe` is running. Both would try to use the same adapter.

The tray UI is in Traditional Chinese when the Windows display language is Chinese, and in English otherwise.

## Options

| Option | Description |
|---|---|
| `--quality hq\|sq\|mq\|abr` | 990 / 660 / 330 kbps, or adaptive starting at 990. Use `abr` or `sq` if the sound drops out |
| `--latency 30` | Capture buffer in ms, 10–500 (default 30). Lower is less delay; raise it if the sound drops out while the computer is busy |
| `--rate 96000` | LDAC sampling rate (default: same as the captured device, usually 48 kHz) |
| `--device "CABLE Output"` | Device to capture (default: loopback of the default playback device) |
| `--addr XX:XX:XX:XX:XX:XX` | Connect to this address |
| `--name TEXT` | When scanning, pick the first device whose name contains TEXT |
| `--scan` | Ignore the remembered headphones and scan again |
| `--list-devices` | List audio devices and exit |
| `--sony-debug` | Print raw Sony control protocol frames (for debugging battery levels) |

## How reconnecting works

1. At startup or after a disconnect, ldac_bridge tries to connect 10 times, waiting 5, 10, 20, 40, then 60 seconds between attempts (about 6 minutes in total).
2. After that, it stops trying and stays connectable. When the headphones are turned on or taken out of the case, they connect back to the last computer they used.
3. Only the paired headphones are accepted. Other devices cannot connect or pair, and the computer is not discoverable.

## Troubleshooting

- **`Intel firmware setup failed`**: the firmware for your adapter is missing from `firmware/`, or the adapter is not in its bootloader. Shut down fully and start the computer again.
- **No sound but it says streaming**: check that the Windows output device is CABLE Input, and that the `peak` value in the `[stats]` line moves while sound is playing.
- **Sound drops out**: if `underruns` in the `[stats]` line goes up, raise `--latency` (e.g. 50). Otherwise try `--quality abr` or `--quality sq`, and keep the adapter's antenna away from USB 3 ports and cables.
- **Bluetooth stops working after sleep**: `ldac_tray.exe` restarts automatically. `ldac_bridge.exe` alone exits, and you start it again.
- **Packet logs**: `hci_dump.pklg` (and `hci_dump.prev.pklg` from the previous run) can be opened in Wireshark.
- **After a Windows Update** the original driver may come back. Repeat step 3.

## Restore Windows Bluetooth

1. Exit ldac_bridge (tray icon → Exit).
2. In Device Manager → Universal Serial Bus devices, right-click your Bluetooth adapter → **Uninstall device**, and tick **Attempt to remove the driver for this device**.
3. Choose **Scan for hardware changes**, or restart. Windows reinstalls its own driver.
4. Pair the headphones in Windows again. Pairing with ldac_bridge replaced the keys stored in the headphones.

## Building from source

Requires [MSYS2](https://www.msys2.org) with the UCRT64 toolchain:

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make patch
```

Then, in Git Bash or an MSYS2 UCRT64 shell, from the repository root:

```bash
sh setup.sh                                # download pinned BTstack, libldac and Intel firmware, apply patches/
sh ldac_bridge/build.sh                    # build ldac_bridge.exe and ldac_tray.exe
sh ldac_bridge/build.sh offline_test.exe   # self-test that does not need Bluetooth
ldac_bridge/offline_test.exe               # should end with OK
```

`third_party/` and `ldac_bridge/firmware/` are created by `setup.sh` and are not in git. GitHub Actions builds every push. Pushing a `v*` tag publishes a release.

### Changes to BTstack

`patches/btstack-avrcp-passthrough-release.patch` fixes the AVRCP pass-through response so it echoes the button-release bit. Without it, some headphones (Sony WF-1000XM6) keep resending the release, and the buttons work only once. When updating BTstack in `setup.sh`, check whether this is fixed upstream.

## Known limitations

- Clock drift between the audio device and Bluetooth is compensated by resampling up to ±0.2 % faster or slower (shown as `drift` in the `[stats]` line), which is inaudible.
- Only Classic Bluetooth is used. LE Audio is not.
- Windows only has a play/pause toggle, so if the headphones and the player get out of sync, press the button once more.

## License

The code in this repository is released under the [MIT License](LICENSE).

The built program links [BTstack](https://github.com/bluekitchen/btstack), whose license allows **non-commercial use only**. ldac_bridge as a whole therefore may not be used or redistributed for any commercial purpose or monetary gain. LDAC encoding uses Sony's libldac (Apache 2.0). Release packages include Intel Bluetooth firmware under Intel's redistribution license. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

LDAC is a trademark of Sony Corporation. This project is not affiliated with or endorsed by Sony, Intel or BlueKitchen.

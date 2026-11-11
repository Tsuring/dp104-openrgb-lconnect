# DP104 OpenRGB + L-Connect

Native OpenRGB lighting support for the **Ticktype DP104**, with an L-Connect 3 compatibility mode. Control the keyboard from L-Connect's OpenRGB device list and apply its color, brightness, speed and direction settings.

```text
L-Connect 3 → OpenRGB → DP104 master → PLC → 104 key LEDs
```

This is an experimental source project for a custom OpenRGB build and a matching patched firmware pair. Stock DP104 firmware does not implement the required protocol. The adapter is compiled into OpenRGB; it is not a loadable plugin or an upstream OpenRGB feature.

## Features

- A real HID detector that registers the keyboard only after an end-to-end PLC capability check.
- Full 104-color frames with chunked transfer, CRC validation and atomic publication.
- L-Connect compatibility mode with horizontal Wave projection based on key coordinates. Ordinary OpenRGB keyboard mode preserves individual LED index order.
- A latest-frame mailbox that keeps keyboard I/O separate from other controllers.
- Bounded recovery after transient errors: fresh capability negotiation followed by the latest frame, using a new transaction.
- A one-second ownership lease that returns lighting to the built-in renderer when host frames stop.
- Source, target manifests, firmware builders, protocol documentation and tests.

## Supported target

| Component | Pinned input / implementation |
| --- | --- |
| Keyboard | Ticktype DP104 |
| Master input | **1.2.0 8K** firmware |
| PLC input | **1.2.0** firmware |
| Compatibility application | **1.2.1** on both master and PLC |
| OpenRGB adapter | Checked against **OpenRGB 1.0rc2** |
| HID interface | `E560:E104`, interface 1, usage `FF60:0061` |

The ordinary master firmware has a different layout and cannot be substituted for the 8K input. A reported application version alone does not establish the polling-rate variant. Exact input hashes and reservations are recorded in [the target manifest](firmware/targets/dp104-v1.2.0-8k.json).

Both matching patched images are required. The current pair uses a fixed 46-byte internal UART envelope and is incompatible with earlier experimental escaped-wire builds. Original vendor firmware, generated firmware images and prebuilt server binaries are not included.

## L-Connect integration

The normal adapter identifies as an OpenRGB keyboard. The tested L-Connect build filters that category, so `DP104Settings.lConnectCompatibility` exposes the same real controller through a generic GPU lighting category and projects L-Connect's linear frames onto the keys' horizontal coordinates. Colors and animation timing still come from L-Connect.

RGB channel order must be explicitly calibrated on the device. On the tested keyboard, the confirmed order is `[1, 0, 2]` (GRB storage); this is not a universal default. [The calibration tool](tools/calibrate.py) prints a device-specific settings fragment. The compatibility option belongs in that validated `DP104Settings` block; it does not replace the calibration fields or bypass the capability check.

L-Connect also expands motherboard zones into numeric device IDs. A combined server must account for controller ordering and existing profiles. The tested integration places compatibility-mode DP104 before the motherboard; see [controller ordering and profile boundaries](docs/architecture.md#color-order-and-openrgb). This repository does not install a combined OpenRGB server or migrate saved profiles.

The DP104 implementation is maintained separately from [the GC575 project](https://github.com/Tsuring/avermedia-gc575-openrgb-lconnect) and the 8.8-inch screen integration. No screen compatibility DLL is required by this adapter.

## Build and test

Requirements: CMake 3.16 or later, a C99/C++17 compiler, and Python 3 for the Python test cases.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

To compile adapter objects against an existing OpenRGB source checkout and enable the stub-HID/controller tests:

```sh
cmake -S . -B build -DDP104_OPENRGB_SOURCE=/path/to/OpenRGB
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

This checks the adapter API and test bindings; it does not build, launch or install a complete OpenRGB server. The integrated test configuration passed **21/21 CTest cases**. Tests use simulated bindings and do not access USB hardware.

### Firmware review tools

Supply the original `DP104_v1.2.0.zip` archive yourself and use **Zig 0.15.2**. Paths can be passed explicitly:

```sh
python -B tools/verify_target.py /path/to/DP104_v1.2.0.zip
python -B tools/plan_firmware_hooks.py /path/to/DP104_v1.2.0.zip --zig /path/to/zig
python -B tools/build_firmware_objects.py --zig /path/to/zig
```

These commands inspect the pinned inputs, plan reservations and compile authored firmware objects. Output stays under ignored `build/firmware`; paths can be supplied explicitly.

The complete-image builders (`build_master_payload.py`, `build_plc_payload.py` and the restore experiments) retain dependencies on local review proofs and CMSIS provenance/source records under ignored `.local/`. Those records are not distributed here. A clean checkout plus the original archive is therefore **not a self-contained recipe for the reviewed firmware images**. The builders are published as implementation/reference source with their validation guards intact; they generate local review artifacts and do not send firmware to a device.

The synthetic CPU harnesses use QEMU through the existing Windows/WSL Ubuntu workflow:

```sh
python -B tools/run_firmware_cpu_tests.py --zig /path/to/zig
python -B tools/run_master_hook_test.py --zig /path/to/zig
python -B tools/run_master_hook_test.py --task --zig /path/to/zig
```

These execute portable authored code or synthetic callback veneers, not the complete keyboard firmware or MCU peripherals. A separate allocator test reads an isolated fragment from the user-supplied original image. See [architecture and evidence boundaries](docs/architecture.md) for the detailed memory layout.

## Device tools

With the optional Python `hidapi` package installed:

```sh
python -B tools/probe.py
python -B tools/probe.py --query
```

The first command inventories the exact interface; `--query` sends a capability request without setting colors. Stock firmware is expected to fail the capability gate. Calibration is a separate, explicit hardware operation described in [the protocol](docs/protocol.md).

## Validation status

The following results were recorded on the tested unit on **2026-11-11**:

| Check | Result |
| --- | --- |
| Master/PLC application queries | Both report 1.2.1; end-to-end CAPS and complete-frame ACKs confirmed |
| Native L-Connect observation | 180 samples over about three minutes; 4,287 additional completed frames, zero frame or exchange failures |
| Firmware varied-frame regression | 1,223 frames / 19,624 exchanges in 60 seconds; no retries or foreign reports |
| Zero-heavy and bare-marker regressions | 248 completed frames each |
| Native fault tests | Fresh CAPS, newest-frame recovery, retry/window limits and interruptible shutdown passed |
| Physical confirmation | Wave and color/brightness/speed/direction changes remain active |
| Existing functions | User confirmed keyboard input, upper display, RAM/GPU/GC575 RGB and 8.8-inch screen/sensors remain normal |

The native observation did not encounter a fault, so recovery behavior is supported by the mock fault tests rather than a live fault-recovery event. Earlier builds exposed a same-process worker stop; the current bounded recovery policy addresses that behavior. Detailed observations and remaining unknowns are in [the architecture record](docs/architecture.md).

Real-time 8K polling, maximum heap/stack demand, flash readback, independent recovery of a corrupted PLC and unlimited runtime are not established. Firmware recovery must be understood before hardware experimentation. Flashing, installation, profile migration and update-maintenance operation scripts are kept outside the published source.

## License

Project code is **GPL-2.0-or-later**; see [LICENSE](LICENSE). Preserve OpenRGB's file-specific notices when combining sources. No vendor DP104 firmware is redistributed.

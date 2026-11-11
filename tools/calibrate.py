"""Explicit volatile color calibration for the experimental DP104 extension.

No firmware write, reset, DFU or boot command is implemented. Run only after
separately approving and installing the reviewed experimental firmware pair.
"""
import argparse
import json
import secrets
import sys


def make_request(pulse=None, order=None, nonce=None):
    request = bytearray(32)
    request[0], request[2] = 0xD2, 0xE1
    if pulse is not None and order is None and type(pulse) is int and 0 <= pulse <= 2:
        request[1], request[4], request[5] = 0xF0, pulse, 32
    elif pulse is None and order is not None and len(order) == 3 and all(type(c) is int for c in order) and sorted(order) == [0, 1, 2]:
        request[1] = 0xF1
        request[4:7] = bytes(order)
        request[7:11] = b"USER"
        nonce = secrets.token_bytes(16) if nonce is None else nonce
        if len(nonce) != 16 or nonce == bytes(16):
            raise ValueError("Calibration requires a fresh nonzero 16-byte nonce.")
        request[11:27] = nonce
    else:
        raise ValueError("Choose one raw channel 0..2 or an RGB-channel permutation.")
    return bytes(request)


def valid_ack(request, response):
    if len(response) != 32 or response[:3] != request[:3] or response[3] != 0:
        return False
    if request[1] == 0xF1:
        return (response[4:7] == request[4:7] and response[7:23] == request[11:27]
                and response[23:] == bytes(9))
    return response[4:] == bytes(28)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--pulse", type=int, choices=range(3),
                       help="Light one raw channel at 32/255 for at most 1000 ms")
    group.add_argument("--rgb-channels", type=int, nargs=3, metavar=("R", "G", "B"),
                       help="Raw channel indices observed as red, green and blue")
    parser.add_argument("--confirm-observed", action="store_true",
                        help="Assert that you personally observed this color permutation")
    args = parser.parse_args()
    if args.rgb_channels is not None and not args.confirm_observed:
        parser.error("--rgb-channels requires --confirm-observed; no default color order exists")
    try:
        request = make_request(args.pulse, args.rgb_channels)
    except ValueError as error:
        parser.error(str(error))
    try:
        import hid
    except ImportError:
        parser.error("The optional Python hidapi module is required.")
    devices = [d for d in hid.enumerate(0xE560, 0xE104)
               if d.get("interface_number") == 1 and d.get("usage_page") == 0xFF60
               and d.get("usage") == 0x61]
    if len(devices) != 1:
        parser.error("Exactly one DP104 FF60:0061 interface is required.")
    device = hid.device()
    try:
        device.open_path(devices[0]["path"])
        for _ in range(4):
            if not device.read(32, 1):
                break
        if device.write(bytes((0,)) + request) != 33:
            raise RuntimeError("Short HID write; no success is assumed.")
        response = bytes(device.read(32, 700))
        if not valid_ack(request, response):
            raise RuntimeError("No exact PLC acknowledgement; calibration was not confirmed.")
    finally:
        device.close()
    result = {"acknowledged": True, "firmwareWritten": False}
    if args.rgb_channels is not None:
        path = devices[0]["path"]
        if isinstance(path, bytes):
            path = path.decode("utf-8", errors="strict")
        result["OpenRGBSettingsFragment"] = {
            "DP104Settings": {"schema": 1, "verifiedColorOrders": [
                {"hidPath": path, "confirmed": True, "rgbChannels": args.rgb_channels}
            ]}
        }
        result["note"] = "Merge this fragment into the correct OpenRGB configuration only after review. No file was changed."
    else:
        result["note"] = "Raw-channel pulse only; observe its physical color. CAPS remains unavailable until the order is confirmed."
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(2)

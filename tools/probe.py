"""Read-only DP104 inventory/capability query. Never sets colors or enters DFU."""
import argparse
import json
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--query", action="store_true",
                        help="Send exactly one D2 CAPS query to one matching device")
    args = parser.parse_args()
    try:
        import hid
    except ImportError:
        parser.error("The optional hidapi Python module is required for this hardware probe.")

    devices = [d for d in hid.enumerate(0xE560, 0xE104)
               if d.get("interface_number") == 1 and
               d.get("usage_page") == 0xFF60 and d.get("usage") == 0x61]
    result = {
        "matchingInterfaces": len(devices),
        "devices": [{k: d.get(k) for k in
                     ("manufacturer_string", "product_string", "interface_number",
                      "usage_page", "usage", "release_number")}
                    for d in devices],
        "querySent": False,
        "directSupported": False,
    }
    if args.query:
        if len(devices) != 1:
            result["reason"] = "Exactly one matching DP104 interface is required."
            print(json.dumps(result, indent=2))
            return 2
        request = bytearray(32)
        request[:3] = bytes((0xD2, 0, 0x51))
        device = hid.device()
        try:
            device.open_path(devices[0]["path"])
            # Bound stale-report draining; this is not a discovery/command scan.
            for _ in range(4):
                if not device.read(32, 1):
                    break
            written = device.write(bytes((0,)) + request)
            result["querySent"] = True
            result["requestHex"] = request.hex()
            if written != 33:
                result["reason"] = "Short HID query write."
                print(json.dumps(result, indent=2))
                return 2
            reply = bytes(device.read(32, 700))
            result["responseHex"] = reply.hex()
            result["directSupported"] = (
                len(reply) == 32 and reply[:4] == bytes((0xD2, 0, 0x51, 0)) and
                reply[4:12] == b"DP104RGB" and reply[12:14] == bytes((1, 0)) and
                int.from_bytes(reply[14:16], "little") == 104 and
                reply[16] == 8 and reply[17] == 1 and
                int.from_bytes(reply[18:20], "little") == 1000 and
                reply[20:] == bytes(12)
            )
            result["reason"] = ("Extension capability confirmed; no colors were sent."
                                if result["directSupported"] else
                                "No compatible end-to-end RGB extension response; do not register a controller.")
        finally:
            device.close()
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())

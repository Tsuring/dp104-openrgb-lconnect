"""Validate the exact 8K input archive without writing or flashing firmware."""
import argparse
import hashlib
import json
import struct
import zipfile
from pathlib import Path

TARGET = Path(__file__).resolve().parents[1] / "firmware/targets/dp104-v1.2.0-8k.json"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def parse_uf2(data, target):
    require(len(data) > 0 and len(data) % 512 == 0, "Invalid UF2 length")
    count = len(data) // 512
    blocks = {}
    for offset in range(0, len(data), 512):
        block = data[offset:offset + 512]
        m0, m1, flags, address, size, number, total, family = struct.unpack_from("<8I", block)
        require(m0 == 0x0A324655 and m1 == 0x9E5D5157 and
                struct.unpack_from("<I", block, 508)[0] == 0x0AB16F30,
                "Invalid UF2 magic")
        require(flags == 0x2000 and size == 256, "Unexpected UF2 flags/payload")
        require(total == count and number < count and number not in blocks,
                "Invalid/duplicate UF2 block number")
        require(family == int(target["family"], 0), "Wrong UF2 family")
        require(address == int(target["base"], 0) + number * 256,
                "Noncontiguous or reordered target addresses")
        blocks[number] = block[32:32 + size]
    require(len(blocks) == count, "Incomplete UF2 block set")
    raw = b"".join(blocks[i] for i in range(count))
    require(len(raw) == target["size"], "Wrong raw image length")
    require(hashlib.sha256(raw).hexdigest() == target["rawSha256"], "Wrong raw image hash")
    return raw


def validate_archive(path):
    spec = json.loads(TARGET.read_text(encoding="utf-8"))
    require(hashlib.sha256(path.read_bytes()).hexdigest() == spec["archiveSha256"],
            "Archive does not match the reviewed DP1041.2.0 input")
    result = {"target": spec["target"], "readOnly": True, "images": []}
    with zipfile.ZipFile(path) as archive:
        for role in ("master", "plc"):
            target = spec[role]
            require(archive.namelist().count(target["archiveEntry"]) == 1,
                    "Missing/duplicate selected archive member")
            data = archive.read(target["archiveEntry"])
            require(hashlib.sha256(data).hexdigest() == target["uf2Sha256"],
                    "Selected UF2 hash differs from the reviewed target")
            raw = parse_uf2(data, target)
            result["images"].append({"role": role, "member": target["archiveEntry"],
                                     "rawSha256": hashlib.sha256(raw).hexdigest(),
                                     "bytes": len(raw), "blocks": len(data) // 512})
    result["patchProduced"] = False
    result["bootloaderAcceptanceVerified"] = False
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(validate_archive(args.archive), indent=2))
    except (ValueError, OSError, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()

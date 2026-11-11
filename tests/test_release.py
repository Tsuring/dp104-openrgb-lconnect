import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from release_version import ROOT, load_release, validate_release


class ReleaseTests(unittest.TestCase):
    def test_project_master_and_plc_advertise_the_same_release(self):
        release = load_release()
        self.assertEqual(release["compatibilityVersion"], "1.2.1")
        self.assertEqual(int(release["packedFirmwareVersion"], 0), 0x121)
        self.assertEqual(release["baseTarget"], "targets/dp104-v1.2.0-8k.json")

    def test_mixed_release_is_rejected(self):
        release = load_release()
        header = (ROOT / "include/dp104_version.h").read_text()
        cmake = (ROOT / "CMakeLists.txt").read_text()
        for field, value in (("compatibilityVersion", "1.2.0"), ("packedFirmwareVersion", "0x120")):
            bad = copy.deepcopy(release)
            bad[field] = value
            with self.assertRaises(ValueError):
                validate_release(bad, header, cmake)
        with self.assertRaises(ValueError):
            validate_release(release, header.replace('"1.2.1"', '"1.2.0"'), cmake)


if __name__ == "__main__":
    unittest.main()

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from calibrate import make_request, valid_ack


class CalibrationToolTests(unittest.TestCase):
    def test_only_bounded_explicit_commands(self):
        for channel in range(3):
            request = make_request(pulse=channel)
            self.assertEqual(request[:6], bytes((0xD2, 0xF0, 0xE1, 0, channel, 32)))
            self.assertEqual(request[6:], bytes(26))
        request = make_request(order=[2, 0, 1], nonce=bytes(range(16)))
        self.assertEqual(request[:11], bytes((0xD2, 0xF1, 0xE1, 0, 2, 0, 1)) + b"USER")
        self.assertEqual(request[11:27], bytes(range(16)))
        self.assertEqual(request[27:], bytes(5))
        for kwargs in ({}, {"pulse": 3}, {"pulse": -1}, {"pulse": 0, "order": [0, 1, 2]},
                       {"order": [0, 0, 2]}, {"order": [0, 1]}):
            with self.assertRaises(ValueError):
                make_request(**kwargs)

    def test_ack_requires_exact_operation_transaction_status_and_zero_tail(self):
        request = make_request(pulse=0)
        ack = request[:3] + bytes(29)
        self.assertTrue(valid_ack(request, ack))
        for index in (0, 1, 2, 3, 4, 31):
            bad = bytearray(ack)
            bad[index] ^= 1
            self.assertFalse(valid_ack(request, bytes(bad)))
        self.assertFalse(valid_ack(request, ack[:-1]))
        self.assertFalse(valid_ack(request, ack + b"\0"))

    def test_old_ack_cannot_confirm_new_order_or_nonce(self):
        req = make_request(order=[2, 0, 1], nonce=bytes(range(16)))
        ack = req[:3] + b"\0" + req[4:7] + req[11:27] + bytes(9)
        self.assertTrue(valid_ack(req, ack))
        self.assertFalse(valid_ack(req, req[:3] + bytes(29)))
        changed_order = make_request(order=[0, 1, 2], nonce=bytes(range(16)))
        self.assertFalse(valid_ack(changed_order, ack))
        changed_nonce = make_request(order=[2, 0, 1], nonce=bytes(range(1, 17)))
        self.assertFalse(valid_ack(changed_nonce, ack))
        with self.assertRaises(ValueError):
            make_request(order=[2, 0, 1], nonce=bytes(16))


if __name__ == "__main__":
    unittest.main()

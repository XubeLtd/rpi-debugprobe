import unittest
from types import SimpleNamespace
from blink_running_led import PRODUCT, parse_args, select_device


class IdentificationTests(unittest.TestCase):
    def test_multiple_probes_require_exact_serial(self):
        devices = [SimpleNamespace(product=PRODUCT, serial_number=value) for value in ["ONE", "TWO"]]
        with self.assertRaises(RuntimeError):
            select_device(devices, None)
        self.assertIs(select_device(devices, "TWO"), devices[1])
        with self.assertRaises(RuntimeError):
            select_device(devices, "ABSENT")

    def test_duplicate_serial_is_ambiguous(self):
        device = SimpleNamespace(product=PRODUCT, serial_number="SAME")
        with self.assertRaises(RuntimeError):
            select_device([device, device], "SAME")

    def test_single_probe_compatibility(self):
        device = SimpleNamespace(product=PRODUCT, serial_number="ONE")
        self.assertIs(select_device([device], None), device)
        self.assertEqual(parse_args(["A"]).seconds, 5)

    def test_duration_is_bounded(self):
        for value in ["0", "-1", "nan", "inf", "121"]:
            with self.assertRaises(SystemExit):
                parse_args(["A", value])
        self.assertEqual(parse_args(["B", "60", "--serial", "ONE"]).serial, "ONE")


if __name__ == "__main__":
    unittest.main()

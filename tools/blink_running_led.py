#!/usr/bin/env python3
# Blinks the DAP Running LED of one probe on the dual-probe Pico with the
# CMSIS-DAP DAP_HostStatus command, then turns it off and exits. Install its
# packages with: pip install -r tools/requirements.txt
# Usage: blink_running_led.py A|B [seconds]
import argparse
import math
import time

VID, PID = 0x2E8A, 0x000C
PRODUCT = "Dual Debugprobe (CMSIS-DAP)"  # PROBE_PRODUCT_STRING
DAP_ITF = {"A": 0, "B": 3}  # PROBE_DAP_ITF, PROBE_B_DAP_ITF
DAP_HOST_STATUS, RUNNING_LED = 0x01, 0x01
HALF_PERIOD = 0.25


def select_device(devices, serial):
    matches = [dev for dev in devices if dev.product == PRODUCT and
               (serial is None or dev.serial_number == serial)]
    if len(matches) != 1:
        raise RuntimeError("Select exactly one connected dual probe with --serial.")
    return matches[0]


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description="Blink one Pico HAT's onboard LED without connecting to its targets.")
    parser.add_argument("probe", choices=DAP_ITF)
    parser.add_argument("seconds", type=float, nargs="?", default=5.0)
    parser.add_argument("--serial", help="Exact Pico USB serial; required when several HATs are connected.")
    args = parser.parse_args(argv)
    if not math.isfinite(args.seconds) or not 0 < args.seconds <= 120:
        parser.error("seconds must be greater than zero and at most 120")
    return args


def main():
    import libusb_package
    import usb.core
    import usb.util

    args = parse_args()

    dev = select_device(usb.core.find(find_all=True, idVendor=VID, idProduct=PID,
                        backend=libusb_package.get_libusb1_backend()), args.serial)
    itf = dev.get_active_configuration()[(DAP_ITF[args.probe], 0)]
    ep_out = usb.util.find_descriptor(itf, custom_match=lambda e: not e.bEndpointAddress & 0x80)
    ep_in = usb.util.find_descriptor(itf, custom_match=lambda e: e.bEndpointAddress & 0x80)

    def set_led(on):
        ep_out.write(bytes([DAP_HOST_STATUS, RUNNING_LED, on]), timeout=1000)
        if bytes(ep_in.read(64, timeout=1000)[:2]) != bytes([DAP_HOST_STATUS, 0x00]):
            raise RuntimeError("probe did not accept DAP_HostStatus")

    usb.util.claim_interface(dev, itf.bInterfaceNumber)
    try:
        end = time.monotonic() + args.seconds
        on = True
        while (left := end - time.monotonic()) > 0:
            set_led(on)
            on = not on
            time.sleep(min(HALF_PERIOD, left))
    finally:
        try:
            set_led(False)
        finally:
            usb.util.release_interface(dev, itf.bInterfaceNumber)
            usb.util.dispose_resources(dev)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# Blinks the DAP Running LED of one probe on the dual-probe Pico with the
# CMSIS-DAP DAP_HostStatus command, then turns it off and exits. Install its
# packages with: pip install -r tools/requirements.txt
# Usage: blink_running_led.py A|B [seconds]
import sys
import time

import libusb_package
import usb.core
import usb.util

VID, PID = 0x2E8A, 0x000C
PRODUCT = "Dual Debugprobe (CMSIS-DAP)"  # PROBE_PRODUCT_STRING
DAP_ITF = {"A": 0, "B": 3}  # PROBE_DAP_ITF, PROBE_B_DAP_ITF
DAP_HOST_STATUS, RUNNING_LED = 0x01, 0x01
HALF_PERIOD = 0.25


def main():
    probe = sys.argv[1].upper() if len(sys.argv) > 1 else ""
    if probe not in DAP_ITF:
        sys.exit("usage: blink_running_led.py A|B [seconds]")
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0

    dev = usb.core.find(idVendor=VID, idProduct=PID, backend=libusb_package.get_libusb1_backend(),
                        custom_match=lambda d: d.product == PRODUCT)
    if dev is None:
        sys.exit("no dual debug probe found")
    itf = dev.get_active_configuration()[(DAP_ITF[probe], 0)]
    ep_out = usb.util.find_descriptor(itf, custom_match=lambda e: not e.bEndpointAddress & 0x80)
    ep_in = usb.util.find_descriptor(itf, custom_match=lambda e: e.bEndpointAddress & 0x80)

    def set_led(on):
        ep_out.write(bytes([DAP_HOST_STATUS, RUNNING_LED, on]))
        if bytes(ep_in.read(64, timeout=1000)[:2]) != bytes([DAP_HOST_STATUS, 0x00]):
            sys.exit("probe did not accept DAP_HostStatus")

    usb.util.claim_interface(dev, itf.bInterfaceNumber)
    try:
        end = time.monotonic() + seconds
        on = True
        while (left := end - time.monotonic()) > 0:
            set_led(on)
            on = not on
            time.sleep(min(HALF_PERIOD, left))
        set_led(False)
    finally:
        usb.util.release_interface(dev, itf.bInterfaceNumber)
        usb.util.dispose_resources(dev)


if __name__ == "__main__":
    main()

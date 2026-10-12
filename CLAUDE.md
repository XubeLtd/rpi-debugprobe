# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Firmware for the Raspberry Pi Debug Probe (CMSIS-DAP SWD probe + USB-to-UART bridge), also runnable on a Pico or Pico 2. It is built on the Pico SDK with TinyUSB and FreeRTOS SMP. This fork adds a variant that runs two independent probes on one Pico 1 (`DUAL_PROBE_PICO`).

## Building

`./setup.sh` installs everything below. It installs the host tools via Homebrew on macOS (on Linux it only lists what's missing) and initialises the FreeRTOS submodule. The Pico SDK, Arm toolchain and picotool go into the git-ignored `.pico-sdk/` folder in the repo: `.pico-sdk/sdk/2.3.1`, `.pico-sdk/toolchain/14_2_Rel1` and `.pico-sdk/picotool/2.3.0/picotool`. Re-running it skips whatever is already installed; the versions are set at the top of the script.

Requirements:
- Pico SDK 2.3.0 or newer. `pico_sdk_import.cmake` enforces this; pass `-DPICO_SDK_PATH=...` or export it.
- The FreeRTOS kernel submodule: `git submodule update --init --recursive`.
- An arm-none-eabi toolchain that includes newlib, such as the Arm GNU Toolchain. Homebrew's `arm-none-eabi-gcc` formula has no newlib and fails to link with `cannot find -lc`. Point CMake at a full toolchain with `-DPICO_TOOLCHAIN_PATH=...`.

Use a separate build directory per variant, because `PICO_BOARD` is cached:

```bash
cmake -B build                                          # Debug Probe -> debugprobe.uf2
cmake -B build-pico -DPICO_BOARD=pico                   # -> debugprobe_on_pico.uf2
cmake -B build-pico2 -DPICO_BOARD=pico2                 # -> debugprobe_on_pico2.uf2
cmake -B build-dual -DPICO_BOARD=pico -DDUAL_PROBE_PICO=ON  # -> debugprobe_dual_on_pico.uf2
cmake --build build-pico -j
```

- `.vscode/tasks.json` builds the dual-probe variant into `build-dual/` using the `.pico-sdk/` dependencies. It flashes with `picotool load -f --vid 0x2e8a --pid 0x000c`, which reboots a running probe into BOOTSEL through the reset interface. The VID/PID filter is needed because picotool only auto-detects a few Raspberry Pi product IDs, not the Debug Probe's `0x000c`. If no running probe is found, the task falls back to a plain `load` for a Pico already in BOOTSEL. Avoid `picotool info` as a device check: picotool 2.3.0 segfaults on it while a Pico is in BOOTSEL.
- There are no unit tests, linter, or CI. To verify a change, build every variant that compiles the touched files. Shared sources such as `main.c`, `cdc_uart.c`, `autobaud.c`, `usb_descriptors.c` and `DAP_config.h` go into all of them.
- `CMakeLists.txt` hard-sets `CMAKE_BUILD_TYPE` to `RelWithDebInfo`. `NDEBUG` is therefore defined, so `assert()` and `configASSERT()` compile to nothing.
- The binary is `copy_to_ram`: code, data and the 64 KB FreeRTOS heap (`heap_4`) all share the 264 KB of SRAM.
- `get-version.sh` generates `<build>/generated/probe/version.h` (`PROBE_VERSION`) from `git describe`.
- Debug logging (`probe_info`/`probe_debug`/`probe_dump` in `src/probe_config.h`) is compiled out with `#if false`. When enabled, it prints over the stdio UART, which is UART0 on single-probe builds.

## Architecture

**Board configuration.** `PICO_BOARD` selects an SDK board header, which defines a macro such as `RASPBERRYPI_PICO`. `include/board_config.h` maps that macro to one of the `include/board_*_config.h` files. Those files define the `PROBE_*` macros: pins, UART, LEDs, and the SWD I/O style. `board_example_config.h` documents every option. The I/O style is `PROBE_IO_RAW`/`PROBE_IO_SWDI` (uses `probe.pio`) or `PROBE_IO_OEN` (uses `probe_oen.pio`). Code checks these macros with `#ifdef`, and `src/probe_config.h` is the common include that pulls them in.

**Tasks** (`src/main.c`). FreeRTOS runs SMP on both cores with a 20 kHz tick, so tick counts are in units of 50 µs.
- `usb_thread` runs TinyUSB's `tud_task` on core 0.
- `dev_mon` (RP2040 only) watches the USB SOF counter and re-initialises the device stack if it stalls.
- Each probe has its own UART thread (`cdc_thread`, highest priority), DAP thread (`dap_thread`) and autobaud thread.
- The per-probe tasks are created in `tud_mount_cb` and deleted in `tud_unmount_cb`. That happens on every USB bus reset, which is why the heap must support freeing.

**DAP data path.**
1. The CMSIS-DAP v2 bulk interface is served by a custom TinyUSB class driver in `src/tusb_edpt_handler.c`, registered through `usbd_app_driver_get_cb`. It does not use TinyUSB's vendor class.
2. That driver fills request/response ring buffers that `dap_thread` drains.
3. `dap_thread` calls `DAP_ExecuteCommand` in ARM's vendored `DAP.c`.
4. `DAP.c` calls into `src/sw_dp_pio.c`, which replaces ARM's bit-banging `SW_DP.c` (that file is intentionally not compiled).
5. `sw_dp_pio.c` calls `src/probe.c`, which drives `src/probe.pio`. Each word in the PIO TX FIFO is either a command (bit count, SWDIO direction, jump address) or data, and one SWCLK period is 4 PIO cycles.

`include/DAP_config.h` connects ARM's code to the probe: `PORT_SWD_SETUP` maps to `probe_init`, and it also holds the reset pin, LEDs, capability flags such as `SWO_UART`, and the DAP_Info strings.

**USB descriptors** (`src/usb_descriptors.c`).
- The default protocol is CMSIS-DAP v2 bulk (`PROBE_DEBUG_PROTOCOL`). The v1 HID callbacks remain in `main.c`.
- The DAP interface must stay interface 0, because old Keil MDK versions only look there.
- MS OS 2.0 descriptors bind WinUSB using the standard CMSIS-DAP `DeviceInterfaceGUID` `{CDB3B5AD-293B-4663-AA36-1AAE46463776}`, which host tools such as Keil MDK search for.
- The reset interface comes from the SDK's `pico_usb_reset`.

**UART bridge** (`src/cdc_uart.c`).
- The thread polls at an interval that scales with the baud rate.
- The host's DTR line suspends and resumes the thread.
- Setting baud rate 9728 (`MAGIC_BAUD`) starts `src/autobaud.c`. It measures edge timings with PIO and a DMA ring buffer, and sends its estimates to the UART thread through a queue.

**Vendored code.** `CMSIS_DAP/` contains ARM's CMSIS-DAP sources plus the CMSIS-Driver USART headers. Don't edit it. Customise through `include/DAP_config.h` or by overriding its `__WEAK` functions. `freertos/` is a git submodule.

## Dual-probe variant (`DUAL_PROBE_PICO`)

- **Two copies of the probe code.** Probe A is built from the normal sources and `PROBE_*` macros in `include/board_pico_dual_probe_config.h`. Probe B builds the same unmodified sources again, via the `src/dual/*_b.c` files. Each of those includes `src/dual/probe_b.h`, which:
  - remaps `PROBE_*` to `PROBE_B_*`;
  - remaps `pio0` to probe B's PIO, because `probe.c` and `probe.pio` hardcode `pio0`;
  - appends `_b` to every externally visible symbol.
- **Keep `probe_b.h` in sync.** A new non-static symbol in `probe.c`, `sw_dp_pio.c`, `tusb_edpt_handler.c`, `DAP.c` or `SWO.c` needs a matching rename in `probe_b.h`. If it's missing, the link fails with a duplicate or undefined symbol.
- **Per-probe state** in project-owned code (`cdc_uart.c`, `autobaud.c`, `swo_uart.c`, `target_vsense.c`) is stored in arrays sized `PROBE_COUNT`. The index is the probe number, which is also the CDC interface number: 0 = A, 1 = B. In `main.c`, the `probes[]` table holds each probe's DAP thread and core affinity: A runs on core 1, B on core 0.
- **`src/dual/dual_probe.c` contains the dual-only glue:**
  - It registers both DAP class drivers (the registration in `tusb_edpt_handler.c` is compiled out in this variant). Each driver claims only interface `PROBE_DAP_ITF` (0 for A, 3 for B); `usb_descriptors.c` statically asserts these numbers.
  - It implements DAP vendor command `0x80`, which returns the target voltage in mV as a little-endian uint16.
  - It claims the SWD state machines, because `probe.c` never does.
- **SWO.** ARM's `SWO.c` captures through the CMSIS drivers `Driver_USART0` and `Driver_USART1` (selected by `USART_PORT`). These are implemented in `src/swo_uart.c` with a PIO 8N1 receiver (`src/swo_uart.pio`) and DMA straight into `SWO.c`'s buffer. Each DMA completion IRQ is enabled only on the core running that probe's DAP thread, because `SWO.c` assumes its callback interrupts that thread.
- **Limits.**
  - Each PIO block has 32 instruction slots. A probe's SWD (11), autobaud (8) and SWO (8) programs all share its block.
  - UART0 is probe B's target UART, so dual builds disable the stdio UART and debug logging has no output.

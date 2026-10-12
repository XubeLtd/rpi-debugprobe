cmake_minimum_required(VERSION 3.12)

option(DUAL_PROBE_PICO "Run two independent probes on a Raspberry Pi Pico (needs PICO_BOARD=pico)" OFF)

# build for the Raspberry Pi Debug Probe by default
set(PICO_BOARD debug_probe CACHE STRING "Board type")

set(CMAKE_BUILD_TYPE RelWithDebInfo)

include(pico_sdk_import.cmake)

set(FREERTOS_KERNEL_PATH ${CMAKE_CURRENT_LIST_DIR}/freertos)
include(FreeRTOS_Kernel_import.cmake)

project(debugprobe)

pico_sdk_init()

add_executable(debugprobe
        src/probe_config.c
        src/main.c
        src/usb_descriptors.c
        src/probe.c
        src/cdc_uart.c
        src/get_serial.c
        src/sw_dp_pio.c
        src/tusb_edpt_handler.c
        src/autobaud.c
)

target_sources(debugprobe PRIVATE
        CMSIS_DAP/CMSIS/DAP/Firmware/Source/DAP.c
        CMSIS_DAP/CMSIS/DAP/Firmware/Source/JTAG_DP.c
        CMSIS_DAP/CMSIS/DAP/Firmware/Source/SWO.c
        #CMSIS_DAP/CMSIS/DAP/Firmware/Source/SW_DP.c
        )

if (DUAL_PROBE_PICO)
    # Probe A is the regular build above; src/dual builds probe B's copy
    target_sources(debugprobe PRIVATE
            src/dual/dual_probe.c
            src/dual/probe_b.c
            src/dual/sw_dp_pio_b.c
            src/dual/tusb_edpt_handler_b.c
            src/dual/dap_b.c
            src/dual/swo_b.c
            src/swo_uart.c
            src/target_vsense.c
            )
    target_include_directories(debugprobe PRIVATE
            CMSIS_DAP/CMSIS/Driver/Include/
            src/dual/
            )
    target_compile_definitions(debugprobe PRIVATE DUAL_PROBE_PICO=1)
    target_link_libraries(debugprobe PRIVATE hardware_adc)
    pico_generate_pio_header(debugprobe ${CMAKE_CURRENT_LIST_DIR}/src/swo_uart.pio)
    # UART0 is probe B's target UART, so it can't be the stdio console
    pico_enable_stdio_uart(debugprobe 0)
else ()
    # The dual probe implements the vendor commands in src/dual/dual_probe.c
    target_sources(debugprobe PRIVATE CMSIS_DAP/CMSIS/DAP/Firmware/Source/DAP_vendor.c)
endif ()

target_include_directories(debugprobe PRIVATE
        CMSIS_DAP/CMSIS/DAP/Firmware/Include/
        CMSIS_DAP/CMSIS/Core/Include/
        include/
        )

target_compile_options(debugprobe PRIVATE -Wall)

pico_generate_pio_header(debugprobe ${CMAKE_CURRENT_LIST_DIR}/src/probe.pio)
pico_generate_pio_header(debugprobe ${CMAKE_CURRENT_LIST_DIR}/src/probe_oen.pio)
pico_generate_pio_header(debugprobe ${CMAKE_CURRENT_LIST_DIR}/src/autobaud.pio)

target_include_directories(debugprobe PRIVATE src)

# add version
add_custom_target(version
        ${CMAKE_SOURCE_DIR}/get-version.sh
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_include_directories(debugprobe PRIVATE
        ${CMAKE_BINARY_DIR}/generated
        )

add_dependencies(debugprobe version)

target_compile_definitions (debugprobe PRIVATE
	PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1
    PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE=1
)

if (DUAL_PROBE_PICO)
    set_target_properties(debugprobe PROPERTIES
        OUTPUT_NAME "debugprobe_dual_on_${PICO_BOARD}"
    )
elseif (NOT PICO_BOARD STREQUAL "debug_probe")
    set_target_properties(debugprobe PROPERTIES
        OUTPUT_NAME "debugprobe_on_${PICO_BOARD}"
    )
endif ()

target_link_libraries(debugprobe PRIVATE
        pico_multicore
        pico_stdlib
        pico_usb_reset
        pico_unique_id
        tinyusb_device
        tinyusb_board
        hardware_pio
        hardware_dma
        hardware_irq
        hardware_clocks
        FreeRTOS-Kernel
        FreeRTOS-Kernel-Heap4
)

pico_set_binary_type(debugprobe copy_to_ram)

pico_add_extra_outputs(debugprobe)

# SPDX-FileCopyrightText: 2026 Alex Rattray
# SPDX-License-Identifier: Apache-2.0

# NimBLE headers contain unused inline transport/OS helpers.
if(APPLE)
  set(keyboard_link_options -Wl,-dead_strip)
else()
  set(keyboard_link_options -Wl,--gc-sections)
endif()

pbl_clar_test(test_keyboard_driver
  SOURCES
    src/bluetooth-fw/nimble/keyboard.c
    # The real parser and its dependencies, so the driver reads advertisement bytes rather than a
    # constant. Unused parts of these files are dropped by the link options below.
    third_party/nimble/mynewt-nimble/nimble/host/src/ble_hs_adv.c
    third_party/nimble/mynewt-nimble/nimble/host/src/ble_uuid.c
    third_party/nimble/mynewt-nimble/porting/nimble/src/endian.c
  INCLUDES
    src/bluetooth-fw/nimble
    third_party/nimble/port/include/sf32lb52
    third_party/nimble/port/include
    third_party/nimble/mynewt-nimble/nimble/include
    third_party/nimble/mynewt-nimble/nimble/host/include
    third_party/nimble/mynewt-nimble/porting/nimble/include
    third_party/nimble/mynewt-nimble/nimble/transport/include
  # The NimBLE pool sizes the firmware compiles with when the option is on.
  DEFINES CONFIG_BT_KEYBOARD CONFIG_BT_LOG_LEVEL=0 MYNEWT_VAL_BLE_MAX_CONNECTIONS=2
          MYNEWT_VAL_BLE_SM_MAX_PROCS=2 MYNEWT_VAL_BLE_STORE_MAX_BONDS=4
  LINK_OPTIONS ${keyboard_link_options}
)

pbl_clar_test(test_keyboard_store
  SOURCES
    src/bluetooth-fw/nimble/keyboard_store.c
  INCLUDES
    src/bluetooth-fw/nimble
    third_party/nimble/port/include/sf32lb52
    third_party/nimble/port/include
    third_party/nimble/mynewt-nimble/nimble/include
    third_party/nimble/mynewt-nimble/nimble/host/include
    third_party/nimble/mynewt-nimble/nimble/transport/include
    third_party/nimble/mynewt-nimble/porting/nimble/include
  DEFINES CONFIG_BT_KEYBOARD MYNEWT_VAL_BLE_MAX_CONNECTIONS=2 MYNEWT_VAL_BLE_SM_MAX_PROCS=2
          MYNEWT_VAL_BLE_STORE_MAX_BONDS=4
  LINK_OPTIONS ${keyboard_link_options}
)

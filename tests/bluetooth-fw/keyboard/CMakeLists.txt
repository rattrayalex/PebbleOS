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
  INCLUDES
    src/bluetooth-fw/nimble
    third_party/nimble/port/include/sf32lb52
    third_party/nimble/port/include
    third_party/nimble/mynewt-nimble/nimble/include
    third_party/nimble/mynewt-nimble/nimble/host/include
    third_party/nimble/mynewt-nimble/porting/nimble/include
    third_party/nimble/mynewt-nimble/nimble/transport/include
  DEFINES CONFIG_BT_KEYBOARD CONFIG_BT_LOG_LEVEL=0
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
  DEFINES CONFIG_BT_KEYBOARD
  LINK_OPTIONS ${keyboard_link_options}
)

# Bluetooth keyboard support

`CONFIG_BT_KEYBOARD` enables an experimental Bluetooth Low Energy keyboard
host. It is off by default and unavailable in recovery firmware. Existing
applications receive normal Pebble button events without SDK changes.

## Compatibility

The keyboard must support HID over GATT, Boot Keyboard Input Report,
Protocol Mode, and authenticated LE Secure Connections pairing. The watch
displays a six-digit passkey for entry on the keyboard. Bluetooth Classic,
legacy pairing, Just Works pairing, report-only keyboards, text entry, and
media keys are not supported.

The host accepts only the eight-byte boot keyboard report after selecting
Boot Protocol Mode. An arbitrary eight-byte Report characteristic is not
assumed to contain keyboard keys.

## Build and use

Configure the normal firmware for your board with the option enabled, then
build using the standard {doc}`building_fw` instructions. For example:

```shell
pbl configure --board getafix@dvt2 -DCONFIG_BT_KEYBOARD=y
pbl build
```

1. Put only the intended keyboard into pairing mode.
2. Open **Settings → Keyboard → Pair keyboard** on the watch.
3. Type the displayed passkey on the keyboard, then press Enter.
4. Use **Disconnect**, **Connect**, or **Forget** from the same screen.

Pairing selects the first connectable advertiser identifying itself as a
keyboard or HID device. If another device is selected, cancel and retry
with that device out of pairing mode. Only the bonded keyboard is eligible
for reconnect; unknown devices are never paired in the background.

Background reconnect uses ten-second connection attempts with a sixty-second
pause between attempts. A sleeping keyboard can therefore take over a minute
to reconnect. **Connect** starts an immediate attempt; **Disconnect** stops
background attempts until Connect or the next Bluetooth restart.

| Keyboard key | Pebble button |
| --- | --- |
| Up arrow | Up |
| Down arrow | Down |
| Enter or Right arrow | Select |
| Escape, Left arrow, or Backspace | Back |

Holding a key preserves the button hold, including each application's
existing repeat and long-click behavior. Aliases for the same button share
one hold. Disconnecting, forgetting, or receiving an invalid report releases
keyboard holds. A physical button still held down remains held after a
keyboard release.

The Enter key used to confirm pairing is ignored until it is released, so
pairing cannot immediately activate the selected menu action.

## Implementation

The NimBLE driver owns a separate central connection and bond record for
one keyboard. Keyboard events never enter the phone gateway's connection
and service-discovery handlers. The option reserves a second connection
without relaxing the existing Secure Connections pairing policy. It also
reserves one accessory bond in addition to the three phone bonds. The nRF52
controller reserves five resolving-list entries, including the local IRK;
external controller capacity still requires hardware validation.

The input module translates boot reports into internal keyboard button
events. KernelMain merges those events with other button events before
the shell and application subscribers receive them. The physical reset
button combination remains handled by the hardware button driver.

## Validation

Host tests cover navigation mappings, chords, aliases, duplicate reports,
invalid report lengths, HID error reports, disconnect releases, and
overlapping keyboard and physical button holds. Bond-store tests cover
credential validation, persistence, invalid records, and write failures.
Driver tests cover pairing and reconnect, scan responses, notification gates,
cancellation, host resets, stopped-state cleanup, and failed-Forget rollback.

```shell
pbl test --no-images -R keyboard
```

Build and host-test results cannot establish radio interoperability or
battery impact. Before enabling this feature in released firmware, test:

- Pairing, rejecting unsupported pairing modes, and cancellation during
  scanning, connection, pairing, and service discovery.
- Keyboard navigation while the phone is connected, including notification
  delivery, app communication, and firmware transfer.
- Keyboard sleep/wake, loss of radio range, watch restart, Bluetooth
  off/on, and a keyboard whose private address rotates.
- Forgetting the keyboard while a key is held, then pairing a different
  keyboard without affecting the phone's bond.
- Rejection of mice, report-only keyboards, malformed reports, and peers
  with incomplete HID services.
- Round and rectangular Settings layouts, including six-digit passkeys
  with leading zeros.
- Controller capacity, heap/stack use, and battery drain with the keyboard
  connected and absent.

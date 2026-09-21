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
keyboard or HID device, either by the keyboard appearance value or by the
HID service identifier. A scan response counts only when a connectable
advertisement from the same address arrived earlier in the same scan; the
driver remembers up to sixteen distinct such addresses, so a repeatedly
advertising neighbour cannot displace the keyboard. Because the marker must
appear in whichever packet the driver acts on, a keyboard that advertises
its name but keeps the HID marker only in its scan response is selected by
that scan response, and the watch shows the advertised name only if that
packet carries one. If another device is selected, cancel and retry with
that device out of pairing mode. Only the bonded keyboard is eligible for
reconnect; unknown devices are never paired in the background.

Background reconnect uses ten-second connection attempts with a sixty-second
pause between attempts. A sleeping keyboard can therefore take over a minute
to reconnect. **Connect** starts an immediate attempt with a faster scan duty
cycle; **Disconnect** stops background attempts until Connect or the next
Bluetooth restart. Temporary connection/encryption transport failures retry
for the saved bond. Authentication failures and missing keys require explicit
user action.

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

Raising `BLE_MAX_CONNECTIONS` from one to two has one consequence worth
naming: both ports derive `BLE_L2CAP_MAX_CHANS` as
`3 * BLE_MAX_CONNECTIONS`, so the L2CAP channel pool doubles from three to
six. That pool is the largest single consumer of the extra RAM this option
costs. All three increases apply only when `CONFIG_BT_KEYBOARD` is set.

The input module translates boot reports into internal keyboard button
events. The Bluetooth host records the latest report and queues at most one
KernelMain callback to deliver it, so a peer notifying at the minimum
connection interval cannot fill the kernel event queue. Both edges survive that
coalescing: a key pressed and released between two deliveries still produces a
press and a release, and a held key released and taken again still produces a
release and a press. What the encoding cannot carry is order or repetition, so
events are emitted per button rather than in the order the peer sent them, and
three or more transitions of one button inside one delivery window collapse to
one edge. KernelMain merges the result with other button events before the
shell and application subscribers receive them. The physical reset button
combination remains handled by the hardware button driver.

Bond storage writes flash from the GATT subscribe callback. NimBLE invokes
GATT client callbacks with its host lock released
(`BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task())` in `ble_gattc_write_cb`),
so that write does not stall the phone link behind the host lock. It does
occupy the host task, which is why the stack measurement below matters.

## Validation

Host tests cover navigation mappings, chords, aliases, duplicate reports,
invalid report lengths, HID error reports, disconnect releases, and
overlapping keyboard and physical button holds. Bond-store tests cover
credential validation, persistence, invalid records, and write failures.
Driver tests cover pairing and reconnect, scan responses, notification gates,
cancellation, host resets, stopped-state cleanup, failed-Forget rollback,
transient encryption failures, peer ownership, gateway-bond isolation,
connection parameter bounds, and coalesced status events. Advertising tests
cover privacy-list preemption and deferred connection/shutdown transitions.
Native framebuffer tests cover Settings states on round and rectangular
screens, including passkeys with leading zeros and large text.

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
  connected and absent. Measure the 5,000-byte NimBLE host stack during
  cold boot, Bluetooth restart, pairing/save, Forget, and failed-Forget
  rollback; bond storage remains serialized on that task.

Record the exact keyboard model and firmware, watch board and commit, phone
model/OS, negotiated pairing mode, discovered HID characteristics, and traces
for each result. No physical keyboard has been validated yet. Native frames
validate rendering with synthetic statuses; they do not establish on-device
Bluetooth behavior.

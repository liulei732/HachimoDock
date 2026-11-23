# Foreground Bluetooth accessory panels

A WiFi6 device with the C6 radio and a working initiating role reports
`widgetBleAccessory=p4-ble-read-v1`. Both P4 chip revisions use the same
contract. Require the exact negotiated value before syncing an accessory panel.

Choose `"accessory":"battery-level"` or `"accessory":"ambient-temperature"`
in a `p4-bounded-runtime-v4` tool. Use a dashboard for the desktop preview,
a valid initial state, empty vars, transitions, tick and buttons. The device
draws a native panel. Its only inputs are local physical controls. See
[the battery example](../examples/ble-accessory-battery/runtime/widget.json)
and [the temperature example](../examples/ble-room-temperature/runtime/widget.json).

On the panel, move the stick left or right to select a hex digit or the
address type. Up or down edits the selection. SW1 approves one read from
that public or static-random LE address. SW1 during the request cancels it.
The panel displays a percentage from Battery Service 180F / Battery Level
2A19, or Celsius to two decimals from Environmental Sensing 181A /
Temperature 2A6E. The accessory must offer a connectable public read with
the default ATT MTU. Encrypted reads, changing private addresses and
notification-only characteristics have no delivery path in this profile.

A connection attempt lasts up to four seconds and the entire read has an
eight-second deadline. Keyboard mode, pairing, advertising, a keyboard
connection or the stored keyboard identity makes the radio unavailable.
Exiting or replacing the panel clears its target and measurement. Packages
cannot supply a peer, initiate a scan, access the address or measurement,
pair, write attributes, or retain readings. Pico and legacy firmware lack
the negotiated capability. P2 direct-community profile p4-local-v1 accepts
pure declarative tools and excludes accessory panels.

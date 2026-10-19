# Bluetooth panel

The Bluetooth rune immediately left of the network button opens adapter/device
status. Scan (or Enter/Space in the popup) performs a Classic Bluetooth inquiry
on each configured HCI controller, then refreshes its connected links and neighbor
cache. Addresses identify devices in this first version. Cached discovery is
explicitly labelled: a cached device is not necessarily still in range. The icon
turns blue after finding a usable controller. Status refreshes every 30 seconds
while the popup is open; inquiries run only on request.

The backend invokes `/usr/sbin/hccontrol -N` with fixed argument vectors, never a
shell. Commands run sequentially and asynchronously, with a 25-second limit per
command. There is no elevation from the desktop. Errors, unavailable controllers
and permission failures appear in the popup. Closing the desktop terminates its
own outstanding query, without stopping Bluetooth services.

Pairing and profile management are not implemented. In a root terminal,
`bluetooth-config scan` provides MidnightBSD's existing interactive controller,
PIN pairing and HID setup. A future graphical pairing implementation needs a
narrow authenticated helper; the window manager must not run as root. Bluetooth
audio and BLE peripherals need separate profile/platform validation.

## Validation on another MidnightBSD system

Build normally and run `ctest --test-dir build -R bluetooth --output-on-failure`.
`python3 tests/bluetooth_smoke.py` exercises the popup in an isolated Xvfb session
with temporary HOME/XDG directories. It does not stop an existing desktop.

On hardware with a configured controller, open the popup and compare adapter
names/addresses with `hccontrol read_node_list` and
`hccontrol -n ubt0hci read_connection_list`. Put a Classic Bluetooth device in
inquiry mode, press Scan and compare with the neighbor cache. Check permission
errors as an ordinary user, multiple adapters, adapter removal during inquiry,
and Escape/outside-click dismissal while scanning. Confirm that panel input
remains responsive. Pair a supported keyboard/mouse using the system tool, then
verify its link appears; this panel does not initiate a profile connection.

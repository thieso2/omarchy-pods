---
type: reference
title: A BlueZ advertisement monitor replaces the discovery scan
description: Unfiltered LE discovery floods the system bus with every nearby device's RSSI; a pattern monitor on 4C 00 07 delivers the same frames with the bus silent, but only with Experimental = true in main.conf
tags: [bluez, ble, advertisement-monitor, performance]
status: stable
verified:
  - by: PSI, top and dbus-monitor samples on Arch, BlueZ 5.87, kernel 7.1.9, in a BLE-dense environment
    at: 2026-08-23
---

# Why the daemon must not run a discovery scan

The daemon's original BLE path held a continuous `QBluetoothDeviceDiscoveryAgent`
LE discovery whenever the pods were disconnected. Discovery makes bluetoothd
broadcast a `PropertiesChanged` for every advertisement of every device in
range, to every BlueZ subscriber on the system bus. Measured on 2026-08-23 with
dozens of BLE devices around: wireplumber alone at 72% CPU, quickshell at 36%,
upowerd, NetworkManager, bluetoothd and dbus-broker at 11–18% each, CPU PSI
`some avg10` around 20% and a load of 8 on 8 cores. The daemon itself sat at
14% — the cost lands on the subscribers, so capping the daemon's CPU does not
help.

# What replaces it

`org.bluez.AdvertisementMonitor1` (`ble/advmonitor.cpp`) registers one
`or_patterns` monitor: AD type `0xFF` (manufacturer data), offset 0, content
`4C 00 07` — Apple's company id little-endian plus the proximity-pairing
message type, so nothing else matches. BlueZ then scans passively and only
matching frames create device objects and bus traffic; frames arrive through
`DeviceFound` plus `ManufacturerData` property updates and funnel into the
same parser the discovery path used. With the monitor active,
`bluetoothctl show` reports `Discovering: no` and a 30-second `dbus-monitor`
of the whole `/org/bluez` namespace caught **one** signal. CPU PSI fell to
0.02%.

# The switch that gates it

BlueZ 5.87 does not expose `org.bluez.AdvertisementMonitorManager1` on the
adapter until `/etc/bluetooth/main.conf` sets `Experimental = true` (set on
this box on 2026-08-23). Without it the daemon logs
`AdvMonitor: no adapter offers org.bluez.AdvertisementMonitorManager1` and
falls back to the old discovery scan — the storm returns but battery still
works, so the failure is soft.

# Not yet observed

A live proximity-pairing frame arriving end-to-end through the monitor: the
pods were idle in a closed case during verification and AirPods stop
advertising after roughly fifteen minutes of inactivity. The pattern bytes
match the frame captured in `tests/tst_blemanager.cpp`
(`071901272021...` keyed under `0x004C`), so the filter admits exactly the
frames the parser consumed under discovery.

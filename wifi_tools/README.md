# WiFi Tools

A Flipper Zero external application that uses the **WiFi Devboard (ESP32-S2)** to scan nearby WiFi
networks and flag security issues such as open or WEP-encrypted access points.

## Features

| Feature | Description |
|---------|-------------|
| **Network Scan** | Lists nearby SSIDs with BSSID, channel, RSSI, and encryption type |
| **Weak-encryption highlight** | Marks open (`Open`) networks clearly in the scan results |

## Hardware Requirements

* Flipper Zero (any revision)
* [WiFi Devboard](https://shop.flipperzero.one/products/wifi-devboard) (ESP32-S2)
  — the devboard must be flashed with the official **Blackmagic / WiFi Marauder** firmware or any
  firmware that exposes the standard **ESP-AT** command interface.

## Build & Install

```sh
# Standalone (uFBT)
ufbt launch APP_DIR=wifi_tools

# Inside the full firmware tree
./fbt fap_wifi_tools
```

Copy the generated `.fap` file to `SD:/apps/GPIO/` on your Flipper.

## Usage

1. Attach the WiFi Devboard to the Flipper Zero GPIO header.
2. Open **Apps → GPIO → WiFi Tools**.
3. Select **Scan Networks** and wait (~5–8 s) for results.
4. Browse the list: networks marked `<!> OPEN` have no encryption.

## Legal Notice

Use only on networks you own or have explicit permission to test.

# WiFi Tools

A defensive WiFi security analysis application for Flipper Zero with the **WiFi Devboard (ESP32-S2)**.
Performs passive network analysis, security auditing, and channel monitoring to help you assess
your network's security posture.

## ⚠️ Legal Notice

**Use only on networks you own or have explicit written permission to test.**
This tool is designed for defensive security assessment and network monitoring.
Unauthorized testing of networks you do not own is illegal in most jurisdictions.

## Features

| Feature | Description |
|---------|-------------|
| **Network Scan** | Lists nearby SSIDs with BSSID, channel, RSSI, encryption type, and security risk level |
| **Security Audit** | Identifies networks with weak encryption (Open, WEP, WPA-only) with actionable recommendations |
| **Channel Analysis** | Visualizes 2.4 GHz channel congestion and recommends optimal channels for AP deployment |
| **Network Monitor** | Tracks known networks and alerts when new or missing networks are detected |
| **Signal Visualization** | ASCII signal strength bars for quick quality assessment |

### Security Risk Levels

The tool assesses each network's security posture:

| Level | Encryption Types | Description |
|-------|------------------|-------------|
| **CRITICAL** | Open, WEP | No encryption or broken encryption - all traffic visible |
| **HIGH** | WPA (no WPA2/3) | Vulnerable to modern attacks - upgrade recommended |
| **MEDIUM** | WPA/WPA2 mixed | Some clients may be vulnerable |
| **LOW** | WPA2, WPA3, WPA2-ENT | Good encryption - continue monitoring |

## Hardware Requirements

* Flipper Zero (any revision)
* [WiFi Devboard](https://shop.flipperzero.one/products/wifi-devboard) (ESP32-S2)
  - Must be flashed with firmware exposing the **ESP-AT** command interface
  - Compatible with official Blackmagic / WiFi Marauder firmware

## Build & Install

### Using uFBT (recommended)

```sh
ufbt launch APP_DIR=wifi_tools
```

### Using full FBT

```sh
# From the flipperzero-firmware root
./fbt fap_wifi_tools
```

### Installation

Copy the generated `.fap` file to `SD:/apps/GPIO/` on your Flipper Zero's SD card.

## Usage

### 1. Basic Network Scan

1. Attach the WiFi Devboard to the Flipper Zero GPIO header
2. Open **Apps → GPIO → WiFi Tools**
3. Select **Scan Networks**
4. Wait 5-8 seconds for results
5. Review network details including:
   - SSID and BSSID (MAC address)
   - Channel and signal strength (RSSI)
   - Encryption type and security risk level

### 2. Security Audit

1. Select **Security Audit** from the menu
2. The tool scans and generates a security report
3. Networks with CRITICAL or HIGH risk are highlighted
4. Actionable recommendations are provided for each vulnerable network

**Example output:**
```
SECURITY AUDIT REPORT
═══════════════════════

SUMMARY
  Critical: 1  High: 2  Medium: 3  Low: 5

⚠️  SECURITY CONCERNS DETECTED
═══════════════════════════════

[CRITICAL] GuestNetwork
  Encryption: Open
  Risk Level: CRITICAL
  <!> No encryption or WEP - All traffic visible
      Immediate action required!

[HIGH] OldRouter
  Encryption: WPA
  Risk Level: HIGH
  [!] WPA-only (no WPA2/3) - Vulnerable to attacks
      Upgrade to WPA2/WPA3 if possible
```

### 3. Channel Analysis

1. Select **Channel Analysis** from the menu
2. View channel congestion visualization for channels 1-13
3. Identify the least congested channel for your AP
4. Non-overlapping channels (1, 6, 11) are marked

**Example output:**
```
CHANNEL ANALYSIS
═══════════════════════════════

Channel Usage (2.4 GHz):

CH  1: ████ (4) ← Non-overlapping
CH  2: ██ (2)
CH  3: █ (1)
CH  4: 
CH  5: 
CH  6: ██████ (6) ← Non-overlapping
CH  7: ███ (3)
...

Best channels for AP setup: 4 (least congested)
Recommended: 1, 6, or 11
```

### 4. Network Monitor

1. Select **Network Monitor** from the menu
2. First scan establishes your baseline of "known" networks
3. Subsequent scans detect:
   - **New networks** (previously unseen BSSIDs)
   - **Missing networks** (known networks no longer present)
4. Notification alert sounds when changes are detected

**Use cases:**
- Detect unauthorized access points in your environment
- Monitor for rogue devices or neighbors' new networks
- Track when your own networks go offline

## Signal Strength Reference

| RSSI (dBm) | Quality | Description |
|------------|---------|-------------|
| -30 to -50 | Excellent | Very close to AP |
| -50 to -60 | Good | Reliable connection |
| -60 to -70 | Fair | Usable, may have issues |
| -70 to -80 | Weak | Unreliable, packet loss |
| -80 to -90 | Poor | Barely detectable |

## Troubleshooting

### "WiFi Devboard not detected"
- Ensure the devboard is properly connected to the GPIO header
- Check that the devboard has power (LED should be on)
- Verify the devboard firmware is running and responsive

### "Scan timed out"
- The ESP32 may be busy or in the wrong mode
- Try power-cycling the devboard
- Ensure the firmware supports ESP-AT commands

### No networks found
- Move to a location with better WiFi coverage
- Check that the devboard antenna is not obstructed
- Verify the ESP32 firmware is functioning correctly

## Technical Details

### ESP-AT Command Interface

This tool communicates with the ESP32 using standard AT commands:

```
AT              → Ping devboard
AT+CWMODE=1     → Set station mode
AT+CWLAP        → List access points
```

Response format:
```
+CWLAP:(<enc>,<ssid>,<rssi>,<bssid>,<channel>)
```

### Encryption Type Mapping

| ESP-AT Code | Encryption | Risk Level |
|-------------|------------|------------|
| 0 | Open | CRITICAL |
| 1 | WEP | CRITICAL |
| 2 | WPA | HIGH |
| 3 | WPA2 | LOW |
| 4 | WPA/WPA2 | MEDIUM |
| 5 | WPA3 | LOW |
| 6 | WPA2-ENT | LOW |
| 7 | WPA3-ENT | LOW |
| 8 | WAPI | MEDIUM |

## Security Best Practices

Based on current research (2024-2025):

1. **Use WPA3** where supported, with WPA2/WPA3 transition mode for compatibility
2. **Disable WPS** - it's fundamentally insecure and easily compromised
3. **Use strong passphrases** - minimum 20 characters, random
4. **Enable 802.11w** (Protected Management Frames) where available
5. **Segment networks** - keep IoT devices on separate VLANs
6. **Regular audits** - scan your environment monthly
7. **Monitor for changes** - use Network Monitor to detect new devices

## License

MIT License - see LICENSE file

## Contributing

Contributions welcome! Please focus on **defensive security features** only.
Offensive capabilities (deauthentication, packet injection, handshake capture)
will not be accepted.

## Acknowledgments

- ESP-AT firmware team for the command interface
- Flipper Zero community for hardware support
- Security researchers who advance WiFi security

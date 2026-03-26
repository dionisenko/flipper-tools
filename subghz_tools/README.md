# Sub-GHz Tools

A Flipper Zero external application that uses the **built-in CC1101 sub-GHz radio** to scan,
capture, and replay signals from garage-door openers, keyfobs, and other remote-control devices.

## Features

| Feature | Description |
|---------|-------------|
| **Scan Frequencies** | Sweeps 13 common remote-control frequencies (300–915 MHz) and reports active ones with RSSI |
| **Capture Signal** | Listens on the strongest detected frequency and records the raw OOK burst |
| **Replay Signal** | Retransmits the last captured burst (for authorised testing only) |

## Hardware Requirements

* Flipper Zero (any revision) — the CC1101 radio is built in, no extra hardware needed.

## Build & Install

```sh
# Standalone (uFBT)
ufbt launch APP_DIR=subghz_tools

# Inside the full firmware tree
./fbt fap_subghz_tools
```

Copy the generated `.fap` file to `SD:/apps/Sub-GHz/` on your Flipper.

## Usage

### Scan Frequencies

1. Open **Apps → Sub-GHz → Sub-GHz Tools**.
2. Select **Scan Frequencies**.
3. The app dwells ~300 ms on each preset frequency and lists those where RSSI > −90 dBm.

### Capture Signal

1. From the main menu select **Capture Signal**.
2. Press the button on the remote you want to capture — the app will record the burst.
3. The display shows frequency, RSSI, byte count, and a hex preview.

### Replay Signal

1. After a successful capture select **Replay Signal**.
2. The last captured burst is retransmitted 3 times on the original frequency.

## Frequency Reference

| Frequency | Region / Use |
|-----------|-------------|
| 315.000 MHz | North America — older garage doors |
| 390.000 MHz | North America — some keyfobs |
| 433.920 MHz | Europe / worldwide — most modern keyfobs |
| 868.350 MHz | Europe — home automation (Z-Wave, etc.) |
| 915.000 MHz | North America — ISM band devices |

## Legal Notice

Capturing and replaying radio signals may be restricted by local law.
Use only on equipment you own or have explicit authorisation to test.

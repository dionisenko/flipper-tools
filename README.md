# flipper-tools

Flipper Zero security research tools — a pet-project for testing common authentication methods and wireless technologies.

## Applications

| App | Description |
|-----|-------------|
| [`wifi_tools`](wifi_tools/README.md) | WiFi network scanner and security analyser (requires WiFi Devboard) |
| [`subghz_tools`](subghz_tools/README.md) | Sub-GHz signal capture/replay for garage-door remotes and similar devices |

## Requirements

* [Flipper Zero](https://flipperzero.one/) device running **official firmware ≥ 0.87** (or compatible
  [Unleashed](https://github.com/DarkFlippers/unleashed-firmware) / [RogueMaster](https://github.com/RogueMaster/flipperzero-firmware-wPlugins) builds).
* [WiFi Devboard](https://shop.flipperzero.one/products/wifi-devboard) for `wifi_tools` (ESP32-S2 module).
* Sub-GHz radio is built into every Flipper Zero — no extra hardware needed for `subghz_tools`.

## Building

These are **external Flipper Zero applications** (`.fap` files).  
Build them with the [Flipper Build Tool (FBT)](https://github.com/flipperdevices/flipperzero-firmware/blob/dev/documentation/fbt.md):

```sh
# inside the flipper-zero firmware tree
./fbt fap_wifi_tools fap_subghz_tools
```

Or use [uFBT](https://github.com/flipperdevices/flipperzero-ufbt) for standalone builds without a full firmware checkout:

```sh
ufbt            # builds all .fap files in the current directory
ufbt launch     # builds and deploys to a connected Flipper via USB
```

## Legal & Ethical Notice

These tools are intended **exclusively for authorised security research and educational purposes**.  
Always obtain explicit permission before testing systems you do not own.  
The authors accept no responsibility for misuse.

## License

GNU General Public License v3 — see [LICENSE](LICENSE).

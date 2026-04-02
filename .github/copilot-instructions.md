# flipper-tools — Copilot Instructions

## Toolkit Source

This repository uses the local dev toolkit at `../dev` as the source of truth for reusable workflow assets.

- Reusable agents: `.github/agents` -> `../../dev/common/agents`
- Reusable skills: `.github/skills` -> `../../dev/common/skills`
- Reusable prompts: `.github/prompts` -> `../../dev/common/prompts`

Do not duplicate reusable workflow behavior in this repo when it already belongs in the toolkit.

## Project Overview

Flipper Zero security research tools — external FAP applications written in C for the Flipper Zero device.

## Stack and Architecture

- **Language**: C (C11), targeting the Flipper Zero embedded runtime
- **Build system**: uFBT (standalone, no firmware checkout) or FBT (inside the Flipper firmware tree)
- **SDK**: Flipper Zero Firmware SDK — `furi`, `furi_hal`, `gui`, `notification`, `lib/subghz`
- **Firmware target**: official firmware ≥ 0.87 or compatible Unleashed / RogueMaster builds

### Module layout

| Directory | App ID | Description |
|-----------|--------|-------------|
| `subghz_tools/` | `subghz_tools` | CC1101 Sub-GHz scan, capture, and replay |
| `wifi_tools/` | `wifi_tools` | ESP32-S2 WiFi devboard scanner via UART AT commands |

Each module is a self-contained Flipper external application:
- `application.fam` — app manifest (appid, name, requires, stack size, category)
- `*.c` — single-file application entry point

## Build Commands

```sh
# Standalone (preferred for development)
ufbt              # builds all .fap files
ufbt launch APP_DIR=subghz_tools   # build + deploy to connected Flipper
ufbt launch APP_DIR=wifi_tools

# Inside the Flipper firmware tree
./fbt fap_subghz_tools fap_wifi_tools
```

## Code Conventions

- Follow existing C style: snake_case, `typedef struct/enum`, explicit `#define` constants
- GUI follows the `ViewDispatcher` + `Submenu` / `TextBox` / `Popup` / `Widget` pattern
- RTOS-safe allocation: `malloc` guarded, `furi_assert` on critical alloc failures
- Keep sub-GHz and UART resource acquisition strictly in the app's open/close lifecycle
- Do not hold the CC1101 radio or UART channel across view transitions
- Stack size is constrained (4 KB for subghz_tools) — avoid large on-stack buffers

## Key APIs

| API | Purpose |
|-----|---------|
| `furi_hal_subghz_*` | CC1101 radio control (frequency, modulation, Rx/Tx, RSSI) |
| `furi_hal_uart_*` | UART for ESP32-S2 WiFi devboard |
| `SubGhzTxRxWorker` | Background Rx/Tx thread |
| `ViewDispatcher` | Scene/view routing |
| `NotificationApp` | LED, vibro, sound notifications |

## Risky Areas

- **Sub-GHz transmit**: replay must only be exercised on devices the developer owns and in RF-shielded environments; the Flipper firmware enforces regional frequency locks but application code still gates replay behind explicit user confirmation
- **UART resource conflict**: other apps may hold `FuriHalUartIdLP`; always check for prior ownership and gracefully handle failure to acquire
- **Stack overflow**: the 4 KB stack is easy to exceed with nested function calls or large local arrays; prefer heap allocation for buffers

## Workflow

For non-trivial tasks:

1. Start with `../dev/INDEX.md`, then read only the one matching subtree index.
2. Use `/Parallelize Task` to let `PM Orchestrator` route to the cheapest sufficient agent.
3. Load only the exact toolkit assets needed for the task.
4. Write reusable findings back into `../dev` when they generalize.

Default routing:

1. `repo-research-cheap` for discovery and context gathering
2. `fast-implementer` for bounded C changes within an existing module
3. `architecture-reviewer` for new module design, API changes, or cross-module concerns

## Legal Constraint

All code is for authorised security research and educational use only.

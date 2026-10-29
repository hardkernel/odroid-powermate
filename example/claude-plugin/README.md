# PowerMate Claude Code plugin

A [Claude Code](https://code.claude.com) plugin that lets Claude operate a
single-board computer (SBC) wired to an ODROID PowerMate:

- Switch the MAIN (VOUT, the switched 9-21 V DC input) and USB (5 V) outputs,
  or pulse the Power/Reset pins of the J4 header (boards with power pins:
  ODROID-M1, M1S, M2, H4)
- Read, wait on, and type into the SBC's UART console: U-Boot, Linux, GRUB,
  EDK2, installers
- Capture complete boot logs, and stop at the bootloader prompt, including
  Rockchip U-Boot with a zero boot delay
- Run a command at a prompt and get back only its output, with the exit code
  for POSIX shells
- Record per-channel power, energy, and CSV samples
- Read PowerMate events such as over-current cutoffs
- Configure the PowerMate itself, from its first network connection to a ready
  test bench: Wi-Fi, IP addressing, AP/STA mode, current limits, UART baud rate,
  output restore, and web credentials. Each change is previewed with its risks
  first

It has two parts:

| Part | Path | Role |
|---|---|---|
| MCP server | `mcp-server/` | Go stdio server. It keeps the PowerMate `/ws` and `/uart` WebSockets open, so console output and power samples are buffered between tool calls. Pattern waits and bootloader interrupts happen inside the server, with no model round trip. |
| Skill | `skills/sbc-remote/SKILL.md` | Procedures and safety rules: boot capture, bootloader entry, login, clean shutdown, hang recovery, power measurement, wiring checks, and PowerMate setup with preview-then-confirm. |

## Requirements

- Go 1.26 or later. `mcp-server/run.sh` builds the server on first start, and
  rebuilds it when the sources change.
- Network access to the PowerMate
- The SBC's debug UART and DC input connected to the PowerMate, with the
  PowerMate UART baud rate matching the SBC (for example 1500000 for
  ODROID-M2)

## Configure

The server reads these environment variables. `.mcp.json` passes them through
from the shell that starts Claude Code:

| Variable | Default | |
|---|---|---|
| `POWERMATE_HOST` | saved address, else `192.168.4.1` | PowerMate address. When unset, the server uses the address saved by `powermate_connect`, then the PowerMate's setup AP address. |
| `POWERMATE_USERNAME` | `admin` | PowerMate web login |
| `POWERMATE_PASSWORD` | `password` | PowerMate web login |

```sh
export POWERMATE_HOST=192.168.30.4
```

Login tokens, the saved PowerMate address, and capture logs are stored in the
plugin data directory (`${CLAUDE_PLUGIN_DATA}`), or in `~/.cache/powermate-mcp`
when that variable is not provided. The token is reused across sessions, because the
firmware keeps only four tokens and evicts the oldest. Without this, every new
session would log the web UI out.

## Install

The repository root is a plugin marketplace (`.claude-plugin/marketplace.json`).
In Claude Code:

```
/plugin marketplace add hardkernel/odroid-powermate
/plugin install powermate@odroid-powermate
```

From a local checkout:

```sh
claude plugin marketplace add /path/to/odroid-powermate
claude plugin install powermate@odroid-powermate
```

To try it for one session without installing:

```sh
claude --plugin-dir example/claude-plugin
```

## Use

Check `/mcp` for the `powermate` server, then ask, for example:

- "Reboot the board and show me where the boot log stops."
- "Stop in U-Boot and print the boot environment."
- "Log in and check the kernel version." Claude asks for the
  password.
- "Measure idle power for 30 seconds, then under `stress-ng` for 60 seconds."
- "I joined this PowerMate to the lab Wi-Fi from my phone, and it is at
  192.168.30.4. Get the ODROID-M2 on it ready for testing." Claude sets the baud
  rate, measures the board, proposes current limits, and confirms each risky
  step.

The skill is also available as `/powermate:sbc-remote`.

## Tools

Tool names are prefixed `mcp__plugin_powermate_powermate__`.

| Tool | Kind | Purpose |
|---|---|---|
| `sbc_status` | read | Outputs, power, stream health, console activity, recent events |
| `sbc_power` | **destructive** | `on`, `off`, `cycle` an output; `press_power`, `press_reset` |
| `sbc_console_read` | read | Cleaned console text since a cursor, optional `grep` |
| `sbc_console_send` | write | Type text and keys (`ctrl-c`, `esc`, `f2`, arrows, ...) |
| `sbc_console_wait` | read | Block until a regex matches the console |
| `sbc_console_run` | write | Run one command at a prompt and return its output (and exit code) |
| `sbc_boot_capture` | **destructive** | Power cycle/on/reset, capture the log until a pattern, optionally interrupt the bootloader |
| `sbc_power_record` | read | Power summary over a window, optional CSV |
| `sbc_events` | read | PowerMate events |
| `powermate_settings` | read | PowerMate configuration, address in use, diagnostics |
| `powermate_connect` | config | Switch the tools to another PowerMate address and optionally remember it |
| `powermate_wifi_scan` | read | Networks visible to the PowerMate |
| `powermate_wifi_connect` | **risky** | Join a Wi-Fi network (STA) |
| `powermate_network` | **risky** | DHCP or static IP |
| `powermate_wifi_mode` | **risky** | `sta` or `apsta`, AP name and password |
| `powermate_current_limits` | **risky** | Warning and critical limits per channel |
| `powermate_device_options` | **risky** | UART baud rate, sensor period, output-state restore |
| `powermate_credentials` | **risky** | Web login |
| `powermate_reboot` | **risky** | Reboot the PowerMate |

The skill pre-approves only the read tools. Power, console-input and
configuration tools still ask for permission.

### Risky configuration changes

Every **risky** `powermate_*` tool is a two-step call. Without `confirm=true` it
changes nothing. It returns the request (secrets masked), the current values,
and a list of risks with the recovery path, and the skill tells Claude to relay
them and wait for the user's agreement. The tools also refuse changes that are
known to lock the PowerMate out or break it:

- Static IPs that are not in the gateway's subnet, network/broadcast addresses,
  and invalid netmasks
- `apsta` without an AP password, unless an open AP is chosen explicitly (the
  firmware would silently open the AP)
- `sta` while the STA link is down, or while the tools reach the PowerMate
  through its AP
- Current limits outside the firmware ranges, or a warning limit at or above its
  critical limit. The preview also flags limits close to the peak current
  measured so far.
- Baud rates the firmware does not support. Web credentials that would overflow
  the firmware's 100-byte login buffer.

The plugin does not use the PowerMate's USB port, which is meant for debugging.
If the PowerMate becomes unreachable, the skill points the user to the router's
DHCP list, to the setup AP at 192.168.4.1 (apsta mode only), or to the USB debug
console (`idf.py monitor` or minicom: `wifi_status`, `wifi_connect <ssid>
<password>`). The last resort is a factory reset: hold the button for 5 s, which
erases all settings and brings back the setup AP for phone setup.

## Notes

- Console text has ANSI escape sequences removed and line endings normalised.
  The raw bytes of each boot capture are saved next to the cleaned log.
- When most of the console bytes are invalid, the tools add a line-noise
  warning. With PowerMate this usually means a loose or missing UART wire, or
  an SBC that does not share GND with the PowerMate output. Out-of-range or
  unstable sensor readings get a similar warning.
- UART writes are split into 1 KiB frames, because the firmware closes the
  WebSocket on frames larger than 2 KiB.
- The console buffer keeps the last 4 MiB, and power samples cover about the
  last 30 minutes.

## Develop

```sh
cd mcp-server
go test ./...
go build -o bin/powermate-mcp .
```

# PMTUI

`pmtui` is a terminal client for the PowerMate HTTP and WebSocket APIs.

The TUI provides:

- Live VIN, MAIN, and USB voltage/current/power
- Bounded btop-style voltage, current, and power history graphs
- MAIN and USB output control
- Confirmed Power and Reset actions
- PowerMate event viewing with a fixed 500-line history
- Runtime diagnostics, including Wi-Fi reconnect details, refreshed only while
  the Debug tab is active
- CSV recording with immediate flush and exclusive file creation
- A raw host-terminal UART session with keyboard and paste input
- UART screen clearing, bounded raw log saving, and baud-rate selection
- Automatic status/UART WebSocket reconnect and HTTP reauthentication

Wi-Fi provisioning and the complete settings editor are not included in this
first version.

## Requirements

- Go 1.26 or later
- Network access to the PowerMate HTTP server

## Run

From this directory:

```sh
go run . -host 192.168.4.1
```

The repository CMake project also provides a standalone `pmtui` target:

```sh
cmake --build build --target pmtui
```

Run this command from the repository root after configuring the normal
ESP-IDF build directory. The resulting host binary is written to
`build/pmtui`.

The program always starts with a TUI login screen. `-host` and `-id` set the
initial values shown on that screen:

```sh
go run . -host 192.168.4.1 -id admin
```

The password is accepted only by the masked field on the login screen. It
cannot be supplied as a command-line argument or environment variable.

Use `-uart` to open the raw UART terminal immediately after a successful
login:

```sh
go run . -host 192.168.4.1 -id admin -uart
```

## Keys

| Key | Action                          |
|-----|---------------------------------|
| `1` | Dashboard                       |
| `2` | Events                          |
| `3` | Runtime diagnostics             |
| `4` | UART stream                     |
| `m` | Toggle MAIN output              |
| `u` | Toggle USB output               |
| `p` | Power action, with confirmation |
| `x` | Reset action, with confirmation |
| `c` | Start or stop CSV recording     |
| `l` | Toggle graph layout              |
| `r` | Refresh the current page        |
| `q` | Quit                            |

The UART WebSocket is connected while the UART terminal or its `Ctrl+T` menu is
active.
Bubble Tea temporarily releases the terminal and the UART WebSocket is then
connected directly to the host terminal's raw stdin/stdout. UART data does not
pass through the Bubble Tea update or rendering loop.

Normal characters, control keys, cursor/function-key sequences, bracketed
paste, ANSI/VT output, target mouse reporting, native host scrollback, and
native drag/copy therefore behave as they do in a normal serial terminal.
Target applications such as `htop`, `vim`, and `vttest` control the outer
terminal directly.

Press `Ctrl+T` to leave the raw session and open the Bubble Tea UART menu for
output control, Power/Reset actions, changing the baud rate, clearing/saving
the local UART buffer, leaving the terminal, or quitting the program. Press
`Ctrl+T` again from the menu to reconnect and send a literal `Ctrl+T` (`0x14`)
to the target. Press `g` or `Esc` to reconnect without sending it.
Press `m` or `u` to toggle the corresponding output and immediately resume the
raw UART terminal.

While the menu is visible, incoming UART data is kept in a bounded pending
buffer and parsed by `github.com/charmbracelet/x/vt` as shadow state. Resuming
the raw session restores alternate-screen, cursor, mouse, and bracketed-paste
state before direct streaming continues. `x/vt` is not on the active UART
rendering path.

The raw UART transport still has no PTY `SIGWINCH` channel, so resizing the
local window cannot asynchronously tell the target its new terminal size. The
local raw receive log retains at most 256 KiB, and at most 1 MiB of raw output
is held while the UART menu is visible.

On the Events page, press `e` to clear the events held by this TUI. This does
not modify event state on the PowerMate.

The Dashboard has separate voltage, current, and power graphs. Each graph
retains the latest 600 samples for VIN, MAIN, and USB and uses its own shared
automatic scale so that the three channels can be compared. The VIN, MAIN, and
USB histories use separate stacked plot areas which expand to use the available
terminal height. Press `l` to switch between horizontal and vertically stacked
metric panels. Vertically stacked panels stretch the available history across
the terminal width. Horizontal panels use the full panel width for their graph
and show the VIN, MAIN, and USB values in a one-line legend below it.

CSV files are created in the current directory with names such as
`powermate_20260728_143000.csv`. Existing files are never overwritten.

## Protocol note

The status WebSocket uses the wire format defined by
`../../proto/status.proto`. This client decodes only the fields used by the
current TUI and safely skips unknown fields. When the firmware protocol changes,
update `protocol.go` together with the `.proto` definition.

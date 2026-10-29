---
name: sbc-remote
description: Operate a single-board computer wired to an ODROID PowerMate - power it on/off or press its power/reset button, capture boot logs, stop in U-Boot, log in and run commands over the UART console, recover a hung board, and measure power draw - and set up the PowerMate itself (Wi-Fi, IP, current limits, baud rate) from factory state into a test bench. Use when the user asks to boot, reboot, flash-test, debug, or benchmark the SBC on the PowerMate, or to install/configure a PowerMate.
allowed-tools: mcp__plugin_powermate_powermate__sbc_status mcp__plugin_powermate_powermate__sbc_console_read mcp__plugin_powermate_powermate__sbc_console_wait mcp__plugin_powermate_powermate__sbc_power_record mcp__plugin_powermate_powermate__sbc_events mcp__plugin_powermate_powermate__powermate_settings
---

# SBC remote control through ODROID PowerMate

The `powermate` MCP server keeps the PowerMate's status and UART streams open the whole
time, so console output and power samples are buffered between tool calls.

## Hardware model

Reference: https://wiki.odroid.com/accessory/powermate (getting started, connectors).

- **main output** (VOUT, J6): the PowerMate's DC input (VIN, 9-21 V) switched straight
  through to the SBC's DC jack. There is no regulator, so the SBC gets whatever the
  supply delivers. Turning it off is pulling the plug. Rated 7 A; above about 4 A
  continuous, the PowerMate's buck converter and load switch exceed 100 C.
- **usb output** (J7): a 5 V USB port (set to about 5.25 V, max 4 A), for USB-powered boards
  such as ODROID-M1S, or for accessories.
- **press_power / press_reset**: pulse the open-drain Power (pin 2) and Reset (pin 4) pins
  of the J4 header, held about 3 s and about 1 s by default. They only work when J4 is
  wired to a board with dedicated power pins (ODROID-M1, M1S, M2, H4). On other boards,
  use `sbc_power` on/off/cycle instead.
- **UART**: the SBC's debug console, bridged at the baud rate set on the PowerMate
  (`uart_baud_rate` in `sbc_status`). ODROID-M2 and other Rockchip boards use 1500000;
  many other boards use 115200. It is a PowerMate setting, changed with
  `powermate_device_options` (see "Configuring the PowerMate").
- **Sensors**: VIN (PowerMate input), MAIN (VOUT) and USB channels, 10 Hz by default.
- Supported boards (wiki): ODROID-C4, HC4, C5, N2+, M1, M1S, M2, and H4 (H4 needs its UART
  configured first).
- The UART bridge can drop data when the Wi-Fi link is poor. Short gaps in the console text
  are expected on a weak link. If full-screen output (htop, menus) looks broken, sending
  `ctrl-l` redraws it.
- The PowerMate cuts outputs on its own when a current limit is exceeded. That shows up in
  `sbc_events` as a warning or critical event.

## Start every task with `sbc_status`

Check `main_output_on`, `uart_stream`/`status_stream` = connected, the latest power, and
`console_last_line` (it shows where the console is: `login:`, `=> `, `# `, a GRUB menu...).
Read `warnings` before doing anything else.

## Cursors

Every console tool returns a `cursor`. Pass it as `since` to the next call when timing
matters, for example take `console_cursor_before` from `sbc_power`, then
`sbc_console_wait(since=...)`. That way output that arrived in between is not missed.
Without `since`, read/wait continue from where the last console tool stopped.

## Wiring problems: check before blaming the target

PowerMate hardware gives garbage, not silence, when wiring is bad:

- A **noise warning** on console output means the UART wire is loose or missing, or the
  SBC's GND is not shared with the PowerMate output. Stop, tell the user what you saw,
  and ask them to check the UART cable and GND. Do not interpret the text, do not cycle
  through baud rates, and do not power-cycle to "fix" it. Change the baud rate only when
  the user confirms the board's console speed differs from `uart_baud_rate`.
- **Sensor warnings** (values out of range, unstable VIN) or readings that jump around
  while the board is idle point at the power wiring or a missing GND, not at the SBC.
- **No UART output at all** after power-on while MAIN draws current: the console may be
  on another UART, the wire may be disconnected, or the baud rate may be wrong.

## Workflows

### Capture a boot log
`sbc_boot_capture` (default: power cycle MAIN, wait for `login:`, 180 s). It returns the
log path, head and tail, boot time, power during boot and PowerMate events. Use `Read` or
`sbc_console_read(grep=...)` for details, and look for the last line before a hang,
kernel panics, `Call trace`, and failed mounts or services.

If the board is already running an OS, ask the user before power-cycling it.

### Stop in the bootloader
- **Rockchip U-Boot** (ODROID-M1/M2 etc.) prints `Hit key to stop autoboot('CTRL+C'): 0`
  with a zero delay, so one key press is too late. Spam Ctrl-C from the U-Boot banner:
  `sbc_boot_capture(interrupt_pattern="U-Boot 20", interrupt_keys=["ctrl-c"],
  interrupt_repeat_seconds=5, until="=> ")`
- **Generic U-Boot**: `interrupt_pattern="Hit any key to stop autoboot"`,
  `interrupt_keys=["space"]`, `until="=> "`.
- **GRUB menu**: `interrupt_pattern="GNU GRUB"`, `interrupt_keys=["down"]` stops the
  countdown. Then `sbc_console_send(text="c")` opens the `grub> ` command line. A hidden
  GRUB menu that only reacts to Shift cannot be reached over serial.
- **EDK2/UEFI**: interrupt on the firmware's `Press ESC`/`F2` hint with `["esc"]` or `["f2"]`,
  and repeat for a second or two.
- Menus redraw the whole screen with escape codes. After each key, read only the new output
  (`since` = the `console_cursor_before` returned by `sbc_console_send`). The cleaned text
  is a flattened screen, so expect repeated menu lines.

At a U-Boot `=> ` prompt, use `sbc_console_run` (`printenv`, `mmc list`, `version`).
Leave with `sbc_console_send(text="boot", enter=true)` and then
`sbc_console_wait("login: $")`. Do not use `sbc_console_run` for `boot`, because its
prompt never returns.

### Log in and run Linux commands
`sbc_console_run` refuses to type at a `login:`/`Password:` prompt. Right after the
server starts it has seen no console output yet, so its first call sends Ctrl-C to get a
fresh prompt. If a foreground job may be running on the console, call `sbc_console_read`
or `sbc_status` first instead. Ask the user for
credentials. Never guess them or try vendor defaults unasked. Then:
1. `sbc_console_send(text=<user>, enter=true)`, then `sbc_console_wait("[Pp]assword: ?$")`
2. `sbc_console_send(text=<password>, enter=true)`, then
   `sbc_console_wait("[#$] $", timeout_seconds=30)`
3. `sbc_console_run(command="uname -a", exit_code=true)`

Notes:
- Kernel messages, and output from background jobs started by the login (for example the
  Ubuntu MOTD release checker), can interleave with command output on the console. As
  root, run `dmesg -n 1` first when you need clean output. Write a command's output to a
  file and `cat` it when the output must be exact.
- For long-running commands, raise `timeout_seconds` or run them in the background
  (`cmd > /tmp/out 2>&1 &`) and poll. On timeout the tool sends Ctrl-C.
- Pagers and full-screen programs do not work well. Use `--no-pager`, `| cat`, `-b`
  batch modes, `SYSTEMD_PAGER=`.
- Keep commands to one line. Do not send secrets you were not given.

### Shut down cleanly, then cut power
1. `sbc_console_run(command="poweroff")`. It may time out because the shell goes away;
   that is expected.
2. `sbc_console_wait("reboot: Power down|System halted|Power down", timeout_seconds=60)`,
   and/or `sbc_power_record(seconds=5)` until MAIN current falls to idle.
3. `sbc_power(action="off")`

### Recover a hung board
Escalate one step at a time and check the console after each one:
1. `sbc_status`: is there console activity (`console_idle_seconds`)? Is power plausible?
2. `sbc_console_send(keys=["enter"])` and `sbc_console_read`. A prompt may simply be
   waiting.
3. If you are logged in and the shell is unresponsive: `sbc_console_send(keys=["ctrl-c"])`.
4. `sbc_power(action="press_reset")`, then `sbc_console_wait("U-Boot|DDR", 20)`.
5. `sbc_power(action="cycle")` or `sbc_boot_capture`. This is a hard power cut, so
   tell the user.

### Measure power
- Idle baseline: `sbc_power_record(seconds=30)`.
- Under load: start the workload in the background with
  `sbc_console_send(text="stress-ng --cpu 0 -t 60 &", enter=true)`, then
  `sbc_power_record(seconds=60, csv=true)`.
- Report MAIN for the board itself and VIN for the whole setup, including PowerMate
  overhead. `energy_wh` integrates over the window.

## Configuring the PowerMate (`powermate_*` tools)

These tools change the PowerMate itself, and a mistake can make it unreachable or leave
the SBC unprotected. Every one of them follows the same protocol:

1. Call it **without** `confirm` first. Nothing changes. It returns the request, the
   current values, and `risks`.
2. Tell the user, in your own words, what will change and **every** risk, plus the
   recovery path for network changes. Ask for an explicit yes.
3. Only then call it again with the same arguments and `confirm=true`.
4. Verify the effect with `powermate_settings` or `sbc_status` before the next change.

Additional rules:
- Make one network change at a time (Wi-Fi network, IP addressing, AP/STA mode). Confirm
  the PowerMate is reachable again before the next one.
- Never guess Wi-Fi passwords, AP passwords or web credentials. Ask the user. Do not
  repeat secrets back in chat; the tools mask them in their output.
- The setup AP (`apsta` mode, `powermate` at 192.168.4.1) exists for the initial setup from
  a phone. While it is on, a failed Wi-Fi or IP change can still be fixed through it. In
  `sta` mode nothing is left on the network, so treat Wi-Fi and IP changes there as
  potentially locking the user out, and say so. The tool refuses `sta` while STA is not
  connected. An open AP (`open_ap=true`) is a supported choice, but it must be the user's
  explicit decision.
- Base current limits on measurements, not guesses (see setup step 5). A warning limit
  below the boot peak makes the PowerMate cut the board off while it boots.
- After `powermate_credentials`, remind the user to update `POWERMATE_PASSWORD`
  (and `POWERMATE_USERNAME`) for this plugin, the web UI and pmtui.

### When the PowerMate cannot be reached
The tools cannot fix this themselves. Tell the user the options, in this order:
1. Look up its address in the router's DHCP list, then `powermate_connect(host=...)`.
2. **USB debug console** (debug use only, not normally connected): connect a PC to the
   PowerMate's USB port and open its console, e.g. `idf.py -p /dev/ttyACM0 monitor` or
   `minicom -D /dev/ttyACM0 -b 115200`. At the `powermate >` prompt run `wifi_status`
   (shows SSID and IP), `wifi_scan`, or `wifi_connect <ssid> <password>`. Warn that opening
   the port can reboot the PowerMate, which switches the SBC off unless output-state
   restore is on.
3. **Factory reset**: hold the PowerMate button for 5 s. This erases ALL PowerMate settings
   (Wi-Fi, IP, limits, baud rate, password) and brings back the setup AP. Then do the
   initial setup again from a phone. It is the user's decision.

## Set up a test bench from a factory-fresh PowerMate

Factory state: Wi-Fi mode `apsta`, setup AP `powermate` (password `hardkernel`) at
192.168.4.1, STA not configured, web login `admin`/`password`, UART 1500000, outputs off.

1. **Wiring (ask, you cannot see it).** The power supply (9-21 V) goes into the PowerMate
   VIN DC jack, and the VOUT terminal goes to the SBC's DC input. USB-powered boards
   (ODROID-M1S) go on the USB port instead. The SBC's UART debug port goes to the
   PowerMate's ODROID UART connector (J3), with GND shared. For ODROID-M1, M1S, M2 and
   H4, J4 Power/Reset goes to the board's power pins if `press_power`/`press_reset` should
   work. Remind the user never to wire the terminal block while powered: the high current
   can spark.
2. **Put it on the network.** The first Wi-Fi setup is the user's job: connect a phone to
   the `powermate` AP, open http://192.168.4.1, log in, and join the lab Wi-Fi in the
   settings. Then ask for the PowerMate's new address (shown in the web UI, or in the
   router's DHCP list) and call `powermate_connect(host=...)`. If this computer itself is
   already on the `powermate` AP, the tools can do the same step: at 192.168.4.1,
   `powermate_wifi_connect` with preview and confirm, then `powermate_connect` to the
   reported STA address.
3. **Check it.** `powermate_settings`: firmware, mode, IP. Suggest a DHCP reservation, or
   `powermate_network(static)` if the user wants a fixed address. Tell the user to
   `export POWERMATE_HOST=<ip>`, or rely on the address the tools saved.
4. **Match the console.** Ask which SBC it is. Set `powermate_device_options(uart_baud_rate)`:
   1500000 for ODROID-M1/M2 and other Rockchip boards, 115200 for most others. Preview,
   then confirm.
5. **Verify and measure.** Ask, then run `sbc_boot_capture` (power-cycles MAIN). Check for
   noise warnings, which point at wiring. Read `power_during_boot` for the boot peak, then
   `sbc_power_record(seconds=30)` idle. If the user wants it, add a load run
   (see "Measure power").
6. **Limits.** Base a proposal on the highest current seen in the boot capture **and** a
   load run (for example `stress-ng --cpu 0` for 60 s, plus any USB devices or displays the
   board will use). Propose warning limits around 1.5x that peak, and at least peak +
   0.5 A. Without a load run, do not propose anything below the factory defaults
   (VIN 4 A, MAIN 3 A, USB 3 A): you may only raise them. A boot peak says nothing
   about peak draw under load. Keep warning limits below what the board's power supply and
   cables are rated for, and above 4 A only for short peaks: VOUT is rated 7 A, but runs over
   100 C under continuous loads above 4 A. The USB port is rated 4 A. Leave critical limits at their defaults unless the user has a
   reason. Preview, explain the risks, confirm.
7. **Options the user should decide.** `restore_output_state` (boards power on again after
   PowerMate power loss: convenient for unattended labs, surprising otherwise). A new web
   password (`powermate_credentials`). Whether to switch to `sta` mode once STA is stable
   (closes the AP, removes the fallback).
8. **Summarize** the final configuration: address, mode, baud rate, limits, options. Also
   say what the user still has to do (env vars, router reservation).

## Safety rules

- `sbc_power` off/cycle and `sbc_boot_capture` with power methods are hard power cuts.
  Prefer a clean shutdown. Ask the user first when the OS is running, unless they
  already asked for the power action.
- After an over-current event in `sbc_events`, do not keep switching the output back on.
  Report the event and the current readings to the user.
- Change PowerMate settings only through the preview-then-confirm protocol above, and only
  when the user asked for that change or agreed to it during setup.
- The PowerMate holds only four login tokens. The server caches its token, so do not ask
  the user to log in again unless a tool reports a login failure.

# chargelimit

A small battery charge limiter for Apple Silicon MacBooks, in the spirit of AlDente:
stop charging at an **upper** limit, resume at a **lower** limit, and calibrate the
battery gauge from time to time.

It is one C daemon, one shell command and a launchd plist — no app, no network access.

## How it works

| Battery level | What happens |
|---|---|
| ≥ upper | charging stops; the Mac runs from the power adapter and the battery stays where it is |
| ≤ lower | charging resumes |
| in between | the current state is kept (hysteresis) |

**Discharge on AC** (on by default). Stopping charge alone can't bring the level down while
plugged in. So when the Mac is on AC and above the upper limit, the daemon disconnects the
power adapter, lets the Mac run from the battery down to one below the upper limit, then
reconnects the adapter with charging still stopped.

Charging and the adapter are controlled through undocumented SMC keys, detected at run time:

| Key | Firmware | Purpose | On / off |
|---|---|---|---|
| `CHTE` (4 bytes) | newer | stop charging | `01000000` / `00000000` |
| `CH0B` + `CH0C` (1 byte) | older | stop charging | `02` / `00` |
| `CHIE` (1 byte) | newer | disconnect adapter | `08` / `00` |
| `CH0I` (1 byte) | older | disconnect adapter | `01` / `00` |

**Calibration.** After the limit has been reached `calib_every` times (default 3), or
`calib_days` days after the last calibration (default 30), the next charge goes to 100%.
Calibration ends once the Mac has stayed at 100% on AC for 5 minutes. There is no timeout:
until that happens, the limiter stays out of the way.

## Requirements

- Apple Silicon MacBook
- macOS 12 or later (developed for macOS 15)
- Xcode Command Line Tools (`xcode-select --install`)

Not supported: Intel Macs (they use different keys), desktop Macs (no battery), and the
firmware shipped from macOS 27 beta 4 on, where Apple gates these keys behind a private
entitlement. In that case the installer refuses to install; on macOS 26.4+ use the
built-in Charge Limit in System Settings → Battery instead.

## Check your Mac first (optional)

Build in the source folder and try the keys by hand, without installing anything:

```bash
clang -O2 -o chargelimitd chargelimitd.c -framework IOKit -framework CoreFoundation
./chargelimitd probe                  # read-only: which keys exist, their size and value
sudo ./chargelimitd adaptertest 60    # disconnect the adapter for 60 s, then reconnect
```

`adaptertest` needs the adapter plugged in and at least 30% battery. It always reconnects
the adapter at the end, on Ctrl+C, or below 25%. If anything interrupts it, run
`sudo ./chargelimitd adaptertest 0` to reconnect. The battery current it shows refreshes
about once a minute, so short tests may report "unclear".

## Install

```bash
bash install.sh
```

To install with discharging off, add `discharge=0` to `/Library/Preferences/chargelimit.conf`
before running the installer, and turn it on later with `sudo chargectl discharge on`.

The installer compiles the daemon, checks that a supported key exists, verifies that every
directory it installs into is root-owned and not writable by others, and loads the daemon.
Running it again upgrades in place and keeps your settings.

Consider turning off **System Settings → Battery → Battery Health ⓘ → Optimized Battery
Charging** if your upper limit is above 80% or you want calibration to run on schedule.
With an upper limit of 80% or less the two get along; calibration may just be delayed.

## Usage

```bash
chargectl status              # show status (no sudo needed)
sudo chargectl set 80 60      # upper 80%, lower 60% (also turns the limiter on)
sudo chargectl calib 3 30     # calibrate every 3 limits or 30 days; 0 turns a condition off
sudo chargectl off            # stop limiting (charge to full as usual)
sudo chargectl on             # limit again; also clears a "had no effect" disable
sudo chargectl discharge off  # don't discharge on AC (on turns it back on)
chargectl log [N]             # last N lines of the log (default 50)
```

Output is in Chinese when `LC_ALL`, `LC_MESSAGES` or `LANG` starts with `zh`, otherwise in
English. Logs are always in English.

`chargectl status` exit codes: `0` OK, `1` no supported charging key, `2` cannot open AppleSMC.

## What happens when…

**You stay plugged in above the upper limit.** The adapter is disconnected and the Mac
runs from the battery down to one below the upper limit, then the adapter is reconnected.
`chargectl status` shows "battery (adapter disconnected)" meanwhile.

**You unplug.** Nothing changes; the battery discharges as usual. If you plug back in
between the limits, it does not charge until the level drops to the lower limit.

**You shut down.** The daemon re-enables charging before it exits, so a Mac that is off and
plugged in charges to 100%. After boot, charging stops again if the level is above the
upper limit. Holding 100% on AC for 5 minutes after boot counts as a calibration.

**You close the lid.** The daemon does not run during sleep. If it was discharging, it
reconnects the adapter first (charging stays stopped) and continues after wake. If it was
charging toward the upper limit, it pauses charging at the current level and resumes on
wake. Below the lower limit, or while calibrating, charging continues and may reach 100%.

**The daemon crashes or is killed.** launchd restarts it (within about a minute); on start
it first reconnects the adapter and restores normal charging, then applies the limits again.

## Safety

- Every write is read back. The first time charging is stopped, the daemon checks 60 s
  later that the battery really stopped charging. If not, it re-enables charging, disables
  itself for this macOS + firmware version, and stays off until `sudo chargectl on` or a
  system update.
- The adapter is reconnected on every exit path (signal, error, restart after a crash, boot)
  and before sleep, and discharging never goes below one under the upper limit. The first
  time the adapter is disconnected, the daemon checks 60 s later that macOS really runs on
  battery; if not, it disables only discharging for this macOS + firmware version, until
  `sudo chargectl discharge on`.
- If disconnecting the adapter fails (the key does not read back as written), the adapter is
  reconnected and discharging pauses for 10 minutes before retrying. After 3 failures in a
  row it stays off until `sudo chargectl discharge on` or a restart. `chargectl status` shows
  the pause and the raw value read back.
- Keys with an unexpected size are never written.
- If another program keeps changing the key (10 times in 10 minutes), the daemon
  re-enables charging and exits.
- The config file is ignored unless it is root-owned and not writable by others.
- Nothing is sent over the network. The state file stores the macOS build, firmware
  version, boot time and pid — no user name, serial number or hardware UUID.

## Files

| Path | Purpose |
|---|---|
| `/Library/PrivilegedHelperTools/com.local.chargelimit` | daemon (runs as root) |
| `/usr/local/bin/chargectl` | command-line tool |
| `/Library/LaunchDaemons/com.local.chargelimit.plist` | launchd job |
| `/Library/Preferences/chargelimit.conf` | settings |
| `/var/db/chargelimit.state` | daemon state (limits reached, last calibration, verification) |
| `/var/db/chargelimit.log` | daemon log (rotated to `.log.1` at 256 KB; also in the system log) |

Config format:

```
enabled=1
upper=80
lower=60
calib_every=3
calib_days=30
discharge=1
```

## Uninstall

```bash
bash install.sh uninstall
```

The adapter and charging are re-enabled and all files except the config are removed
(including the log).

## Development

```bash
cc -Wall -Wextra -o test_policy tests/test_policy.c && ./test_policy   # decision logic, any OS
clang -O2 -Wall -Wextra -o chargelimitd chargelimitd.c -framework IOKit -framework CoreFoundation
./chargelimitd status
```

Commands of the daemon binary itself: `status`, `probe` (no root needed); `run`, `inhibit`,
`allow`, `clearfault`, `adaptertest [s]` (root). `inhibit` and `allow` refuse to run while
the daemon is running.

`policy.h` holds the decision logic (limits, calibration, key reconciliation) with no
macOS dependencies, so it can be unit-tested anywhere. `chargelimitd.c` adds SMC access,
battery readings, state persistence and launchd integration.

## Disclaimer

This tool writes undocumented SMC keys whose meaning Apple may change at any time. It
includes the safeguards above, but use it at your own risk. Not affiliated with Apple.
The approach follows open-source tools. Codes were generated by Claude Opus5.5 Medium.

## License

[MIT](LICENSE)

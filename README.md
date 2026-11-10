# chargelimit

A small battery charge limiter for Apple Silicon MacBooks (macOS 15 - 26), in the spirit of AlDente:
stop charging at an **upper** limit, resume at a **lower** limit, and calibrate the
battery gauge from time to time.

It is one C daemon, one shell command and a launchd plist — no app, no network access.

## How it works

| Battery level | What happens |
|---|---|
| ≥ upper | charging stops; the Mac runs from the power adapter and the battery stays where it is |
| ≤ lower | charging resumes |
| in between | the current state is kept (hysteresis) |

Charging is controlled through undocumented SMC keys, detected at run time:

| Key | Firmware | Stop / allow |
|---|---|---|
| `CHTE` (4 bytes) | newer | `01000000` / `00000000` |
| `CH0B` + `CH0C` (1 byte) | older | `02` / `00` |

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

## Install

```bash
bash install.sh
```

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
chargectl log                 # last 2 hours of logs
```

Output is in Chinese when `LC_ALL`, `LC_MESSAGES` or `LANG` starts with `zh`, otherwise in
English. Logs are always in English.

`chargectl status` exit codes: `0` OK, `1` no supported charging key, `2` cannot open AppleSMC.

## What happens when…

**You unplug.** Nothing changes; the battery discharges as usual. If you plug back in
between the limits, it does not charge until the level drops to the lower limit.

**You shut down.** The daemon re-enables charging before it exits, so a Mac that is off and
plugged in charges to 100%. After boot, charging stops again if the level is above the
upper limit. Holding 100% on AC for 5 minutes after boot counts as a calibration.

**You close the lid.** The daemon does not run during sleep. If it was charging toward the
upper limit, it pauses charging at the current level and resumes on wake. Below the lower
limit, or while calibrating, charging continues and may reach 100%.

**The daemon crashes or is killed.** launchd restarts it; on start it first restores normal
charging, then applies the limits again.

## Safety

- Every write is read back. The first time charging is stopped, the daemon checks 60 s
  later that the battery really stopped charging. If not, it re-enables charging, disables
  itself for this macOS + firmware version, and stays off until `sudo chargectl on` or a
  system update.
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

Config format:

```
enabled=1
upper=80
lower=60
calib_every=3
calib_days=30
```

## Uninstall

```bash
bash install.sh uninstall
```

Charging is re-enabled and all files except the config are removed.

## Development

```bash
cc -Wall -Wextra -o test_policy tests/test_policy.c && ./test_policy   # decision logic, any OS
clang -O2 -Wall -Wextra -o chargelimitd chargelimitd.c -framework IOKit -framework CoreFoundation
./chargelimitd status
```

`policy.h` holds the decision logic (limits, calibration, key reconciliation) with no
macOS dependencies, so it can be unit-tested anywhere. `chargelimitd.c` adds SMC access,
battery readings, state persistence and launchd integration.

## Disclaimer

This tool writes undocumented SMC keys whose meaning Apple may change at any time. It
includes the safeguards above, but use it at your own risk. Not affiliated with Apple.
The approach follows open-source tools. Codes were generated using Claude Opus 5.5 
Medium.

## License

[MIT](LICENSE)

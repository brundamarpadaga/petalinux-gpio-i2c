# Embedded Linux Bring-Up on Zynq-7000 SoC (Zybo Z7-20)

A PetaLinux project that brings up embedded Linux on the Zybo Z7-20 board, integrating custom FPGA peripherals (AXI GPIO, AXI IIC) with Linux kernel drivers. Features LED control via PL GPIO and an SSD1306 OLED display driven over I2C.

**Status:** BME280 temperature/humidity sensor is wired, verified on hardware, and publishing to HiveMQ Cloud over MQTT/TLS every 5 seconds, with live readings mirrored on the OLED. **Current expansion in progress:** a MATLAB live dashboard that subscribes to the HiveMQ topic and plots temperature/humidity in real time.

---

## Table of Contents

- [Hardware Requirements](#hardware-requirements)
- [Software Requirements](#software-requirements)
- [Project Structure](#project-structure)
- [Build Instructions](#build-instructions)
- [Booting from SD Card](#booting-from-sd-card)
- [LED Control via sysfs GPIO](#led-control-via-sysfs-gpio)
- [I2C / OLED Setup](#i2c--oled-setup)
- [Ethernet Setup](#ethernet-setup)
- [IoT Expansion: BME280 + MQTT](#iot-expansion-bme280--mqtt)
- [MATLAB Live Dashboard (In Progress)](#matlab-live-dashboard-in-progress)
- [Debugging Reference](#debugging-reference)
- [Key Learnings](#key-learnings)

---

## Hardware Requirements

**Working:**
- Zybo Z7-20 development board
- MicroSD card (8 GB+)
- SSD1306 OLED display (I2C, address `0x3C`) connected to Pmod JD
- BME280 temperature/humidity sensor (I2C, address `0x76`) — shares the same I2C bus as the OLED
- Cat5e/Cat6 Ethernet cable (any standard cable, connect to router/switch)
- USB-UART cable for serial console

---

## Software Requirements

- PetaLinux 2024.2
- Vivado 2024.2 (for hardware design / XSA export)
- Host machine: Ubuntu 20.04 (WSL or native)

---

## Project Structure

```
gpio_project/
├── design.xsa                              # Vivado hardware export
├── constraints.xdc                         # Pin constraints
├── components/plnl/device-tree/
│   └── device-tree/pl.dtsi                 # Auto-generated PL device tree
└── project-spec/meta-user/
    ├── recipes-bsp/device-tree/files/
    │   └── system-user.dtsi                # Custom device tree overlay
    └── recipes-apps/oled-gpio-app/
        ├── files/
        │   ├── oled-gpio-app.c             # Application source
        │   └── Makefile
        └── oled-gpio-app.bb                # BitBake recipe
```

---

## Build Instructions

### 1. Create project from BSP

```bash
petalinux-create -t project -s xilinx-zc702-v2024.2-final.bsp -n zybo_hdmi_eth
cd zybo_hdmi_eth
```

### 2. Import hardware from Vivado

```bash
petalinux-config --get-hw-description=<path_to_xsa_exported_from_vivado>
```

### 3. Configure kernel — enable Xilinx I2C driver

```bash
petalinux-config -c kernel
# Navigate to: Device Drivers → I2C → I2C Hardware Bus support
# Enable: [*] Xilinx I2C Controller  (press 'y', not 'm')
```

### 4. Configure rootfs (optional)

```bash
petalinux-config -c rootfs
```

### 5. Build

```bash
petalinux-build
```

### 6. Package boot files

```bash
petalinux-package --boot --fsbl --u-boot --fpga --force
```

---

## Booting from SD Card

At the U-Boot prompt:

```bash
fatload mmc 0 0x3000000 image.ub
bootm 0x3000000
```

---

## LED Control via sysfs GPIO

> **Important:** LEDs are connected through **AXI GPIO in PL** (gpiochip1016), not PS GPIO (gpiochip898). Use PL GPIO numbers (≥ 1016).

```bash
sudo su

# 1. Export the GPIO pin
echo 1020 > /sys/class/gpio/export

# 2. Set direction to output
echo out > /sys/class/gpio/gpio1020/direction

# 3. Toggle LED on/off
echo 1 > /sys/class/gpio/gpio1020/value
echo 0 > /sys/class/gpio/gpio1020/value
```

To identify the correct GPIO chip and base number:

```bash
cat /sys/class/gpio/gpiochip*/label
```

> **Note:** For production applications, `libgpiod` is recommended over sysfs. In this project, the application is cross-compiled on the host using PetaLinux's Yocto build system and installed directly into the root filesystem.

---

## I2C / OLED Setup

The OLED (SSD1306) is connected to Pmod JD and driven via **AXI IIC** (PL I2C).

### Verify I2C bus is available

```bash
ls /dev/i2c-*
# Expected: /dev/i2c-0  /dev/i2c-1
```

### Scan for OLED device

```bash
i2cdetect -y 1
# Should show 0x3C
```

### Read / write over I2C

```bash
i2cget -y 1 0x3c 0x00
i2cset -y 1 0x3c 0x00 0xAE
```

---

## Ethernet Setup

### Background

Ethernet on the Zybo Z7-20 is **PS-native** — the Gigabit Ethernet MAC (GEM0, exposed as `eth0` in Linux) connects to the onboard RTL8211E PHY via MIO pins 16–27. No Vivado or PL changes are needed; it is always present in the Zynq PS.

In Vivado: Zynq PS IP → MIO Configuration → Peripherals → **ENET 0** should be enabled on MIO 16–27. ✅ (already confirmed in this project)

The kernel driver is `macb` (Cadence GEM), enabled by default in the PetaLinux BSP.

### Verification Steps

```bash
# Step 1 — Check interface exists (requires Ethernet cable plugged in)
ip link show
# eth0 should appear; NO-CARRIER means cable is not connected

# Step 2 — Get an IP via DHCP (run as root)
sudo su
udhcpc -i eth0
# Should print: bound to <IP address>

# Step 3 — Confirm internet reachability
ping -c 4 8.8.8.8
```

### Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `eth0` not in `ip link show` | `CONFIG_MACB` not enabled | `petalinux-config -c kernel` → enable Cadence GEM |
| `NO-CARRIER` | Cable not connected | Plug in Ethernet cable to router/switch |
| `udhcpc: socket: Operation not permitted` | Not running as root | Run `sudo su` first |
| DHCP fails / no IP | No DHCP server on network | Connect to router, not directly to PC |
| PHY not detected | PHY driver missing | Enable `CONFIG_MICREL_PHY` in kernel config |

### Status: ✅ Working — `eth0` up, DHCP lease obtained, internet reachable

---

## IoT Expansion: BME280 + MQTT

### Goal

Read temperature and humidity from a BME280 sensor over I2C and publish the data to HiveMQ Cloud broker via MQTT over Ethernet, on a timed interval. Display current readings on the SSD1306 OLED locally at the same time.

### Status: ✅ Working end-to-end on hardware

Sensor readings (temperature and humidity), MQTT/TLS publish to HiveMQ Cloud, and the OLED readout are all confirmed working on the board.

### Target Architecture

```
[BME280 sensor]
      │ I2C (same bus as OLED, or second Pmod)
      ▼
[PetaLinux C app: mqtt-sensor-app]
      │ reads + formats JSON
      ▼
[MQTT publish via paho-mqtt-c over TLS]
      │ eth0
      ▼
[HiveMQ Cloud broker]
      │
      ▼
[Web dashboard / Grafana]
```

### Progress

- [x] **Step 1 — Verify Ethernet** — `eth0` confirmed working, DHCP lease obtained, internet reachable
- [x] **Step 2 — Wire BME280** to Pmod JD, same I2C bus as the OLED
  - SDO pin pulled to GND → I2C address `0x76` (confirmed via `i2cdetect`, chip ID register reads `0x60`, genuine BME280)
- [x] **Step 3 — Detect sensor on I2C bus**
  ```bash
  i2cdetect -y 1
  # Shows 0x3C (OLED) and 0x76 (BME280)
  ```
- [x] **Step 4 — Write BME280 userspace reader** using `i2c-dev`
  - Read raw ADC values from BME280 registers
  - Apply BME280 compensation formula from datasheet (section 4.2.3)
  - Same pattern as the SSD1306 app, but reading data instead of writing display commands
- [x] **Step 5 — Add paho-mqtt-c to rootfs**
  - `paho-mqtt-c 1.3.8` sourced from `meta-openembedded/meta-oe` (already in bblayers)
  - Built with `-DPAHO_WITH_SSL=ON` — TLS support included
  - `ca-certificates` added to rootfs for server certificate verification
  - Added to `project-spec/meta-user/conf/user-rootfsconfig`
- [x] **Step 6 — Write mqtt-sensor-app** (`recipes-apps/mqtt-sensor-app/`)
  - Publishes live BME280 readings as JSON to topic `zynq/sensor/bme280`
  - Connects to HiveMQ Cloud over TLS (port 8883) with username/password auth
  - Credentials stored in `/etc/mqtt-sensor.conf` — not hardcoded, gitignored
  - CA cert path explicitly set: `ssl_opts.trustStore = "/etc/ssl/certs/ca-certificates.crt"`
  - Publishes every 5 seconds; graceful shutdown on `SIGINT`/`SIGTERM`
  ```
  # Config file format (on target at /etc/mqtt-sensor.conf)
  broker=ssl://<cluster>.s1.eu.hivemq.cloud:8883
  username=<user>
  password=<password>
  ```
- [x] **Step 7 — Set up HiveMQ Cloud**
  - Free cluster provisioned at `console.hivemq.cloud`
  - Subscribe to `zynq/sensor/bme280` in the HiveMQ web client to monitor live data
- [x] **Step 8 — Test end-to-end** — deployed binary to board, confirmed publish reaches HiveMQ (see "TLS publish failing" below for the blocker that had to be fixed first)
- [x] **Step 9 — Write BME280 driver** — `read_sensor()` does real forced-mode I2C reads + Bosch datasheet §4.2.3 compensation (temp + humidity; pressure intentionally skipped since it's not published). **Verified on hardware** — temperature was correct from the start; humidity was stuck near 0% until the integer-overflow bug below was found and fixed (see "Humidity stuck near 0%").
- [x] **Step 10 — Display readings on OLED** — `mqtt-sensor-app` now also drives the SSD1306 (reusing the same I2C bus, separate fd/address from the BME280) and shows a live `T:`/`H:` readout on every publish cycle, while continuing to publish to HiveMQ. OLED init is non-fatal: if the display isn't present, publishing continues without it.

### Troubleshooting: TLS publish failing with `rc=-1`

`mqtt-sensor-app` connected fine over TCP but failed the TLS handshake (`Failed to connect, rc=-1`) even with valid HiveMQ credentials and a correct CA trust store path.

**Root cause:** the Zybo Z7-20 has no battery-backed RTC. On boot, `date` read back `Fri Mar 9 2018` (`hwclock -r` confirms: `Cannot access the Hardware Clock via any known method`). With `ssl_opts.enableServerCertAuth = 1`, OpenSSL rejects the HiveMQ server certificate because the board's clock predates the certificate's validity window. Manually forcing the clock to the real date (`date -s "<current date>"`) before launching the app immediately fixed it — confirming the clock, not credentials or networking, was the blocker.

**First fix attempt (didn't actually work):** added the `ntpdate` package via `user-rootfsconfig` / `rootfs_config`, relying on its `/etc/network/if-up.d/ntpdate-sync` hook, which `ifupdown` is supposed to run automatically the moment `eth0` gets its DHCP lease (`S01networking` calls `ifup -a` at boot from `/etc/rc5.d/`, confirmed present and running). This looked like it worked in initial testing, but that was because the Ethernet cable happened to already be plugged in and a manual `date -s` fix from an earlier test was masking the real behavior. Once isolated with a clean test (stale clock forced, then `ifup eth0` alone), it turned out the hook script is completely correct and works fine when run **by hand** (`/etc/network/if-up.d/ntpdate-sync`) — but is **never invoked automatically** by boot. Root cause: this image's `ifup` is the BusyBox applet, which does not execute `/etc/network/if-up.d/` scripts despite the directory/symlink existing (a BusyBox limitation, not a config mistake).

**Actual fix:** hook the clock sync into `/etc/udhcpc.d/` instead, which *is* run-parts'd by `udhcpc` itself (confirmed on hardware — it's the same mechanism that installs DNS servers via the stock `50default` script on every successful lease). Added `project-spec/meta-user/recipes-core/busybox/busybox_%.bbappend`, which installs a new `51ntpdate` script alongside busybox's own `50default` without modifying it:
```sh
[ "$1" = bound -o "$1" = renew ] && /usr/sbin/ntpdate -s -b pool.ntp.org time.google.com
exit 0
```
The `ntpdate` package (still enabled via `user-rootfsconfig`) provides the binary; the old `recipes-support/ntp/ntp_%.bbappend` NTP-server override is now dead weight (nothing reads `/etc/default/ntpdate` anymore) but harmless — left in place rather than ripped out mid-debug.

**Verified on hardware:** with the clock deliberately forced stale (`date -s "2018-03-09 12:00:00"`) then `ifdown eth0 && ifup eth0`, the clock self-corrected the moment `udhcpc` reached the `bound` state, with zero manual intervention. Next step: rebuild + reflash to bake this in, then confirm it also fires from a cold boot.

### Troubleshooting: Humidity stuck near 0%

Temperature readings were correct from the start, but humidity always reported ~0.02%RH regardless of actual conditions.

**Root cause:** a 32-bit integer overflow in `bme280_compensate_humidity()`, caused by C operator precedence. The Bosch datasheet's reference formula requires the `>> 14` shift to apply to only the right-hand multiplicand of a product, but the original code was written as `(L * R) >> 14`. Since `*` binds tighter than `>>` in C, this is what it already did syntactically — the bug was that the *values* being multiplied together before any shift overflowed `int32_t` (~3.2×10¹² before wraparound vs. a ~2.1×10⁹ ceiling), producing garbage. Reproduced exactly on a host build: the buggy expression gave `v_x1_final = 72414` (→ 0.02%RH, matching the board); adding one parenthesis to shift the smaller intermediate value first before multiplying kept everything inside `int32_t` range and gave `194581384` (→ 46.39%RH, a sane reading).

**Fix:** re-parenthesized the compensation formula so the `>> 14` divides down a sub-term before the final multiplication, matching the Bosch reference exactly (`project-spec/meta-user/recipes-apps/mqtt-sensor-app/files/mqtt-sensor-app.c`, `bme280_compensate_humidity()`). Verified against reproduced board output in a standalone host test before touching the on-target build.

**Lesson:** for tricky fixed-point/bit-shift math, reproduce the exact expression in a small host program with the real input values rather than hand-tracing precedence — it's easy to convince yourself the code is correct by inspection when the actual failure is a magnitude/overflow issue, not a logic error.

### New Files Added

```
project-spec/meta-user/
├── recipes-apps/mqtt-sensor-app/
│   ├── mqtt-sensor-app.bb              # BitBake recipe (depends on paho-mqtt-c)
│   └── files/
│       ├── mqtt-sensor-app.c           # Publisher app source (BME280 read + OLED display + MQTT/TLS publish)
│       ├── Makefile                    # Links -lpaho-mqtt3cs (SSL variant)
│       ├── mqtt-sensor.conf            # Real credentials — gitignored
│       └── mqtt-sensor.conf.example    # Placeholder template — tracked in git
├── recipes-support/ntp/
│   ├── ntp_%.bbappend                  # Overrides ntpdate's default (empty) NTP server list — vestigial, see Troubleshooting
│   └── files/ntpdate.default           # NTPSERVERS="pool.ntp.org time.google.com"
└── recipes-core/busybox/
    ├── busybox_%.bbappend              # Adds 51ntpdate alongside busybox's own 50default udhcpc script
    └── files/51ntpdate                 # Runs ntpdate on the udhcpc "bound"/"renew" event — the fix that actually works
```

### Notes

- No Vivado changes needed for Ethernet — GEM0 is PS-native
- BME280 uses same I2C interface already proven with SSD1306
- Linking `-lpaho-mqtt3cs` (not `-lpaho-mqtt3c`) is required — the `s` suffix means SSL

---

## MATLAB Live Dashboard (In Progress)

### Goal

A live, auto-updating MATLAB plot of temperature/humidity, subscribed directly to the HiveMQ Cloud topic (`zynq/sensor/bme280`), that launches automatically on the host desktop at Windows logon — no manual steps, no PNG snapshots.

### Design

- MATLAB `mqttclient` (Industrial Communication Toolbox) connects over TLS to HiveMQ Cloud and subscribes to the sensor topic.
- A visible figure with `animatedline` (dual `yyaxis` for temp + humidity) is updated live via `addpoints` / `drawnow limitrate` on a rolling time window.
- A supervisor loop wraps the connection: if the broker connection drops or the FPGA board is powered off, the plot shows a "waiting for board" status and keeps retrying with backoff — the MQTT connection to the broker is independent of the board being on, so no MATLAB restart is needed when the board comes back.
- HiveMQ credentials are read via `getenv('HIVEMQ_PASSWORD')` (Windows user environment variable), not hardcoded in the script.
- Auto-launch is intended to be handled by **Windows Task Scheduler**: a `LogonTrigger` running `matlab.exe -nosplash -r "mqtt_dashboard"` as the interactive user (needed so the figure window can actually appear).

### Status: ⏳ Script verified interactively; Task Scheduler auto-launch not yet working

- [x] Live dashboard script written and run manually — confirmed connecting to HiveMQ and plotting live data correctly.
- [x] Reconnect/backoff and "board offline" handling implemented in the supervisor loop (not yet stress-tested end-to-end with the board actually power-cycled mid-session).
- [ ] **Task Scheduler auto-launch at logon — currently not firing.** Task has been imported, but MATLAB does not start automatically at system start. Not yet root-caused; next debugging pass should check the task's History tab for a logged error, confirm "Run only when user is logged on" is selected (a live figure needs an interactive session), and verify the `matlab.exe` path / working directory in the imported task match the real install.
- [ ] Decide on a license-independent long-term path: `MATLAB Compiler` (needed to produce a standalone `.exe` that runs on the free MATLAB Runtime without a license) is **not** in the current MATLAB toolbox install, so that option is blocked unless it can be added to the license. Alternatives under consideration: keep relying on a permanent/student MATLAB license for Task Scheduler + full `matlab.exe`, or move the live visualization to ThingSpeak (MathWorks' cloud IoT platform, which runs MATLAB visualizations cloud-side with no local license needed).

### Security Note

A HiveMQ Cloud password was at one point pasted in plaintext into a debugging session for this project. **That credential should be rotated in the HiveMQ Cloud console** and the new value used for the `HIVEMQ_PASSWORD` environment variable (and `/etc/mqtt-sensor.conf` on the board) going forward, if this hasn't been done already.

---

## Debugging Reference

### Hardware Verification

```bash
# Check FPGA bitstream is loaded
cat /sys/class/fpga_manager/fpga0/state
# Expected: operating

# Verify PL device tree nodes loaded
ls /sys/firmware/devicetree/base/amba_pl/
# Should list: i2c@41600000

# Check compatible string
cat /sys/firmware/devicetree/base/amba_pl/i2c@41600000/compatible

# Check interrupt property
cat /sys/firmware/devicetree/base/amba_pl/i2c@41600000/interrupts | hexdump -C
```

### Driver Debugging

```bash
# Check if I2C driver loaded
dmesg | grep -i "i2c\|xiic\|cdns"

# Check kernel config
zcat /proc/config.gz | grep CONFIG_I2C_XILINX

# Check Ethernet driver
zcat /proc/config.gz | grep CONFIG_MACB

# List loaded platform drivers
ls /sys/bus/platform/drivers/
```

### Network / Time Sync Diagnostics

```bash
# Check interface exists and link state (NO-CARRIER = cable unplugged)
ip link show

# Bring up eth0 and get a DHCP lease manually (run as root)
sudo su
udhcpc -i eth0
# Should print: bound to <IP address>, and DNS servers being added to /etc/resolv.conf

# Confirm internet reachability by IP
ping -c 4 8.8.8.8

# Check system clock (no battery-backed RTC on Zybo Z7-20 — boots with a stale/arbitrary time)
date

# Check hardware clock (expected to fail — confirms no RTC)
hwclock -r

# Force the clock to the real date/time — unblocks TLS testing without a rebuild
date -s "YYYY-MM-DD HH:MM:SS"

# Run the MQTT publisher manually and watch for connect/publish errors
/usr/bin/mqtt-sensor-app
# "Failed to connect, rc=-1" with a stale clock usually means TLS cert-date validation failed,
# not bad credentials — verify by forcing the date above and re-running

# Test the boot-time clock-sync fix WITHOUT rebooting: this filesystem is an
# in-memory initramfs (`mount | grep ' / '` shows `type rootfs`, not ext4), so
# nothing written via the shell survives a reboot anyway — a live reboot test
# only re-proves what's already baked into the flashed image. Instead, force a
# fresh DHCP negotiation and watch the same udhcpc code path boot would use:
date -s "2018-03-09 12:00:00"     # deliberately re-break the clock
ifdown eth0 && ifup eth0          # forces a real "bound" event
date                               # should self-correct if the fix is working
```

### Rebuild After Vivado Changes

```bash
petalinux-config --get-hw-description=/path/to/design.xsa

# Inspect generated device tree
cat components/plnl/device-tree/device-tree/pl.dtsi | grep -A 15 i2c

# Clean rebuild
petalinux-build -c device-tree -x cleansstate
petalinux-build
petalinux-package --boot --fsbl --fpga --u-boot --force
```

---

## Key Learnings

### PS-PL Architecture (Zynq-7000)

- **MIO pins** are hardwired to the PS and cannot be routed to Pmod connectors.
- **PL pins** require implementing peripherals (e.g., AXI IIC) in FPGA fabric.
- **EMIO** can route PS peripherals through the PL fabric to custom pins, but requires IOBUF primitives for bidirectional signals (SDA/SCL).
- **GEM0 (Ethernet)** is PS-native via MIO — no PL fabric needed.

### Device Tree and Interrupt Binding

A missing interrupt connection in Vivado (AXI IIC interrupt not wired to `IRQ_F2P`) caused the device tree node to be generated without an `interrupts` property, preventing the kernel driver from probing. Fix: connect `axi_iic_0:iic2intc_irpt → xlconcat → PS:IRQ_F2P[0]` in Vivado, re-export XSA, and rebuild.

### Debugging Methodology (Bottom-Up)

1. **Physical layer** — verify power, connections, pull-ups, cable links
2. **FPGA layer** — check bitstream is loaded, pins constrained
3. **Device tree layer** — verify node exists with correct properties
4. **Driver layer** — confirm driver is compiled (`=y`) and probing
5. **Application layer** — test with `i2cdetect` / `i2cget` / `ping`

### No-RTC Boards and TLS

Boards without a battery-backed RTC boot with an arbitrary/stale system clock. This is invisible for plaintext protocols but breaks any TLS client that validates certificate dates (`enableServerCertAuth`) — the handshake fails with a generic error (paho reported `rc=-1`) that doesn't obviously point to "wrong clock." `/etc/network/if-up.d/` hooks look like the obvious place to run one-shot time sync (e.g. `ntpdate`) the moment an interface gets a DHCP lease — but verify that mechanism actually fires on your image before trusting it: BusyBox's `ifup` applet doesn't execute those hooks at all, even though the directory and any installed hook scripts are present and individually functional. `/etc/udhcpc.d/` (run-parts'd by `udhcpc` itself, same place DNS servers get added) is the reliable equivalent on a BusyBox-based rootfs.

### Technical Achievements

- Debugged missing interrupt bindings by correlating Vivado hardware connections with auto-generated device tree nodes
- Identified and enabled missing `CONFIG_I2C_XILINX` kernel driver through config analysis
- Distinguished MIO (fixed) vs PL (flexible) pin architectures in Zynq
- Implemented PS I2C via EMIO with IOBUF primitives for bidirectional I/O
- Wrote SSD1306 OLED driver from datasheet, implementing I2C protocol, framebuffer management, and GDDRAM addressing
- Developed and cross-compiled a custom GPIO LED control app using `petalinux-create -t apps`


### Acknowledgements

- AI assistance (code generation, debugging guidance, project architecture) provided by [Claude](https://claude.ai) (Anthropic).
---

## Author

**brundamarpadaga** — [github.com/brundamarpadaga](https://github.com/brundamarpadaga)

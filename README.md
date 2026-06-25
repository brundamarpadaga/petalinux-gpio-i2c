# Embedded Linux Bring-Up on Zynq-7000 SoC (Zybo Z7-20)

A PetaLinux project that brings up embedded Linux on the Zybo Z7-20 board, integrating custom FPGA peripherals (AXI GPIO, AXI IIC) with Linux kernel drivers. Features LED control via PL GPIO and an SSD1306 OLED display driven over I2C.

**Current expansion in progress:** Adding BME280 temperature/humidity sensor + Ethernet to publish sensor data to the cloud via MQTT (HiveMQ).

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
- [IoT Expansion: BME280 + MQTT (In Progress)](#iot-expansion-bme280--mqtt-in-progress)
- [Debugging Reference](#debugging-reference)
- [Key Learnings](#key-learnings)

---

## Hardware Requirements

**Working:**
- Zybo Z7-20 development board
- MicroSD card (8 GB+)
- SSD1306 OLED display (I2C, address `0x3C`) connected to Pmod JD
- USB-UART cable for serial console

**To add for IoT expansion:**
- BME280 temperature/humidity sensor (I2C, address `0x76` or `0x77`)
- Cat5e/Cat6 Ethernet cable (any standard cable, connect to router/switch)

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

### Status: ⏳ Pending (need Ethernet cable)

---

## IoT Expansion: BME280 + MQTT (In Progress)

### Goal

Read temperature and humidity from a BME280 sensor over I2C and publish the data to HiveMQ cloud broker via MQTT over Ethernet, on a timed interval. Optionally display current readings on the SSD1306 OLED locally.

### Target Architecture

```
[BME280 sensor]
      │ I2C (same bus as OLED, or second Pmod)
      ▼
[PetaLinux C app: sensor_publisher]
      │ reads + formats JSON
      ▼
[MQTT publish via libpaho-mqtt]
      │ eth0
      ▼
[HiveMQ Cloud broker]
      │
      ▼
[Web dashboard / Grafana]
```

### Step-by-Step Plan

- [ ] **Step 1 — Verify Ethernet** (see [Ethernet Setup](#ethernet-setup) above — needs cable)
- [ ] **Step 2 — Wire BME280** to a Pmod connector (same I2C bus or a second one)
  - Pull SDO pin to GND → I2C address `0x76`
  - Pull SDO pin to VCC → I2C address `0x77`
- [ ] **Step 3 — Detect sensor on I2C bus**
  ```bash
  i2cdetect -y 1
  # Should show 0x76 or 0x77
  ```
- [ ] **Step 4 — Write BME280 userspace reader** using `i2c-dev`
  - Read raw ADC values from BME280 registers
  - Apply BME280 compensation formula from datasheet (pseudocode provided in datasheet section 4.2.3)
  - Same pattern as the SSD1306 app, but reading data instead of writing display commands
- [ ] **Step 5 — Add libpaho-mqtt to rootfs**
  ```bash
  petalinux-config -c rootfs
  # Search for: paho-mqtt
  # Enable: [*] paho-mqtt-c
  ```
  Or add a BitBake recipe (same pattern as `oled-gpio-app.bb`)
- [ ] **Step 6 — Write sensor_publisher.c**
  ```c
  // Pseudocode structure
  while (1) {
      float temp     = bme280_read_temperature(i2c_fd);
      float humidity = bme280_read_humidity(i2c_fd);

      // Format payload
      snprintf(payload, sizeof(payload),
               "{\"temperature\": %.2f, \"humidity\": %.2f}", temp, humidity);

      // Publish to HiveMQ
      MQTTClient_publishMessage(client, "zybo/sensors", &pubmsg, &token);

      sleep(10);
  }
  ```
- [ ] **Step 7 — Set up HiveMQ Cloud**
  - Free tier at console.hivemq.cloud
  - Create a cluster, get broker URL + credentials
  - Create topics: `zybo/sensors/temperature`, `zybo/sensors/humidity`
- [ ] **Step 8 — Test end-to-end**
  - Use HiveMQ web client to subscribe and watch live data
- [ ] **Step 9 (optional) — Display readings on OLED**
  - Show current temp/humidity on SSD1306 while simultaneously publishing to cloud

### Notes

- No Vivado changes needed for Ethernet — GEM0 is PS-native
- BME280 uses same I2C interface already proven with SSD1306
- `libcurl` is an alternative to paho-mqtt if HTTP/REST (e.g., ThingSpeak) is preferred

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

### Technical Achievements

- Debugged missing interrupt bindings by correlating Vivado hardware connections with auto-generated device tree nodes
- Identified and enabled missing `CONFIG_I2C_XILINX` kernel driver through config analysis
- Distinguished MIO (fixed) vs PL (flexible) pin architectures in Zynq
- Implemented PS I2C via EMIO with IOBUF primitives for bidirectional I/O
- Wrote SSD1306 OLED driver from datasheet, implementing I2C protocol, framebuffer management, and GDDRAM addressing
- Developed and cross-compiled a custom GPIO LED control app using `petalinux-create -t apps`


## Acknowledgements

AI assistance (code generation, debugging guidance, project architecture) provided by [Claude](https://claude.ai) (Anthropic).
---

## Author

**brundamarpadaga** — [github.com/brundamarpadaga](https://github.com/brundamarpadaga)

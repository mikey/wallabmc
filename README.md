![WallaBMC logo](img/logo.png)

# WallaBMC

## Overview

WallaBMC is a simple, lightweight Baseboard Management Controller (BMC) firmware suitable for STM32 and similar class microcontrollers. Built on the Zephyr RTOS, WallaBMC provides essential BMC functionality including network management, host power control, and web-based administration through a Redfish-compliant interface.

WallaBMC is designed for embedded systems requiring BMC capabilities without the complexity of full-featured BMC solutions. It provides core functionality for monitoring and managing host systems through industry-standard interfaces.

### Features

* **LED Status Indicators**: Visual feedback for system status
* **IPv4 Networking**: Static IP or DHCP with mDNS hostname resolution
* **Redfish Interface**: Industry-standard RESTful API for management
* **Web Interface**: HTTP/HTTPS web UI for administration
* **BMC Console**: Management console accessible via serial or web interface
* **Persistent Configuration**: Settings stored across reboots
* **Host Power Control**: Power on/off management for host systems
* **Host Console**: Serial console access

### Hardware Support

WallaBMC currently supports the following hardware platforms:

| Hardware | Zephyr board name | Description |
| --- | --- | --- |
| **SiFive HiFive Premier P550 MCU** | hifive_premier_p550_mcu | RISC-V based platform |
| **STM32 Nucleo F767ZI** | nucleo_f767zi | ARM Cortex-M7 development board (standalone, no host CPU) |
| **Espressif ESP32-C6 DevKitC** | esp32c6_devkitc/esp32c6/hpcore | RISC-V, Wi-Fi 6, wired to a SpacemiT K3 SoM260 host (see [below](#esp32-c6-devkitc--spacemit-k3-som260)) |
| **Seeed Studio XIAO ESP32-C6** | xiao_esp32c6/esp32c6/hpcore | Same SoC as above on a 21×17.5 mm board with USB-C; BMC console over USB Serial/JTAG (see [below](#seeed-xiao-esp32-c6)) |
| **qemu** | qemu_cortex_m3 | see [run_qemu_ci.py](scripts/run_qemu_ci.py) |

### Screenshot

The main page of the web interface is shown below, click to enlarge.

[![Web UI screenshot](img/web-ui-thumbnail.png)](img/web-ui.png)

### ESP32-C6 DevKitC + SpacemiT K3 SoM260

The ESP32-C6 port wires the DevKitC's GPIO header to a SpacemiT K3
SoM260 host so the K3 can be reset, powered, and console-monitored
remotely.

#### Wiring

Five wires between the ESP32-C6 DevKitC and the K3 SoM260:

| ESP32-C6 pin | Direction | K3 SoM260 signal | Notes |
| --- | --- | --- | --- |
| GPIO4 | out | host UART RX | UART1 TX — host serial console (115200 8N1) |
| GPIO5 | in | host UART TX | UART1 RX — host serial console |
| GPIO6 | out (open-drain) | PWRBTN# input | Active-low momentary press |
| GPIO7 | out (open-drain) | SYSRESET# input | Active-low momentary press |
| GND | — | GND | Common reference, required |

GPIO16/17 stay reserved for the BMC's own UART0 console (the
``cu.usbserial-…`` device when the DevKitC is plugged into a host PC).

> **Voltage warning.** The ESP32-C6 GPIOs are **not** 5 V tolerant.
> The PWRBTN# and SYSRESET# pins are driven open-drain, so they only
> sink — the K3's internal pull-up sets the un-asserted level, and the
> ESP32 never sources voltage onto these pins. If the K3 holds either
> line above 3.3 V when un-asserted, add a small N-MOSFET (gate = ESP32
> GPIO, drain = K3 pin, source = GND) between them. The UART1 RX line
> (GPIO5) does see the K3's TX voltage directly; if the K3 UART runs
> above 3.3 V, level-shift it.

#### Build and flash

This board uses Espressif's built-in Simple Boot rather than MCUboot,
so the build skips ``--sysbuild``. Wi-Fi credentials and (optionally) a
fixed admin password are supplied at build time via ``-D`` overrides:

```
# One-time: fetch the Espressif Wi-Fi/PHY binary blobs.
west blobs fetch hal_espressif

cd zephyr
west build -b esp32c6_devkitc/esp32c6/hpcore ../wallabmc --pristine \
    -- -DCONFIG_WIFI_CREDENTIALS_STATIC_SSID='"your-ssid"' \
       -DCONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD='"your-password"' \
       -DCONFIG_DEFAULT_ADMIN_PASSWORD='"admin"'
west flash
```

On boot the BMC associates to Wi-Fi, picks up a DHCPv4 lease, and logs
the address on the BMC console. Browse to ``http://<that-ip>/`` for
the web UI; the BMC shell is available there, on the BMC UART, or
(once the K3 is up) the host UART console is reachable via the web
UI's host-console terminal and TCP port 22.

#### Host control from the shell

```
power on          # 200 ms press on GPIO6 (if BMC believes host is off)
power off         # 200 ms press on GPIO6 (if BMC believes host is on)
power force-off   # 6 s press on GPIO6
reset             # 1 s press on GPIO7
```

### Seeed XIAO ESP32-C6

Same SoC as the DevKitC port above, on Seeed Studio's 21×17.5 mm XIAO
form factor with a USB-C connector. The BMC console runs over the SoC's
built-in USB Serial/JTAG (the XIAO's USB-C port), which frees UART0 for
the host serial bridge. Only four GPIOs on the XIAO connector are
needed for host control; the rest of the pads are free for other uses.

#### Wiring

Five wires between the XIAO ESP32-C6 and the host board:

| XIAO pin | GPIO | Direction | Host signal | Notes |
| --- | --- | --- | --- | --- |
| D6 | GPIO16 | out | host UART RX | UART0 TX — host serial console (115200 8N1) |
| D7 | GPIO17 | in | host UART TX | UART0 RX — host serial console |
| D1 | GPIO1 | out (open-drain) | PWRBTN# input | Active-low momentary press |
| D2 | GPIO2 | out (open-drain) | SYSRESET# input | Active-low momentary press |
| GND | — | — | GND | Common reference, required |

The same voltage caveats from the DevKitC port apply: ESP32-C6 GPIOs are
not 5 V tolerant, so use a level-shifting MOSFET on PWRBTN# / SYSRESET#
if the host pulls those lines above 3.3 V un-asserted, and level-shift
the host's UART TX into D7 (GPIO17) if it runs above 3.3 V.

#### Build and flash

Same Espressif Simple Boot / no-sysbuild flow as the DevKitC. Plug the
XIAO into your build host over USB-C — the same USB cable provides
power, flashes the firmware, and carries the BMC console:

```
# One-time: fetch the Espressif Wi-Fi/PHY binary blobs.
west blobs fetch hal_espressif

cd zephyr
west build -b xiao_esp32c6/esp32c6/hpcore ../wallabmc --pristine \
    -- -DCONFIG_WIFI_CREDENTIALS_STATIC_SSID='"your-ssid"' \
       -DCONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD='"your-password"' \
       -DCONFIG_DEFAULT_ADMIN_PASSWORD='"admin"'
west flash
```

Host control from the shell is the same as the DevKitC port:

```
power on          # 200 ms press on GPIO1 (if BMC believes host is off)
power off         # 200 ms press on GPIO1 (if BMC believes host is on)
power force-off   # 6 s press on GPIO1
reset             # 1 s press on GPIO2
```

## Using

### Prerequisites

Before getting started, ensure you have a proper Zephyr development environment. Follow the official [Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html).

Required tools:

* West (Zephyr's meta-tool)
* CMake (version 3.20.0 or later)
* Python 3
* A toolchain for your target platform (ARM or RISC-V)
* OpenOCD or appropriate flashing tool for your hardware


### Quick instructions (existing zephyr build env)

Clone the repo into your home dir

```
cd $HOME
git clone https://github.com/tenstorrent/wallabmc.git
```

Go to your zephyr build dir:

```
west build --sysbuild -b nucleo_f767zi ~/wallabmc
```
Where `nucleo_f767zi` can be replaced with the boards in [Hardware Support](#hardware-support).

See [flashing](#flashing) on how to install

### Installation

For those without a pre-existing zephyr build env

#### Initialize Workspace

The first step is to initialize the workspace folder where WallaBMC and all Zephyr modules will be cloned:

```
# Initialize workspace for WallaBMC (main branch)
west init -m https://github.com/tenstorrent/wallabmc.git --mr main workspace
# update Zephyr modules
cd workspace
west update
```

#### Building

To build the application, run the following command:

```
cd zephyr
west build --sysbuild -b nucleo_f767zi ../wallabmc
```

Where `nucleo_f767zi` can be replaced with the boards in [Hardware Support](#Hardware-Support)

### Supported boards:

See the [Hardware Support](#Hardware-Support) section.

### Flashing

```
west flash --runner openocd
```

Alternatively, use the `build/wallabmc/zephyr/zephyr.signed.hex` and
`build/mcuboot/zephyr/zephyr.hex` files, and run the openocd commands:

```
flash write_image erase build/mcuboot/zephyr/zephyr.hex
flash write_image erase build/wallabmc/zephyr/zephyr.signed.hex
```
Also see instructions on [flashing the p550](boards/sifive/hifive_premier_p550_mcu/support/README.md)

### Running

When the system has booted, a slow-blinking status LED indicates the system
is running.

The Nucleo exposes an STM32 UART as a serial device over the USB port.
WallaBMC puts the BMC console on this serial device that displays boot
and log messages, and can be used to query and configure the device.

WallaBMC supports networking over ethernet and by default uses DHCP with the
hostname ``wallabmc`` to get an IP address.

WallaBMC opens an HTTP (and possibly HTTPS) port, which provides Redfish and
Web UI. The Web UI can also access the BMC console.

## BMC shell

The Zephyr shell has been extended with wallabmc commands, and can be accessed
via the MCU serial console or the WebUI or websocket.

The websocket BMC shell endpoint URL is /console/bmc and supports ws and
wss if ``CONFIG_APP_HTTPS`` (e.g., ``wss://wallabmc.local.net/console/bmc``).

## Host serial console

The Nucleo has USART6 connected to pins D0/D1 RX/TX on the CN10 connector.
WallaBMC uses this as the host serial console which can be accessed with
the WebUI terminal or telnet port 22. The pins would have to be connected
to something useful (e.g., each other have a loopback UART that echoes back
what is transmitted to it).

The P550 host serial console UART is connected to an actual serial console
UART on the host CPU.

The websocket host console endpoint URL is /console/host and supports ws and
wss if ``CONFIG_APP_HTTPS`` (e.g., ``wss://wallabmc.local.net/console/host``).

### Settings and configuration

* ``help`` command listing with hierarchical help (e.g., ``help config``).
* ``config`` shell command can be used to configure the BMC.
* ``power`` can power the host on and off. On the Nucleo board there is no
  host CPU so one of the LEDs is a stand-in for a host power GPIO.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for information on how to contribute to this project.

## License

This project is licensed under the terms described in:

* [LICENSE](LICENSE) – code license
* [LICENSE_understanding.txt](LICENSE_understanding.txt) – license summary and clarification
* [LICENSE-DOCS](LICENSE-DOCS) – Creative Commons license for all documentation and logos

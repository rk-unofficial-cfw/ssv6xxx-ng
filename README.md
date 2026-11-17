# ssv6xxx-ng

A from-scratch rewrite of the Linux Wi-Fi driver for the **SSV6051**
(South Silicon Valley / iComm "Cabrio") 802.11b/g/n SDIO chipset.

The original vendor driver targets Android kernels 2.6–3.x, weighs in at
~30k lines and relies on many obsolete kernel interfaces. `ssv6xxx-ng`
is a clean mac80211 driver for modern kernels (**6.6 and later**), built
out-of-tree, with no kernel-version `#ifdef`s.

> **Status: experimental.** Station mode works through mac80211
> (scan/connect). The driver is under hardware bring-up; expect rough
> edges. See [TODO.md](TODO.md) for the roadmap and known gaps.

## Features

- Station mode only (scan, connect) via mac80211
- Complete SDIO transport: CMD52/CMD53, register and data I/O ports,
  SRAM writes, interrupt handling
- Firmware download with checksum verification
- eFuse readout: MAC address, SAR, crystal offset, TX power
- Hardware initialisation: packet-buffer allocation, IQK calibration
  (`HOST_CMD_INIT_CALI`), PHY/RF tables
- Hardware encryption for WEP40/104, TKIP and CCMP-128 (keys stored in
  chip SRAM); management frames are handled in software
- GCMP/SMS4 keys are rejected so mac80211 falls back to software crypto
- EDCA (`conf_tx`) queue parameters
- Firmware wedge recovery (`ieee80211_restart_hw`)
- Bit-mask debug logging and counters through debugfs

Not supported yet: AP / P2P, 802.11n aggregation (AMPDU), automatic rate
control, real RSSI reporting, host-side power save, WPA3/PMF. See
[TODO.md](TODO.md).

## Requirements

- Linux kernel **6.6 or later** with `CONFIG_MAC80211`, `CONFIG_MMC`
  and `CONFIG_FW_LOADER`
- Kernel headers for the running kernel (or a kernel build tree)
- An SSV6051 chip attached to an SDIO host
- The firmware image **`ssv6051-wifi.bin`**

The firmware is proprietary and **is not included in this repository**.
Install it as `/lib/firmware/ssv6051-wifi.bin`. A different file name can
be selected with the `fw_name` module parameter.

## Building

Out-of-tree, against the running kernel:

```sh
make
sudo make install
```

Against a specific kernel tree:

```sh
make KDIR=/path/to/kernel
```

Treat warnings as errors:

```sh
make W=1
```

The code builds warning-free with `W=1` and passes `checkpatch`.

### In-tree patch

A patch that adds the driver to a kernel source tree can be generated:

```sh
./gen-kernel-patch.sh /path/to/kernel out.patch
cd /path/to/kernel && patch -p1 < /path/to/out.patch
```

Then enable `CONFIG_SSV6051=m`.

## Usage

```sh
sudo modprobe ssv6051
```

The interface is then managed like any other Wi-Fi device (`iw`,
`wpa_supplicant`, NetworkManager, ...).

### Module parameters

| Parameter        | Description                                             |
|------------------|---------------------------------------------------------|
| `debug`          | Debug mask (see below)                                  |
| `fw_name`        | Firmware file name (default `ssv6051-wifi.bin`)         |
| `initmac`        | MAC address override, e.g. `00:11:22:33:44:55`          |
| `xtal_clock`     | Crystal frequency in MHz: 24, 26 or 40 (default 24)     |
| `volt_regulator` | 0 = DCDC, 1 = LDO (default 1)                           |
| `def_chan`       | Default channel 1–14 (default 6)                        |

By default the MAC address is read from the chip eFuse.

## Debugging

Debug output is selected with a bit mask (hexadecimal):

| Bit     | Area   | Bit     | Area   |
|---------|--------|---------|--------|
| `0x001` | SDIO   | `0x040` | REG    |
| `0x002` | HCI    | `0x080` | EFUSE  |
| `0x004` | FW     | `0x100` | IQK    |
| `0x008` | KEY    | `0x200` | INIT   |
| `0x010` | TX     | `0x020` | RX     |

```sh
sudo modprobe ssv6051 debug=0x3ff
# or at runtime:
echo 0x3ff | sudo tee /sys/module/ssv6051/parameters/debug
sudo dmesg -n 8
```

The default mask is `DBG_INIT | DBG_FW`.

Counters and a register dump are exposed through debugfs:

```
/sys/kernel/debug/ssv6051/stats
/sys/kernel/debug/ssv6051/regdump
```

## Source layout

| File         | Purpose                                                |
|--------------|--------------------------------------------------------|
| `main.c`     | SDIO probe/remove, mac80211 `ieee80211_ops`            |
| `sdio.c`     | SDIO transport                                         |
| `fw.c`       | Firmware loading and checksum                          |
| `efuse.c`    | eFuse parsing                                          |
| `init.c`     | Hardware init, calibration, PHY/RF tables              |
| `hci.c/.h`   | Host-firmware command/event protocol                   |
| `tx.c`/`rx.c`| Data path                                              |
| `key.c`      | Hardware key handling                                  |
| `debugfs.c`  | Statistics and register dump                           |
| `regs.h`     | Register definitions                                   |
| `tables.h`   | PHY/RF tables (copied verbatim from the original driver)|

The HCI structures in `hci.h` must stay bit-identical to the original
driver, because the firmware (build 12978) depends on their exact layout.

## Contributing

- Follow the kernel C coding style and keep `checkpatch` clean.
- New features should be added to [TODO.md](TODO.md) first.
- Please do not commit generated `*.patch` files.

## License and credits

Licensed under the GNU General Public License. Parts of the code are
derived from the original `ssv6xxx` driver:

- Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
- Copyright (c) 2015 iComm Corporation

The firmware is the property of its vendor and is not distributed here.

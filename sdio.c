// SPDX-License-Identifier: GPL-2.0
/*
 * SDIO protocol:
 *   - register access through regIOPort: write 4-byte address, read/write 4 bytes
 *   - data access through dataIOPort: CMD53 memcpy, length-aligned
 *   - SRAM write: address to 0xc0000860, Fn1 status handshake
 *   - packet length registers REG_CARD_PKT_LEN_0/1 before each RX read
 *
 * Based on the ssv6xxx driver:
 *   Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 *   Copyright (c) 2015 iComm Corporation
 */

#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/module.h>

#include "ssv6051.h"

#define SSV_VENDOR_ID   0x3030
#define SSV_CABRIO_DEVID 0x3030

#define SDIO_BLOCK_SIZE        0x80
#define SDIO_LOW_SPEED_CLOCK   25000000
#define SDIO_HIGH_SPEED_CLOCK  50000000

/* SDIO function-1 control registers (from hwif/sdio/sdio_def.h) */
#define REG_DATA_IO_PORT_0     0x00
#define REG_DATA_IO_PORT_1     0x01
#define REG_DATA_IO_PORT_2     0x02
#define REG_INT_MASK           0x04
#define REG_INT_STATUS         0x08
#define REG_INT_TRIGGER        0x09
#define REG_Fn1_STATUS         0x0c
#define REG_CARD_PKT_LEN_0     0x10
#define REG_CARD_PKT_LEN_1     0x11
#define REG_CARD_FW_DL_STATUS  0x12
#define REG_PMU_WAKEUP         0x67
#define REG_REG_IO_PORT_0      0x70
#define REG_REG_IO_PORT_1      0x71
#define REG_REG_IO_PORT_2      0x72
#define REG_OUTPUT_TIMING_REG  0x55
/* SDIO output drive timing; the reference platform config (ssv6051.cfg,
 * RK3128) sets CONFIG_PLATFORM_SDIO_OUTPUT_TIMING=3 and this value is
 * programmed at probe. 0 causes CRC errors on the large
 * CMD53 SRAM writes of the firmware download.
 */
#define SDIO_OUTPUT_TIMING     0x03

/* SRAM address-write register used by the SRAM write handshake */
#define SRAM_ADDR_REG          0xc0000860

#define MAX_RX_FRAME_SIZE      0x900

static const struct sdio_device_id ssv6051_sdio_devices[] = {
	{ SDIO_DEVICE(SSV_VENDOR_ID, SSV_CABRIO_DEVID) },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(sdio, ssv6051_sdio_devices);

/* The MMC core does not retry data requests: retry transient bus errors
 * (-ETIMEDOUT/-EILSEQ) at driver level. Safe: a timed-out request did not
 * reach the card, and a CRC-failed frame is discarded by the card, so the
 * whole transfer is simply redone from the start.
 */
#define SDIO_IO_RETRIES 3

static int ssv6051_sdio_memcpy(struct ssv6051_dev *sdev, bool write, u32 port,
			       void *buf, size_t len)
{
	int ret = 0, i;

	for (i = 0; i < SDIO_IO_RETRIES; i++) {
		if (write)
			ret = sdio_memcpy_toio(sdev->func, port, buf, len);
		else
			ret = sdio_memcpy_fromio(sdev->func, buf, port, len);
		if (ret != -ETIMEDOUT && ret != -EILSEQ)
			return ret;
		ssv_warn(sdev->dev,
			 "SDIO %s %s (%zu bytes, %d), retry %d/%d\n",
			 write ? "write" : "read",
			 ret == -ETIMEDOUT ? "timed out" : "CRC error",
			 len, ret, i + 1, SDIO_IO_RETRIES);
	}
	return ret;
}

static void ssv6051_sdio_claim(struct ssv6051_dev *sdev)
{
	sdio_claim_host(sdev->func);
}

static void ssv6051_sdio_release(struct ssv6051_dev *sdev)
{
	sdio_release_host(sdev->func);
}

/* Set the SDIO bus clock: low speed for the firmware download, high speed only
 * once the firmware is loaded.
 */
static void ssv6051_sdio_set_clock(struct ssv6051_dev *sdev, u32 hz)
{
	struct mmc_host *host = sdev->func->card->host;

	if (hz < host->f_min)
		hz = host->f_min;
	else if (hz > host->f_max)
		hz = host->f_max;

	/* Always visible: the clock must be verified
	 * against a known-good dmesg during bring-up.
	 */
	dev_err(sdev->dev, "set SDIO clock %u Hz\n", hz);
	sdio_claim_host(sdev->func);
	host->ios.clock = hz;
	host->ops->set_ios(host, &host->ios);
	mdelay(20);
	sdio_release_host(sdev->func);
}

int ssv6051_sdio_read_reg(struct ssv6051_dev *sdev, u32 addr, u32 *val)
{
	u8 data[4];
	int ret;

	ssv_dbg(DBG_REG, sdev->dev, "read reg 0x%08x\n", addr);

	ssv6051_sdio_claim(sdev);
	data[0] = (addr >> 0) & 0xff;
	data[1] = (addr >> 8) & 0xff;
	data[2] = (addr >> 16) & 0xff;
	data[3] = (addr >> 24) & 0xff;

	ret = sdio_memcpy_toio(sdev->func, sdev->reg_io_port, data, 4);
	if (ret) {
		ssv_err(sdev->dev, "SDIO reg read: address write failed (%d)\n",
			ret);
		goto out;
	}

	ret = ssv6051_sdio_memcpy(sdev, false, sdev->reg_io_port, data, 4);
	if (ret) {
		ssv_err(sdev->dev, "SDIO reg read: data read failed (%d)\n",
			ret);
		goto out;
	}

	*val = data[0] | (data[1] << 8) | (data[2] << 16) |
	       ((u32)data[3] << 24);
	ssv_dbg(DBG_REG, sdev->dev, "read reg 0x%08x -> 0x%08x\n", addr, *val);
out:
	if (ret)
		sdev->stats.sdio_io_errors++;
	ssv6051_sdio_release(sdev);
	return ret;
}

/* Lockless variant: caller must already hold the SDIO host claim. */
static int ssv6051_sdio_write_reg_locked(struct ssv6051_dev *sdev, u32 addr,
					 u32 val)
{
	u8 data[8];
	int ret;

	data[0] = (addr >> 0) & 0xff;
	data[1] = (addr >> 8) & 0xff;
	data[2] = (addr >> 16) & 0xff;
	data[3] = (addr >> 24) & 0xff;
	data[4] = (val >> 0) & 0xff;
	data[5] = (val >> 8) & 0xff;
	data[6] = (val >> 16) & 0xff;
	data[7] = (val >> 24) & 0xff;

	ret = ssv6051_sdio_memcpy(sdev, true, sdev->reg_io_port, data, 8);
	if (ret) {
		ssv_err(sdev->dev, "reg write 0x%08x <- 0x%08x failed (%d)\n",
			addr, val, ret);
		sdev->stats.sdio_io_errors++;
	}
	return ret;
}

int ssv6051_sdio_write_reg(struct ssv6051_dev *sdev, u32 addr, u32 val)
{
	int ret;

	ssv_dbg(DBG_REG, sdev->dev, "write reg 0x%08x <- 0x%08x\n", addr, val);

	ssv6051_sdio_claim(sdev);
	ret = ssv6051_sdio_write_reg_locked(sdev, addr, val);
	ssv6051_sdio_release(sdev);
	return ret;
}

int ssv6051_sdio_read_data(struct ssv6051_dev *sdev, void *buf, size_t *len)
{
	int ret;
	size_t pkt_len, readsize;

	ssv6051_sdio_claim(sdev);

	pkt_len = sdio_readb(sdev->func, REG_CARD_PKT_LEN_0, &ret);
	if (ret) {
		ssv_err(sdev->dev, "SDIO read: pkt len low byte failed (%d)\n",
			ret);
		goto out;
	}
	pkt_len |= (size_t)sdio_readb(sdev->func, REG_CARD_PKT_LEN_1, &ret) << 8;
	if (ret) {
		ssv_err(sdev->dev, "SDIO read: pkt len high byte failed (%d)\n",
			ret);
		goto out;
	}
	if (pkt_len == 0 || pkt_len > MAX_RX_FRAME_SIZE) {
		ssv_dbg(DBG_RX, sdev->dev, "SDIO read: bogus pkt len %zu\n",
			pkt_len);
		ret = -EIO;
		goto out;
	}

	readsize = sdio_align_size(sdev->func, pkt_len);
	ret = ssv6051_sdio_memcpy(sdev, false, sdev->data_io_port, buf,
				  readsize);
	if (ret) {
		ssv_err(sdev->dev, "SDIO read failed (%zu bytes, %d)\n",
			readsize, ret);
		sdev->stats.sdio_io_errors++;
		goto out;
	}
	*len = pkt_len;
	ssv_dbg(DBG_SDIO, sdev->dev, "SDIO read %zu bytes\n", pkt_len);
out:
	ssv6051_sdio_release(sdev);
	return ret;
}

int ssv6051_sdio_write_data(struct ssv6051_dev *sdev, void *buf, size_t len)
{
	int ret;
	size_t writesize;

	ssv6051_sdio_claim(sdev);
	writesize = sdio_align_size(sdev->func, len);
	ret = ssv6051_sdio_memcpy(sdev, true, sdev->data_io_port, buf,
				  writesize);
	if (ret == -EILSEQ || ret == -ETIMEDOUT) {
		ssv_err(sdev->dev, "SDIO write bus error (%zu bytes, %d)\n",
			writesize, ret);
		ret = -EIO;
	} else if (ret) {
		ssv_err(sdev->dev, "SDIO write failed (%zu bytes, %d)\n",
			writesize, ret);
	}
	if (ret)
		sdev->stats.sdio_io_errors++;
	else
		ssv_dbg(DBG_SDIO, sdev->dev, "SDIO wrote %zu bytes\n", len);
	ssv6051_sdio_release(sdev);
	return ret;
}

int ssv6051_sdio_write_sram(struct ssv6051_dev *sdev, u32 addr, u8 *data,
			    u32 size)
{
	int ret;

	ssv6051_sdio_claim(sdev);
	/* Use the lockless variant: the caller holds the claim. */
	ret = ssv6051_sdio_write_reg_locked(sdev, SRAM_ADDR_REG, addr);
	if (ret)
		goto out;
	sdio_writeb(sdev->func, 0x2, REG_Fn1_STATUS, &ret);
	if (ret)
		goto out;
	ret = ssv6051_sdio_memcpy(sdev, true, sdev->data_io_port, data, size);
	if (ret)
		goto out;
	sdio_writeb(sdev->func, 0, REG_Fn1_STATUS, &ret);
out:
	if (ret)
		ssv_err(sdev->dev, "SRAM write failed (0x%08x, %u, %d)\n",
			addr, size, ret);
	ssv6051_sdio_release(sdev);
	return ret;
}

/* SDIO function-1 control registers (INT_MASK/INT_STATUS/...) are accessed
 * with CMD52 byte I/O at the raw byte address, not through regIOPort.
 */
int ssv6051_sdio_read_byte(struct ssv6051_dev *sdev, u32 addr, u8 *val)
{
	int ret;

	ssv6051_sdio_claim(sdev);
	*val = sdio_readb(sdev->func, addr, &ret);
	ssv6051_sdio_release(sdev);
	return ret;
}

int ssv6051_sdio_write_byte(struct ssv6051_dev *sdev, u32 addr, u8 val)
{
	int ret;

	ssv6051_sdio_claim(sdev);
	sdio_writeb(sdev->func, val, addr, &ret);
	ssv6051_sdio_release(sdev);
	return ret;
}

int ssv6051_sdio_irq_status(struct ssv6051_dev *sdev, u32 *status)
{
	u8 val;
	int ret;

	ret = ssv6051_sdio_read_byte(sdev, REG_INT_STATUS, &val);
	if (ret == 0)
		*status = val;
	return ret;
}

int ssv6051_sdio_pmu_wakeup(struct ssv6051_dev *sdev)
{
	int ret;

	ssv6051_sdio_claim(sdev);
	sdio_writeb(sdev->func, 0x01, REG_PMU_WAKEUP, &ret);
	mdelay(10);
	sdio_writeb(sdev->func, 0x00, REG_PMU_WAKEUP, &ret);
	ssv6051_sdio_release(sdev);
	return ret;
}

/* Read the I/O port addresses the card exposes in its own registers. */
static void ssv6051_sdio_read_ports(struct ssv6051_dev *sdev)
{
	struct sdio_func *func = sdev->func;
	int err;

	ssv6051_sdio_claim(sdev);
	sdev->data_io_port =
		(u32)sdio_readb(func, REG_DATA_IO_PORT_0, &err) |
		((u32)sdio_readb(func, REG_DATA_IO_PORT_1, &err) << 8) |
		((u32)sdio_readb(func, REG_DATA_IO_PORT_2, &err) << 16);
	sdev->reg_io_port =
		(u32)sdio_readb(func, REG_REG_IO_PORT_0, &err) |
		((u32)sdio_readb(func, REG_REG_IO_PORT_1, &err) << 8) |
		((u32)sdio_readb(func, REG_REG_IO_PORT_2, &err) << 16);
	/* Also program the platform output timing
	 * (0x55) and reset Fn1_STATUS before any further I/O.
	 */
	sdio_writeb(func, SDIO_OUTPUT_TIMING, REG_OUTPUT_TIMING_REG, &err);
	sdio_writeb(func, 0x00, REG_Fn1_STATUS, &err);
	ssv6051_sdio_release(sdev);

	/* Always visible: the values must be compared
	 * against a known-good dmesg when the bring-up misbehaves.
	 */
	dev_err(sdev->dev, "dataIOPort 0x%08x regIOPort 0x%08x\n",
		sdev->data_io_port, sdev->reg_io_port);
}

static void ssv6051_sdio_irq_handler(struct sdio_func *func)
{
	struct ssv6051_dev *sdev = sdio_get_drvdata(func);

	if (sdev && sdev->started)
		ssv6051_hci_irq_handler(sdev);
}

/* Claim/release the SDIO function IRQ. Called from hci_start/hci_stop: the card
 * interrupt is armed only once the firmware is up and running.
 */
int ssv6051_sdio_irq_attach(struct ssv6051_dev *sdev)
{
	int ret;

	/* sdio_claim_irq() issues CMD52 without taking the host itself
	 * (mmc_wait_for_cmd WARNs on !host->claimed): hold it here.
	 */
	sdio_claim_host(sdev->func);
	ret = sdio_claim_irq(sdev->func, ssv6051_sdio_irq_handler);
	sdio_release_host(sdev->func);
	return ret;
}

void ssv6051_sdio_irq_detach(struct ssv6051_dev *sdev)
{
	sdio_claim_host(sdev->func);
	sdio_release_irq(sdev->func);
	sdio_release_host(sdev->func);
}


static int ssv6051_sdio_probe(struct sdio_func *func,
			      const struct sdio_device_id *id)
{
	struct ieee80211_hw *hw;
	struct ssv6051_dev *sdev;
	int ret;

	ssv_dbg(DBG_INIT, &func->dev, "SDIO probe: vendor 0x%04x device 0x%04x\n",
		func->vendor, func->device);

	/* Set these quirks before any I/O. */
	func->card->quirks |= MMC_QUIRK_LENIENT_FN0;
	func->card->quirks |= MMC_QUIRK_BLKSZ_FOR_BYTE_MODE;

	/* Allocate mac80211 first: hw->priv is the single sdev instance. */
	hw = ssv6051_alloc_mac80211();
	if (!hw) {
		ssv_err(&func->dev, "ieee80211_alloc_hw failed\n");
		return -ENOMEM;
	}
	sdev = hw->priv;
	sdev->hw = hw;
	sdev->dev = &func->dev;
	sdev->func = func;
	sdev->cfg.fw_name = ssv6051_firmware_name;
	sdev->cfg.channel = 1;
	/* Hardware settings: crystal type,
	 * voltage regulator, default channel. */
	if (ssv6051_xtal_clock == 40)
		sdev->cfg.crystal_type = SSV6051_IQK_CFG_XTAL_40M;
	else if (ssv6051_xtal_clock == 24)
		sdev->cfg.crystal_type = SSV6051_IQK_CFG_XTAL_24M;
	else
		sdev->cfg.crystal_type = SSV6051_IQK_CFG_XTAL_26M;
	sdev->cfg.volt_regulator = ssv6051_volt_regulator ? 1 : 0;
	sdev->cfg.def_chan = clamp(ssv6051_def_chan, 1, 14);
	/* Default TX scales, overwritten by the ADR_TX_GAIN_FACTOR entry
	 * in ssv6051_hw_tables_init (init_iqk_cfg defaults). */
	sdev->cfg.def_tx_scale_11b = 0x75;
	sdev->cfg.def_tx_scale_11b_p0d5 = 0x75;
	sdev->cfg.def_tx_scale_11g = 0x80;
	sdev->cfg.def_tx_scale_11g_p0d5 = 0x80;
	sdev->hw_wsid = -1;
	sdev->associated = false;
	mutex_init(&sdev->mem_lock);

	sdio_set_drvdata(func, sdev);

	/* Firmware download runs at low speed; switch to
	 * high speed only once the firmware is in place.
	 */
	ssv6051_sdio_set_clock(sdev, SDIO_LOW_SPEED_CLOCK);

	/* Wake the card PMU before touching the function (with the
	 * card core powered down the MCU never starts and the
	 * firmware handshake never completes).
	 */
#ifdef CONFIG_PM
	ret = ssv6051_sdio_pmu_wakeup(sdev);
	if (ret)
		ssv_warn(&func->dev, "PMU wakeup failed (%d)\n", ret);
#endif

	/* All SDIO I/O requires the host to be claimed. The driver's I/O
	 * helpers claim it themselves (nesting is allowed), but the direct
	 * core calls below do not. Hold the host for the whole bring-up.
	 */
	sdio_claim_host(func);

	ret = sdio_enable_func(func);
	if (ret) {
		ssv_err(&func->dev, "SDIO enable failed (%d)\n", ret);
		goto err_free_hw;
	}
	/* Wait 10ms after enabling the function. */
	usleep_range(10000, 15000);
	ret = sdio_set_block_size(func, SDIO_BLOCK_SIZE);
	if (ret) {
		ssv_err(&func->dev, "SDIO set block size failed (%d)\n", ret);
		goto err_disable;
	}

	ssv6051_sdio_read_ports(sdev);

	/* Read the chip identity registers: read-only, never written by the
	 * host, so a sane ASCII value here proves the register read path
	 * works.
	 */
	{
		u32 id[4];
		int i;
		u8 chip_id[17];

		/* Assemble CHIP_ID_3 first, each
		 * word byte-swapped (LONGSWAP/bswap).
		 */
		for (i = 0; i < 4; i++) {
			if (ssv6051_sdio_read_reg(sdev, ADR_CHIP_ID_3 - i * 4,
						  &id[i]))
				break;
			((u32 *)chip_id)[i] = cpu_to_be32(id[i]);
		}
		chip_id[16] = 0;
		if (i == 4)
			dev_err(sdev->dev, "chip id: %.16s\n", chip_id);
		else
			dev_err(sdev->dev,
				"chip id read failed at word %d\n", i);
	}

	/* Efuse read before the register bring-up. */
	ssv6051_efuse_read_all_map(sdev);

	/* MAC/HCI register bring-up before the firmware download.
	 */
	ret = ssv6051_pre_fw_init(sdev);
	if (ret) {
		ssv_err(&func->dev, "pre-fw init failed (%d)\n", ret);
		goto err_disable;
	}

	/* No SDIO IRQ handler during the firmware download: the IRQ
	 * is claimed only at HCI start (after firmware and HW init). Claiming it here would arm the card's
	 * function interrupt (CCCR IntEnable) while the card CPU reboots
	 * for the checksum handshake.
	 */

	/* Mandatory bring-up sequence: HCI -> firmware -> efuse -> HW init.
	 */
	ret = ssv6051_hci_init(sdev);
	if (ret)
		goto err_disable;

	ret = ssv6051_load_firmware(sdev);
	if (ret)
		goto err_hci_deinit;

	/* Firmware is in place: switch to high speed. */
	ssv6051_sdio_set_clock(sdev, SDIO_HIGH_SPEED_CLOCK);

	ret = ssv6051_init_hw(sdev);
	if (ret)
		goto err_hci_deinit;

	ret = ssv6051_register_mac80211(sdev);
	if (ret)
		goto err_hci_deinit;

	ssv6051_debugfs_init(sdev);

	sdio_release_host(func);
	return 0;

err_hci_deinit:
	/* The SDIO IRQ is claimed at HCI start (after this point), so no
	 * handler can be running here and the workqueue is idle.
	 */
	ssv6051_deinit_hw(sdev);
	ssv6051_hci_deinit(sdev);
err_disable:
	sdio_disable_func(func);
err_free_hw:
	sdio_release_host(func);
	ieee80211_free_hw(hw);
	return ret;
}

static void ssv6051_sdio_remove(struct sdio_func *func)
{
	struct ssv6051_dev *sdev = sdio_get_drvdata(func);

	ssv_dbg(DBG_INIT, &func->dev, "SDIO remove\n");
	if (!sdev)
		return;

	ssv6051_debugfs_deinit(sdev);
	/* unregister_mac80211 runs the stop op, which masks the card
	 * interrupt sources (hci_stop). */
	ssv6051_unregister_mac80211(sdev);

	/* Detach the SDIO IRQ BEFORE tearing down the workqueue: a rx_work
	 * in flight would block on sdio_claim_host() held by this thread
	 * and cancel_work_sync() would deadlock. */
	sdio_claim_host(func);
	sdio_release_irq(func);
	sdio_release_host(func);

	/* Cancel the workers, drain the TX queues, free the RX buffer. */
	ssv6051_hci_deinit(sdev);

	/* Quiesce the chip: RF power down and security key pbuf back to
	 * the pool, then let the MCU sleep before the function is
	 * disabled. */
	ssv6051_deinit_hw(sdev);
	ssv6051_hci_fw_sleep(sdev);

	/* Back to the low download clock, then disable the function. */
	ssv6051_sdio_set_clock(sdev, SDIO_LOW_SPEED_CLOCK);
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);

	sdio_set_drvdata(func, NULL);
	ieee80211_free_hw(sdev->hw);
}

static struct sdio_driver ssv6051_sdio_driver = {
	.name = SSV6051_DRV_NAME,
	.id_table = ssv6051_sdio_devices,
	.probe = ssv6051_sdio_probe,
	.remove = ssv6051_sdio_remove,
};

int __init ssv6051_sdio_register(void)
{
	return sdio_register_driver(&ssv6051_sdio_driver);
}

void __exit ssv6051_sdio_unregister(void)
{
	sdio_unregister_driver(&ssv6051_sdio_driver);
}

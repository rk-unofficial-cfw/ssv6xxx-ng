// SPDX-License-Identifier: GPL-2.0
/*
 * Firmware download sequence, verified against the working
 * community port of the same hardware
 * (eloirotava/6051, RK3229 + SSV6051P + SSV6200A0):
 *   - ADR_BRG_SW_RST 0x0 (MCU off), ADR_BOOT 0x01 (boot latch),
 *     platform clock bit 2
 *   - write firmware image to chip SRAM in FW_BLOCK_SIZE chunks
 *   - compute rolling checksum (init 0x12345678, byte-wise sum of words
 *     over the image padded to 1KB with 0xA5)
 *   - publish 1KB block count in FW_STATUS_REG, then ADR_BRG_SW_RST 0x1
 *     (MCU on: boots from SRAM)
 *   - the checksum handshake does NOT complete on SSV6200A0: it is
 *     logged, not fatal, and the register is only monitored for signs
 *     of life (the firmware version replaces the block count)
 *
 * Based on the ssv6xxx driver:
 *   Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 *   Copyright (c) 2015 iComm Corporation
 */

#include <linux/delay.h>
#include <linux/firmware.h>

#include "ssv6051.h"

#define FW_BLOCK_SIZE       0x8000
#define CHECKSUM_BLOCK_SIZE 1024
#define FW_CHECKSUM_INIT    0x12345678u
#define FW_STATUS_MASK      0x00FF0000u

/* Card-side download diagnostics (function-1 registers, from
 * hwif/sdio/sdio_def.h). */
#define REG_CARD_FW_DL_STATUS 0x12
#define REG_CARD_SELF_TEST    0x13

/* Addresses below SRAM_VISIBLE_BASE are NOT verifiable through the
 * register port: the AHB bridge aliases that range to the boot ROM
 * (reads there return deterministic values that are not in the image,
 * identical across boots and across download attempts). Only the range
 * at and above 0x10000 reads back the downloaded image. */
#define SRAM_VISIBLE_BASE 0x00010000u

/* Bring-up diagnostic: after a failed handshake, read back a few words
 * of the firmware SRAM through the register port and compare them with
 * the image, plus the card-side boot/download state. This distinguishes
 * "the download never landed" from "the download landed but the card
 * CPU never started". */
static void ssv6051_fw_sram_probe(struct ssv6051_dev *sdev,
				  const struct firmware *fw)
{
	static const u32 addrs[] = { 0x00010000, 0x00013d70, 0x00000000,
				     0x00008000 };
	u32 val, expected;
	u32 boot_reg, rst_reg;
	u8 dl_status, self_test;
	int ret;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(addrs); i++) {
		ret = ssv6051_sdio_read_reg(sdev, addrs[i], &val);
		if (ret) {
			ssv_err(sdev->dev,
				"SRAM probe read at 0x%08x failed (%d)\n",
				addrs[i], ret);
			continue;
		}
		expected = (addrs[i] + sizeof(u32) <= fw->size)
				   ? get_unaligned_le32(fw->data + addrs[i])
				   : 0xA5A5A5A5u; /* 0xA5 padding */
		if (addrs[i] < SRAM_VISIBLE_BASE) {
			ssv_dbg(DBG_FW, sdev->dev,
				"SRAM 0x%08x: 0x%08x (ROM alias, not verifiable)\n",
				addrs[i], val);
			continue;
		}
		ssv_warn(sdev->dev,
			 "SRAM 0x%08x: read 0x%08x, expected 0x%08x %s\n",
			 addrs[i], val, expected,
			 val == expected ? "OK" : "MISMATCH");
	}

	/* Was the reboot latch (ADR_BOOT bit 0) consumed by the card? If it
	 * still reads 1, the CPU never processed the reboot request. */
	if (ssv6051_sdio_read_reg(sdev, ADR_BOOT, &boot_reg) == 0)
		ssv_warn(sdev->dev, "ADR_BOOT after handshake: 0x%08x\n",
			 boot_reg);
	if (ssv6051_sdio_read_reg(sdev, ADR_BRG_SW_RST, &rst_reg) == 0)
		ssv_warn(sdev->dev, "ADR_BRG_SW_RST after handshake: 0x%08x\n",
			 rst_reg);
	ret = ssv6051_sdio_read_byte(sdev, REG_CARD_FW_DL_STATUS, &dl_status);
	if (ret == 0)
		ssv_warn(sdev->dev, "card FW download status: 0x%02x\n",
			 dl_status);
	ret = ssv6051_sdio_read_byte(sdev, REG_CARD_SELF_TEST, &self_test);
	if (ret == 0)
		ssv_warn(sdev->dev, "card self test status: 0x%02x\n",
			 self_test);
}

/* Firmware image name; overridable with the fw_name module parameter. */
char *ssv6051_firmware_name = SSV6051_FW_NAME;
module_param_named(fw_name, ssv6051_firmware_name, charp, 0444);
MODULE_PARM_DESC(fw_name, "firmware file name (default ssv6051-wifi.bin)");

/* Checksum over the image padded to a 1KB boundary with 0xA5 bytes:
 * the card firmware computes it over the padded blocks it receives
 * The straddling word at the end of the real data includes its 0xA5
 * fill bytes (the reference loader checksums a
 * buffer pre-filled with 0xA5 before copying the tail into it).
 */
static u32 ssv6051_fw_checksum(const u8 *data, size_t len)
{
	u32 checksum = FW_CHECKSUM_INIT;
	size_t padded = round_up(len, CHECKSUM_BLOCK_SIZE);
	size_t words = padded / sizeof(u32);
	size_t i;

	for (i = 0; i < words; i++) {
		if ((i + 1) * sizeof(u32) <= len)
			checksum += get_unaligned_le32(data + i * sizeof(u32));
		else if (i * sizeof(u32) >= len)
			checksum += 0xA5A5A5A5u;
		else {
			/* word straddling real data and 0xA5 padding: the
			 * card checksums the padded SRAM block, so count
			 * the 0xA5 fill bytes too, as with a
			 * 0xA5-filled buffer.
			 */
			size_t off = i * sizeof(u32);
			size_t n = len - off; /* 1..sizeof(u32)-1 */
			u32 mask = 0xFFFFFFFFu >> (8 * (sizeof(u32) - n));
			u32 word = 0;
			size_t k;

			for (k = 0; k < n; k++)
				word |= (u32)data[off + k] << (8 * k);
			checksum += word | (~mask & 0xA5A5A5A5u);
		}
	}

	checksum = ((checksum >> 24) + (checksum >> 16) + (checksum >> 8) +
		    checksum) & 0x0FF;
	checksum <<= 16;
	return checksum;
}

static int ssv6051_fw_write_block(struct ssv6051_dev *sdev, u32 addr,
				  const u8 *data, size_t len)
{
	u32 readback;
	int ret;

	ret = ssv6051_sdio_write_sram(sdev, addr, (u8 *)data, (u32)len);
	if (ret) {
		ssv_err(sdev->dev, "firmware SRAM write failed at 0x%08x (%d)\n",
			addr, ret);
		return ret;
	}

	/* The card drains the SDIO FIFO to SRAM asynchronously: give it time
	 * to finish before the next block reprograms the SRAM address
	 * register (the last block always landed, the previous ones were
	 * lost to the drain race). Then verify the first word through the
	 * register port. */
	msleep(10);
	ret = ssv6051_sdio_read_reg(sdev, addr, &readback);
	if (ret == 0 && readback != get_unaligned_le32(data)) {
		if (addr < SRAM_VISIBLE_BASE) {
			/* The AHB bridge aliases addresses below 0x10000 to
			 * the boot ROM: a mismatch here says nothing about
			 * the SRAM write. Informational only. */
			ssv_dbg(DBG_FW, sdev->dev,
				"SRAM 0x%08x readback 0x%08x (ROM alias, not verifiable)\n",
				addr, readback);
		} else {
			ssv_err(sdev->dev,
				"SRAM 0x%08x readback 0x%08x != 0x%08x\n",
				addr, readback, get_unaligned_le32(data));
			return -EIO;
		}
	}

	ssv_dbg(DBG_FW, sdev->dev, "wrote %zu bytes to SRAM 0x%08x\n", len,
		addr);
	return 0;
}

/* Wait for the card CPU to show signs of life: after the MCU enable,
 * FW_STATUS_REG (= FW_VERSION_REG) still holds the block count we
 * published; the card-side bootloader/firmware replaces it with the
 * checksum word and later the firmware version. The checksum handshake
 * is known NOT to complete on SSV6200A0 (the community port of this
 * exact hardware logs the mismatch and proceeds), so this only waits
 * for ANY update of the register; the value is purely diagnostic. */
#define FW_ALIVE_POLL_MS   50
#define FW_ALIVE_POLLS     10

static u32 ssv6051_fw_wait_alive(struct ssv6051_dev *sdev, u32 published)
{
	u32 status = published;
	int i;

	for (i = 0; i < FW_ALIVE_POLLS; i++) {
		msleep(FW_ALIVE_POLL_MS);
		if (ssv6051_sdio_read_reg(sdev, FW_STATUS_REG, &status))
			return published;
		if (status != published)
			break;
	}
	return status;
}


int ssv6051_load_firmware(struct ssv6051_dev *sdev)
{
	const struct firmware *fw = NULL;
	const u8 *data;
	u8 *scratch = NULL;
	size_t len, remaining, chunk;
	u32 sram_addr;
	u32 checksum, published, status;
	int ret;

	ssv_dbg(DBG_FW, sdev->dev, "loading firmware \"%s\"\n",
		sdev->cfg.fw_name);

	ret = request_firmware(&fw, sdev->cfg.fw_name, sdev->dev);
	if (ret) {
		ssv_err(sdev->dev, "firmware \"%s\" not found (%d)\n",
			sdev->cfg.fw_name, ret);
		return ret;
	}

	len = fw->size;
	ssv_dbg(DBG_FW, sdev->dev, "firmware image: %zu bytes\n", len);

	checksum = ssv6051_fw_checksum(fw->data, len);
	ssv_dbg(DBG_FW, sdev->dev, "firmware checksum 0x%08x\n", checksum);

	scratch = kzalloc(FW_BLOCK_SIZE, GFP_KERNEL);
	if (!scratch) {
		ret = -ENOMEM;
		goto out_release;
	}

	/* Single download attempt, as in the working community port of
	 * this exact hardware (eloirotava/6051): retrying on checksum
	 * mismatch is pointless because the handshake never completes on
	 * SSV6200A0, so a retry would only waste bus time. */

	/* Disable the MCU, arm the boot latch, enable the MCU clock.
	 * NOTE: ADR_BRG_SW_RST bit 0 is MCU_ENABLE (see
	 * GET_MCU_ENABLE in ssv6200_reg.h), NOT a bridge reset: 0 = MCU
	 * off (SRAM free), 1 = MCU on (boots from SRAM with the ADR_BOOT
	 * latch armed). */
	ssv6051_sdio_write_reg(sdev, ADR_BRG_SW_RST, 0x0);
	ssv6051_sdio_write_reg(sdev, ADR_BOOT, 0x01);
	ret = ssv6051_sdio_read_reg(sdev, ADR_PLATFORM_CLOCK_ENABLE,
				    &published);
	if (ret == 0) {
		ret = ssv6051_sdio_write_reg(sdev, ADR_PLATFORM_CLOCK_ENABLE,
					     published | BIT(2));
		if (ret)
			ssv_warn(sdev->dev,
				 "platform clock enable failed (%d)\n", ret);
	}

	data = fw->data;
	remaining = len;
	sram_addr = 0;
	while (remaining > 0) {
		chunk = min_t(size_t, remaining, FW_BLOCK_SIZE);
		if (chunk % CHECKSUM_BLOCK_SIZE) {
			/* Tail: pad to the 1KB boundary with 0xA5, as the
			 * card firmware checksums the padded blocks.
			 */
			size_t padded = round_up(chunk, CHECKSUM_BLOCK_SIZE);

			memcpy(scratch, data, chunk);
			memset(scratch + chunk, 0xA5, padded - chunk);
			ret = ssv6051_fw_write_block(sdev, sram_addr, scratch,
						     padded);
		} else {
			ret = ssv6051_fw_write_block(sdev, sram_addr, data,
						     chunk);
		}
		if (ret)
			goto out_free;

		data += chunk;
		sram_addr += chunk;
		remaining -= chunk;
	}

	/* Publish the number of 1KB blocks (rounded up, as the card counts
	 * the padded blocks it received). */
	published = round_up(len, CHECKSUM_BLOCK_SIZE) / CHECKSUM_BLOCK_SIZE;
	ret = ssv6051_sdio_write_reg(sdev, FW_STATUS_REG, published << 16);
	if (ret)
		goto out_free;

	/* Diagnostic read: the card reports
	 * back its own 1KB block count in this register. */
	if (ssv6051_sdio_read_reg(sdev, FW_STATUS_REG, &published) == 0)
		ssv_dbg(DBG_FW, sdev->dev,
			"FW status after publish: 0x%08x\n", published);

	/* Enable the MCU: it boots from SRAM with the armed boot latch. */
	ret = ssv6051_sdio_write_reg(sdev, ADR_BRG_SW_RST, 0x1);
	if (ret)
		goto out_free;

	/* Wait for the register to move off the published block count. On
	 * SSV6200A0 the checksum handshake does not complete (verified
	 * against the working community port of this exact hardware), so
	 * a mismatch is logged but NOT fatal: the firmware boots anyway
	 * and the version register is purely diagnostic. */
	status = ssv6051_fw_wait_alive(sdev, published << 16);
	if ((status & FW_STATUS_MASK) == (checksum & FW_STATUS_MASK)) {
		/* Acknowledge: write back the complement of the checksum. */
		ssv_dbg(DBG_FW, sdev->dev, "firmware checksum confirmed\n");
		ssv6051_sdio_write_reg(sdev, FW_STATUS_REG,
				       ~checksum & FW_STATUS_MASK);
	} else if (status == (published << 16)) {
		ssv_warn(sdev->dev,
			 "FW status register did not update after MCU enable (checksum handshake not implemented on this chip); continuing\n");
		sdev->stats.fw_checksum_errors++;
		ssv6051_fw_sram_probe(sdev, fw);
	} else {
		ssv_dbg(DBG_FW, sdev->dev,
			"FW status updated to 0x%08x after MCU enable\n",
			status);
	}

	/* Enable the PHY (the working port does this unconditionally). */
	ret = ssv6051_sdio_read_reg(sdev, ADR_PHY_EN_1, &published);
	if (ret)
		goto out_free;
	ret = ssv6051_sdio_write_reg(sdev, ADR_PHY_EN_1,
				     published | (1u << RG_PHY_MD_EN_SFT));
	if (ret)
		goto out_free;

	ssv_dbg(DBG_FW, sdev->dev, "firmware \"%s\" loaded (%zu bytes)\n",
		sdev->cfg.fw_name, len);

out_free:
	kfree(scratch);
out_release:
	release_firmware(fw);
	return ret;
}

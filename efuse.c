// SPDX-License-Identifier: GPL-2.0
/*
 * eFuse read sequence:
 *   - enable efuse controller (0xC0000328 <- 0x11)
 *   - read chip identity from SSV_EFUSE_ID_RAW_DATA_BASE
 *   - read the raw map section by section through SSV_EFUSE_READ_SWITCH /
 *     SSV_EFUSE_RAW_DATA_BASE
 *   - parse the map into calibration values and MAC address
 *   - restore the efuse controller mode on exit
 *
 * Based on the ssv6xxx driver:
 *   Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 *   Copyright (c) 2015 iComm Corporation
 */

#include <linux/etherdevice.h>

#include "ssv6051.h"

#define EFUSE_CTRL_MODE_REG   0xC0000328
#define EFUSE_CTRL_MODE_ON    0x11
#define EFUSE_CTRL_MODE_OFF   0x1800000Au

/* Item table layout: { offset, byte_cnts }
 * indexed by enum ssv6051_efuse_item_id. */
static const struct {
	u8 offset;
	u8 byte_cnts;
} ssv6051_efuse_layout[SSV6051_EFUSE_ITEMS] = {
	[EFUSE_R_CALIBRATION_RESULT]     = { 4, 0 },
	[EFUSE_SAR_RESULT]               = { 4, 8 },
	[EFUSE_MAC]                      = { 4, 8 },
	[EFUSE_CRYSTAL_FREQUENCY_OFFSET] = { 4, 48 },
	[EFUSE_TX_POWER_INDEX_1]         = { 4, 8 },
	[EFUSE_TX_POWER_INDEX_2]         = { 4, 8 },
};

static int ssv6051_efuse_ctrl_mode(struct ssv6051_dev *sdev, u32 mode)
{
	int ret;

	ret = ssv6051_sdio_write_reg(sdev, EFUSE_CTRL_MODE_REG, mode);
	if (ret) {
		ssv_err(sdev->dev, "efuse controller mode 0x%x failed (%d)\n",
			mode, ret);
		return ret;
	}
	ssv_dbg(DBG_EFUSE, sdev->dev, "efuse controller mode 0x%x\n", mode);
	return 0;
}

static int ssv6051_efuse_read_raw(struct ssv6051_dev *sdev, u8 *buf,
				  size_t len)
{
	u32 val;
	int ret, i;

	/* Read the chip identity first: the efuse controller must be
	 * stepped through the ID read before the section reads; skipping it left
	 * the controller in a state where later PMU register writes hang.
	 */
	ret = ssv6051_sdio_write_reg(sdev, SSV_EFUSE_ID_READ_SWITCH, 0x1);
	if (ret)
		return ret;
	ret = ssv6051_sdio_read_reg(sdev, SSV_EFUSE_ID_RAW_DATA_BASE,
				    &sdev->cfg.chip_identity);
	if (ret)
		return ret;
	ssv_dbg(DBG_EFUSE, sdev->dev, "chip identity 0x%08x\n",
		sdev->cfg.chip_identity);

	/* Probe read of section 0 before the map loop: it triggers the controller
	 * for the first section and doubles as the empty-efuse check
	 * (an all-zero first dword means the efuse is empty). */
	ret = ssv6051_sdio_write_reg(sdev, SSV_EFUSE_READ_SWITCH, 0x1);
	if (ret)
		return ret;
	ret = ssv6051_sdio_read_reg(sdev, SSV_EFUSE_RAW_DATA_BASE, &val);
	if (ret)
		return ret;
	if (val == 0x00) {
		ssv_dbg(DBG_EFUSE, sdev->dev, "efuse empty\n");
		return 0;
	}

	for (i = 0; i < EFUSE_MAX_SECTION_MAP; i++) {
		ret = ssv6051_sdio_write_reg(
			sdev, SSV_EFUSE_READ_SWITCH + i * 4, 0x1);
		if (ret)
			return ret;
		ret = ssv6051_sdio_read_reg(
			sdev, SSV_EFUSE_RAW_DATA_BASE + i * 4, &val);
		if (ret)
			return ret;
		if (i * 4 + 4 <= len)
			memcpy(buf + i * 4, &val, 4);
	}
	return 0;
}

static void ssv6051_efuse_parse_map(struct ssv6051_dev *sdev, const u8 *pbuf,
				    u8 *mac_addr)
{
	const u8 *rtemp8 = pbuf;
	u8 idx = 0, shift = 0;
	u16 value;
	u16 pos = 0; /* running position in bits (efuse_real_content_len) */
	size_t i;

	for (i = 0; i < SSV6051_EFUSE_ITEMS; i++)
		sdev->efuse_items[i].value = 0;

	do {
		idx = (*rtemp8 >> shift) & 0xF;
		switch (idx) {
		case EFUSE_R_CALIBRATION_RESULT:
		case EFUSE_CRYSTAL_FREQUENCY_OFFSET:
		case EFUSE_TX_POWER_INDEX_1:
		case EFUSE_TX_POWER_INDEX_2:
		case EFUSE_SAR_RESULT:
			/* The mask width comes from the static layout table: the
			 * per-device efuse_items[].byte_cnts is never populated. */
			if (shift) {
				rtemp8++;
				value = (u16)(*(const u16 *)rtemp8) &
					((1 << ssv6051_efuse_layout[idx].byte_cnts) -
					 1);
			} else {
				value = (u16)(*(const u16 *)rtemp8 >> 4) &
					((1 << ssv6051_efuse_layout[idx].byte_cnts) -
					 1);
			}
			sdev->efuse_items[idx].value = value;
			pos += (ssv6051_efuse_layout[idx].offset +
				ssv6051_efuse_layout[idx].byte_cnts);
			break;
		case EFUSE_MAC:
			if (shift) {
				rtemp8++;
				memcpy(mac_addr, rtemp8, ETH_ALEN);
			} else {
				for (i = 0; i < ETH_ALEN; i++) {
					mac_addr[i] =
						(u8)((*(const u16 *)rtemp8 >> 4) &
						     0xFF);
					rtemp8++;
				}
			}
			pos += (ssv6051_efuse_layout[idx].offset +
				ssv6051_efuse_layout[idx].byte_cnts);
			break;
		default:
			idx = 0;
			break;
		}

		shift = pos % 8;
		rtemp8 = &pbuf[pos / 8];
	} while (idx != 0);
}

void ssv6051_efuse_read_all_map(struct ssv6051_dev *sdev)
{
	u8 mac[ETH_ALEN] = { 0 };
	u8 map[EFUSE_HWSET_MAX_SIZE / 8];

	/* Zero the mapping table before the read: needed
	 * when the efuse is empty and the read returns early. */
	memset(map, 0, sizeof(map));
	int ret;

	ssv_dbg(DBG_EFUSE, sdev->dev, "reading efuse map\n");

	ret = ssv6051_efuse_ctrl_mode(sdev, EFUSE_CTRL_MODE_ON);
	if (ret)
		return;

	ret = ssv6051_efuse_read_raw(sdev, map, sizeof(map));
	if (ret) {
		dev_info(sdev->dev, "efuse map read failed (%d)\n", ret);
		goto out;
	}

	/* Always visible: the efuse content is bring-up critical (MAC,
	 * calibration), so it is printed unconditionally. */
	dev_info(sdev->dev, "chip identity from efuse: 0x%08x\n",
		 sdev->cfg.chip_identity);
	dev_info(sdev->dev, "efuse map (%zu bytes): %*phN\n", sizeof(map),
		 (int)sizeof(map), map);

	ssv6051_efuse_parse_map(sdev, map, mac);

	/* Publish the parsed values into the device configuration. */
	sdev->cfg.r_calibration_result =
		(u8)sdev->efuse_items[EFUSE_R_CALIBRATION_RESULT].value;
	sdev->cfg.sar_result =
		(u8)sdev->efuse_items[EFUSE_SAR_RESULT].value;
	sdev->cfg.crystal_frequency_offset =
		(u8)sdev->efuse_items[EFUSE_CRYSTAL_FREQUENCY_OFFSET].value;
	sdev->cfg.tx_power_index_1 =
		(u8)sdev->efuse_items[EFUSE_TX_POWER_INDEX_1].value;
	sdev->cfg.tx_power_index_2 =
		(u8)sdev->efuse_items[EFUSE_TX_POWER_INDEX_2].value;

	ssv_dbg(DBG_EFUSE, sdev->dev,
		"chip identity 0x%08x\n", sdev->cfg.chip_identity);
	ssv_dbg(DBG_EFUSE, sdev->dev,
		"r_calibration_result 0x%02x\n", sdev->cfg.r_calibration_result);
	ssv_dbg(DBG_EFUSE, sdev->dev, "sar_result 0x%02x\n",
		sdev->cfg.sar_result);
	ssv_dbg(DBG_EFUSE, sdev->dev, "crystal_frequency_offset 0x%02x\n",
		sdev->cfg.crystal_frequency_offset);
	ssv_dbg(DBG_EFUSE, sdev->dev, "tx_power_index_1 0x%02x\n",
		sdev->cfg.tx_power_index_1);
	ssv_dbg(DBG_EFUSE, sdev->dev, "tx_power_index_2 0x%02x\n",
		sdev->cfg.tx_power_index_2);
	ssv_dbg(DBG_EFUSE, sdev->dev, "MAC %pM\n", mac);

	/* Use the efuse MAC when valid and not overridden. */
	if (!sdev->cfg.ignore_efuse_mac && is_valid_ether_addr(mac)) {
		ether_addr_copy(sdev->perm_addr, mac);
		dev_info(sdev->dev, "using MAC address from e-fuse: %pM\n",
			 sdev->perm_addr);
	} else {
		eth_random_addr(sdev->perm_addr);
		dev_info(sdev->dev,
			 "e-fuse MAC invalid or ignored, using random MAC: %pM\n",
			 sdev->perm_addr);
	}

out:
	ssv6051_efuse_ctrl_mode(sdev, EFUSE_CTRL_MODE_OFF);
}

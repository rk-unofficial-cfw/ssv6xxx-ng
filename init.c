// SPDX-License-Identifier: GPL-2.0
/*
 * Hardware init: PHY/RF tables, calibration, MAC setup.
 *
 * Based on the ssv6xxx driver:
 *   Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 *   Copyright (c) 2015 iComm Corporation
 */

#include "ssv6051.h"
#include "tables.h"

#define PHY_INFO_TBL1_SIZE 39
#define PHY_INFO_TBL2_SIZE 39
#define PHY_INFO_TBL3_SIZE 8
/* Size of the combined phy_info_tbl (TBL1+TBL2+TBL3): the pbuf
 * allocation size. */
#define PHY_INFO_TBL_SIZE (PHY_INFO_TBL1_SIZE * 4 + PHY_INFO_TBL2_SIZE * 4 + \
			   PHY_INFO_TBL3_SIZE * 4)

#define MAX_PHY_SETTING_TABLE_SIZE 1920
#define MAX_RF_SETTING_TABLE_SIZE  512

/* Packet buffer allocator: writes size|type to ADR_WR_ALC and reads the
 * allocated SRAM address back. Retries until the firmware responds. */
u32 ssv6051_pbuf_alloc(struct ssv6051_dev *sdev, int size, int type)
{
	u32 regval = 0;
	int cnt = 100;
	int ret;

	mutex_lock(&sdev->mem_lock);
	do {
		ret = ssv6051_sdio_write_reg(sdev, ADR_WR_ALC,
					     (u32)(size | (type << 16)));
		if (ret)
			break;
		ret = ssv6051_sdio_read_reg(sdev, ADR_WR_ALC, &regval);
		if (ret)
			break;
		if (regval != 0)
			break;
		msleep(1);
	} while (--cnt);
	mutex_unlock(&sdev->mem_lock);

	if (regval == 0) {
		ssv_err(sdev->dev, "pbuf alloc of %d bytes failed\n", size);
		sdev->stats.tx_errors++;
	} else {
		ssv_dbg(DBG_INIT, sdev->dev, "pbuf alloc %d bytes at 0x%08x\n",
			size, regval);
	}
	return regval;
}

bool ssv6051_pbuf_free(struct ssv6051_dev *sdev, u32 pbuf_addr)
{
	u32 regval = 0;
	int ret;

	mutex_lock(&sdev->mem_lock);
	ret = ssv6051_sdio_write_reg(sdev, ADR_CH0_TRIG_1,
				     (M_ENG_TRASH_CAN << 8) |
					     (pbuf_addr >> 8));
	if (!ret)
		ret = ssv6051_sdio_read_reg(sdev, ADR_MCU_STATUS, &regval);
	mutex_unlock(&sdev->mem_lock);

	if (ret) {
		ssv_warn(sdev->dev, "pbuf free of 0x%08x failed (%d)\n",
			 pbuf_addr, ret);
		return false;
	}
	ssv_dbg(DBG_INIT, sdev->dev, "pbuf freed 0x%08x\n", pbuf_addr);
	return true;
}

/* PHY info macros */
#define SET_PHY_INFO(_l, _r, _mcs, _sb, _n)                                    \
	(((_l) << 24) | ((_r) << 16) | ((_mcs) << 8) | ((_sb) << 4) | (_n))
#define SET_PHY_L_LENGTH(_l_ba, _l_rts, _l_cts_ack)                            \
	(((_l_ba) << 12) | ((_l_rts) << 6) | (_l_cts_ack))

/* PHY rate info tables (including the 6051-Z variant). Programmed into the chip rate-info SRAM. */
static const u32 ssv6051_phy_info_tbl1[PHY_INFO_TBL1_SIZE] = {
	0x0C000000, 0x0C000100, 0x0C000200, 0x0C000300,
	0x0C000140, 0x0C000240, 0x0C000340, 0x00000001,
	0x00000101, 0x00000201, 0x00000301, 0x0C000401,
	0x0C000501, 0x0C000601, 0x0C000701, 0x00030002,
	0x00030102, 0x00030202, 0x0C030302, 0x0C030402,
	0x0C030502, 0x0C030602, 0x10030702, 0x00030082,
	0x00030182, 0x00030282, 0x0C030382, 0x0C030482,
	0x0C030582, 0x0C030682, 0x10030782, 0x00030042,
	0x00030142, 0x00030242, 0x0C030342, 0x0C030442,
	0x0C030542, 0x0C030642, 0x10030742,
};

static const u32 ssv6051_phy_info_tbl2[PHY_INFO_TBL2_SIZE] = {
	SET_PHY_INFO(314, 0, 0, 0, 0),
	SET_PHY_INFO(258, 0, 1, 0, 0),  SET_PHY_INFO(223, 0, 1, 0, 0),
	SET_PHY_INFO(213, 0, 1, 0, 0),  SET_PHY_INFO(162, 0, 4, 0, 0),
	SET_PHY_INFO(127, 0, 4, 0, 0),  SET_PHY_INFO(117, 0, 4, 0, 0),
	SET_PHY_INFO(60, 7, 7, 0, 0),   SET_PHY_INFO(52, 7, 7, 0, 0),
	SET_PHY_INFO(48, 9, 9, 0, 0),   SET_PHY_INFO(44, 9, 9, 0, 0),
	SET_PHY_INFO(44, 11, 11, 0, 0), SET_PHY_INFO(40, 11, 11, 0, 0),
	SET_PHY_INFO(40, 11, 11, 0, 0), SET_PHY_INFO(40, 11, 11, 0, 0),
	SET_PHY_INFO(76, 7, 7, 0, 1),   SET_PHY_INFO(64, 9, 9, 1, 1),
	SET_PHY_INFO(60, 9, 9, 2, 1),   SET_PHY_INFO(60, 11, 11, 3, 1),
	SET_PHY_INFO(56, 11, 11, 4, 1), SET_PHY_INFO(56, 11, 11, 5, 1),
	SET_PHY_INFO(56, 11, 11, 5, 1), SET_PHY_INFO(56, 11, 11, 5, 1),
	SET_PHY_INFO(76, 7, 7, 6, 1),   SET_PHY_INFO(64, 9, 9, 1, 1),
	SET_PHY_INFO(60, 9, 9, 2, 1),   SET_PHY_INFO(60, 11, 11, 3, 1),
	SET_PHY_INFO(56, 11, 11, 4, 1), SET_PHY_INFO(56, 11, 11, 5, 1),
	SET_PHY_INFO(56, 11, 11, 5, 1), SET_PHY_INFO(56, 11, 11, 5, 1),
	SET_PHY_INFO(64, 7, 7, 0, 0),   SET_PHY_INFO(52, 9, 9, 0, 0),
	SET_PHY_INFO(48, 9, 9, 0, 0),   SET_PHY_INFO(48, 11, 11, 0, 0),
	SET_PHY_INFO(44, 11, 11, 0, 0), SET_PHY_INFO(44, 11, 11, 0, 0),
	SET_PHY_INFO(44, 11, 11, 0, 0), SET_PHY_INFO(44, 11, 11, 0, 0),
};

static const u32 ssv6051_phy_info_tbl3[PHY_INFO_TBL3_SIZE] = {
	SET_PHY_L_LENGTH(50, 38, 35),   SET_PHY_L_LENGTH(35, 29, 26),
	SET_PHY_L_LENGTH(29, 26, 23),   SET_PHY_L_LENGTH(26, 23, 23),
	SET_PHY_L_LENGTH(23, 23, 20),   SET_PHY_L_LENGTH(23, 20, 20),
	SET_PHY_L_LENGTH(47, 38, 35),   SET_PHY_L_LENGTH(0, 0, 0),
};

/* RX frame-type decision table (generic variant). First MAC_DECITBL1_SIZE entries go to
 * ADR_MRX_FLT_TB0, the remaining MAC_DECITBL2_SIZE to ADR_MRX_FLT_EN0. */
#define SET_DEC_TBL(_type, _mask, _action, _drop)                              \
	(((_type) << 9) | ((_mask) << 3) | ((_action) << 1) | (_drop))
#define ACTION_DO_NOTHING   0
#define ACTION_UPDATE_NAV   1
#define ACTION_RESET_NAV    2
#define ACTION_SIGNAL_ACK   3
#define FRAME_ACCEPT        0
#define FRAME_DROP          1

static const u16 ssv6051_deci_tbl[MAC_DECITBL1_SIZE + MAC_DECITBL2_SIZE] = {
	SET_DEC_TBL(0x1e, 0x3e, ACTION_RESET_NAV, FRAME_DROP),
	SET_DEC_TBL(0x18, 0x3e, ACTION_SIGNAL_ACK, FRAME_ACCEPT),
	SET_DEC_TBL(0x1a, 0x3f, ACTION_DO_NOTHING, FRAME_ACCEPT),
	SET_DEC_TBL(0x10, 0x38, ACTION_DO_NOTHING, FRAME_DROP),
	0, 0, 0,
	SET_DEC_TBL(0x05, 0x3f, ACTION_SIGNAL_ACK, FRAME_ACCEPT),
	SET_DEC_TBL(0x0b, 0x3f, ACTION_SIGNAL_ACK, FRAME_ACCEPT),
	SET_DEC_TBL(0x01, 0x3d, ACTION_SIGNAL_ACK, FRAME_ACCEPT),
	SET_DEC_TBL(0x00, 0x00, ACTION_DO_NOTHING, FRAME_ACCEPT),
	SET_DEC_TBL(0x00, 0x00, ACTION_SIGNAL_ACK, FRAME_ACCEPT),
	SET_DEC_TBL(0x00, 0x00, ACTION_DO_NOTHING, FRAME_DROP),
	SET_DEC_TBL(0x00, 0x00, ACTION_UPDATE_NAV, FRAME_DROP),
	SET_DEC_TBL(0x00, 0x00, ACTION_RESET_NAV, FRAME_DROP),
	SET_DEC_TBL(0x00, 0x00, ACTION_SIGNAL_ACK, FRAME_DROP),
	0x2008, 0x1001, 0x0400, 0x0400, 0x2000, 0x800e, 0x0800, 0x0b88, 0x0800,
};

/* RF synthesizer calibration table: rows indexed by the crystal type
 * (26M, 40M, 24M), one entry per channel. */
struct ssv6051_calib_entry {
	u16 channel_id;
	u32 rf_ctrl_N;
	u32 rf_ctrl_F;
	u16 rf_precision_default;
};

static const struct ssv6051_calib_entry
ssv6051_vt_tbl[3][14] = {
	{ /* 26 MHz */
		{ 1, 0xB9, 0x89D89E, 3859 }, { 2, 0xB9, 0xEC4EC5, 3867 },
		{ 3, 0xBA, 0x4EC4EC, 3875 }, { 4, 0xBA, 0xB13B14, 3883 },
		{ 5, 0xBB, 0x13B13B, 3891 }, { 6, 0xBB, 0x762762, 3899 },
		{ 7, 0xBB, 0xD89D8A, 3907 }, { 8, 0xBC, 0x3B13B1, 3915 },
		{ 9, 0xBC, 0x9D89D9, 3923 }, { 10, 0xBD, 0x000000, 3931 },
		{ 11, 0xBD, 0x627627, 3939 }, { 12, 0xBD, 0xC4EC4F, 3947 },
		{ 13, 0xBE, 0x276276, 3955 }, { 14, 0xBF, 0x13B13B, 3974 },
	},
	{ /* 40 MHz */
		{ 1, 0xf1, 0x333333, 3859 }, { 2, 0xf1, 0xB33333, 3867 },
		{ 3, 0xf2, 0x333333, 3875 }, { 4, 0xf2, 0xB33333, 3883 },
		{ 5, 0xf3, 0x333333, 3891 }, { 6, 0xf3, 0xB33333, 3899 },
		{ 7, 0xf4, 0x333333, 3907 }, { 8, 0xf4, 0xB33333, 3915 },
		{ 9, 0xf5, 0x333333, 3923 }, { 10, 0xf5, 0xB33333, 3931 },
		{ 11, 0xf6, 0x333333, 3939 }, { 12, 0xf6, 0xB33333, 3947 },
		{ 13, 0xf7, 0x333333, 3955 }, { 14, 0xf8, 0x666666, 3974 },
	},
	{ /* 24 MHz */
		{ 1, 0xC9, 0x000000, 3859 }, { 2, 0xC9, 0x6AAAAB, 3867 },
		{ 3, 0xC9, 0xD55555, 3875 }, { 4, 0xCA, 0x400000, 3883 },
		{ 5, 0xCA, 0xAAAAAB, 3891 }, { 6, 0xCB, 0x155555, 3899 },
		{ 7, 0xCB, 0x800000, 3907 }, { 8, 0xCB, 0xEAAAAB, 3915 },
		{ 9, 0xCC, 0x555555, 3923 }, { 10, 0xCC, 0xC00000, 3931 },
		{ 11, 0xCD, 0x2AAAAB, 3939 }, { 12, 0xCD, 0x955555, 3947 },
		{ 13, 0xCE, 0x000000, 3955 }, { 14, 0xCF, 0x000000, 3974 },
	},
};

#define VT_TBL_ROW(crystal) ((crystal) == SSV6051_IQK_CFG_XTAL_40M ? 1 : \
			     (crystal) == SSV6051_IQK_CFG_XTAL_24M ? 2 : 0)

/* ABB/ADC overrides for the SSV6051P on channels 13/14:
 * { reg_addr, ch1_12_value, ch13_14_value }. The ch1_12 values are read
 * back from the chip after the INIT_CALI has applied the tables. */
static const struct ssv6051_reg_pair ssv6051_ch13_14_p[] = {
	{ ADR_ABB_REGISTER_1, 0x151559fc },
	{ ADR_RX_ADC_REGISTER, 0x20d000d2 },
};
#define SSV6051_CH_CFG_NUM ARRAY_SIZE(ssv6051_ch13_14_p)

/* Read-modify-write of a chip register. */
int ssv6051_reg_set_bits(struct ssv6051_dev *sdev, u32 reg, u32 set,
			 u32 clr)
{
	u32 regval;
	int ret;

	/* One claim for the whole read-modify-write (the claim is
	 * recursive for the same task). */
	sdio_claim_host(sdev->func);
	ret = ssv6051_sdio_read_reg(sdev, reg, &regval);
	if (!ret) {
		regval &= ~clr;
		regval |= set;
		ret = ssv6051_sdio_write_reg(sdev, reg, regval);
	}
	sdio_release_host(sdev->func);
	return ret;
}

/* RF power on/off. */
static int ssv6051_rf_enable(struct ssv6051_dev *sdev)
{
	return ssv6051_reg_set_bits(sdev, ADR_RF_HARD_WIRE_PIN,
				    RF_HARD_WIRE_RF_ON, RF_HARD_WIRE_RF_MSK);
}

static int ssv6051_rf_disable(struct ssv6051_dev *sdev)
{
	return ssv6051_reg_set_bits(sdev, ADR_RF_HARD_WIRE_PIN,
				    RF_HARD_WIRE_RF_OFF, RF_HARD_WIRE_RF_MSK);
}

/* Channel selection: program the RF synthesizer
 * for the channel and poll the lock flag (RETRY_MAX x 1ms, FAIL_MAX
 * rounds). Channels 13/14 switch the ABB/ADC overrides for the Z/P
 * variants. */
#define VT_LOCK_RETRY_MAX 20
#define VT_FAIL_MAX       100

/* Also called from ssv6051_config (mac80211 channel changes / software
 * scan). */
int ssv6051_set_channel(struct ssv6051_dev *sdev, int ch)
{
	const struct ssv6051_calib_entry *e;
	unsigned int fail_cnt = 0;
	u32 regval;
	int ret = 0;
	int chidx;
	int row;
	bool found = false;

	if ((sdev->cfg.chip_identity == SSV6051Z ||
	     sdev->cfg.chip_identity == SSV6051P) &&
	    ARRAY_SIZE(ssv6051_ch13_14_p) == 2) {
		if (ch >= 13) {
			if (!sdev->cfg.ipd_channel_touch) {
				for (chidx = 0; chidx < SSV6051_CH_CFG_NUM;
				     chidx++)
					ssv6051_sdio_write_reg(sdev,
						ssv6051_ch13_14_p[chidx].address,
						ssv6051_ch13_14_p[chidx].data);
				sdev->cfg.ipd_channel_touch = true;
			}
		} else if (sdev->cfg.ipd_channel_touch) {
			for (chidx = 0; chidx < SSV6051_CH_CFG_NUM; chidx++)
				ssv6051_sdio_write_reg(sdev,
					ssv6051_ch13_14_p[chidx].address,
					sdev->cfg.ch1_12_value[chidx]);
			sdev->cfg.ipd_channel_touch = false;
		}
	}

	row = VT_TBL_ROW(sdev->cfg.crystal_type);
	for (chidx = 0; chidx < 14; chidx++) {
		if (ssv6051_vt_tbl[row][chidx].channel_id == ch) {
			found = true;
			break;
		}
	}
	if (!found) {
		ssv_err(sdev->dev, "channel %d not in vt_tbl\n", ch);
		return -EINVAL;
	}
	e = &ssv6051_vt_tbl[row][chidx];

	ret = ssv6051_rf_disable(sdev);
	if (ret)
		return ret;

	do {
		/* 40 MHz crystals divide the SX clock by 2. */
		ret = ssv6051_reg_set_bits(sdev, ADR_SYN_DIV_SDM_XOSC,
					   (sdev->cfg.crystal_type ==
						SSV6051_IQK_CFG_XTAL_40M)
						   ? (1u << 13) : (0u << 13),
					   (1u << 13));
		if (ret)
			break;
		ret = ssv6051_reg_set_bits(sdev, ADR_SX_LCK_BIN_REGISTERS_I,
					   (1u << 19), (1u << 19));
		if (ret)
			break;
		ret = ssv6051_reg_set_bits(sdev, ADR_SYN_REGISTER_1,
					   e->rf_ctrl_F, 0x00ffffffu);
		if (ret)
			break;
		ret = ssv6051_reg_set_bits(sdev, ADR_SYN_REGISTER_2,
					   e->rf_ctrl_N, 0x7ffu);
		if (ret)
			break;
		ret = ssv6051_sdio_read_reg(sdev, ADR_SX_LCK_BIN_REGISTERS_I,
					    &regval);
		if (ret)
			break;
		ret = ssv6051_reg_set_bits(sdev, ADR_SX_LCK_BIN_REGISTERS_II,
					   e->rf_precision_default, 0x1fffu);
		if (ret)
			break;
		ret = ssv6051_reg_set_bits(sdev, ADR_MANUAL_ENABLE_REGISTER,
					   (0u << 14), (1u << 14));
		if (ret)
			break;
		ret = ssv6051_reg_set_bits(sdev, ADR_MANUAL_ENABLE_REGISTER,
					   (1u << 14), (1u << 14));
		if (ret)
			break;

		{
			int retry;

			for (retry = 0; retry < VT_LOCK_RETRY_MAX; retry++) {
				mdelay(1);
				ret = ssv6051_sdio_read_reg(sdev,
						ADR_READ_ONLY_FLAGS_1, &regval);
				if (ret)
					break;
				if (regval & 0x00000002) {
					ret = ssv6051_sdio_read_reg(sdev,
							ADR_READ_ONLY_FLAGS_2,
							&regval);
					if (ret)
						break;
					ret = ssv6051_rf_enable(sdev);
					ssv_dbg(DBG_INIT, sdev->dev,
						"locked to channel %d\n", ch);
					sdev->cfg.channel = ch;
					goto exit;
				}
			}
		}
		fail_cnt++;
		ssv_warn(sdev->dev, "channel %d lock fail round %u\n", ch,
			 fail_cnt);
	} while (fail_cnt < VT_FAIL_MAX && ret == 0);

	if (ret == 0 && fail_cnt >= VT_FAIL_MAX)
		ret = -ETIMEDOUT;
	ssv_err(sdev->dev, "failed to lock channel %d\n", ch);
	return ret;

exit:
	/* TX power offset from the efuse; both
	 * indices are 0 on this module, so this is usually a no-op. */
	if (ch <= 7) {
		if (sdev->cfg.tx_power_index_1)
			ssv6051_reg_set_bits(sdev, ADR_RX_TX_FSM_REGISTER,
					     sdev->cfg.tx_power_index_1
							<< RG_TX_GAIN_OFFSET_SFT,
					     RG_TX_GAIN_OFFSET_I_MSK);
	} else if (sdev->cfg.tx_power_index_2) {
		ssv6051_reg_set_bits(sdev, ADR_RX_TX_FSM_REGISTER,
				     sdev->cfg.tx_power_index_2
						<< RG_TX_GAIN_OFFSET_SFT,
				     RG_TX_GAIN_OFFSET_I_MSK);
	}
	return ret;
}

/* IQ init flags (fx_sel): the calibration runs the firmware executes on
 * INIT_CALI (PAPD is not enabled here). */
#define SSV6051_IQK_INIT_FLAGS						\
	(SSV6051_IQK_TEMPERATURE | SSV6051_IQK_RXDC | SSV6051_IQK_RXRC |	\
	 SSV6051_IQK_TXDC | SSV6051_IQK_TXIQ | SSV6051_IQK_RXIQ)

/* Per-chip RF table overrides: Q_P1/Q_P2/Q, Z and P variants tune
 * eight synthesizer registers differently. Applied to the payload copy
 * before the INIT_CALI command is sent. */
static void ssv6051_rf_tbl_chip_adjust(struct ssv6051_reg_pair *tbl,
				       size_t n, u32 chip_identity)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (chip_identity == SSV6051Q_P1 ||
		    chip_identity == SSV6051Q_P2 ||
		    chip_identity == SSV6051Q) {
			if (tbl[i].address == 0xCE010008)
				tbl[i].data = 0x008DF61B;
			else if (tbl[i].address == 0xCE010014)
				tbl[i].data = 0x3D3E84FE;
			else if (tbl[i].address == 0xCE010018)
				tbl[i].data = 0x01457D79;
			else if (tbl[i].address == 0xCE01001C)
				tbl[i].data = 0x000103A7;
			else if (tbl[i].address == 0xCE010020)
				tbl[i].data = 0x000103A6;
			else if (tbl[i].address == 0xCE01002C)
				tbl[i].data = 0x00032CA8;
			else if (tbl[i].address == 0xCE010048)
				tbl[i].data = 0xFCCCCF27;
			else if (tbl[i].address == 0xCE010050)
				tbl[i].data = 0x00444000;
		} else if (chip_identity == SSV6051Z) {
			if (tbl[i].address == 0xCE010008)
				tbl[i].data = 0x004D561C;
			else if (tbl[i].address == 0xCE010014)
				tbl[i].data = 0x3D9E84FE;
			else if (tbl[i].address == 0xCE010018)
				tbl[i].data = 0x00457D79;
			else if (tbl[i].address == 0xCE01001C)
				tbl[i].data = 0x000103EB;
			else if (tbl[i].address == 0xCE010020)
				tbl[i].data = 0x000103EA;
			else if (tbl[i].address == 0xCE01002C)
				tbl[i].data = 0x00062CA8;
			else if (tbl[i].address == 0xCE010048)
				tbl[i].data = 0xFCCCCF27;
			else if (tbl[i].address == 0xCE010050)
				tbl[i].data = 0x0047C000;
		} else if (chip_identity == SSV6051P) {
			if (tbl[i].address == 0xCE010008)
				tbl[i].data = 0x008B7C1C;
			else if (tbl[i].address == 0xCE010014)
				tbl[i].data = 0x3D7E84FE;
			else if (tbl[i].address == 0xCE010018)
				tbl[i].data = 0x01457D79;
			else if (tbl[i].address == 0xCE01001C)
				tbl[i].data = 0x000103EB;
			else if (tbl[i].address == 0xCE010020)
				tbl[i].data = 0x000103EA;
			else if (tbl[i].address == 0xCE01002C)
				tbl[i].data = 0x00032CA8;
			else if (tbl[i].address == 0xCE010048)
				tbl[i].data = 0xFCCCCC27;
			else if (tbl[i].address == 0xCE010050)
				tbl[i].data = 0x0047C000;
			else if (tbl[i].address == 0xC0001D00)
				/* DLDO_LEVEL 4 (0x5E): the working community port of
				 * this same module keeps the table default here; level 5
				 * (0x5F000040) makes the
				 * MCU unstable on this hardware (it boots, delivers a
				 * couple of frames, then dies and stops servicing the
				 * SDIO RX FIFO). */
				tbl[i].data = 0x5E000040;
		}
	}
}

/* Crystal-dependent RF overrides on the payload copy: the 24 MHz crystal replaces the synthesizer and
 * DPLL entries, and the efuse crystal offset tunes the XO CBANK. */
static void ssv6051_rf_tbl_crystal_adjust(struct ssv6051_dev *sdev,
					  struct ssv6051_reg_pair *tbl,
					  size_t n)
{
	size_t i;

	if (sdev->cfg.crystal_type == SSV6051_IQK_CFG_XTAL_24M) {
		for (i = 0; i < n; i++) {
			if (tbl[i].address == ADR_SX_ENABLE_REGISTER)
				tbl[i].data = 0x0003E07C;
			else if (tbl[i].address == ADR_DPLL_DIVIDER_REGISTER)
				tbl[i].data = 0x00406000;
			else if (tbl[i].address ==
					 ADR_DPLL_FB_DIVIDER_REGISTERS_I)
				tbl[i].data = 0x00000028;
			else if (tbl[i].address ==
					 ADR_DPLL_FB_DIVIDER_REGISTERS_II)
				tbl[i].data = 0x00000000;
		}
	}

	if (sdev->cfg.crystal_frequency_offset) {
		for (i = 0; i < n; i++) {
			if (tbl[i].address ==
				    ADR_SYN_KVCO_XO_FINE_TUNE_CBANK) {
				tbl[i].data &= RG_XOSC_CBANK_XO_I_MSK;
				tbl[i].data |=
					(sdev->cfg.crystal_frequency_offset
					 << RG_XOSC_CBANK_XO_SFT);
			}
		}
	}
}

/* Send the IQK init-calibration command to the firmware: HOST_CMD_INIT_CALI
 * carrying the IQK config followed by the PHY setting and RF tables
 * The firmware
 * applies both tables itself and runs the calibrations listed in fx_sel;
 * the host never writes these registers over SDIO. */
static int ssv6051_do_iq_calib(struct ssv6051_dev *sdev)
{
	/* The default TX scales come from the patched ADR_TX_GAIN_FACTOR
	 * entry (ssv6051_hw_tables_init extracts them from the PHY
	 * setting table). */
	struct ssv6051_iqk_cfg iqk_cfg = {
		.cfg_xtal = sdev->cfg.crystal_type,
		.cfg_pa = SSV6051_IQK_CFG_PA_DEF,
		.cfg_tssi_trgt = 26,
		.cfg_tssi_div = 3,
		.cfg_def_tx_scale_11b = sdev->cfg.def_tx_scale_11b,
		.cfg_def_tx_scale_11b_p0d5 = sdev->cfg.def_tx_scale_11b_p0d5,
		.cfg_def_tx_scale_11g = sdev->cfg.def_tx_scale_11g,
		.cfg_def_tx_scale_11g_p0d5 = sdev->cfg.def_tx_scale_11g_p0d5,
		.cmd_sel = SSV6051_IQK_CMD_INIT_CALI,
		.fx_sel = SSV6051_IQK_INIT_FLAGS,
	};
	struct ssv6051_host_cmd *host_cmd;
	struct ssv6051_reg_pair *rf;
	u8 *payload;
	struct sk_buff *skb;
	size_t phy_size = sizeof(ssv6051_phy_setting);
	size_t rf_size = sizeof(ssv6051_asic_rf_setting);
	size_t total = SSV6051_HOST_CMD_HDR_LEN + sizeof(iqk_cfg) +
		       phy_size + rf_size;
	int ret;

	/* The firmware parses the config with a fixed
	 * layout: 10 u32:8 bitfields pack into three words with padding. */
	BUILD_BUG_ON(sizeof(struct ssv6051_iqk_cfg) != 28);

	if (phy_size > MAX_PHY_SETTING_TABLE_SIZE ||
	    rf_size > MAX_RF_SETTING_TABLE_SIZE) {
		ssv_err(sdev->dev, "PHY/RF table size overflow (%zu/%zu)\n",
			phy_size, rf_size);
		return -EINVAL;
	}

	switch (sdev->cfg.chip_identity) {
	case SSV6051Q_P1:
	case SSV6051Q_P2:
	case SSV6051Q:
	case SSV6051Z:
	case SSV6051P:
		break;
	default:
		/* Without a known variant the RF table would be applied
		 * untuned. */
		ssv_err(sdev->dev,
			"unknown chip identity 0x%08x, no RF setting\n",
			 sdev->cfg.chip_identity);
		return -ENODEV;
	}

	skb = dev_alloc_skb(total);
	if (!skb)
		return -ENOMEM;

	skb_put(skb, total);
	host_cmd = (struct ssv6051_host_cmd *)skb->data;
	memset(host_cmd, 0, total);
	host_cmd->len = total;
	host_cmd->c_type = HOST_CMD;
	host_cmd->h_cmd = SSV6XXX_HOST_CMD_INIT_CALI;
	/* Write past the fixed header through a raw u8 pointer: the
	 * flexible-array union in the header confuses FORTIFY_SOURCE. */
	payload = (u8 *)host_cmd + SSV6051_HOST_CMD_HDR_LEN;
	memcpy(payload, &iqk_cfg, sizeof(iqk_cfg));
	memcpy(payload + sizeof(iqk_cfg), ssv6051_phy_setting, phy_size);
	rf = (struct ssv6051_reg_pair *)(payload + sizeof(iqk_cfg) + phy_size);
	memcpy(rf, ssv6051_asic_rf_setting, rf_size);

	/* Apply the per-chip and crystal adjustments to the payload copy,
	 * so the static tables stay untouched. */
	ssv6051_rf_tbl_chip_adjust(rf, rf_size / sizeof(*rf),
				   sdev->cfg.chip_identity);
	ssv6051_rf_tbl_crystal_adjust(sdev, rf, rf_size / sizeof(*rf));

	ssv_dbg(DBG_IQK, sdev->dev,
		"IQK init: xtal=%u identity=0x%08x phy=%zu rf=%zu total=%zu\n",
		iqk_cfg.cfg_xtal, sdev->cfg.chip_identity, phy_size, rf_size,
		total);

	ret = ssv6051_hci_send_cmd_skb(sdev, skb);
	dev_kfree_skb(skb);
	if (ret)
		return ret;

	/* Give the firmware time to run the calibration. */
	msleep(50);
	return 0;
}

/* PHY info tables: programmed into the chip rate-info SRAM. */
static void ssv6051_load_phy_info_tbl(struct ssv6051_dev *sdev, u32 base,
				      const u32 *tbl, u32 words)
{
	u32 i;

	for (i = 0; i < words; i++)
		ssv6051_sdio_write_reg(sdev, base + i * 4, tbl[i]);
}

/* MAC soft reset: the bit auto-clears when the reset completes. It is
 * polled in a tight loop up to 10000 times and the init fails if it
 * never clears. */
static int ssv6051_mac_reset(struct ssv6051_dev *sdev)
{
	int i;
	int ret;
	u32 regval = 1;

	ret = ssv6051_sdio_write_reg(sdev, ADR_BRG_SW_RST,
				     1u << MAC_SW_RST_SFT);
	if (ret)
		return ret;
	for (i = 0; i < 10000; i++) {
		ret = ssv6051_sdio_read_reg(sdev, ADR_BRG_SW_RST, &regval);
		if (ret)
			return ret;
		if (regval == 0)
			return 0;
	}

	ssv_err(sdev->dev, "MAC reset failed (reg 0x%08x)\n", regval);
	return -ENODEV;
}

/* Chip SRAM buffers and MAC global settings, all programmed BEFORE the
 * firmware download: pbuf for the key table + PHY info tables, SCRT_SET,
 * rate-info tables, GLBLE_SET, station address, traps, WSID and the RX
 * flow engine routing. */
static int ssv6051_mac_setup(struct ssv6051_dev *sdev)
{
	u32 regval;
	u32 i;
	int ret;

	/* Channel status priority. */
	ssv6051_sdio_write_reg(sdev, ADR_CH_STA_PRI, 0x1213);

	/* Packet buffer for the security key table followed by the PHY
	 * info tables (one allocation of
	 * sizeof(phy_info_tbl) + sizeof(struct ssv6051_hw_sec)). */
	sdev->hw_sec_key = ssv6051_pbuf_alloc(sdev,
					      PHY_INFO_TBL_SIZE +
					      sizeof(struct ssv6051_hw_sec),
					      0 /* NOTYPE_BUF */);
	if (sdev->hw_sec_key == 0 || (sdev->hw_sec_key >> 28) != 8) {
		ssv_err(sdev->dev, "failed to allocate key/PHY-info pbuf\n");
		return -ENOMEM;
	}
	sdev->hw_pinfo = sdev->hw_sec_key + sizeof(struct ssv6051_hw_sec);

	/* Zero the key table area, then point the security engine at it
	 * (SCRT_SET) and ignore replay checking until
	 * the firmware configures it. */
	for (i = 0; i < sizeof(struct ssv6051_hw_sec); i += 4)
		ssv6051_sdio_write_reg(sdev, sdev->hw_sec_key + i, 0);
	ret = ssv6051_sdio_read_reg(sdev, ADR_SCRT_SET, &regval);
	if (ret)
		goto err_free;
	regval &= SCRT_PKT_ID_I_MSK;
	regval |= ((sdev->hw_sec_key >> 16) << SCRT_PKT_ID_SFT);
	ret = ssv6051_sdio_write_reg(sdev, ADR_SCRT_SET, regval);
	if (ret)
		goto err_free;
	ret = ssv6051_sdio_read_reg(sdev, ADR_SCRT_SET, &regval);
	if (ret)
		goto err_free;
	regval &= SCRT_RPLY_IGNORE_I_MSK;
	regval |= (1u << SCRT_RPLY_IGNORE_SFT);
	ret = ssv6051_sdio_write_reg(sdev, ADR_SCRT_SET, regval);
	if (ret)
		goto err_free;

	/* PHY rate info tables: TBL1 in the fixed INFO0 window, TBL2/TBL3
	 * in the pbuf area, indexed through INFO_IDX_ADDR/INFO_LEN_ADDR. */
	ssv6051_load_phy_info_tbl(sdev, ADR_INFO0, ssv6051_phy_info_tbl1,
				  PHY_INFO_TBL1_SIZE);
	ssv6051_load_phy_info_tbl(sdev, sdev->hw_pinfo, ssv6051_phy_info_tbl2,
				  PHY_INFO_TBL2_SIZE);
	ssv6051_load_phy_info_tbl(sdev,
				  sdev->hw_pinfo + PHY_INFO_TBL2_SIZE * 4,
				  ssv6051_phy_info_tbl3, PHY_INFO_TBL3_SIZE);
	ssv6051_sdio_write_reg(sdev, ADR_INFO_RATE_OFFSET, 0x00040000);
	ssv6051_sdio_write_reg(sdev, ADR_INFO_IDX_ADDR, sdev->hw_pinfo);
	ssv6051_sdio_write_reg(sdev, ADR_INFO_LEN_ADDR,
			       sdev->hw_pinfo + PHY_INFO_TBL2_SIZE * 4);

	/* Global MAC settings: STA mode, duplicate filter
	 * on, TX reserved pages, RX packet buffer offset. */
	ssv6051_sdio_write_reg(sdev, ADR_GLBLE_SET,
			       (0u << OP_MODE_SFT) |
			       (0u << SNIFFER_MODE_SFT) |
			       (1u << DUP_FLT_SFT) |
			       (SSV6200_TX_PKT_RSVD_SETTING
					<< TX_PKT_RSVD_SFT) |
			       ((u32)RXPB_OFFSET << PB_OFFSET_SFT));

	/* Station address, empty BSSID, no ether-type demux. */
	ssv6051_sdio_write_reg(sdev, ADR_STA_MAC_0,
			       *(const u32 *)&sdev->perm_addr[0]);
	ssv6051_sdio_write_reg(sdev, ADR_STA_MAC_1,
			       *(const u32 *)&sdev->perm_addr[4]);
	ssv6051_sdio_write_reg(sdev, ADR_BSSID_0, 0);
	ssv6051_sdio_write_reg(sdev, ADR_BSSID_1, 0);
	ssv6051_sdio_write_reg(sdev, ADR_TX_ETHER_TYPE_0, 0);
	ssv6051_sdio_write_reg(sdev, ADR_TX_ETHER_TYPE_1, 0);
	ssv6051_sdio_write_reg(sdev, ADR_RX_ETHER_TYPE_0, 0);
	ssv6051_sdio_write_reg(sdev, ADR_RX_ETHER_TYPE_1, 0);

	/* Reason traps, unknown-frame trap to the HCI engine, empty WSID
	 * watch list. */
	ssv6051_sdio_write_reg(sdev, ADR_REASON_TRAP0, 0x7FBC7F87);
	ssv6051_sdio_write_reg(sdev, ADR_REASON_TRAP1, 0x0000003F);
	ssv6051_sdio_write_reg(sdev, ADR_TRAP_HW_ID, M_ENG_HWHCI);
	ssv6051_sdio_write_reg(sdev, ADR_WSID0, 0);
	ssv6051_sdio_write_reg(sdev, ADR_WSID1, 0);

	/* RX flow engine: data goes through the HW crypto engine (HW
	 * WEP/TKIP/CCMP), management and control straight to the HCI
	 * engine; the firmware reprograms these once running. */
	ssv6051_sdio_write_reg(sdev, ADR_RX_FLOW_DATA,
			       M_ENG_MACRX | (M_ENG_ENCRYPT_SEC << 4) |
			       (M_ENG_HWHCI << 8));
	ssv6051_sdio_write_reg(sdev, ADR_RX_FLOW_MNG,
			       M_ENG_MACRX | (M_ENG_HWHCI << 4));
	ssv6051_sdio_write_reg(sdev, ADR_RX_FLOW_CTRL,
			       M_ENG_MACRX | (M_ENG_HWHCI << 4));

	/* RX frame-type decision table. */
	for (i = 0; i < MAC_DECITBL1_SIZE; i++)
		ssv6051_sdio_write_reg(sdev, ADR_MRX_FLT_TB0 + i * 4,
				       ssv6051_deci_tbl[i]);
	for (i = 0; i < MAC_DECITBL2_SIZE; i++)
		ssv6051_sdio_write_reg(sdev, ADR_MRX_FLT_EN0 + i * 4,
				       ssv6051_deci_tbl[MAC_DECITBL1_SIZE + i]);

	return 0;

err_free:
	ssv6051_pbuf_free(sdev, sdev->hw_sec_key);
	sdev->hw_sec_key = 0;
	sdev->hw_pinfo = 0;
	return ret;
}

/* Write a register table: a
 * plain write loop, NO read-back confirmation. Several entries land on
 * read-only hardware registers (e.g. 0xce010094 ADR_READ_ONLY_FLAGS_1,
 * the synth lock flags), so a confirm would fail spuriously. */
static int ssv6051_write_reg_table(struct ssv6051_dev *sdev,
				   const struct ssv6051_reg_pair *tbl,
				   size_t n)
{
	size_t i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = ssv6051_sdio_write_reg(sdev, tbl[i].address, tbl[i].data);
		if (ret)
			return ret;
	}
	return 0;
}

/* Pre-firmware bring-up: this sequence runs BEFORE the firmware
 * download. The PHY setting and ASIC RF tables ARE programmed here over
 * SDIO: they carry the PMU/regulator and synthesizer configuration the card
 * CPU needs to boot (an LDO 6051P with a 24 MHz crystal never starts
 * without them -- the INIT_CALI payload alone is not enough, since the
 * MCU must be running to receive it). The INIT_CALI still carries both
 * tables post-fw.
 */
static int ssv6051_hw_tables_init(struct ssv6051_dev *sdev)
{
	struct ssv6051_reg_pair *rf;
	struct ssv6051_reg_pair *phy;
	size_t rf_n, phy_n, i;
	u32 regval;
	int ret;

	rf_n = ARRAY_SIZE(ssv6051_asic_rf_setting);
	phy_n = ARRAY_SIZE(ssv6051_phy_setting);
	rf = kmemdup(ssv6051_asic_rf_setting, rf_n * sizeof(*rf), GFP_KERNEL);
	phy = kmemdup(ssv6051_phy_setting, phy_n * sizeof(*phy), GFP_KERNEL);
	if (!rf || !phy) {
		kfree(rf);
		kfree(phy);
		return -ENOMEM;
	}

	/* Per-chip-identity synthesizer tuning. */
	ssv6051_rf_tbl_chip_adjust(rf, rf_n, sdev->cfg.chip_identity);

	/* Crystal-dependent overrides: 24 MHz synth/DPLL values and the
	 * efuse XO fine tune. */
	ssv6051_rf_tbl_crystal_adjust(sdev, rf, rf_n);

	/* Voltage regulator selection:
	 * ADR_PMU_2 bit 0 selects LDO (0) or DCDC (1). */
	for (i = 0; i < rf_n; i++) {
		if (rf[i].address == ADR_PMU_2) {
			rf[i].data &= ~0x1u;
			rf[i].data |= sdev->cfg.volt_regulator ? 0x0u : 0x1u;
		}
	}

	/* TX gain factor (ADR_TX_GAIN_FACTOR override):
	 * per-identity default, then extract the default TX scales for the
	 * INIT_CALI config. */
	for (i = 0; i < phy_n; i++) {
		if (phy[i].address != ADR_TX_GAIN_FACTOR)
			continue;
		switch (sdev->cfg.chip_identity) {
		case SSV6051Q_P1:
		case SSV6051Q_P2:
		case SSV6051Q:
			phy[i].data = 0x5B606C72;
			break;
		case SSV6051Z:
			phy[i].data = 0x60606060;
			break;
		case SSV6051P:
			phy[i].data = 0x6C726C72;
			break;
		}
		sdev->cfg.def_tx_scale_11b = (phy[i].data >> 0) & 0xff;
		sdev->cfg.def_tx_scale_11b_p0d5 = (phy[i].data >> 8) & 0xff;
		sdev->cfg.def_tx_scale_11g = (phy[i].data >> 16) & 0xff;
		sdev->cfg.def_tx_scale_11g_p0d5 = (phy[i].data >> 24) & 0xff;
		break;
	}

	/* RF table first. */
	ret = ssv6051_write_reg_table(sdev, rf, rf_n);
	kfree(rf);
	if (ret)
		goto out_free_phy;

	/* PHY off before reconfiguring. */
	ret = ssv6051_sdio_write_reg(sdev, ADR_PHY_EN_1, 0);
	if (ret)
		goto out_free_phy;

	/* Reset the baseband PLL unless the clock switch was already done. */
	ret = ssv6051_sdio_read_reg(sdev, ADR_PHY_EN_0, &regval);
	if (ret)
		goto out_free_phy;
	if (regval & (1u << RG_RF_BB_CLK_SEL_SFT)) {
		ssv_dbg(DBG_INIT, sdev->dev, "clock switch already done\n");
	} else {
		ret = ssv6051_sdio_read_reg(sdev, ADR_DPLL_CP_PFD_REGISTER,
					    &regval);
		if (ret)
			goto out_free_phy;
		regval |= (1u << RG_DP_BBPLL_PD_SFT) |
			  (1u << RG_DP_BBPLL_SDM_EDGE_SFT);
		ret = ssv6051_sdio_write_reg(sdev, ADR_DPLL_CP_PFD_REGISTER,
					     regval);
		if (ret)
			goto out_free_phy;
		regval &= ~((1u << RG_DP_BBPLL_PD_SFT) |
			    (1u << RG_DP_BBPLL_SDM_EDGE_SFT));
		ret = ssv6051_sdio_write_reg(sdev, ADR_DPLL_CP_PFD_REGISTER,
					     regval);
		if (ret)
			goto out_free_phy;
		mdelay(10);
	}

	/* PHY table. */
	ret = ssv6051_write_reg_table(sdev, phy, phy_n);
	kfree(phy);
	phy = NULL;
	if (ret)
		return ret;

	/* SDIO built-in self test. */
	ret = ssv6051_sdio_write_reg(sdev, ADR_TRX_DUMMY_REGISTER,
				     0xEAAAAAAA);
	if (ret)
		return ret;
	ret = ssv6051_sdio_read_reg(sdev, ADR_TRX_DUMMY_REGISTER, &regval);
	if (ret)
		return ret;
	if (regval != 0xEAAAAAAA)
		ssv_warn(sdev->dev,
			 "SDIO self test failed: 0x%08x != 0xEAAAAAAA\n",
			 regval);
	else
		ssv_dbg(DBG_INIT, sdev->dev, "SDIO self test passed\n");

	/* Pad muxing and clock selection. */
	ret = ssv6051_sdio_write_reg(sdev, ADR_PAD53, 0x21);
	if (ret)
		return ret;
	ret = ssv6051_sdio_write_reg(sdev, ADR_PAD54, 0x3000);
	if (ret)
		return ret;
	ret = ssv6051_sdio_write_reg(sdev, ADR_PIN_SEL_0, 0x4000);
	if (ret)
		return ret;
	ret = ssv6051_sdio_write_reg(sdev, 0xc0000304, 0x01);
	if (ret)
		return ret;
	ret = ssv6051_sdio_write_reg(sdev, 0xc0000308, 0x01);
	if (ret)
		return ret;
	ret = ssv6051_sdio_write_reg(sdev, ADR_CLOCK_SELECTION, 0x3);
	if (ret)
		return ret;
	ret = ssv6051_sdio_write_reg(sdev, ADR_TRX_DUMMY_REGISTER,
				     0xAAAAAAAA);
	if (ret)
		return ret;

	/* Synthesizer lock to the default channel (cfg.def_chan). */
	ret = ssv6051_set_channel(sdev, sdev->cfg.def_chan);
	if (ret)
		return ret;

	/* PHY enable (all blocks). */
	ret = ssv6051_sdio_write_reg(sdev, ADR_PHY_EN_1, SSV6051_PHY_EN_ALL);
	if (ret)
		return ret;

	return 0;

out_free_phy:
	kfree(phy);
	return ret;
}

int ssv6051_pre_fw_init(struct ssv6051_dev *sdev)
{
	u32 regval;
	int ret;

	ret = ssv6051_hw_tables_init(sdev);
	if (ret)
		return ret;

	/* PHY off while reconfiguring. */
	ret = ssv6051_sdio_read_reg(sdev, ADR_PHY_EN_1, &regval);
	if (ret == 0)
		ret = ssv6051_sdio_write_reg(sdev, ADR_PHY_EN_1,
					     regval & ~RG_PHY_MD_EN_MSK);
	if (ret)
		return ret;

	/* MAC soft reset, polled to completion. */
	ret = ssv6051_mac_reset(sdev);
	if (ret)
		return ret;

	ret = ssv6051_sdio_write_reg(sdev, ADR_TXQ4_MTX_Q_AIFSN, 0xffff2101);
	if (ret)
		return ret;

	ret = ssv6051_sdio_read_reg(sdev, ADR_MTX_BCN_EN_MISC, &regval);
	if (ret)
		return ret;
	ret = ssv6051_sdio_write_reg(sdev, ADR_MTX_BCN_EN_MISC,
				     regval & ~MTX_HALT_MNG_UNTIL_DTIM_MSK);
	if (ret)
		return ret;

	ret = ssv6051_sdio_write_reg(sdev, ADR_CONTROL, 0x12000006);
	if (ret)
		return ret;

	ret = ssv6051_sdio_write_reg(sdev, ADR_RX_TIME_STAMP_CFG,
				     (28u << MRX_STP_OFST_SFT) | 0x01);
	if (ret)
		return ret;

	ret = ssv6051_sdio_write_reg(sdev, ADR_HCI_TX_RX_INFO_SIZE,
		(u32)TXPB_OFFSET << TX_PBOFFSET_SFT |
			(u32)SSV6051_TX_DESC_BYTE_LEN << TX_INFO_SIZE_SFT |
			(u32)SSV6051_RX_DESC_BYTE_LEN << RX_INFO_SIZE_SFT |
			(u32)SSV6051_RX_PINFO_PAD << RX_LAST_PHY_SIZE_SFT);
	if (ret)
		return ret;

	/* Share the MMU with the MCU. */
	ret = ssv6051_sdio_read_reg(sdev, ADR_MMU_CTRL, &regval);
	if (ret)
		return ret;
	ret = ssv6051_sdio_write_reg(sdev, ADR_MMU_CTRL,
				     regval | (0xffu << MMU_SHARE_MCU_SFT));
	if (ret)
		return ret;

	/* RX watchdog: disable the low nibble. */
	ret = ssv6051_sdio_read_reg(sdev, ADR_MRX_WATCH_DOG, &regval);
	if (ret)
		return ret;
	ret = ssv6051_sdio_write_reg(sdev, ADR_MRX_WATCH_DOG,
				     regval & 0xfffffff0u);
	if (ret)
		return ret;

	/* TX/RX ID thresholds. */
	ret = ssv6051_sdio_read_reg(sdev, ADR_TRX_ID_THRESHOLD, &regval);
	if (ret)
		return ret;
	regval = (regval & 0xffff0000u) |
		 (SSV6200_ID_TX_THRESHOLD << TX_ID_THOLD_SFT) |
		 (SSV6200_ID_RX_THRESHOLD << RX_ID_THOLD_SFT);
	ret = ssv6051_sdio_write_reg(sdev, ADR_TRX_ID_THRESHOLD, regval);
	if (ret)
		return ret;

	/* TX/RX page thresholds. */
	ret = ssv6051_sdio_read_reg(sdev, ADR_ID_LEN_THREADSHOLD1, &regval);
	if (ret)
		return ret;
	regval = (regval & 0x0fu) |
		 (SSV6200_PAGE_TX_THRESHOLD << ID_TX_LEN_THOLD_SFT) |
		 (SSV6200_PAGE_RX_THRESHOLD << ID_RX_LEN_THOLD_SFT);
	ret = ssv6051_sdio_write_reg(sdev, ADR_ID_LEN_THREADSHOLD1, regval);
	if (ret)
		return ret;

	/* TSF timer enable. */
	ret = ssv6051_sdio_read_reg(sdev, ADR_MTX_BCN_EN_MISC, &regval);
	if (ret)
		return ret;
	ret = ssv6051_sdio_write_reg(sdev, ADR_MTX_BCN_EN_MISC,
				     regval | (1u << MTX_TSF_TIMER_EN_SFT));
	if (ret)
		return ret;

	/* Chip SRAM buffers (key table + PHY info tables) and global MAC
	 * configuration. */
	ret = ssv6051_mac_setup(sdev);
	if (ret)
		return ret;

	/* Route the card's internal interrupt sources to the SDIO host
	 * interface (done before the firmware download): unmask MBOX_INT_3, HCI_INT_1 and the UART debug
	 * sources, keep everything else masked. Without it the reset
	 * default 0xffffffff would keep the firmware's HCI/mailbox
	 * notifications from ever reaching the host. The function-level
	 * gate (REG_INT_MASK) stays closed until hci_start.
	 */
	ret = ssv6051_sdio_write_reg(sdev, ADR_SDIO_MASK,
				     SSV6051_SDIO_INT_MASK);
	if (ret)
		return ret;

	/* TX low-threshold interrupt trigger: the
	 * card raises the TX resource-low interrupt when free TX pages drop
	 * to PAGE_TRIGGER or free TX IDs to ID_TRIGGER. Without it the host
	 * never learns that the card TX resources are exhausted. */
	ret = ssv6051_sdio_write_reg(sdev, ADR_TX_LIMIT_INTR,
				     0x80000000u |
				     (SSV6200_TX_LOWTHRESHOLD_ID_TRIGGER << 16) |
				     SSV6200_TX_LOWTHRESHOLD_PAGE_TRIGGER);
	if (ret)
		return ret;

	return 0;
}

/* Post-firmware init: the IQK calibration runs only once the firmware
 * is running. The pbuf/key-table and PHY info programming is NOT here:
 * it happens before the download (ssv6051_mac_setup). */
int ssv6051_init_hw(struct ssv6051_dev *sdev)
{
	u32 regval;
	int ret;

	ssv_dbg(DBG_INIT, sdev->dev, "init HW\n");

	/* Run the IQK init calibration (mandatory part of init). */
	ret = ssv6051_do_iq_calib(sdev);
	if (ret) {
		ssv_err(sdev->dev, "IQK calibration failed (%d)\n", ret);
		return ret;
	}

	/* Snapshot the ABB/ADC registers for the channel 1-12 override
	 * bookkeeping (read right after the INIT_CALI has applied the
	 * tables). */
	if (sdev->cfg.chip_identity == SSV6051Z ||
	    sdev->cfg.chip_identity == SSV6051P) {
		ssv6051_sdio_read_reg(sdev, ADR_ABB_REGISTER_1,
				      &sdev->cfg.ch1_12_value[0]);
		ssv6051_sdio_read_reg(sdev, ADR_RX_ADC_REGISTER,
				      &sdev->cfg.ch1_12_value[1]);
	}

	/* Log the chip id register: ADR_CHIP_ID_* hold the
	 * ASCII identity string, not the numeric variant. The numeric
	 * chip_identity used for the RF table selection comes from the
	 * efuse ID read at probe and must not
	 * be overwritten here. */
	ret = ssv6051_sdio_read_reg(sdev, ADR_CHIP_ID_0, &regval);
	if (ret == 0)
		ssv_dbg(DBG_INIT, sdev->dev,
			"chip id register 0: 0x%08x\n", regval);

	return 0;
}

/* Chip-side teardown: the SX/RF is powered down and the security key
 * pbuf is returned to the chip pool. Skipping either leaves the chip
 * pool and the RF dirty, and the next probe then runs on a half-torn-down
 * device. Tolerant of bus errors: the card may already be wedged when the
 * teardown runs. */
void ssv6051_deinit_hw(struct ssv6051_dev *sdev)
{
	u32 regval;

	/* RF power down: bits 12-13 of the hard-wire pin register to 01. */
	if (ssv6051_sdio_read_reg(sdev, ADR_RF_HARD_WIRE_PIN, &regval) == 0)
		ssv6051_sdio_write_reg(sdev, ADR_RF_HARD_WIRE_PIN,
				       (regval & ~(0x03u << 12)) |
				       (0x01u << 12));

	/* Return the security key table pbuf to the chip pool. */
	if (sdev->hw_sec_key) {
		ssv6051_pbuf_free(sdev, sdev->hw_sec_key);
		sdev->hw_sec_key = 0;
	}
}

// SPDX-License-Identifier: GPL-2.0
/*
 * mac80211 glue: SDIO probe/remove, ieee80211_ops, module glue.
 *
 * Based on the ssv6xxx driver:
 *   Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 *   Copyright (c) 2015 iComm Corporation
 */

#include <linux/module.h>
#include <net/mac80211.h>

#include "ssv6051.h"

#define SDIO_BLOCK_SIZE 0x80

MODULE_AUTHOR("ssv6xxx-ng contributors");
MODULE_DESCRIPTION("SSV6051 SDIO WLAN driver (rewritten, kernel >= 6.6)");
MODULE_LICENSE("GPL");

unsigned int ssv6051_debug = DBG_INIT | DBG_FW;
module_param_named(debug, ssv6051_debug, uint, 0644);
MODULE_PARM_DESC(debug, "debug mask (bitwise OR of DBG_* flags)");

char *ssv6051_initmac;
module_param_named(initmac, ssv6051_initmac, charp, 0444);
MODULE_PARM_DESC(initmac, "MAC address override, e.g. 00:11:22:33:44:55");

/* Board-specific hardware settings: crystal frequency, voltage
 * regulator type and default channel. */
unsigned int ssv6051_xtal_clock = 24;
module_param_named(xtal_clock, ssv6051_xtal_clock, uint, 0444);
MODULE_PARM_DESC(xtal_clock, "crystal frequency in MHz: 24, 26 or 40 (default 24)");

unsigned int ssv6051_volt_regulator = 1;
module_param_named(volt_regulator, ssv6051_volt_regulator, uint, 0444);
MODULE_PARM_DESC(volt_regulator, "voltage regulator type: 0 = DCDC, 1 = LDO (default 1)");

unsigned int ssv6051_def_chan = 6;
module_param_named(def_chan, ssv6051_def_chan, uint, 0444);
MODULE_PARM_DESC(def_chan, "default channel 1-14 (default 6)");

/* ------------------------------------------------------------------ */
/* Regulatory / band data                                              */
/* ------------------------------------------------------------------ */
static struct ieee80211_channel ssv6051_channels[] = {
	{ .center_freq = 2412, .hw_value = 1 },
	{ .center_freq = 2417, .hw_value = 2 },
	{ .center_freq = 2422, .hw_value = 3 },
	{ .center_freq = 2427, .hw_value = 4 },
	{ .center_freq = 2432, .hw_value = 5 },
	{ .center_freq = 2437, .hw_value = 6 },
	{ .center_freq = 2442, .hw_value = 7 },
	{ .center_freq = 2447, .hw_value = 8 },
	{ .center_freq = 2452, .hw_value = 9 },
	{ .center_freq = 2457, .hw_value = 10 },
	{ .center_freq = 2462, .hw_value = 11 },
	{ .center_freq = 2467, .hw_value = 12 },
	{ .center_freq = 2472, .hw_value = 13 },
	{ .center_freq = 2484, .hw_value = 14 },
};

static struct ieee80211_rate ssv6051_rates[] = {
	/* hw_value = firmware rate index:
	 * CCK 0-3 (short preamble variants 4-6), OFDM 7-14. */
	{ .bitrate = 10,  .hw_value = 0 },
	{ .bitrate = 20,  .hw_value = 1,  .hw_value_short = 4,
	  .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 55,  .hw_value = 2,  .hw_value_short = 5,
	  .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 110, .hw_value = 3,  .hw_value_short = 6,
	  .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 60,  .hw_value = 7 },
	{ .bitrate = 90,  .hw_value = 8 },
	{ .bitrate = 120, .hw_value = 9 },
	{ .bitrate = 180, .hw_value = 10 },
	{ .bitrate = 240, .hw_value = 11 },
	{ .bitrate = 360, .hw_value = 12 },
	{ .bitrate = 480, .hw_value = 13 },
	{ .bitrate = 540, .hw_value = 14 },
};

static struct ieee80211_supported_band ssv6051_band_2ghz = {
	.band = NL80211_BAND_2GHZ,
	.channels = ssv6051_channels,
	.n_channels = ARRAY_SIZE(ssv6051_channels),
	.bitrates = ssv6051_rates,
	.n_bitrates = ARRAY_SIZE(ssv6051_rates),
	.ht_cap = {
		.cap = IEEE80211_HT_CAP_SUP_WIDTH_20_40,
		.ampdu_factor = IEEE80211_HT_MAX_AMPDU_16K,
		.ampdu_density = IEEE80211_HT_MPDU_DENSITY_NONE,
		.mcs = { .rx_mask = { 0xFF },
			 .tx_params = IEEE80211_HT_MCS_TX_DEFINED },
	},
};

static const u32 ssv6051_cipher_suites[] = {
	WLAN_CIPHER_SUITE_WEP40,
	WLAN_CIPHER_SUITE_WEP104,
	WLAN_CIPHER_SUITE_TKIP,
	WLAN_CIPHER_SUITE_CCMP,
};

/* ------------------------------------------------------------------ */
/* mac80211 operations                                                 */
/* ------------------------------------------------------------------ */

static int ssv6051_start(struct ieee80211_hw *hw)
{
	struct ssv6051_dev *sdev = hw->priv;
	int ret;

	ssv_dbg(DBG_INIT, sdev->dev, "start\n");

	/* Tune the channel BEFORE arming the card interrupt: once the SDIO
	 * IRQ is claimed the firmware starts delivering frames and every
	 * other SDIO user has to contend with the interrupt thread. */
	ret = ssv6051_set_channel(sdev, sdev->cfg.channel);
	if (ret)
		return ret;

	ret = ssv6051_hci_start(sdev);
	if (ret)
		return ret;

	ieee80211_wake_queues(hw);
	return 0;
}

static void ssv6051_stop(struct ieee80211_hw *hw, bool suspend)
{
	struct ssv6051_dev *sdev = hw->priv;

	ssv_dbg(DBG_INIT, sdev->dev, "stop (suspend=%d)\n", suspend);
	ieee80211_stop_queues(hw);
	ssv6051_hci_stop(sdev);
}

static int ssv6051_add_interface(struct ieee80211_hw *hw,
				 struct ieee80211_vif *vif)
{
	struct ssv6051_dev *sdev = hw->priv;
	struct ssv6051_vif_priv *vif_priv = (void *)vif->drv_priv;

	ssv_dbg(DBG_INIT, sdev->dev, "add interface type %d\n", vif->type);

	if (vif->type != NL80211_IFTYPE_STATION)
		return -EOPNOTSUPP;

	vif_priv->vif = vif;
	ether_addr_copy(vif_priv->mac_addr, vif->addr);
	vif_priv->assoc = false;
	sdev->vif = vif;
	return 0;
}

static void ssv6051_remove_interface(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif)
{
	struct ssv6051_dev *sdev = hw->priv;

	ssv_dbg(DBG_INIT, sdev->dev, "remove interface\n");
	sdev->vif = NULL;
}

static int ssv6051_config(struct ieee80211_hw *hw, int radio_idx, u32 changed)
{
	struct ssv6051_dev *sdev = hw->priv;
	int channel;
	int ret = 0;

	if (changed & IEEE80211_CONF_CHANGE_CHANNEL) {
		channel = ieee80211_frequency_to_channel(
			hw->conf.chandef.chan->center_freq);
		/* Tune the RF synthesizer: this is what makes the
		 * mac80211 software scan work, since it retunes via .config
		 * for every channel. The EDCA TX queues are paused
		 * around the retune so no data frame
		 * is in flight while the RF changes channel; management
		 * frames (probe requests) use the separate mgmt queue and
		 * keep flowing. */
		ret = ssv6051_hci_txq_pause(sdev, 0x0F); /* TXQ_EDCA_0..3 */
		if (ret == 0) {
			ret = ssv6051_set_channel(sdev, channel);
			ssv6051_hci_txq_resume(sdev, 0x0F);
		}
		if (ret == 0)
			sdev->cfg.channel = channel;
		ssv_dbg(DBG_INIT, sdev->dev, "channel set to %d (%d)\n",
			channel, ret);
	}
	return ret;
}

/* mac80211 software scan hooks: lowering the 11B CCA thresholds
 * during the scan could reduce false CCA; v1 keeps these as stubs,
 * the channel switching happens in .config. */
static void ssv6051_sw_scan_start(struct ieee80211_hw *hw,
				  struct ieee80211_vif *vif,
				  const u8 *mac_addr)
{
	struct ssv6051_dev *sdev = hw->priv;

	ssv_dbg(DBG_INIT, sdev->dev, "scan start\n");
}

static void ssv6051_sw_scan_complete(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif)
{
	struct ssv6051_dev *sdev = hw->priv;

	ssv_dbg(DBG_INIT, sdev->dev, "scan complete\n");
}

static void ssv6051_bss_info_changed(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif,
				     struct ieee80211_bss_conf *info,
				     u64 changed)
{
	struct ssv6051_dev *sdev = hw->priv;
	struct ssv6051_vif_priv *vif_priv = (void *)vif->drv_priv;

	if (changed & BSS_CHANGED_ASSOC) {
		vif_priv->assoc = vif->cfg.assoc;
		sdev->associated = vif->cfg.assoc;
		if (vif->cfg.assoc) {
			ether_addr_copy(sdev->bssid, vif->cfg.ap_addr);
			ssv_dbg(DBG_INIT, sdev->dev,
				"associated to %pM\n", sdev->bssid);
		} else {
			eth_zero_addr(sdev->bssid);
			ssv_dbg(DBG_INIT, sdev->dev, "disassociated\n");
		}
	}
}

static u64 ssv6051_prepare_multicast(struct ieee80211_hw *hw,
				     struct netdev_hw_addr_list *mc_list)
{
	return 0;
}

static void ssv6051_configure_filter(struct ieee80211_hw *hw,
				     unsigned int changed_flags,
				     unsigned int *total_flags, u64 multicast)
{
	struct ssv6051_dev *sdev = hw->priv;
	unsigned int new_flags = 0;

	if (*total_flags & FIF_ALLMULTI)
		new_flags |= FIF_ALLMULTI;
	if (*total_flags & FIF_BCN_PRBRESP_PROMISC)
		new_flags |= FIF_BCN_PRBRESP_PROMISC;
	*total_flags = new_flags;
	ssv_dbg(DBG_INIT, sdev->dev, "filter flags 0x%x\n", new_flags);
}

static int ssv6051_set_rts_threshold(struct ieee80211_hw *hw, int radio_idx,
				     u32 value)
{
	struct ssv6051_dev *sdev = hw->priv;

	ssv_dbg(DBG_INIT, sdev->dev, "RTS threshold %u\n", value);
	return 0;
}

/* EDCA parameters for a HW queue. Without
 * them the card MAC has no valid channel-access timing and frames are
 * taken from the TX queue but never transmitted (TX IDs never
 * released). */
static int ssv6051_conf_tx(struct ieee80211_hw *hw,
			   struct ieee80211_vif *vif, unsigned int link_id,
			   u16 ac, const struct ieee80211_tx_queue_params *params)
{
	struct ssv6051_dev *sdev = hw->priv;
	u32 cw;
	int ret;

	if (ac >= ARRAY_SIZE(ssv6051_txq_map))
		return -EINVAL;

	/* QoS enable bit follows the vif QoS state (QOS_EN in
	 * ADR_GLBLE_SET, bit 4). */
	ret = ssv6051_reg_set_bits(sdev, ADR_GLBLE_SET,
				   (u32)vif->bss_conf.qos << 4, 0x10u);
	if (ret) {
		ssv_err(sdev->dev, "conf_tx: GLBLE_SET QOS_EN failed (%d)\n",
			ret);
		return ret;
	}

	cw = (params->aifs - 1) & 0xf;
	cw |= (ilog2(params->cw_min + 1) & 0xf) << TXQ_MTX_Q_ECWMIN_SFT;
	cw |= (ilog2(params->cw_max + 1) & 0xf) << TXQ_MTX_Q_ECWMAX_SFT;
	cw |= (params->txop & 0xff) << TXQ_MTX_Q_TXOP_LIMIT_SFT;

	ret = ssv6051_sdio_write_reg(sdev,
		ADR_TXQ0_MTX_Q_AIFSN + 0x100 * ssv6051_txq_map[ac], cw);
	/* Always visible during bring-up: a failure here leaves the card
	 * without channel-access timing. */
	ssv_dbg(DBG_INIT, sdev->dev,
		"conf_tx ac %u -> hwq %u: aifs %u cw %u-%u txop %u = 0x%08x (%d)\n",
		ac, ssv6051_txq_map[ac], params->aifs, params->cw_min,
		params->cw_max, params->txop, cw, ret);
	return ret;
}

static const struct ieee80211_ops ssv6051_ops = {
	.tx = ssv6051_tx,
	/* Mandatory since kernel ~6.10: ieee80211_alloc_hw_nm() WARNs and fails
	 * without it. This generic handler pushes frames from the mac80211
	 * intermediate queues through our .tx op.
	 */
	.wake_tx_queue = ieee80211_handle_wake_tx_queue,
	/* Mandatory for drivers without real channel context support: the
	 * "all or nothing" check in ieee80211_alloc_hw_nm() requires either
	 * all chanctx ops or these emulate stubs (which translate chanctx
	 * changes into .config() calls).
	 */
	.add_chanctx = ieee80211_emulate_add_chanctx,
	.remove_chanctx = ieee80211_emulate_remove_chanctx,
	.change_chanctx = ieee80211_emulate_change_chanctx,
	.start = ssv6051_start,
	.stop = ssv6051_stop,
	.add_interface = ssv6051_add_interface,
	.remove_interface = ssv6051_remove_interface,
	.config = ssv6051_config,
	.bss_info_changed = ssv6051_bss_info_changed,
	.sw_scan_start = ssv6051_sw_scan_start,
	.sw_scan_complete = ssv6051_sw_scan_complete,
	.prepare_multicast = ssv6051_prepare_multicast,
	.configure_filter = ssv6051_configure_filter,
	.conf_tx = ssv6051_conf_tx,
	.sta_add = ssv6051_sta_add,
	.sta_remove = ssv6051_sta_remove,
	.set_key = ssv6051_set_key,
	.set_rts_threshold = ssv6051_set_rts_threshold,
};

/* ------------------------------------------------------------------ */
/* mac80211 registration (called from SDIO probe)                      */
/* ------------------------------------------------------------------ */

/* Allocate the mac80211 device. The private area doubles as the one and
 * only ssv6051_dev instance (no struct copies, so the SDIO driver data
 * and the mac80211 private data always point at the same state).
 */
struct ieee80211_hw *ssv6051_alloc_mac80211(void)
{
	struct ieee80211_hw *hw;

	hw = ieee80211_alloc_hw(sizeof(struct ssv6051_dev), &ssv6051_ops);
	if (!hw)
		return NULL;

	hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	/* The TX packet buffer layout expected by the firmware: the 802.11
	 * frame sits TXPB_OFFSET bytes into the buffer, preceded by an
	 * optional AMPDU delimiter. The TX path
	 * pushes the descriptor into this headroom. */
	hw->extra_tx_headroom = TXPB_OFFSET + AMPDU_DELIMITER_LEN;
	hw->wiphy->max_scan_ssids = 4;
	hw->wiphy->bands[NL80211_BAND_2GHZ] = &ssv6051_band_2ghz;
	hw->wiphy->cipher_suites = ssv6051_cipher_suites;
	hw->wiphy->n_cipher_suites = ARRAY_SIZE(ssv6051_cipher_suites);
	hw->queues = 4;
	hw->vif_data_size = sizeof(struct ssv6051_vif_priv);
	hw->sta_data_size = sizeof(struct ssv6051_sta_priv);
	hw->max_rates = 1;
	hw->max_rate_tries = 4;

	return hw;
}

int ssv6051_register_mac80211(struct ssv6051_dev *sdev)
{
	struct ieee80211_hw *hw = sdev->hw;
	int ret;

	SET_IEEE80211_DEV(hw, sdev->dev);
	SET_IEEE80211_PERM_ADDR(hw, sdev->perm_addr);
	ieee80211_hw_set(hw, SIGNAL_DBM);
	ieee80211_hw_set(hw, REPORTS_TX_ACK_STATUS);
	ieee80211_hw_set(hw, CONNECTION_MONITOR);

	ret = ieee80211_register_hw(hw);
	if (ret) {
		ssv_err(sdev->dev, "ieee80211_register_hw failed (%d)\n", ret);
		return ret;
	}

	ssv_dbg(DBG_INIT, sdev->dev, "mac80211 registered\n");
	return 0;
}

/* Firmware wedge recovery:
 * stop the HCI, switch the PHY off (ADR_PHY_EN_1 <- 0), reload the
 * firmware and let mac80211 re-run drv_stop/drv_start (channel + HCI
 * start + re-association). Called from the RX work on the firmware's
 * SOC_EVT_RESET_HOST / SOC_EVT_WATCHDOG_TRIGGER events — without it a
 * wedged firmware stays wedged forever (SDIO timeouts, empty scans). */
void ssv6051_restart_hw(struct ssv6051_dev *sdev)
{
	ssv_warn(sdev->dev, "firmware wedge: restarting hardware\n");

	ssv6051_hci_stop(sdev);
	ssv6051_sdio_write_reg(sdev, ADR_PHY_EN_1, 0x0);

	/* The register bring-up and the firmware image must be redone
	 * before mac80211 re-runs .start (which only tunes the channel and
	 * arms the HCI). */
	if (ssv6051_pre_fw_init(sdev))
		return;
	if (ssv6051_load_firmware(sdev))
		return;

	ieee80211_restart_hw(sdev->hw);
}

void ssv6051_unregister_mac80211(struct ssv6051_dev *sdev)
{
	if (sdev->hw && sdev->hw->wiphy->registered)
		ieee80211_unregister_hw(sdev->hw);
}

/* ------------------------------------------------------------------ */
/* SDIO driver                                                         */
/* ------------------------------------------------------------------ */

static int __init ssv6051_init(void)
{
	pr_info(SSV6051_DRV_NAME ": loading\n");
	return ssv6051_sdio_register();
}

static void __exit ssv6051_exit(void)
{
	pr_info(SSV6051_DRV_NAME ": unloading\n");
	ssv6051_sdio_unregister();
}

module_init(ssv6051_init);
module_exit(ssv6051_exit);

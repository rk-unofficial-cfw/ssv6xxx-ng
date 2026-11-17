// SPDX-License-Identifier: GPL-2.0
/*
 * RX path: strips the RX descriptor and hands frames to mac80211.
 *
 * Based on the ssv6xxx driver:
 *   Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 *   Copyright (c) 2015 iComm Corporation
 */

#include "ssv6051.h"

/* RX frame layout: the 802.11 frame is
 * preceded by the RX descriptor and the RX PHY info, and followed by a
 * 4-byte padding trailer. */
#define SSV6051_RX_DESC_LEN (sizeof(struct ssv6051_rx_desc) + \
			     sizeof(struct ssv6051_rxphy_info))
#define SSV6051_RX_PINFO_PAD 4

/* Parse an RX descriptor and return the payload length. */
static int ssv6051_rx_parse_desc(struct ssv6051_dev *sdev,
				 const struct ssv6051_rx_desc *rx_desc,
				 u32 *payload_len)
{
	if (rx_desc->len == 0 || rx_desc->len > MAX_RX_FRAME_SIZE) {
		ssv_dbg(DBG_RX, sdev->dev, "bogus RX length %u\n",
			rx_desc->len);
		sdev->stats.rx_errors++;
		return -EINVAL;
	}

	*payload_len = rx_desc->len;
	return 0;
}

/* Log the first frames (bring-up diagnostic): the content tells whether
 * the card is delivering real air traffic or an event in a loop. */
#define RX_DEBUG_FIRST_FRAMES 8

/* Handle a frame delivered by the firmware. */
static void ssv6051_rx_handle_frame(struct ssv6051_dev *sdev,
				    struct sk_buff *skb)
{
	struct ieee80211_rx_status *rx_status;
	struct ssv6051_rx_desc *rx_desc;
	u32 payload_len = 0;
	u8 c_type, rate_idx;
	int ret;

	if (skb->len < SSV6051_RX_DESC_LEN + SSV6051_RX_PINFO_PAD) {
		ssv_dbg(DBG_RX, sdev->dev, "short RX frame (%u bytes)\n",
			skb->len);
		sdev->stats.rx_dropped++;
		dev_kfree_skb(skb);
		return;
	}

	rx_desc = (struct ssv6051_rx_desc *)skb->data;
	c_type = rx_desc->c_type;

	if (sdev->stats.rx_pkts < RX_DEBUG_FIRST_FRAMES) {
		ssv_dbg(DBG_RX, sdev->dev,
			"RX frame %u: len=%u c_type=%u\n",
			sdev->stats.rx_pkts + 1, skb->len, c_type);
		ssv_dbg_hex(DBG_RX, sdev->dev, "RX frame: ", skb->data,
			    min_t(u32, skb->len, 128));
	}

	if (c_type == HOST_EVENT) {
		/* Firmware SOC event. Most have no v1 consumer (rate control
		 * reports, logs, ...); the watchdog/reset ones are the firmware's
		 * only way to ask for recovery and MUST be handled, or a wedged
		 * firmware stays wedged forever. */
		struct ssv6051_host_event *evt = (struct ssv6051_host_event *)skb->data;

		switch (evt->h_event) {
		case SOC_EVT_RESET_HOST:
		case SOC_EVT_WATCHDOG_TRIGGER:
			dev_warn(sdev->dev, "firmware event %u: requesting HW restart\n",
				 evt->h_event);
			sdev->stats.rx_dropped++;
			dev_kfree_skb(skb);
			ssv6051_restart_hw(sdev);
			return;
		default:
			ssv_dbg(DBG_RX, sdev->dev, "host event %u dropped\n",
				evt->h_event);
			sdev->stats.rx_dropped++;
			dev_kfree_skb(skb);
			return;
		}
	}

	if (c_type > M2_RXEVENT) {
		ssv_dbg(DBG_RX, sdev->dev, "unexpected c_type %u\n", c_type);
		sdev->stats.rx_dropped++;
		dev_kfree_skb(skb);
		return;
	}

	ret = ssv6051_rx_parse_desc(sdev, rx_desc, &payload_len);
	if (ret) {
		dev_kfree_skb(skb);
		return;
	}
	if (payload_len == 0) {
		ssv_dbg(DBG_RX, sdev->dev, "empty RX frame\n");
		sdev->stats.rx_dropped++;
		dev_kfree_skb(skb);
		return;
	}

	/* The descriptor length is the total frame length (descriptor +
	 * payload + padding): it must match the bytes we actually read
	 * from the card, or the frame is truncated/corrupted. */
	if (payload_len != skb->len) {
		ssv_dbg(DBG_RX, sdev->dev, "RX descriptor len %u exceeds frame %u\n",
			 payload_len, skb->len);
		sdev->stats.rx_errors++;
		dev_kfree_skb(skb);
		return;
	}

	skb_pull(skb, SSV6051_RX_DESC_LEN);
	skb_trim(skb, skb->len - SSV6051_RX_PINFO_PAD);

	rx_status = IEEE80211_SKB_RXCB(skb);
	memset(rx_status, 0, sizeof(*rx_status));
	rx_status->freq = ieee80211_channel_to_frequency(
		sdev->cfg.channel, NL80211_BAND_2GHZ);
	rx_status->band = NL80211_BAND_2GHZ;
	rx_status->signal = -50; /* TODO: real RSSI from rxphy_info */
	/* Clamp to the band's rate table; MCS indices arrive only with
	 * AMPDU (not in v1) and would need HT rate reporting. */
	rate_idx = rx_desc->rate_idx & 0x3F;
	rx_status->rate_idx = min_t(u8, rate_idx,
				    sdev->hw->wiphy->bands[NL80211_BAND_2GHZ]
						    ->n_bitrates -
					    1);
	/* Protected data frames were decrypted by the chip (ENCRYPT_SEC
	 * engine) only when the matching key is installed in HW: then IV
	 * and ICV/MIC are already stripped and the protected bit cleared
	 * Otherwise mac80211 decrypts them. */
	{
		struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;

		if (ieee80211_is_data(hdr->frame_control) &&
		    ieee80211_has_protected(hdr->frame_control) &&
		    (is_multicast_ether_addr(hdr->addr1) ?
			     sdev->hw_group_crypt : sdev->hw_pair_crypt)) {
			hdr->frame_control &=
				~cpu_to_le16(IEEE80211_FCTL_PROTECTED);
			rx_status->flag |= RX_FLAG_DECRYPTED |
					   RX_FLAG_IV_STRIPPED;
		}
	}

	if (rx_desc->l3cs_err || rx_desc->l4cs_err) {
		rx_status->flag |= RX_FLAG_FAILED_FCS_CRC;
		sdev->stats.rx_errors++;
	}

	sdev->stats.rx_pkts++;
	ieee80211_rx_irqsafe(sdev->hw, skb);
}

/* Handle one frame delivered by the HCI RX path. */
void ssv6051_rx_process(struct ssv6051_dev *sdev, struct sk_buff *skb)
{
	ssv6051_rx_handle_frame(sdev, skb);
}

// SPDX-License-Identifier: GPL-2.0
/*
 * TX path: builds the TX descriptor and queues frames for the card.
 *
 * Based on the ssv6xxx driver:
 *   Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 *   Copyright (c) 2015 iComm Corporation
 */

#include "ssv6051.h"

#define TXPB_OFFSET 80

/* AC -> hardware TX queue mapping:
 * VO->3, VI->2, BE->1, BK->0; mac80211's queue mapping is VO=0, VI=1,
 * BE=2, BK=3). The chip has 5 HW TX queues (0-3 = EDCA, 4 = management;
 * the interrupt bit of each queue follows its index). */
const u8 ssv6051_txq_map[4] = { 3, 2, 1, 0 };
#define SSV6051_TXQ_MGMT 4

/* Frame header length helper. */
static u8 ssv6051_frame_hdrlen(const struct ieee80211_hdr *hdr, bool ht)
{
	u16 fc = le16_to_cpu(hdr->frame_control);
	u8 len = (u8)ieee80211_hdrlen(fc);

	if (ieee80211_is_data_qos(fc) && ht)
		len += 2;
	return len;
}

/* Build the TX descriptor for a frame and hand it to the HCI layer. */
void ssv6051_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
		struct sk_buff *skb)
{
	struct ssv6051_dev *sdev = hw->priv;
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	struct ssv6051_tx_desc *tx_desc;
	struct ssv6051_sta_priv *sta_priv = NULL;
	int hw_txqid;
	u32 tx_flags = 0;
	u8 ac;
	u16 seq_no;

	if (!sdev->started) {
		ssv_dbg(DBG_TX, sdev->dev, "not started, dropping frame\n");
		dev_kfree_skb(skb);
		return;
	}

	if (control && control->sta)
		sta_priv = (struct ssv6051_sta_priv *)control->sta->drv_priv;

	/* The driver owns the sequence number when mac80211 asks for it
	 * (+0x10 per new MPDU, fragments keep it). */
	if (info->flags & IEEE80211_TX_CTL_ASSIGN_SEQ) {
		if (info->flags & IEEE80211_TX_CTL_FIRST_FRAGMENT)
			seq_no = (u16)atomic_add_return(0x10, &sdev->seq_no);
		else
			seq_no = (u16)atomic_read(&sdev->seq_no);
		hdr->seq_ctrl &= cpu_to_le16(IEEE80211_SCTL_FRAG);
		hdr->seq_ctrl |= cpu_to_le16(seq_no & IEEE80211_SCTL_SEQ);
	}

	ac = skb_get_queue_mapping(skb);
	hw_txqid = (ac < ARRAY_SIZE(ssv6051_txq_map)) ?
			   ssv6051_txq_map[ac] :
			   M_ENG_TX_EDCA0;

	tx_desc = skb_push(skb, sizeof(*tx_desc));
	memset(tx_desc, 0, sizeof(*tx_desc));

	tx_desc->len = skb->len;
	tx_desc->c_type = M2_TXREQ;
	tx_desc->f80211 = 1;
	tx_desc->qos = ieee80211_is_data_qos(hdr->frame_control) ? 1 : 0;
	tx_desc->ht = 0;
	tx_desc->use_4addr = ieee80211_has_a4(hdr->frame_control) ? 1 : 0;
	tx_desc->more_data = ieee80211_has_morefrags(hdr->frame_control) ? 1 : 0;
	tx_desc->stype_b5b4 = (le16_to_cpu(hdr->frame_control) >> 4) & 0x3;
	tx_desc->frag = (tx_desc->more_data ||
			 (hdr->seq_ctrl & cpu_to_le16(IEEE80211_SCTL_FRAG))) ?
				1 :
				0;
	tx_desc->unicast = !is_multicast_ether_addr(hdr->addr1);
	tx_desc->tx_burst = tx_desc->frag;
	tx_desc->hdr_offset = TXPB_OFFSET;
	tx_desc->hdr_len = ssv6051_frame_hdrlen(hdr, tx_desc->ht);
	tx_desc->payload_offset = tx_desc->hdr_offset + tx_desc->hdr_len;

	if (info->control.use_rts)
		tx_desc->do_rts_cts = IEEE80211_TX_RC_USE_RTS_CTS;
	else if (info->control.use_cts_prot)
		tx_desc->do_rts_cts = IEEE80211_TX_RC_USE_CTS_PROTECT;

	if (sta_priv)
		tx_desc->wsid = (sta_priv->hw_wsid >= 0) ?
					(u32)sta_priv->hw_wsid :
					0x0F;
	else
		tx_desc->wsid = 0x0F;

	tx_desc->txq_idx = hw_txqid;
	tx_desc->TxF_ID = 0;

	if (info->flags & IEEE80211_TX_CTL_AMPDU)
		tx_desc->aggregation = 1;

	if (info->flags & IEEE80211_TX_CTL_SEND_AFTER_DTIM)
		tx_desc->bc_que = 1;

	/* ACK policy: multicast and control
	 * frames are not acked, QoS frames follow the QoS control field. */
	if (!tx_desc->unicast)
		tx_desc->ack_policy = 1;
	else if (tx_desc->qos)
		tx_desc->ack_policy =
			(*ieee80211_get_qos_ctl(hdr) & 0x60) >> 5;
	else if (ieee80211_is_ctl(hdr->frame_control))
		tx_desc->ack_policy = 1;

	if (info->flags & IEEE80211_TX_CTL_NO_ACK)
		tx_desc->ack_policy = 1;

	if (ieee80211_is_mgmt(hdr->frame_control) ||
	    ieee80211_is_nullfunc(hdr->frame_control) ||
	    ieee80211_is_qos_nullfunc(hdr->frame_control))
		tx_desc->txq_idx = SSV6051_TXQ_MGMT;

	/* Flow engine chain: one engine per
	 * nibble, lowest first. The frame enters from the host interface
	 * (M_ENG_HWHCI), optionally goes through the crypto engine
	 * (M_ENG_ENCRYPT) and ends in the TX queue engine (txq_idx +
	 * M_ENG_TX_EDCA0; the mgmt queue 4 maps to M_ENG_TX_MNG). */
	tx_desc->fCmd = tx_desc->txq_idx + M_ENG_TX_EDCA0;

	/* HW encryption of protected data frames (management frames are
	 * encrypted by mac80211, SW_MGMT_TX). The group key is selected
	 * through the AP's WSID. */
	if (ieee80211_has_protected(hdr->frame_control) &&
	    (ieee80211_is_data_qos(hdr->frame_control) ||
	     ieee80211_is_data(hdr->frame_control))) {
		if (tx_desc->unicast ? (sdev->hw_pair_crypt &&
					tx_desc->wsid != 0x0F) :
				       (sdev->hw_group_crypt &&
					sdev->hw_wsid >= 0)) {
			if (!tx_desc->unicast)
				tx_desc->wsid = sdev->hw_wsid;
			tx_desc->fCmd = (tx_desc->fCmd << 4) | M_ENG_ENCRYPT;
		}
	}
	tx_desc->fCmd = (tx_desc->fCmd << 4) | M_ENG_HWHCI;

	/* TX rate selection: mac80211's rate control gives the index into
	 * the band rate table (rates[0].idx; .count is the retry count);
	 * the HW rate is that entry's hw_value, or hw_value_short for CCK
	 * with short preamble. The driver has no HT TX
	 * (tx_desc->ht is always 0), so MCS rates fall back to the lowest
	 * basic rate. The control rate (RTS/CTS) always goes at the lowest
	 * basic rate (CTS-protect). */
	tx_desc->drate_idx = 0;
	if (!(info->control.rates[0].flags & IEEE80211_TX_RC_MCS) &&
	    info->control.rates[0].idx >= 0) {
		const struct ieee80211_rate *rate =
			ieee80211_get_tx_rate(hw, info);

		if ((info->control.rates[0].flags &
		     IEEE80211_TX_RC_USE_SHORT_PREAMBLE) &&
		    (rate->flags & IEEE80211_RATE_SHORT_PREAMBLE))
			tx_desc->drate_idx = rate->hw_value_short;
		else
			tx_desc->drate_idx = rate->hw_value;
	}
	tx_desc->crate_idx = 0;

	tx_flags = HCI_FLAGS_NO_FLOWCTRL;
	ssv_dbg(DBG_TX, sdev->dev,
		"TX %u bytes on queue %d (wsid %u, c_type %u)\n", skb->len,
		tx_desc->txq_idx, tx_desc->wsid, tx_desc->c_type);

	/* Queue on the descriptor's queue (mgmt frames go to TXQ4); the actual card
	 * write happens in the TX work (atomic context here). */
	if (ssv6051_hci_tx_skb(sdev, skb, tx_desc->txq_idx, tx_flags)) {
		sdev->stats.tx_dropped++;
		ssv_dbg(DBG_TX, sdev->dev, "TX queue %d rejected frame\n",
			tx_desc->txq_idx);
		/* Queue rejected: the skb is still ours. ieee80211_free_txskb
		 * (not dev_kfree_skb) keeps the iTXQ/AQL accounting of the
		 * frame consistent. */
		ieee80211_free_txskb(hw, skb);
	}
}

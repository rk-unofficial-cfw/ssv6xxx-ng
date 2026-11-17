// SPDX-License-Identifier: GPL-2.0
/*
 * Based on the ssv6xxx driver:
 *   Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 *   Copyright (c) 2015 iComm Corporation
 */

#include <linux/crc32.h>

#include "ssv6051.h"

/* Bit number of a TX queue within the interrupt mask. */
static inline u32 ssv6051_hci_get_int_bitno(int txqid)
{
	if (txqid == SSV_HW_TXQ_NUM - 1)
		return 1;
	return txqid + 3;
}

static void ssv6051_hci_txq_init(struct ssv6051_dev *sdev)
{
	int i;

	for (i = 0; i < SSV_HW_TXQ_NUM; i++) {
		sdev->hw_txq[i].txq_no = i;
		skb_queue_head_init(&sdev->hw_txq[i].qhead);
		sdev->hw_txq[i].max_qsize = SSV_HW_TXQ_MAX_SIZE;
		sdev->hw_txq[i].resum_thres = SSV_HW_TXQ_RESUME_THRES;
		sdev->hw_txq[i].paused = false;
		sdev->hw_txq[i].tx_pkt = 0;
		sdev->hw_txq[i].tx_flags = 0;
	}
}

static void ssv6051_hci_rx_rearm(struct work_struct *work);
static void ssv6051_hci_tx_work(struct work_struct *work);
static void ssv6051_hci_tx_retry(struct work_struct *work);

int ssv6051_hci_init(struct ssv6051_dev *sdev)
{
	ssv_dbg(DBG_HCI, sdev->dev, "init\n");

	sdev->int_mask = 0;
	sdev->started = false;
	ssv6051_hci_txq_init(sdev);

	sdev->rx_buf = dev_alloc_skb(MAX_RX_FRAME_SIZE);
	if (!sdev->rx_buf)
		return -ENOMEM;

	skb_queue_head_init(&sdev->rx_skb_q);

	/* Parsing/delivery work: never touches the SDIO bus, so it cannot
	 * contend with the IRQ thread for the host claim. */
	sdev->wq = alloc_workqueue(SSV6051_DRV_NAME "-wq", WQ_UNBOUND, 1);
	if (!sdev->wq) {
		dev_kfree_skb(sdev->rx_buf);
		sdev->rx_buf = NULL;
		return -ENOMEM;
	}
	INIT_WORK(&sdev->rx_work, ssv6051_hci_rx_work);
	INIT_DELAYED_WORK(&sdev->rx_rearm, ssv6051_hci_rx_rearm);
	INIT_WORK(&sdev->tx_work, ssv6051_hci_tx_work);
	INIT_DELAYED_WORK(&sdev->tx_retry, ssv6051_hci_tx_retry);

	return 0;
}

void ssv6051_hci_deinit(struct ssv6051_dev *sdev)
{
	struct sk_buff *skb;
	int i;

	ssv_dbg(DBG_HCI, sdev->dev, "deinit\n");

	ssv6051_hci_stop(sdev);

	if (sdev->wq) {
		cancel_work_sync(&sdev->rx_work);
		cancel_delayed_work_sync(&sdev->rx_rearm);
		cancel_work_sync(&sdev->tx_work);
		cancel_delayed_work_sync(&sdev->tx_retry);
		destroy_workqueue(sdev->wq);
		sdev->wq = NULL;
	}
	/* Drain frames still queued in the TX queues and the RX queue. */
	for (i = 0; i < SSV_HW_TXQ_NUM; i++) {
		while ((skb = skb_dequeue(&sdev->hw_txq[i].qhead)) != NULL)
			dev_kfree_skb(skb);
	}
	while ((skb = skb_dequeue(&sdev->rx_skb_q)) != NULL)
		dev_kfree_skb(skb);
	if (sdev->rx_buf) {
		dev_kfree_skb(sdev->rx_buf);
		sdev->rx_buf = NULL;
	}
}

/* Send the power-save host command so the MCU stops using the SDIO
 * interface before the host disables the function (this also redirects
 * the RX flow engines to the trash can so no frame is pushed to a dying
 * host). Only meaningful once the firmware is loaded: call it from
 * remove, never from the probe error paths. */
void ssv6051_hci_fw_sleep(struct ssv6051_dev *sdev)
{
	struct ssv6051_host_cmd *host_cmd;
	struct sk_buff *skb;

	/* No frame may reach the host while it is going away. */
	ssv6051_sdio_write_reg(sdev, ADR_RX_FLOW_DATA,
			       M_ENG_MACRX | (M_ENG_TRASH_CAN << 4));
	ssv6051_sdio_write_reg(sdev, ADR_RX_FLOW_MNG,
			       M_ENG_MACRX | (M_ENG_TRASH_CAN << 4));
	ssv6051_sdio_write_reg(sdev, ADR_RX_FLOW_CTRL,
			       M_ENG_MACRX | (M_ENG_TRASH_CAN << 4));

	skb = dev_alloc_skb(SSV6051_HOST_CMD_HDR_LEN);
	if (!skb)
		return;
	skb_put(skb, SSV6051_HOST_CMD_HDR_LEN);
	host_cmd = (struct ssv6051_host_cmd *)skb->data;
	memset(host_cmd, 0, SSV6051_HOST_CMD_HDR_LEN);
	host_cmd->len = SSV6051_HOST_CMD_HDR_LEN;
	host_cmd->c_type = HOST_CMD;
	host_cmd->h_cmd = SSV6XXX_HOST_CMD_PS;
	ssv6051_hci_send_cmd_skb(sdev, skb);
	dev_kfree_skb(skb);
}

/* Pause/resume the card HW TX queues: mask bits 0-4 select TXQ0-4 and are written to
 * ADR_MTX_MISC_EN bits 16-20. Used around channel switches so no data
 * frame is in flight while the RF retunes; management frames live in
 * TXQ4 and are not paused by the EDCA mask. */
static int ssv6051_hci_txq_pause_resume(struct ssv6051_dev *sdev,
					u32 txq_mask, bool pause)
{
	u32 regval;
	int ret, txqid;

	for (txqid = 0; txqid < SSV_HW_TXQ_NUM; txqid++) {
		if (!(txq_mask & (1 << txqid)))
			continue;
		sdev->hw_txq[txqid].paused = pause;
	}

	ret = ssv6051_sdio_read_reg(sdev, ADR_MTX_MISC_EN, &regval);
	if (ret)
		return ret;
	regval &= ~MTX_TXQ_PAUSE_MSK;
	if (pause)
		regval |= (txq_mask & 0x1F) << MTX_TXQ_PAUSE_SFT;
	return ssv6051_sdio_write_reg(sdev, ADR_MTX_MISC_EN, regval);
}

int ssv6051_hci_txq_pause(struct ssv6051_dev *sdev, u32 txq_mask)
{
	return ssv6051_hci_txq_pause_resume(sdev, txq_mask, true);
}

int ssv6051_hci_txq_resume(struct ssv6051_dev *sdev, u32 txq_mask)
{
	return ssv6051_hci_txq_pause_resume(sdev, txq_mask, false);
}

int ssv6051_hci_start(struct ssv6051_dev *sdev)
{
	int ret;

	ssv_dbg(DBG_HCI, sdev->dev, "start\n");

	ret = ssv6051_sdio_pmu_wakeup(sdev);
	if (ret)
		return ret;

	/* Claim the SDIO IRQ here, not at probe: the card
	 * interrupt is armed only at HCI start, i.e. after firmware,
	 * HW init and channel setup. Claiming it earlier would enable the
	 * card's function interrupt (CCCR IntEnable) while the card CPU is
	 * still rebooting for the firmware checksum handshake.
	 */
	ret = ssv6051_sdio_irq_attach(sdev);
	if (ret) {
		ssv_err(sdev->dev, "SDIO claim IRQ failed (%d)\n", ret);
		return ret;
	}

	ret = ssv6051_sdio_irq_status(sdev, &sdev->int_mask);
	if (ret)
		return ret;

	/* Unmask only the interrupts we service: RX and SoC events.
	 * INT_TX is the level "mgmt queue has room" interrupt: it is
	 * almost always asserted and the TX work polls the card resources
	 * itself, so leaving it unmasked makes the MMC IRQ thread spin and
	 * starve every other SDIO user of the host claim (unmask it
	 * only while frames are queued).
	 * REG_INT_MASK is an SDIO function-1 register written with CMD52. */
	sdev->int_mask = SSV6XXX_INT_RX | SSV6XXX_INT_SOC;
	ret = ssv6051_sdio_write_byte(sdev, REG_INT_MASK,
				      (u8)(~sdev->int_mask & 0xFF));
	if (ret)
		return ret;

	sdev->started = true;
	return 0;
}

void ssv6051_hci_stop(struct ssv6051_dev *sdev)
{
	ssv_dbg(DBG_HCI, sdev->dev, "stop\n");

	if (!sdev->started)
		return;

	/* Mask all interrupt sources (CMD52 byte write), then detach the
	 * IRQ at HCI stop.
	 */
	ssv6051_sdio_write_byte(sdev, REG_INT_MASK, 0xFF);
	ssv6051_sdio_irq_detach(sdev);
	sdev->int_mask = 0;
	sdev->started = false;
}

int ssv6051_hci_send_cmd_skb(struct ssv6051_dev *sdev, struct sk_buff *skb)
{
	ssv_dbg(DBG_HCI, sdev->dev, "send cmd skb (%u bytes)\n", skb->len);
	return ssv6051_sdio_write_data(sdev, skb->data, skb->len);
}

/* Card TX resource check: the free
 * TX pages/IDs read from the card must cover the frame, else the
 * firmware's TX handling overruns and wedges. Returns true when the
 * frame can be sent. */
static bool ssv6051_hci_txq_has_resource(struct ssv6051_dev *sdev,
					 int txqid, struct sk_buff *skb)
{
	static const u8 max_frame[SSV_HW_TXQ_NUM] = {
		SSV6051_ID_Q0_OUT_QUEUE, SSV6051_ID_Q1_OUT_QUEUE,
		SSV6051_ID_Q2_OUT_QUEUE, SSV6051_ID_Q3_OUT_QUEUE,
		SSV6051_ID_MNG_QUEUE
	};
	u32 page_count, free_page, free_id, max_f;
	int ret;

	/* Page count formula expected by the firmware (the skb already
	 * includes the 80-byte descriptor). */
	page_count = skb->len + SSV6200_ALLOC_RSVD;
	if (page_count & HW_MMU_PAGE_MASK)
		page_count = (page_count >> HW_MMU_PAGE_SHIFT) + 1;
	else
		page_count = page_count >> HW_MMU_PAGE_SHIFT;

	if (txqid == SSV_HW_TXQ_NUM - 1) {
		struct ssv6051_hci_txq_info2 info;

		ret = ssv6051_sdio_read_reg(sdev, ADR_TX_ID_ALL_INFO2,
					    (u32 *)&info);
		if (ret)
			return true; /* don't stall on a read failure */
		free_page = SSV6200_PAGE_TX_THRESHOLD - info.tx_use_page;
		free_id = SSV6200_ID_TX_THRESHOLD - info.tx_use_id;
		max_f = max_frame[txqid] - info.txq4_size;
	} else {
		struct ssv6051_hci_txq_info info;
		u8 qsize;

		ret = ssv6051_sdio_read_reg(sdev, ADR_TX_ID_ALL_INFO,
					    (u32 *)&info);
		if (ret)
			return true;
		free_page = SSV6200_PAGE_TX_THRESHOLD - info.tx_use_page;
		free_id = SSV6200_ID_TX_THRESHOLD - info.tx_use_id;
		switch (txqid) {
		case 0: qsize = info.txq0_size; break;
		case 1: qsize = info.txq1_size; break;
		case 2: qsize = info.txq2_size; break;
		default: qsize = info.txq3_size; break;
		}
		max_f = max_frame[txqid] - qsize;
	}

	return free_page >= page_count && free_id > 0 && max_f > 0;
}

/* TX worker: drains the HW TX queues and writes the frames to the
 * card. mac80211's .tx may run in ATOMIC context (softirq, tasklet,
 * wake_tx_queue), so the sleeping SDIO writes must never happen there:
 * .tx only enqueues and this work (process context) does the card I/O
 * — hence the split. */
static void ssv6051_hci_tx_work(struct work_struct *work)
{
	struct ssv6051_dev *sdev = container_of(work, struct ssv6051_dev,
					       tx_work);
	struct sk_buff *skb;
	int i, ret;

	/* Management queue last-index first (probe requests during scan
	 * live in TXQ4), then the EDCA queues. */
	for (i = SSV_HW_TXQ_NUM - 1; i >= 0; i--) {
		struct ssv6051_hw_txq *txq = &sdev->hw_txq[i];

		while ((skb = skb_dequeue(&txq->qhead)) != NULL) {
			if (!sdev->started) {
				/* HCI stopped (interface down): the card IRQ is
				 * detached, drop the leftovers instead of
				 * writing to a possibly quiesced card. */
				sdev->stats.tx_dropped++;
				ieee80211_free_txskb(sdev->hw, skb);
				continue;
			}
			if (!ssv6051_hci_txq_has_resource(sdev, i, skb)) {
				/* Card TX resources exhausted: put the frame back
				 * and retry later. */
				skb_queue_head(&txq->qhead, skb);
				queue_delayed_work(sdev->wq, &sdev->tx_retry,
						   msecs_to_jiffies(20));
				return;
			}
			ret = ssv6051_sdio_write_data(sdev, skb->data,
						      skb->len);
			if (ret) {
				sdev->stats.tx_dropped++;
				ssv_dbg(DBG_TX, sdev->dev,
					"TXQ%d write failed (%d)\n",
					i, ret);
				ieee80211_free_txskb(sdev->hw, skb);
				continue;
			}
			sdev->stats.tx_pkts++;

			/* Return the frame to mac80211: strip the descriptor and
			 * report a (fake) ACK. mac80211 owns the skb from here: it does
			 * the accounting (iTXQ airtime/AQL debt is repaid only in
			 * ieee80211_tx_status) and frees it. */
			skb_pull(skb, sizeof(struct ssv6051_tx_desc));
			ieee80211_tx_info_clear_status(IEEE80211_SKB_CB(skb));
			IEEE80211_SKB_CB(skb)->flags |= IEEE80211_TX_STAT_ACK;
			IEEE80211_SKB_CB(skb)->status.ack_signal = 100;
			ieee80211_tx_status_ni(sdev->hw, skb);
		}
	}
}

/* Retry the TX drain after a resource stall (the card frees pages/IDs
 * as its firmware completes the frames). */
static void ssv6051_hci_tx_retry(struct work_struct *work)
{
	struct ssv6051_dev *sdev = container_of(work, struct ssv6051_dev,
					       tx_retry.work);

	if (sdev->started)
		queue_work(sdev->wq, &sdev->tx_work);
}

int ssv6051_hci_tx_skb(struct ssv6051_dev *sdev, struct sk_buff *skb,
		       int txqid, u32 flags)
{
	struct ssv6051_hw_txq *txq;

	if (txqid < 0 || txqid >= SSV_HW_TXQ_NUM)
		return -EINVAL;

	txq = &sdev->hw_txq[txqid];
	txq->tx_flags = flags;

	/* Atomic context here: queue only, never touch the card. */
	if (skb_queue_len(&txq->qhead) >= txq->max_qsize) {
		txq->paused = true;
		ssv_dbg(DBG_HCI, sdev->dev, "TXQ%d full (%d frames)\n", txqid,
			skb_queue_len(&txq->qhead));
		return -ENOBUFS;
	}

	if (flags & HCI_FLAGS_ENQUEUE_HEAD)
		skb_queue_head(&txq->qhead, skb);
	else
		skb_queue_tail(&txq->qhead, skb);
	txq->tx_pkt++;

	queue_work(sdev->wq, &sdev->tx_work);
	return 0;
}

/* Read frames from the chip. Each frame gets
 * its own skb, queued for parsing in the RX work (which does no SDIO
 * I/O). The loop runs while the card reports RX pending, up to 32
 * frames. If the card keeps reporting the SAME packet length, the
 * firmware is not popping the packet from its FIFO (it died or is
 * stuck): bail out and mask the RX interrupt, or the MMC irq thread
 * would livelock on this handler with the host claim held forever.
 * The rearm work un-masks it after a delay to give the card a chance
 * to recover. */
#define RX_STALL_REPEATS 16
#define RX_STALL_REARM_MS 1000
#define RX_DEBUG_FIRST_FRAMES 8

static void ssv6051_hci_rx_rearm(struct work_struct *work)
{
	struct ssv6051_dev *sdev = container_of(work, struct ssv6051_dev,
						rx_rearm.work);
	u32 mask;

	/* Un-mask RX again (REG_INT_MASK: 1 = masked). */
	mask = SSV6XXX_INT_RX | SSV6XXX_INT_SOC;
	ssv6051_sdio_write_byte(sdev, REG_INT_MASK, (u8)~mask & 0xFF);
	ssv_dbg(DBG_HCI, sdev->dev, "RX interrupt re-armed\n");
}

static void ssv6051_hci_rx_poll(struct ssv6051_dev *sdev)
{
	struct sk_buff *skb;
	size_t len;
	int ret, cnt;
	u32 status, crc;
	int same = 0;
	bool stalled = false;

	for (cnt = 0; cnt < 32; cnt++) {
		ret = ssv6051_sdio_irq_status(sdev, &status);
		if (ret)
			break;

		if (!(status & SSV6XXX_INT_RX))
			break;

		len = MAX_RX_FRAME_SIZE;
		ret = ssv6051_sdio_read_data(sdev, sdev->rx_buf->data, &len);
		if (ret) {
			ssv_dbg(DBG_RX, sdev->dev, "RX read failed (%d)\n", ret);
			break;
		}
		if (len == 0)
			break;

		/* Pop-stall detection: the card keeps handing out the same
		 * packet, i.e. the firmware is not consuming its RX FIFO.
		 * Beacons from the same AP share both length and the leading
		 * bytes, so the packet is considered the same only on a full
		 * CRC match: if so the RX interrupt is masked and retried
		 * later. */
		crc = crc32_le(~0, sdev->rx_buf->data, len);
		if (len == sdev->rx_last_len && crc == sdev->rx_last_crc) {
			if (++same >= RX_STALL_REPEATS) {
				ssv_warn(sdev->dev,
					 "card not popping RX packet (len %zu repeated %d times): firmware not servicing the RX FIFO, masking RX\n",
					 len, same);
				stalled = true;
				break;
			}
		} else {
			same = 0;
			sdev->rx_last_len = len;
			sdev->rx_last_crc = crc;
		}

		/* Hand the buffer to the queue: parsing and mac80211 delivery
		 * happen in the RX work, which does no SDIO I/O, keeping
		 * the parsing off the interrupt context. */
		skb = sdev->rx_buf;
		skb_put(skb, len);
		sdev->rx_buf = dev_alloc_skb(MAX_RX_FRAME_SIZE);
		if (!sdev->rx_buf) {
			ssv_err(sdev->dev, "RX buffer allocation failure\n");
			sdev->rx_buf = skb;
			break;
		}

		sdev->stats.rx_pkts++;
		/* One line per frame is affordable even on a slow console (the
		 * hex dumps in rx.c are the expensive part and stay gated to the
		 * first frames). */
		ssv_dbg(DBG_RX, sdev->dev, "RX frame %u bytes\n", (u32)len);

		skb_queue_tail(&sdev->rx_skb_q, skb);
	}

	if (stalled) {
		/* Mask RX at the card (REG_INT_MASK: 1 = masked). */
		ssv6051_sdio_write_byte(sdev, REG_INT_MASK,
					(u8)~SSV6XXX_INT_SOC & 0xFF);
		queue_delayed_work(sdev->wq, &sdev->rx_rearm,
				   msecs_to_jiffies(RX_STALL_REARM_MS));
	}
}

/* Parse and deliver the queued frames: NO SDIO I/O here, so this work
 * can never starve the SDIO host claim (the frames were already read by
 * the IRQ handler). */
void ssv6051_hci_rx_work(struct work_struct *work)
{
	struct ssv6051_dev *sdev = container_of(work, struct ssv6051_dev,
						rx_work);
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&sdev->rx_skb_q)) != NULL)
		ssv6051_rx_process(sdev, skb);
}

/* SDIO interrupt handler, called from the MMC core sdio_irq_thread which
 * already holds the SDIO host claim.
 * The RX frames are read INLINE here: deferring them to a workqueue
 * would require re-claiming the host, and with the card interrupting
 * continuously the MMC thread would keep the claim almost permanently,
 * starving (livelock) both the worker and any other SDIO user (e.g. the
 * channel tuning in .config). */
void ssv6051_hci_irq_handler(struct ssv6051_dev *sdev)
{
	u32 status;
	int ret;

	ret = ssv6051_sdio_irq_status(sdev, &status);
	if (ret)
		return;

	sdev->stats.isr_count++;
	if (status & SSV6XXX_INT_RX)
		sdev->stats.isr_rx_count++;
	if (status & SSV6XXX_INT_TX)
		sdev->stats.isr_tx_count++;
	if (status & SSV6XXX_INT_SOC)
		sdev->stats.isr_miss_count++;

	if (status & SSV6XXX_INT_RX) {
		ssv6051_hci_rx_poll(sdev);
		queue_work(sdev->wq, &sdev->rx_work);
	}
}

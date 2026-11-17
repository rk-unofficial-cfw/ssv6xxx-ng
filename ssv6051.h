/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared definitions: device state, debug logging, cipher mapping.
 *
 * Based on the ssv6xxx driver:
 *   Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 *   Copyright (c) 2015 iComm Corporation
 */
#ifndef _SSV6051_H_
#define _SSV6051_H_

#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/firmware.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mutex.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <net/mac80211.h>

#include "regs.h"
#include "hci.h"

#define SSV6051_DRV_NAME    "ssv6051"
#define SSV6051_FW_NAME     "ssv6051-sw.bin"
#define SSV_MAX_FRAME_SIZE  2304

/* ------------------------------------------------------------------ */
/* Debug logging: bit mask via module parameter debug=0x...            */
/* ------------------------------------------------------------------ */
#define DBG_SDIO   0x00000001u  /* SDIO transactions, bus errors        */
#define DBG_HCI    0x00000002u  /* HCI framing, queues, timeouts        */
#define DBG_FW     0x00000004u  /* firmware download, version handshake */
#define DBG_KEY    0x00000008u  /* cipher mapping, SRAM key slots       */
#define DBG_TX     0x00000010u  /* TX path statistics, drops            */
#define DBG_RX     0x00000020u  /* RX path statistics, drops            */
#define DBG_REG    0x00000040u  /* register I/O                         */
#define DBG_EFUSE  0x00000080u  /* efuse values read from chip          */
#define DBG_IQK    0x00000100u  /* calibration config and results       */
#define DBG_INIT   0x00000200u  /* init/deinit sequence                 */
#define DBG_ALL    0x7FFFFFFFu

extern unsigned int ssv6051_debug;
extern unsigned int ssv6051_fw_retry;
extern char *ssv6051_initmac;
extern unsigned int ssv6051_xtal_clock;
extern unsigned int ssv6051_volt_regulator;
extern unsigned int ssv6051_def_chan;

#define ssv_dbg(cat, dev, fmt, ...)                                        \
	do {                                                               \
		if (ssv6051_debug & (cat))                                 \
			dev_dbg((dev), "%s: " fmt, __func__,               \
				##__VA_ARGS__);                            \
	} while (0)

#define ssv_dbg_hex(cat, dev, tag, buf, len)                               \
	do {                                                               \
		if (ssv6051_debug & (cat))                                 \
			print_hex_dump_debug(tag, DUMP_PREFIX_OFFSET,      \
					     16, 1, (buf), (len), true);   \
	} while (0)

#define ssv_err(dev, fmt, ...)                                             \
	dev_err((dev), "%s: " fmt, __func__, ##__VA_ARGS__)

#define ssv_warn(dev, fmt, ...)                                            \
	dev_warn((dev), "%s: " fmt, __func__, ##__VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Cipher mapping                                                      */
/* ------------------------------------------------------------------ */
enum ssv6051_cipher {
	SSV_CIPHER_NONE = 0,
	SSV_CIPHER_WEP40,
	SSV_CIPHER_WEP104,
	SSV_CIPHER_TKIP,
	SSV_CIPHER_CCMP,
	SSV_CIPHER_SMS4,
	SSV_CIPHER_INVALID = -1,
};

#define SSV_SECURITY_KEY_LEN 32

/* Key layout as stored in chip SRAM */
struct ssv6051_hw_key {
	u8 key[SSV_SECURITY_KEY_LEN];
	u32 tx_pn_l;
	u32 tx_pn_h;
	u32 rx_pn_l;
	u32 rx_pn_h;
} __packed;

struct ssv6051_hw_sta_key {
	u8 pair_key_idx : 4;
	u8 group_key_idx : 4;
	u8 valid;
	u8 reserve[2];
	struct ssv6051_hw_key pair;
} __packed;

struct ssv6051_hw_sec {
	struct ssv6051_hw_key group_key[3];
	struct ssv6051_hw_sta_key sta_key[8];
} __packed;

/* ------------------------------------------------------------------ */
/* Configuration (from efuse / module parameters)                      */
/* ------------------------------------------------------------------ */
enum ssv6051_efuse_item_id {
	/* Item IDs are written by the calibration tool into the efuse map
	 * nibbles: they start at 1, NOT 0.
	 * A map nibble of 0 terminates the parse. */
	EFUSE_R_CALIBRATION_RESULT = 1,
	EFUSE_SAR_RESULT,
	EFUSE_MAC,
	EFUSE_CRYSTAL_FREQUENCY_OFFSET,
	EFUSE_TX_POWER_INDEX_1,
	EFUSE_TX_POWER_INDEX_2,
	SSV6051_EFUSE_ITEMS
};

struct ssv6051_efuse_item {
	u8 byte_cnts;
	u8 offset;
	u16 value;
};

struct ssv6051_cfg {
	u32 chip_identity;
	u32 crystal_type;
	u32 channel;               /* current channel (1-14)      */
	u32 volt_regulator;        /* 0 = DCDC, 1 = LDO */
	u32 def_chan;              /* default channel at init      */
	/* Default TX scales, extracted from the patched ADR_TX_GAIN_FACTOR
	 * entry. */
	u8 def_tx_scale_11b;
	u8 def_tx_scale_11b_p0d5;
	u8 def_tx_scale_11g;
	u8 def_tx_scale_11g_p0d5;
	u8 r_calibration_result;
	u8 sar_result;
	u8 crystal_frequency_offset;
	u8 tx_power_index_1;
	u8 tx_power_index_2;
	bool ignore_efuse_mac;
	bool ipd_channel_touch;    /* ch13/14 ABB/ADC override active */
	u32 ch1_12_value[2];       /* ABB/ADC values for channels 1-12 */
	char *fw_name;
};

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */
struct ssv6051_stats {
	u32 isr_count;
	u32 isr_tx_count;
	u32 isr_rx_count;
	u32 isr_miss_count;
	u32 tx_pkts;
	u32 tx_dropped;
	u32 tx_errors;
	u32 rx_pkts;
	u32 rx_dropped;
	u32 rx_errors;
	u32 fw_checksum_errors;
	u32 sdio_io_errors;
	u32 cmd_timeouts;
};

/* ------------------------------------------------------------------ */
/* Per-device state                                                    */
/* ------------------------------------------------------------------ */
struct ssv6051_hw_txq {
	u32 txq_no;
	struct sk_buff_head qhead;
	int max_qsize;
	int resum_thres;
	bool paused;
	u32 tx_pkt;
	u32 tx_flags;
};

struct ssv6051_vif_priv {
	struct ieee80211_vif *vif;
	u8 mac_addr[ETH_ALEN];
	bool assoc;
};

struct ssv6051_sta_priv {
	struct ieee80211_sta *sta;
	int hw_wsid;
};

struct ssv6051_dev {
	struct device *dev;
	struct sdio_func *func;
	struct ieee80211_hw *hw;

	/* No sdio_lock mutex: all SDIO I/O is serialised by the host claim
	 * alone (sdio_claim_host). An extra mutex
	 * here created an ABBA deadlock with the MMC sdio_irq_thread: it
	 * holds the host claim while calling our IRQ handler (which needs
	 * the mutex), while a process-context caller held the mutex and
	 * waited for the claim. */
	struct mutex mem_lock;       /* serialises pbuf alloc/free  */
	u32 int_mask;
	bool started;

	struct ssv6051_cfg cfg;

	/* SDIO glue ports (read from card registers at probe) */
	u32 data_io_port;
	u32 reg_io_port;

	/* HCI state */
	struct ssv6051_hw_txq hw_txq[SSV_HW_TXQ_NUM];
	struct sk_buff *rx_buf;
	/* CRC of the last RX packet, for the pop-stall detection: identical
	 * length is normal (beacons from the same AP), only identical
	 * content means the card is serving the same packet over and over. */
	u32 rx_last_crc;
	size_t rx_last_len;
	/* RX frames read from the card by the IRQ handler (the only place
	 * doing SDIO I/O), parsed and delivered by the work below (which
	 * never touches the SDIO bus: no host-claim contention). */
	struct sk_buff_head rx_skb_q;
	struct workqueue_struct *wq;
	struct work_struct rx_work;
	struct delayed_work rx_rearm;
	/* Card writes for queued TX frames: .tx may run in atomic context,
	 * so the sleeping SDIO writes happen in this work instead. */
	struct work_struct tx_work;
	/* Re-drain after a card TX resource stall. */
	struct delayed_work tx_retry;

	/* Chip SRAM buffers */
	u32 hw_sec_key;     /* SRAM address of struct ssv6051_hw_sec */
	u32 hw_pinfo;       /* SRAM address of PHY info tables       */

	/* efuse items parsed at probe (see efuse.c) */
	struct ssv6051_efuse_item efuse_items[SSV6051_EFUSE_ITEMS];

	/* Connection state (v1: single STA interface) */
	bool associated;
	struct ieee80211_vif *vif;
	u8 bssid[ETH_ALEN];
	u8 perm_addr[ETH_ALEN];
	int hw_wsid;
	atomic_t seq_no;

	/* HW crypto (key.c). Single STA interface: only the AP gets a HW
	 * WSID (0); sec_shadow mirrors the key table in chip SRAM. */
	struct ssv6051_hw_sec sec_shadow;
	bool hw_pair_crypt;	/* pairwise key in HW (TX encrypt + RX decrypt) */
	bool hw_group_crypt;	/* group key in HW */
	bool hw_wep;		/* WEP default keys in HW */

	struct ssv6051_stats stats;

	struct dentry *debugfs_dir;
};

/* module parameters (defined in main.c) */
extern unsigned int ssv6051_debug;
extern char *ssv6051_initmac;
extern char *ssv6051_firmware_name;  /* defined in fw.c */

/* SDIO device IDs (from hwif/sdio/sdio.c) */
#define SSV_VENDOR_ID    0x3030
#define SSV_CABRIO_DEVID 0x3030

/* sdio.c */
int ssv6051_sdio_read_reg(struct ssv6051_dev *sdev, u32 addr, u32 *val);
int ssv6051_sdio_write_reg(struct ssv6051_dev *sdev, u32 addr, u32 val);
int ssv6051_reg_set_bits(struct ssv6051_dev *sdev, u32 reg, u32 set,
			 u32 clr);
int ssv6051_sdio_read_data(struct ssv6051_dev *sdev, void *buf, size_t *len);
int ssv6051_sdio_write_data(struct ssv6051_dev *sdev, void *buf, size_t len);
int ssv6051_sdio_write_sram(struct ssv6051_dev *sdev, u32 addr, u8 *data,
			    u32 size);
int ssv6051_sdio_irq_attach(struct ssv6051_dev *sdev);
void ssv6051_sdio_irq_detach(struct ssv6051_dev *sdev);
int ssv6051_sdio_read_byte(struct ssv6051_dev *sdev, u32 addr, u8 *val);
int ssv6051_sdio_write_byte(struct ssv6051_dev *sdev, u32 addr, u8 val);
int ssv6051_sdio_irq_status(struct ssv6051_dev *sdev, u32 *status);
int ssv6051_sdio_pmu_wakeup(struct ssv6051_dev *sdev);

/* fw.c */
int ssv6051_load_firmware(struct ssv6051_dev *sdev);

/* efuse.c */
void ssv6051_efuse_read_all_map(struct ssv6051_dev *sdev);

/* init.c */
int ssv6051_pre_fw_init(struct ssv6051_dev *sdev);
int ssv6051_init_hw(struct ssv6051_dev *sdev);
void ssv6051_deinit_hw(struct ssv6051_dev *sdev);
int ssv6051_set_channel(struct ssv6051_dev *sdev, int ch);
u32 ssv6051_pbuf_alloc(struct ssv6051_dev *sdev, int size, int type);
bool ssv6051_pbuf_free(struct ssv6051_dev *sdev, u32 pbuf_addr);

/* hci.c */
int ssv6051_hci_init(struct ssv6051_dev *sdev);
void ssv6051_hci_deinit(struct ssv6051_dev *sdev);
int ssv6051_hci_start(struct ssv6051_dev *sdev);
void ssv6051_hci_stop(struct ssv6051_dev *sdev);
int ssv6051_hci_txq_pause(struct ssv6051_dev *sdev, u32 txq_mask);
int ssv6051_hci_txq_resume(struct ssv6051_dev *sdev, u32 txq_mask);
void ssv6051_hci_fw_sleep(struct ssv6051_dev *sdev);
int ssv6051_hci_send_cmd_skb(struct ssv6051_dev *sdev, struct sk_buff *skb);
int ssv6051_hci_tx_skb(struct ssv6051_dev *sdev, struct sk_buff *skb,
		       int txqid, u32 flags);
void ssv6051_hci_rx_work(struct work_struct *work);
void ssv6051_hci_irq_handler(struct ssv6051_dev *sdev);

/* key.c */
enum ssv6051_cipher ssv6051_prepare_key(struct ieee80211_key_conf *key);
int ssv6051_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
		    struct ieee80211_vif *vif, struct ieee80211_sta *sta,
		    struct ieee80211_key_conf *key);
int ssv6051_sta_add(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		    struct ieee80211_sta *sta);
int ssv6051_sta_remove(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		       struct ieee80211_sta *sta);

/* tx.c */
extern const u8 ssv6051_txq_map[4];
void ssv6051_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
		struct sk_buff *skb);

/* rx.c */
void ssv6051_rx_process(struct ssv6051_dev *sdev, struct sk_buff *skb);

/* debugfs.c */
int ssv6051_debugfs_init(struct ssv6051_dev *sdev);
void ssv6051_debugfs_deinit(struct ssv6051_dev *sdev);

/* sdio.c */
int ssv6051_sdio_register(void);
void ssv6051_sdio_unregister(void);

/* main.c */
struct ieee80211_hw *ssv6051_alloc_mac80211(void);
int ssv6051_register_mac80211(struct ssv6051_dev *sdev);
void ssv6051_unregister_mac80211(struct ssv6051_dev *sdev);
void ssv6051_restart_hw(struct ssv6051_dev *sdev);

#endif /* _SSV6051_H_ */

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Based on the ssv6xxx driver:
 *   Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 *   Copyright (c) 2015 iComm Corporation
 */
#ifndef _SSV6051_HCI_H_
#define _SSV6051_HCI_H_

#include <linux/skbuff.h>

#define SSV_HW_TXQ_NUM          5
#define SSV_HW_TXQ_MAX_SIZE     64
#define SSV_HW_TXQ_RESUME_THRES ((SSV_HW_TXQ_MAX_SIZE >> 2) * 3)

#define HCI_FLAGS_ENQUEUE_HEAD  0x00000001
#define HCI_FLAGS_NO_FLOWCTRL   0x00000002

#define SSV6XXX_INT_RX            0x00000001
#define SSV6XXX_INT_TX            0x00000002
#define SSV6XXX_INT_SOC           0x00000004
#define SSV6XXX_INT_LOW_EDCA_0    0x00000008
#define SSV6XXX_INT_LOW_EDCA_1    0x00000010
#define SSV6XXX_INT_LOW_EDCA_2    0x00000020
#define SSV6XXX_INT_LOW_EDCA_3    0x00000040
#define SSV6XXX_INT_RESOURCE_LOW  0x00000080

#define SSV62XX_TX_MAX_RATES 3

/* Rate control / retry parameters (from fw_rc_retry_params) */
struct ssv6051_fw_rc_retry_params {
	u32 count : 4;
	u32 drate : 6;
	u32 crate : 6;
	u32 rts_cts_nav : 16;
	u32 frame_consume_time : 10;
	u32 dl_length : 12;
	u32 RSVD : 10;
} __packed;

/* Host commands */
enum ssv6051_host_cmd_id {
	SSV6XXX_HOST_CMD_START = 0,
	SSV6XXX_HOST_CMD_LOG,
	SSV6XXX_HOST_CMD_PS,
	SSV6XXX_HOST_CMD_INIT_CALI,
	SSV6XXX_HOST_CMD_RX_TPUT,
	SSV6XXX_HOST_CMD_TX_TPUT,
	SSV6XXX_HOST_CMD_WSID_OP,
	SSV6XXX_HOST_CMD_SET_NOA,
	SSV6XXX_HOST_SOC_CMD_MAXID,
};

/* WSID host command payload (firmware FW_WSID_WATCH_LIST). */
enum ssv6051_wsid_ops {
	SSV6XXX_WSID_OPS_ADD,
	SSV6XXX_WSID_OPS_DEL,
	SSV6XXX_WSID_OPS_RESETALL,
	SSV6XXX_WSID_OPS_ENABLE_CAPS,
	SSV6XXX_WSID_OPS_DISABLE_CAPS,
	SSV6XXX_WSID_OPS_HWWSID_PAIRWISE_SET_TYPE,
	SSV6XXX_WSID_OPS_HWWSID_GROUP_SET_TYPE,
};

enum ssv6051_wsid_sec_type {
	SSV6XXX_WSID_SEC_SW,
	SSV6XXX_WSID_SEC_HW,
};

struct ssv6051_wsid_params {
	u8 cmd;
	u8 wsid_idx;
	u8 target_wsid[6];
	u8 hw_security;
} __packed;

/* SoC events (firmware built WITHOUT CONFIG_P2P_NOA:
 * the firmware never sends SOC_EVT_NOA, so the IDs after SOC_EVT_LOG
 * must NOT be shifted by one or RESET_HOST/WATCHDOG_TRIGGER decode
 * wrong). */
enum ssv6051_soc_event {
	SOC_EVT_GET_REG_RESP = 0,
	SOC_EVT_NO_BA,
	SOC_EVT_RC_MPDU_REPORT,
	SOC_EVT_RC_AMPDU_REPORT,
	SOC_EVT_LOG,
	SOC_EVT_USER_END,
	SOC_EVT_SDIO_TEST_COMMAND,
	SOC_EVT_RESET_HOST,
	SOC_EVT_SDIO_TXTPUT_RESULT,
	SOC_EVT_WATCHDOG_TRIGGER,
	SOC_EVT_MAXID,
};

/* TX descriptor (from ssv6200_tx_desc) */
struct ssv6051_tx_desc {
	u32 len : 16;
	u32 c_type : 3;
	u32 f80211 : 1;
	u32 qos : 1;
	u32 ht : 1;
	u32 use_4addr : 1;
	u32 RSVD_0 : 3;
	u32 bc_que : 1;
	u32 security : 1;
	u32 more_data : 1;
	u32 stype_b5b4 : 2;
	u32 extra_info : 1;
	u32 fCmd;
	u32 hdr_offset : 8;
	u32 frag : 1;
	u32 unicast : 1;
	u32 hdr_len : 6;
	u32 tx_report : 1;
	u32 tx_burst : 1;
	u32 ack_policy : 2;
	u32 aggregation : 1;
	u32 RSVD_1 : 3;
	u32 do_rts_cts : 2;
	u32 reason : 6;
	u32 payload_offset : 8;
	u32 RSVD_4 : 7;
	u32 RSVD_2 : 1;
	u32 fCmdIdx : 3;
	u32 wsid : 4;
	u32 txq_idx : 3;
	u32 TxF_ID : 6;
	u32 rts_cts_nav : 16;
	u32 frame_consume_time : 10;
	u32 crate_idx : 6;
	u32 drate_idx : 6;
	u32 dl_length : 12;
	u32 RSVD_3 : 14;
	u32 RESERVED[8];
	struct ssv6051_fw_rc_retry_params rc_params[SSV62XX_TX_MAX_RATES];
} __packed;

/* RX descriptor (from ssv6200_rx_desc) */
struct ssv6051_rx_desc {
	u32 len : 16;
	u32 c_type : 3;
	u32 f80211 : 1;
	u32 qos : 1;
	u32 ht : 1;
	u32 use_4addr : 1;
	u32 l3cs_err : 1;
	u32 l4cs_err : 1;
	u32 align2 : 1;
	u32 RSVD_0 : 2;
	u32 psm : 1;
	u32 stype_b5b4 : 2;
	u32 extra_info : 1;
	u32 edca0_used : 4;
	u32 edca1_used : 5;
	u32 edca2_used : 5;
	u32 edca3_used : 5;
	u32 mng_used : 4;
	u32 tx_page_used : 9;
	u32 hdr_offset : 8;
	u32 frag : 1;
	u32 unicast : 1;
	u32 hdr_len : 6;
	u32 RxResult : 8;
	u32 wildcard_bssid : 1;
	u32 RSVD_1 : 1;
	u32 reason : 6;
	u32 payload_offset : 8;
	u32 tx_id_used : 8;
	u32 fCmdIdx : 3;
	u32 wsid : 4;
	u32 RSVD_3 : 3;
	u32 rate_idx : 6;
};

/* RX PHY info (from ssv6200_rxphy_info) */
struct ssv6051_rxphy_info {
	u32 len : 16;
	u32 rsvd0 : 16;
	u32 mode : 3;
	u32 ch_bw : 3;
	u32 preamble : 1;
	u32 ht_short_gi : 1;
	u32 rate : 7;
	u32 rsvd1 : 1;
	u32 smoothing : 1;
	u32 no_sounding : 1;
	u32 aggregate : 1;
	u32 stbc : 2;
	u32 fec : 1;
	u32 n_ess : 2;
	u32 rsvd2 : 8;
	u32 l_length : 12;
	u32 l_rate : 3;
	u32 rsvd3 : 17;
	u32 rsvd4;
	u32 rpci : 8;
	u32 snr : 8;
	u32 service : 16;
};

/* Host command header (from cfg_host_cmd / HDR_HostCmd) */
struct ssv6051_host_cmd {
	u32 len : 16;
	u32 c_type : 3;
	u32 RSVD0 : 5;
	u32 h_cmd : 8;
	u32 cmd_seq_no;
	union {
		u32 dummy;
		u8 dat8[0];
		u16 dat16[0];
		u32 dat32[0];
	};
} __packed;

#define SSV6051_HOST_CMD_HDR_LEN (sizeof(struct ssv6051_host_cmd))

/* IQK calibration config: the firmware parses this payload, so the
 * layout is fixed. Deliberately NOT __packed: the u32:8 bitfields
 * pack four per word with trailing padding, giving sizeof 28 == IQK_CFG_LEN
 * (packed it would be 26 and every field after byte 10 would land on the
 * wrong offset). Enforced by BUILD_BUG_ON in ssv6051_do_iq_calib(). */
struct ssv6051_iqk_cfg {
	u32 cfg_xtal : 8;
	u32 cfg_pa : 8;
	u32 cfg_pabias_ctrl : 8;
	u32 cfg_pacascode_ctrl : 8;
	u32 cfg_tssi_trgt : 8;
	u32 cfg_tssi_div : 8;
	u32 cfg_def_tx_scale_11b : 8;
	u32 cfg_def_tx_scale_11b_p0d5 : 8;
	u32 cfg_def_tx_scale_11g : 8;
	u32 cfg_def_tx_scale_11g_p0d5 : 8;
	u32 cmd_sel;
	union {
		u32 fx_sel;
		u32 argv;
	};
	u32 phy_tbl_size;
	u32 rf_tbl_size;
};

/* Crystal types */
#define SSV6051_IQK_CFG_XTAL_26M 0
#define SSV6051_IQK_CFG_XTAL_40M 1
#define SSV6051_IQK_CFG_XTAL_24M 2

/* PA types */
#define SSV6051_IQK_CFG_PA_DEF    0
#define SSV6051_IQK_CFG_PA_LI_MPB 1
#define SSV6051_IQK_CFG_PA_LI_EVB 2
#define SSV6051_IQK_CFG_PA_HP     3

#define SSV6051_IQK_CMD_INIT_CALI 0

/* Calibration runs requested in fx_sel. */
#define SSV6051_IQK_TEMPERATURE 0x00000004u
#define SSV6051_IQK_RXDC       0x00000008u
#define SSV6051_IQK_RXRC       0x00000010u
#define SSV6051_IQK_TXDC       0x00000020u
#define SSV6051_IQK_TXIQ       0x00000040u
#define SSV6051_IQK_RXIQ       0x00000080u
#define SSV6051_IQK_PAPD       0x00000200u

/* Card TX resource usage: read from ADR_TX_ID_ALL_INFO / _INFO2 before every frame
 * send for the flow control. */
struct ssv6051_hci_txq_info {
	u32 tx_use_page : 8;
	u32 tx_use_id : 6;
	u32 txq0_size : 4;
	u32 txq1_size : 4;
	u32 txq2_size : 5;
	u32 txq3_size : 5;
};

struct ssv6051_hci_txq_info2 {
	u32 tx_use_page : 9;
	u32 tx_use_id : 8;
	u32 txq4_size : 4;
	u32 rsvd : 11;
};

/* Host event header (from cfg_host_event) */
struct ssv6051_host_event {
	u32 len : 16;
	u32 c_type : 3;
	u32 RSVD0 : 5;
	u32 h_event : 8;
	u32 evt_seq_no;
	u8 dat[0];
} __packed;

#endif /* _SSV6051_HCI_H_ */

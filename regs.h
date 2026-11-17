/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register definitions for the SSV6051, limited to what the STA path needs.
 *
 * Based on the ssv6xxx driver:
 *   Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 *   Copyright (c) 2015 iComm Corporation
 */
#ifndef _SSV6051_REGS_H_
#define _SSV6051_REGS_H_

/* Bus / block bases */
#define SYS_REG_BASE          0xc0000000
#define SPI_REG_BASE          0xc0000a00
#define PHY_RATE_INFO_BASE    0xca000200
#define MAC_GLB_SET_BASE      0xca000300
#define MB_REG_BASE           0xcd000000
#define ID_MNG_REG_BASE       0xcd010000
#define MMU_REG_BASE          0xcf000000
#define INT_REG_BASE          0xc0000e00

/* System block */
#define ADR_BRG_SW_RST              (SYS_REG_BASE + 0x00000000)
#define ADR_BOOT                    (SYS_REG_BASE + 0x00000004)
#define ADR_PLATFORM_CLOCK_ENABLE   (SYS_REG_BASE + 0x0000001c)

/* Card interrupt controller: routing mask of the internal IRQ sources
 * towards the SDIO host interface (1 = masked). It is programmed before
 * the firmware download so that
 * HCI_INT_1 (bit 14) and MBOX_INT_3 (bit 13) reach the host as soon as
 * the firmware starts using the HCI/mailbox; the UART debug sources
 * (bits 15-16) are unmasked too. Without it the reset default
 * (0xffffffff) keeps everything masked and the host never gets
 * firmware notifications.
 */
#define ADR_SDIO_MASK               (INT_REG_BASE + 0x0000003c)
#define SSV6051_SDIO_INT_MASK       0xfffe1fffu

/* Firmware status / boot handshake: FW_STATUS_REG doubles as the
 * version register (FW_VERSION_REG): during the download handshake it carries the checksum word, once the
 * firmware is running it reports its build number (12978). */
#define FW_STATUS_REG     ADR_TX_SEG
#define FW_STATUS_MASK    0x00FF0000u
#define FW_VERSION_REG    ADR_TX_SEG
#define SSV_FIRMWARE_VERSION 12978u
#define ADR_TX_SEG        (SPI_REG_BASE + 0x00000010)

/* Efuse controller (read sequence magic in efuse.c) */
#define SSV_EFUSE_ID_READ_SWITCH   0xC2000128
#define SSV_EFUSE_ID_RAW_DATA_BASE 0xC200014C
#define SSV_EFUSE_READ_SWITCH      0xC200012C
#define SSV_EFUSE_RAW_DATA_BASE    0xC2000150
#define EFUSE_HWSET_MAX_SIZE       (256 - 32)
#define EFUSE_MAX_SECTION_MAP      (EFUSE_HWSET_MAX_SIZE >> 5)
#define EFUSE_MAGIC_ENABLE         0xC0000328u
#define EFUSE_MAGIC_DISABLE        0x1800000Au

/* MAC/HCI init registers, programmed before the firmware download. */
#define ADR_CONTROL              0xc1000000u /* HCI_REG_BASE */
#define ADR_HCI_TX_RX_INFO_SIZE  (0xc1000000u + 0x00000030)
#define ADR_RX_TIME_STAMP_CFG    (0xc6000000u + 0x000000ec)
#define ADR_MRX_WATCH_DOG        (0xc6000000u + 0x0000011c)
#define ADR_MTX_BCN_EN_MISC      (0xc6002000u + 0x000000a8)
/* HW TX queue pause: bits 16-20 pause the card TX queues 0-4
 * (txq_mask << 16, mask 0x1F). */
#define ADR_MTX_MISC_EN          (0xc6002000u + 0x00000008)
#define MTX_TXQ_PAUSE_SFT        16
#define MTX_TXQ_PAUSE_MSK        0x001f0000u
#define ADR_TXQ4_MTX_Q_AIFSN     (0xc6002500u + 0x00000004)
/* EDCA parameters per HW queue (base + 0x100 * queue + 4): AIFS bits 0-3,
 * ECWmin bits 8-11, ECWmax bits 12-15, TXOP limit bits 16-31.
 * Written by .conf_tx. */
#define ADR_TXQ0_MTX_Q_AIFSN     (0xc6002100u + 0x00000004)
#define TXQ_MTX_Q_ECWMIN_SFT     8
#define TXQ_MTX_Q_ECWMAX_SFT     12
#define TXQ_MTX_Q_TXOP_LIMIT_SFT 16
#define ADR_TRX_ID_THRESHOLD     (ID_MNG_REG_BASE + 0x00000020)
/* TX resource-low interrupt trigger: bit 31 enables
 * the interrupt, bits 16-23 the free-ID trigger, bits 0-8 the free-page
 * trigger. */
#define ADR_TX_LIMIT_INTR        (ID_MNG_REG_BASE + 0x0000004c)
/* TX resource usage (read before
 * every frame send: free pages/IDs must cover the frame, else the
 * firmware's TX handling overruns and wedges). */
#define ADR_TX_ID_ALL_INFO       (ID_MNG_REG_BASE + 0x00000050)
#define ADR_TX_ID_ALL_INFO2      (ID_MNG_REG_BASE + 0x0000006c)
#define SSV6200_TX_PKT_RSVD      (SSV6200_TX_PKT_RSVD_SETTING * 16)
#define SSV6200_ALLOC_RSVD       (TXPB_OFFSET + SSV6200_TX_PKT_RSVD)
#define HW_MMU_PAGE_SHIFT        0x8
#define HW_MMU_PAGE_MASK         0xff
/* Per-queue max frames in flight: queue 0=BK, 1=BE, 2=VI, 3=VO, 4=management. */
#define SSV6051_ID_Q0_OUT_QUEUE  8
#define SSV6051_ID_Q1_OUT_QUEUE  15
#define SSV6051_ID_Q2_OUT_QUEUE  16
#define SSV6051_ID_Q3_OUT_QUEUE  16
#define SSV6051_ID_MNG_QUEUE     8
#define SSV6200_TX_LOWTHRESHOLD_PAGE_TRIGGER                            \
	(SSV6200_PAGE_TX_THRESHOLD - (SSV6200_PAGE_TX_THRESHOLD / 2))
#define SSV6200_TX_LOWTHRESHOLD_ID_TRIGGER 2
#define ADR_ID_LEN_THREADSHOLD1  (ID_MNG_REG_BASE + 0x00000038)
#define ADR_MMU_CTRL             (MMU_REG_BASE + 0x00000000)

#define MAC_SW_RST_SFT           1
#define TX_PBOFFSET_SFT          0
#define TX_INFO_SIZE_SFT         8
#define RX_INFO_SIZE_SFT         16
#define RX_LAST_PHY_SIZE_SFT     24
#define MRX_STP_OFST_SFT         8
#define MTX_TSF_TIMER_EN_SFT     5
#define MTX_HALT_MNG_UNTIL_DTIM_MSK 0x00000040u
#define TX_ID_THOLD_SFT          0
#define RX_ID_THOLD_SFT          8
#define ID_TX_LEN_THOLD_SFT      4
#define ID_RX_LEN_THOLD_SFT      13
#define MMU_SHARE_MCU_SFT        16

#define TXPB_OFFSET              80
/* Delimiter preceding each frame in an A-MPDU aggregate. */
#define AMPDU_DELIMITER_LEN      4
/* Descriptor sizes: tx desc 80 bytes,
 * rx desc + phy info 36 bytes, rx pinfo pad 4 bytes.
 */
#define SSV6051_TX_DESC_BYTE_LEN 80
#define SSV6051_RX_DESC_BYTE_LEN 36
#define SSV6051_RX_PINFO_PAD     4

/* ECO (SSV6051) thresholds. */
#define SSV6200_ID_TX_THRESHOLD  19
#define SSV6200_ID_RX_THRESHOLD  60
#define SSV6200_PAGE_TX_THRESHOLD 115
#define SSV6200_PAGE_RX_THRESHOLD 115

/* RF/PHY/clock init registers. */
#define ADR_PHY_EN_0             (0xce000000u)
#define ADR_DPLL_CP_PFD_REGISTER (0xce010000u + 0x0000005c)

/* RF power-down: forcing bits 12-13 of the hard-wire pin register to 01
 * turns the SX/RF off at teardown. */
#define ADR_RF_HARD_WIRE_PIN     (0xce010000u + 0x00000000)

/* RF synthesizer registers touched by the IQK table adjustments. */
#define ADR_SX_ENABLE_REGISTER   (0xce010000u + 0x00000038)
#define ADR_DPLL_DIVIDER_REGISTER (0xce010000u + 0x00000060)
#define ADR_SYN_KVCO_XO_FINE_TUNE_CBANK (0xce010000u + 0x00000050)
#define ADR_DPLL_FB_DIVIDER_REGISTERS_I  (0xce010000u + 0x0000009c)
#define ADR_DPLL_FB_DIVIDER_REGISTERS_II (0xce010000u + 0x000000a0)
#define RG_XOSC_CBANK_XO_I_MSK   0xfff87fffu
#define RG_XOSC_CBANK_XO_SFT     15

/* RX flow routing (the RX engines are redirected to the trash can
 * before sending the MCU to sleep). */
#define ADR_RX_FLOW_DATA         (0xc6000000u + 0x000000e0)
#define ADR_RX_FLOW_MNG          (0xc6000000u + 0x000000e4)
#define ADR_RX_FLOW_CTRL         (0xc6000000u + 0x000000e8)
#define ADR_TRX_DUMMY_REGISTER   (0xce010000u + 0x0000008c)
#define ADR_PAD53                (0xc0000300u + 0x00000088)
#define ADR_PAD54                (0xc0000300u + 0x0000008c)
#define ADR_PIN_SEL_0            (0xc0000300u + 0x000000cc)
#define ADR_CLOCK_SELECTION      (0xc0000000u + 0x00000018)

#define RG_RF_BB_CLK_SEL_SFT     31
#define RG_DP_BBPLL_PD_SFT       9
#define RG_DP_BBPLL_SDM_EDGE_SFT 31
#define RG_PHYRX_MD_EN_MSK       0x00000002u
#define RG_PHYTX_MD_EN_MSK       0x00000004u
#define RG_PHY11GN_MD_EN_MSK     0x00000008u
#define RG_PHY11B_MD_EN_MSK      0x00000010u
#define RG_PHYRXFIFO_MD_EN_MSK   0x00000020u
#define RG_PHYTXFIFO_MD_EN_MSK   0x00000040u
#define RG_PHY11BGN_MD_EN_MSK    0x00000100u
#define SSV6051_PHY_EN_ALL       (RG_PHYRX_MD_EN_MSK | RG_PHYTX_MD_EN_MSK | \
				  RG_PHY11GN_MD_EN_MSK | RG_PHY11B_MD_EN_MSK | \
				  RG_PHYRXFIFO_MD_EN_MSK | RG_PHYTXFIFO_MD_EN_MSK | \
				  RG_PHY11BGN_MD_EN_MSK)

/* RF power enable/disable: bits 12-13 of the hard-wire pin register, 10 = on, 01 = off. */
#define RF_HARD_WIRE_RF_MSK   (0x03u << 12)
#define RF_HARD_WIRE_RF_ON    (0x02u << 12)
#define RF_HARD_WIRE_RF_OFF   (0x01u << 12)

/* PHY enable, set after the firmware version check. */
#define ADR_PHY_EN_1      (0xce000000u + 0x00000004)
#define RG_PHY_MD_EN_SFT  0
#define RG_PHY_MD_EN_MSK  0x00000001u

/* Mailbox / MCU */
#define ADR_MCU_STATUS   (MB_REG_BASE + 0x00000018)
#define ADR_CH0_TRIG_1   (MB_REG_BASE + 0x00000010)
#define ADR_WR_ALC       (ID_MNG_REG_BASE + 0x00000000)
#define ADR_CH_STA_PRI   (ID_MNG_REG_BASE + 0x00000004)
#define CH0_FULL_MSK     0x00000001u

/* RF synthesizer / channel registers (CSR_RF_BASE 0xce010000, CSR_PHY_BASE
 * 0xce000000), used by channel switching and the pre-fw table writes. */
#define ADR_TX_GAIN_FACTOR          (0xce000000u + 0x000071bc)
#define ADR_MANUAL_ENABLE_REGISTER  (0xce010000u + 0x00000004)
#define ADR_ABB_REGISTER_1          (0xce010000u + 0x0000000c)
#define ADR_RX_TX_FSM_REGISTER      (0xce010000u + 0x0000002c)
#define ADR_RX_ADC_REGISTER         (0xce010000u + 0x00000030)
#define ADR_SYN_REGISTER_1          (0xce010000u + 0x0000003c)
#define ADR_SYN_REGISTER_2          (0xce010000u + 0x00000040)
#define ADR_SYN_DIV_SDM_XOSC        (0xce010000u + 0x0000004c)
#define ADR_SX_LCK_BIN_REGISTERS_I  (0xce010000u + 0x00000088)
#define ADR_READ_ONLY_FLAGS_1       (0xce010000u + 0x00000094)
#define ADR_READ_ONLY_FLAGS_2       (0xce010000u + 0x00000098)
#define ADR_SX_LCK_BIN_REGISTERS_II (0xce010000u + 0x000000a4)
#define ADR_PMU_2                   (0xc0001d00u + 0x00000008)
#define RG_TX_GAIN_OFFSET_I_MSK     0xf87fffffu
#define RG_TX_GAIN_OFFSET_SFT       23

/* Global MAC settings, all programmed before the firmware download. */
#define ADR_GLBLE_SET          (MAC_GLB_SET_BASE + 0x0000001c)
#define ADR_REASON_TRAP0       (MAC_GLB_SET_BASE + 0x00000020)
#define ADR_REASON_TRAP1       (MAC_GLB_SET_BASE + 0x00000024)
#define ADR_BSSID_0            (MAC_GLB_SET_BASE + 0x00000028)
#define ADR_BSSID_1            (MAC_GLB_SET_BASE + 0x0000002c)
#define ADR_STA_MAC_0          (MAC_GLB_SET_BASE + 0x00000030)
#define ADR_STA_MAC_1          (MAC_GLB_SET_BASE + 0x00000034)
#define OP_MODE_MSK            0x00000003u
#define OP_MODE_SFT            0
#define PB_OFFSET_SFT          8
#define SNIFFER_MODE_SFT       16
#define DUP_FLT_SFT            17
#define TX_PKT_RSVD_SFT        18
#define SSV6200_TX_PKT_RSVD_SETTING 0x3
#define RXPB_OFFSET            80
#define SSV6200_OPMODE_STA     0

/* RX frame-type decision table: MAC_DECITBL1_SIZE entries in MRX_FLT_TB0, then
 * MAC_DECITBL2_SIZE entries in MRX_FLT_EN0. */
#define ADR_MRX_FLT_TB0        (0xc6000000u + 0x00000070)
#define ADR_MRX_FLT_EN0        (0xc6000000u + 0x000000b0)
#define ADR_TRAP_HW_ID         (0xc6000000u + 0x00000134)
#define MAC_DECITBL1_SIZE      16
#define MAC_DECITBL2_SIZE      9

/* WSID watch list (HIF info block). */
#define ADR_WSID0              (0xca000000u + 0x00000000)
#define ADR_WSID1              (0xca000000u + 0x00000050)
/* Per-WSID RX sequence counters, TID0..TID7 (cleared when a WSID is
 * programmed). */
#define ADR_WSID0_TID0_RX_SEQ  (0xc6000000u + 0x00000140)
#define ADR_WSID0_TID7_RX_SEQ  (0xc6000000u + 0x0000015c)

/* Ether-type filters (HCI block). */
#define ADR_TX_ETHER_TYPE_0    (0xc1000000u + 0x00000050)
#define ADR_TX_ETHER_TYPE_1    (0xc1000000u + 0x00000054)
#define ADR_RX_ETHER_TYPE_0    (0xc1000000u + 0x00000060)
#define ADR_RX_ETHER_TYPE_1    (0xc1000000u + 0x00000064)

/* Security key table programming (packet ID masks bits 6-12,
 * replay-ignore is bit 16). */
#define ADR_SCRT_SET        (MAC_GLB_SET_BASE + 0x00000038)
#define SCRT_PKT_ID_I_MSK   0xffffe03fu
#define SCRT_PKT_ID_SFT     6
#define PAIR_SCRT_I_MSK     0xfffffff8u
#define PAIR_SCRT_SFT       0
#define GRP_SCRT_I_MSK      0xffffffc7u
#define GRP_SCRT_SFT        3
#define SCRT_RPLY_IGNORE_I_MSK 0xfffeffffu
#define SCRT_RPLY_IGNORE_SFT   16

/* PHY rate info table: TBL1 lives in the fixed INFO0 register window,
 * TBL2/TBL3 in a chip-SRAM pbuf pointed to by INFO_IDX_ADDR/
 * INFO_LEN_ADDR. */
#define ADR_INFO0   (PHY_RATE_INFO_BASE + 0x00000000)
#define ADR_INFO1   (PHY_RATE_INFO_BASE + 0x00000004)
#define ADR_INFO2   (PHY_RATE_INFO_BASE + 0x00000008)
#define ADR_INFO_RATE_OFFSET (PHY_RATE_INFO_BASE + 0x000000a0)
#define ADR_INFO_IDX_ADDR    (PHY_RATE_INFO_BASE + 0x000000a4)
#define ADR_INFO_LEN_ADDR    (PHY_RATE_INFO_BASE + 0x000000a8)

/* Engine IDs */
#define M_ENG_CPU          0x00
#define M_ENG_HWHCI        0x01
#define M_ENG_EMPTY        0x02
#define M_ENG_ENCRYPT      0x03
#define M_ENG_MACRX        0x04
#define M_ENG_MIC          0x05
#define M_ENG_TX_EDCA0     0x06
#define M_ENG_TX_EDCA1     0x07
#define M_ENG_TX_EDCA2     0x08
#define M_ENG_TX_EDCA3     0x09
#define M_ENG_TX_MNG       0x0A
#define M_ENG_ENCRYPT_SEC  0x0B
#define M_ENG_MIC_SEC      0x0C
#define M_ENG_TRASH_CAN    0x0F

/* Frame content types (HCI c_type field) */
#define M0_TXREQ     0
#define M1_TXREQ     1
#define M2_TXREQ     2
#define M0_RXEVENT   3
#define M2_RXEVENT   4
#define HOST_CMD     5
#define HOST_EVENT   6
#define TEST_CMD     7

/* Chip identities */
#define SSV6051Q_P1  0x00000000
#define SSV6051Q_P2  0x70000000
#define SSV6051Z     0x71000000
#define SSV6051Q     0x73000000
#define SSV6051P     0x75000000

/* SDIO function-1 control registers */
#define REG_INT_MASK           0x04
#define REG_INT_STATUS         0x08

/* Maximum RX frame size */
#define MAX_RX_FRAME_SIZE      0x900

/* Chip ID registers */
#define ADR_CHIP_ID_0   (SYS_REG_BASE + 0x00000008)
#define ADR_CHIP_ID_1   (SYS_REG_BASE + 0x0000000c)
#define ADR_CHIP_ID_2   (SYS_REG_BASE + 0x00000010)
#define ADR_CHIP_ID_3   (SYS_REG_BASE + 0x00000014)

#endif /* _SSV6051_REGS_H_ */

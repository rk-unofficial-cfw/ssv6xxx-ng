// SPDX-License-Identifier: GPL-2.0
/*
 * Based on the ssv6xxx driver:
 *   Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 *   Copyright (c) 2015 iComm Corporation
 */

#include <linux/debugfs.h>
#include <linux/seq_file.h>

#include "ssv6051.h"

static int ssv6051_debugfs_stats_show(struct seq_file *m, void *v)
{
	struct ssv6051_dev *sdev = m->private;
	struct ssv6051_stats *st = &sdev->stats;

	seq_printf(m, "interrupts:      %u\n", st->isr_count);
	seq_printf(m, "  rx:            %u\n", st->isr_rx_count);
	seq_printf(m, "  tx:            %u\n", st->isr_tx_count);
	seq_printf(m, "  missed:        %u\n", st->isr_miss_count);
	seq_printf(m, "tx packets:      %u\n", st->tx_pkts);
	seq_printf(m, "tx dropped:      %u\n", st->tx_dropped);
	seq_printf(m, "tx errors:       %u\n", st->tx_errors);
	seq_printf(m, "rx packets:      %u\n", st->rx_pkts);
	seq_printf(m, "rx dropped:      %u\n", st->rx_dropped);
	seq_printf(m, "rx errors:       %u\n", st->rx_errors);
	seq_printf(m, "fw checksum err: %u\n", st->fw_checksum_errors);
	seq_printf(m, "sdio io errors:  %u\n", st->sdio_io_errors);
	seq_printf(m, "cmd timeouts:    %u\n", st->cmd_timeouts);
	seq_printf(m, "associated:      %d\n", sdev->associated);
	seq_printf(m, "channel:         %u\n", sdev->cfg.channel);

	/* Card TX resource usage (ADR_TX_ID_ALL_INFO / _INFO2): stuck
	 * counters here (tx_use_id at the threshold, txq4_size at max)
	 * mean the firmware never completes frames — the root cause of
	 * "TXQ4 full" / auth timeouts. */
	{
		struct ssv6051_hci_txq_info info;
		struct ssv6051_hci_txq_info2 info2;

		if (ssv6051_sdio_read_reg(sdev, ADR_TX_ID_ALL_INFO,
					  (u32 *)&info) == 0) {
			seq_printf(m,
				   "tx pages used:   %u / %u\n",
				   info.tx_use_page,
				   SSV6200_PAGE_TX_THRESHOLD);
			seq_printf(m,
				   "tx ids used:     %u / %u\n",
				   info.tx_use_id,
				   SSV6200_ID_TX_THRESHOLD);
			seq_printf(m,
				   "txq0-3 size:      %u %u %u %u\n",
				   info.txq0_size, info.txq1_size,
				   info.txq2_size, info.txq3_size);
		}
		if (ssv6051_sdio_read_reg(sdev, ADR_TX_ID_ALL_INFO2,
					  (u32 *)&info2) == 0)
			seq_printf(m, "txq4 size:        %u / %u\n",
				   info2.txq4_size, SSV6051_ID_MNG_QUEUE);
	}

	seq_printf(m, "data I/O port:   0x%08x\n", sdev->data_io_port);
	seq_printf(m, "reg I/O port:    0x%08x\n", sdev->reg_io_port);
	seq_printf(m, "hw sec key:      0x%08x\n", sdev->hw_sec_key);
	seq_printf(m, "hw wsid:         %d\n", sdev->hw_wsid);
	return 0;
}

static int ssv6051_debugfs_regdump_show(struct seq_file *m, void *v)
{
	struct ssv6051_dev *sdev = m->private;
	u32 val;
	int ret, i;

	for (i = 0; i < 16; i++) {
		ret = ssv6051_sdio_read_reg(sdev, i * 4, &val);
		if (ret)
			break;
		seq_printf(m, "%08x: %08x\n", i * 4, val);
	}
	return ret;
}

static void *ssv6051_debugfs_stats_start(struct seq_file *m, loff_t *pos)
{
	return (*pos == 0) ? SEQ_START_TOKEN : NULL;
}

static void ssv6051_debugfs_stats_stop(struct seq_file *m, void *v)
{
}

static void *ssv6051_debugfs_stats_next(struct seq_file *m, void *v,
					loff_t *pos)
{
	(*pos)++;
	return NULL;
}

static const struct seq_operations ssv6051_debugfs_stats_sops = {
	.start = ssv6051_debugfs_stats_start,
	.next = ssv6051_debugfs_stats_next,
	.stop = ssv6051_debugfs_stats_stop,
	.show = ssv6051_debugfs_stats_show,
};

static const struct seq_operations ssv6051_debugfs_regdump_sops = {
	.start = ssv6051_debugfs_stats_start,
	.next = ssv6051_debugfs_stats_next,
	.stop = ssv6051_debugfs_stats_stop,
	.show = ssv6051_debugfs_regdump_show,
};

DEFINE_SEQ_ATTRIBUTE(ssv6051_debugfs_stats);
DEFINE_SEQ_ATTRIBUTE(ssv6051_debugfs_regdump);

int ssv6051_debugfs_init(struct ssv6051_dev *sdev)
{
	struct dentry *dir;

	dir = debugfs_create_dir(SSV6051_DRV_NAME, NULL);
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	sdev->debugfs_dir = dir;
	debugfs_create_file("stats", 0400, dir, sdev,
			    &ssv6051_debugfs_stats_fops);
	debugfs_create_file("regdump", 0400, dir, sdev,
			    &ssv6051_debugfs_regdump_fops);
	debugfs_create_u32("debug", 0600, dir, &ssv6051_debug);
	return 0;
}

void ssv6051_debugfs_deinit(struct ssv6051_dev *sdev)
{
	debugfs_remove_recursive(sdev->debugfs_dir);
	sdev->debugfs_dir = NULL;
}

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UFS_MEDIATEK_XIAOMI_H
#define _UFS_MEDIATEK_XIAOMI_H

#include <linux/completion.h>
#include <linux/types.h>

#include <ufs/ufs.h>
#include <ufs/ufshcd.h>

struct ufs_xiaomi {
	struct ufs_hba *hba;
	u32 reserved0;
	u32 pa_err_cnt_total;
	u32 pa_lane_err_cnt[4];
	u32 pa_line_reset_err_cnt;
	u32 dl_err_cnt_total;
	u32 dl_nac_received_err_cnt;
	u32 dl_tcx_replay_timer_expired_err_cnt;
	u32 dl_afcx_request_timer_expired_err_cnt;
	u32 dl_fcx_protection_timer_expired_err_cnt;
	u32 dl_crc_err_cnt;
	u32 dl_rx_buffer_overflow_err_cnt;
	u32 dl_max_frame_length_exceeded_err_cnt;
	u32 dl_wrong_sequence_number_err_cnt;
	u32 dl_afc_frame_syntax_err_cnt;
	u32 dl_nac_frame_syntax_err_cnt;
	u32 dl_eof_syntax_err_cnt;
	u32 dl_frame_syntax_err_cnt;
	u32 dl_bad_ctrl_symbol_type_err_cnt;
	u32 dl_pa_init_err_cnt;
	u32 dl_pa_error_ind_received;
	u32 dme_err_cnt;
	u64 err_state;
	char err_reason[10][32];
	u8 reserved1[0x2d8];
	struct completion ufs_xiaomi_comp;
};

struct ufs_xiaomi *get_ufs_xiaomi(void);

int ufshcd_query_descriptor_retry_xm(struct ufs_hba *hba,
				     enum query_opcode opcode,
				     enum desc_idn idn, u8 index,
				     u8 selector, u8 *desc_buf,
				     int *buf_len);
int ufshcd_read_desc_param_sel(struct ufs_hba *hba, enum desc_idn desc_id,
			       int desc_index, u8 selector,
			       u8 param_offset, u8 *param_read_buf,
			       u8 param_size);

void ufs_xiaomi_register_hba(struct ufs_hba *hba);
void ufs_xiaomi_lun_configured(struct ufs_hba *hba, u64 lun);
void ufshcd_update_err_state(u32 evt, u32 val);
void ufshcd_update_uic_error_cnt(u32 evt, u32 val);

#endif /* _UFS_MEDIATEK_XIAOMI_H */

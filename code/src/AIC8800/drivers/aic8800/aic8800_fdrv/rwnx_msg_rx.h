/**
 ****************************************************************************************
 *
 * @file rwnx_msg_rx.h
 *
 * @brief RX function declarations
 *
 * Copyright (C) RivieraWaves 2012-2019
 *
 ****************************************************************************************
 */

#ifndef _RWNX_MSG_RX_H_
#define _RWNX_MSG_RX_H_

void rwnx_conn_track_init(struct rwnx_vif *rwnx_vif);
void rwnx_conn_track_deinit(struct rwnx_vif *rwnx_vif);
int rwnx_conn_track_start(struct rwnx_vif *rwnx_vif,
                          enum rwnx_conn_txn_kind kind,
                          const u8 *target_bssid, const u8 *prev_bssid);
void rwnx_conn_track_abort(struct rwnx_vif *rwnx_vif, const char *reason);
enum rwnx_conn_txn_kind rwnx_conn_track_kind(struct rwnx_vif *rwnx_vif);
u32 rwnx_conn_track_id(struct rwnx_vif *rwnx_vif);
void rwnx_conn_cancel(struct rwnx_vif *rwnx_vif, u16 reason,
                      const char *source);

void rwnx_rx_handle_msg(struct rwnx_hw *rwnx_hw, struct ipc_e2a_msg *msg);
void rwnx_rx_handle_print(struct rwnx_hw *rwnx_hw, u8 *msg, u32 len);

#endif /* _RWNX_MSG_RX_H_ */

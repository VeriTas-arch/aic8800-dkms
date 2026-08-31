/**
 ****************************************************************************************
 *
 * @file rwnx_msg_rx.c
 *
 * @brief RX function definitions
 *
 * Copyright (C) RivieraWaves 2012-2019
 *
 ****************************************************************************************
 */

#include <linux/vmalloc.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>
#include "rwnx_defs.h"
#include "rwnx_msg_rx.h"
#include "rwnx_prof.h"
#include "rwnx_tx.h"
#ifdef CONFIG_RWNX_BFMER
#include "rwnx_bfmer.h"
#endif //(CONFIG_RWNX_BFMER)
#ifdef CONFIG_RWNX_FULLMAC
#include "rwnx_debugfs.h"
#include "rwnx_msg_tx.h"
#include "rwnx_tdls.h"
#endif /* CONFIG_RWNX_FULLMAC */
#include "rwnx_events.h"
#include "rwnx_compat.h"
#include "aicwf_txrxif.h"
#ifdef AICWF_USB_SUPPORT
#include "aicwf_usb.h"
#endif
#ifdef CONFIG_USE_WIRELESS_EXT
#include "aicwf_wext_linux.h"
#endif

static int rwnx_freq_to_idx(struct rwnx_hw *rwnx_hw, int freq)
{
    struct ieee80211_supported_band *sband;
    int band, ch, idx = 0;

    for (band = NL80211_BAND_2GHZ; band < NUM_NL80211_BANDS; band++) {
#ifdef CONFIG_RWNX_FULLMAC
        sband = rwnx_hw->wiphy->bands[band];
#endif /* CONFIG_RWNX_FULLMAC */
        if (!sband) {
            continue;
        }

        for (ch = 0; ch < sband->n_channels; ch++, idx++) {
            if (sband->channels[ch].center_freq == freq) {
                goto exit;
            }
        }
    }

    return -ENOENT;

exit:
    // Channel has been found, return the index
    return idx;
}

/***************************************************************************
 * Messages from MM task
 **************************************************************************/
static inline int rwnx_rx_chan_pre_switch_ind(struct rwnx_hw *rwnx_hw,
                                              struct rwnx_cmd *cmd,
                                              struct ipc_e2a_msg *msg)
{
    struct rwnx_vif *rwnx_vif;
    int chan_idx = ((struct mm_channel_pre_switch_ind *)msg->param)->chan_index;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    REG_SW_SET_PROFILING_CHAN(rwnx_hw, SW_PROF_CHAN_CTXT_PSWTCH_BIT);

#ifdef CONFIG_RWNX_FULLMAC
    list_for_each_entry(rwnx_vif, &rwnx_hw->vifs, list) {
        if (rwnx_vif->up && rwnx_vif->ch_index == chan_idx) {
			AICWFDBG(LOGDEBUG, "rwnx_txq_vif_stop\r\n");
            rwnx_txq_vif_stop(rwnx_vif, RWNX_TXQ_STOP_CHAN, rwnx_hw);
        }
    }
#endif /* CONFIG_RWNX_FULLMAC */

    REG_SW_CLEAR_PROFILING_CHAN(rwnx_hw, SW_PROF_CHAN_CTXT_PSWTCH_BIT);

    return 0;
}

static inline int rwnx_rx_chan_switch_ind(struct rwnx_hw *rwnx_hw,
                                          struct rwnx_cmd *cmd,
                                          struct ipc_e2a_msg *msg)
{
    struct rwnx_vif *rwnx_vif;
    int chan_idx = ((struct mm_channel_switch_ind *)msg->param)->chan_index;
    bool roc     = ((struct mm_channel_switch_ind *)msg->param)->roc;
    bool roc_tdls = ((struct mm_channel_switch_ind *)msg->param)->roc_tdls;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    REG_SW_SET_PROFILING_CHAN(rwnx_hw, SW_PROF_CHAN_CTXT_SWTCH_BIT);

#ifdef CONFIG_RWNX_FULLMAC
    if (roc_tdls) {
        u8 vif_index = ((struct mm_channel_switch_ind *)msg->param)->vif_index;
        list_for_each_entry(rwnx_vif, &rwnx_hw->vifs, list) {
            if (rwnx_vif->vif_index == vif_index) {
                rwnx_vif->roc_tdls = true;
                rwnx_txq_tdls_sta_start(rwnx_vif, RWNX_TXQ_STOP_CHAN, rwnx_hw);
            }
        }
    } else if (!roc) {
        list_for_each_entry(rwnx_vif, &rwnx_hw->vifs, list) {
            if (rwnx_vif->up && rwnx_vif->ch_index == chan_idx) {
                rwnx_txq_vif_start(rwnx_vif, RWNX_TXQ_STOP_CHAN, rwnx_hw);
            }
        }
    } else {
        /* Retrieve the allocated RoC element */
        struct rwnx_roc_elem *roc_elem = rwnx_hw->roc_elem;

        /* If mgmt_roc is true, remain on channel has been started by ourself */
        if (!roc_elem->mgmt_roc) {
            /* Inform the host that we have switch on the indicated off-channel */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 6, 0)
	    cfg80211_ready_on_channel(roc_elem->wdev->netdev, (u64)(rwnx_hw->roc_cookie_cnt),
                                      roc_elem->chan, NL80211_CHAN_HT20, roc_elem->duration, GFP_ATOMIC);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
	    cfg80211_ready_on_channel(roc_elem->wdev, (u64)(rwnx_hw->roc_cookie_cnt),
                                      roc_elem->chan, NL80211_CHAN_HT20, roc_elem->duration, GFP_ATOMIC);
#else
            cfg80211_ready_on_channel(roc_elem->wdev, (u64)(rwnx_hw->roc_cookie_cnt),
                                      roc_elem->chan, roc_elem->duration, GFP_ATOMIC);
#endif
        }

        /* Keep in mind that we have switched on the channel */
        roc_elem->on_chan = true;

        // Enable traffic on OFF channel queue
        rwnx_txq_offchan_start(rwnx_hw);
    }

    tasklet_schedule(&rwnx_hw->task);

    rwnx_hw->cur_chanctx = chan_idx;
    rwnx_radar_detection_enable_on_cur_channel(rwnx_hw);

#endif /* CONFIG_RWNX_FULLMAC */

    REG_SW_CLEAR_PROFILING_CHAN(rwnx_hw, SW_PROF_CHAN_CTXT_SWTCH_BIT);

    return 0;
}

static inline int rwnx_rx_tdls_chan_switch_cfm(struct rwnx_hw *rwnx_hw,
                                                struct rwnx_cmd *cmd,
                                                struct ipc_e2a_msg *msg)
{
    return 0;
}

static inline int rwnx_rx_tdls_chan_switch_ind(struct rwnx_hw *rwnx_hw,
                                               struct rwnx_cmd *cmd,
                                               struct ipc_e2a_msg *msg)
{
#ifdef CONFIG_RWNX_FULLMAC
    // Enable traffic on OFF channel queue
    rwnx_txq_offchan_start(rwnx_hw);

    return 0;
#endif
}

static inline int rwnx_rx_tdls_chan_switch_base_ind(struct rwnx_hw *rwnx_hw,
                                                    struct rwnx_cmd *cmd,
                                                    struct ipc_e2a_msg *msg)
{
    struct rwnx_vif *rwnx_vif;
    u8 vif_index = ((struct tdls_chan_switch_base_ind *)msg->param)->vif_index;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

#ifdef CONFIG_RWNX_FULLMAC
    list_for_each_entry(rwnx_vif, &rwnx_hw->vifs, list) {
        if (rwnx_vif->vif_index == vif_index) {
            rwnx_vif->roc_tdls = false;
            rwnx_txq_tdls_sta_stop(rwnx_vif, RWNX_TXQ_STOP_CHAN, rwnx_hw);
        }
    }
    return 0;
#endif
}

static inline int rwnx_rx_tdls_peer_ps_ind(struct rwnx_hw *rwnx_hw,
                                           struct rwnx_cmd *cmd,
                                           struct ipc_e2a_msg *msg)
{
    struct rwnx_vif *rwnx_vif;
    u8 vif_index = ((struct tdls_peer_ps_ind *)msg->param)->vif_index;
    bool ps_on = ((struct tdls_peer_ps_ind *)msg->param)->ps_on;

#ifdef CONFIG_RWNX_FULLMAC
    list_for_each_entry(rwnx_vif, &rwnx_hw->vifs, list) {
        if (rwnx_vif->vif_index == vif_index && rwnx_vif->sta.tdls_sta) {
            rwnx_vif->sta.tdls_sta->tdls.ps_on = ps_on;
            // Update PS status for the TDLS station
            rwnx_ps_bh_enable(rwnx_hw, rwnx_vif->sta.tdls_sta, ps_on);
        }
    }

    return 0;
#endif
}

static inline int rwnx_rx_remain_on_channel_exp_ind(struct rwnx_hw *rwnx_hw,
                                                    struct rwnx_cmd *cmd,
                                                    struct ipc_e2a_msg *msg)
{
#ifdef CONFIG_RWNX_FULLMAC
    /* Retrieve the allocated RoC element */
    struct rwnx_roc_elem *roc_elem = rwnx_hw->roc_elem;
    /* Get VIF on which RoC has been started */
    struct rwnx_vif *rwnx_vif = NULL;// = container_of(roc_elem->wdev, struct rwnx_vif, wdev);

    RWNX_DBG(RWNX_FN_ENTRY_STR);
    if(roc_elem == NULL){
        AICWFDBG(LOGERROR,"%s roc_elem == NULL \r\n", __func__);
        return 0;
    }

    rwnx_vif = container_of(roc_elem->wdev, struct rwnx_vif, wdev);


#ifdef CREATE_TRACE_POINTS
    /* For debug purpose (use ftrace kernel option) */
    trace_roc_exp(rwnx_vif->vif_index);
#endif
    /* If mgmt_roc is true, remain on channel has been started by ourself */
    /* If RoC has been cancelled before we switched on channel, do not call cfg80211 */
    if (!roc_elem->mgmt_roc && roc_elem->on_chan) {
        /* Inform the host that off-channel period has expired */

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 6, 0)
	cfg80211_remain_on_channel_expired(roc_elem->wdev->netdev, (u64)(rwnx_hw->roc_cookie_cnt),
                                           roc_elem->chan, NL80211_CHAN_HT20, GFP_ATOMIC);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
	cfg80211_remain_on_channel_expired(roc_elem->wdev, (u64)(rwnx_hw->roc_cookie_cnt),
                                           roc_elem->chan, NL80211_CHAN_HT20, GFP_ATOMIC);
#else
        cfg80211_remain_on_channel_expired(roc_elem->wdev, (u64)(rwnx_hw->roc_cookie_cnt),
                                           roc_elem->chan, GFP_ATOMIC);
#endif
    }

    /* De-init offchannel TX queue */
    rwnx_txq_offchan_deinit(rwnx_vif);

    /* Increase the cookie counter cannot be zero */
    rwnx_hw->roc_cookie_cnt++;

    if (rwnx_hw->roc_cookie_cnt == 0) {
        rwnx_hw->roc_cookie_cnt = 1;
    }

    /* Free the allocated RoC element */
    kfree(roc_elem);
    rwnx_hw->roc_elem = NULL;

	AICWFDBG(LOGTRACE, "%s exit\r\n", __func__);
#endif /* CONFIG_RWNX_FULLMAC */
    return 0;
}

static inline int rwnx_rx_p2p_vif_ps_change_ind(struct rwnx_hw *rwnx_hw,
                                                struct rwnx_cmd *cmd,
                                                struct ipc_e2a_msg *msg)
{
    int vif_idx  = ((struct mm_p2p_vif_ps_change_ind *)msg->param)->vif_index;
    int ps_state = ((struct mm_p2p_vif_ps_change_ind *)msg->param)->ps_state;
    struct rwnx_vif *vif_entry;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

#ifdef CONFIG_RWNX_FULLMAC
    if (vif_idx < 0 || vif_idx >= ARRAY_SIZE(rwnx_hw->vif_table))
        goto exit;
    vif_entry = rwnx_hw->vif_table[vif_idx];

    if (vif_entry && vif_entry->ndev) {
        goto found_vif;
    }
#endif /* CONFIG_RWNX_FULLMAC */

    goto exit;

found_vif:

#ifdef CONFIG_RWNX_FULLMAC
    if (ps_state == MM_PS_MODE_OFF) {
        // Start TX queues for provided VIF
        rwnx_txq_vif_start(vif_entry, RWNX_TXQ_STOP_VIF_PS, rwnx_hw);
    }
    else {
        // Stop TX queues for provided VIF
        rwnx_txq_vif_stop(vif_entry, RWNX_TXQ_STOP_VIF_PS, rwnx_hw);
    }
#endif /* CONFIG_RWNX_FULLMAC */

exit:
    return 0;
}

static inline int rwnx_rx_channel_survey_ind(struct rwnx_hw *rwnx_hw,
                                             struct rwnx_cmd *cmd,
                                             struct ipc_e2a_msg *msg)
{
    struct mm_channel_survey_ind *ind = (struct mm_channel_survey_ind *)msg->param;
    // Get the channel index
    int idx = rwnx_freq_to_idx(rwnx_hw, ind->freq);
    // Get the survey
    struct rwnx_survey_info *rwnx_survey;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    if (idx < 0 || idx >= ARRAY_SIZE(rwnx_hw->survey)) {
        AICWFDBG_RATELIMITED(LOGERROR,
                             "Ignored survey for unknown frequency:%u\n",
                             ind->freq);
        return 0;
    }

    rwnx_survey = &rwnx_hw->survey[idx];

    // Store the received parameters
    rwnx_survey->chan_time_ms = ind->chan_time_ms;
    rwnx_survey->chan_time_busy_ms = ind->chan_time_busy_ms;
    rwnx_survey->noise_dbm = ind->noise_dbm;
    rwnx_survey->filled = (SURVEY_INFO_TIME |
                           SURVEY_INFO_TIME_BUSY);

    if (ind->noise_dbm != 0) {
        rwnx_survey->filled |= SURVEY_INFO_NOISE_DBM;
    }

    return 0;
}

static inline int rwnx_rx_p2p_noa_upd_ind(struct rwnx_hw *rwnx_hw,
                                          struct rwnx_cmd *cmd,
                                          struct ipc_e2a_msg *msg)
{
    return 0;
}

static inline int rwnx_rx_rssi_status_ind(struct rwnx_hw *rwnx_hw,
                                          struct rwnx_cmd *cmd,
                                          struct ipc_e2a_msg *msg)
{
    struct mm_rssi_status_ind *ind = (struct mm_rssi_status_ind *)msg->param;
    int vif_idx  = ind->vif_index;
    unsigned int rssi_status_raw = 0;
    bool low_event;
    struct rwnx_vif *vif_entry;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    memcpy(&rssi_status_raw, &ind->rssi_status, sizeof(ind->rssi_status));
    low_event = rssi_status_raw != 0;
    if (rssi_status_raw > 1)
        atomic_inc(&rwnx_hw->runtime_stats.cqm_rssi_noncanonical_status);

#ifdef CONFIG_RWNX_FULLMAC
    if (vif_idx < 0 || vif_idx >= ARRAY_SIZE(rwnx_hw->vif_table)) {
        atomic_inc(&rwnx_hw->runtime_stats.cqm_rssi_invalid_vif);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "rssi_cqm invalid vif:%d event:%s rssi:%d raw_status:%u\n",
                             vif_idx, low_event ? "low" : "high",
                             ind->rssi, rssi_status_raw);
        return 0;
    }
    vif_entry = rwnx_hw->vif_table[vif_idx];
    if (vif_entry && vif_entry->ndev) {
        atomic_inc(low_event ? &rwnx_hw->runtime_stats.cqm_rssi_low_events :
                               &rwnx_hw->runtime_stats.cqm_rssi_high_events);
        AICWFDBG(LOGINFO,
                 "rssi_cqm vif:%d event:%s rssi:%d raw_status:%u\r\n",
                 vif_idx, low_event ? "low" : "high", ind->rssi,
                 rssi_status_raw);
        cfg80211_cqm_rssi_notify(vif_entry->ndev,
                                 low_event ? NL80211_CQM_RSSI_THRESHOLD_EVENT_LOW :
                                             NL80211_CQM_RSSI_THRESHOLD_EVENT_HIGH,
                                 ind->rssi, GFP_ATOMIC);
    } else {
        atomic_inc(&rwnx_hw->runtime_stats.cqm_rssi_invalid_vif);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "rssi_cqm missing vif:%d event:%s rssi:%d raw_status:%u\n",
                             vif_idx, low_event ? "low" : "high",
                             ind->rssi, rssi_status_raw);
    }
#endif /* CONFIG_RWNX_FULLMAC */

    return 0;
}

static inline int rwnx_rx_pktloss_notify_ind(struct rwnx_hw *rwnx_hw,
                                             struct rwnx_cmd *cmd,
                                             struct ipc_e2a_msg *msg)
{
#ifdef CONFIG_RWNX_FULLMAC
    struct mm_pktloss_ind *ind = (struct mm_pktloss_ind *)msg->param;
    struct rwnx_vif *vif_entry;
    int vif_idx  = ind->vif_index;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    if (vif_idx < 0 || vif_idx >= ARRAY_SIZE(rwnx_hw->vif_table))
        return 0;
    vif_entry = rwnx_hw->vif_table[vif_idx];
    if (vif_entry) {
        cfg80211_cqm_pktloss_notify(vif_entry->ndev, (const u8 *)ind->mac_addr.array,
                                    ind->num_packets, GFP_ATOMIC);
    }
#endif /* CONFIG_RWNX_FULLMAC */

    return 0;
}

static inline int rwnx_apm_staloss_ind(struct rwnx_hw *rwnx_hw,
                                                struct rwnx_cmd *cmd,
                                                struct ipc_e2a_msg *msg)
{
    struct mm_apm_staloss_ind *ind = (struct mm_apm_staloss_ind *)msg->param;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    memcpy(rwnx_hw->sta_mac_addr, ind->mac_addr, 6);
    rwnx_hw->apm_vif_idx = ind->vif_idx;

    queue_work(rwnx_hw->apmStaloss_wq, &rwnx_hw->apmStalossWork);

    return 0;
}


static inline int rwnx_rx_csa_counter_ind(struct rwnx_hw *rwnx_hw,
                                          struct rwnx_cmd *cmd,
                                          struct ipc_e2a_msg *msg)
{
    struct mm_csa_counter_ind *ind = (struct mm_csa_counter_ind *)msg->param;
    struct rwnx_vif *vif;
    bool found = false;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    // Look for VIF entry
    list_for_each_entry(vif, &rwnx_hw->vifs, list) {
        if (vif->vif_index == ind->vif_index) {
            found=true;
            break;
        }
    }

    if (found) {
#ifdef CONFIG_RWNX_FULLMAC
        if (vif->ap.csa)
            vif->ap.csa->count = ind->csa_count;
        else
            netdev_err(vif->ndev, "CSA counter update but no active CSA");

#endif
    }

    return 0;
}

#ifdef CONFIG_RWNX_FULLMAC
static inline int rwnx_rx_csa_finish_ind(struct rwnx_hw *rwnx_hw,
                                         struct rwnx_cmd *cmd,
                                         struct ipc_e2a_msg *msg)
{
    struct mm_csa_finish_ind *ind = (struct mm_csa_finish_ind *)msg->param;
    struct rwnx_vif *vif;
    bool found = false;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    // Look for VIF entry
    list_for_each_entry(vif, &rwnx_hw->vifs, list) {
        if (vif->vif_index == ind->vif_index) {
            found=true;
            break;
        }
    }

    if (found) {
        if (RWNX_VIF_TYPE(vif) == NL80211_IFTYPE_AP ||
            RWNX_VIF_TYPE(vif) == NL80211_IFTYPE_P2P_GO) {
            if (vif->ap.csa) {
                vif->ap.csa->status = ind->status;
                vif->ap.csa->ch_idx = ind->chan_idx;
                schedule_work(&vif->ap.csa->work);
            } else
                netdev_err(vif->ndev, "CSA finish indication but no active CSA");
        } else {
            if (ind->status == 0) {
                rwnx_chanctx_unlink(vif);
                rwnx_chanctx_link(vif, ind->chan_idx, NULL);
                if (rwnx_hw->cur_chanctx == ind->chan_idx) {
                    rwnx_radar_detection_enable_on_cur_channel(rwnx_hw);
                    rwnx_txq_vif_start(vif, RWNX_TXQ_STOP_CHAN, rwnx_hw);
                } else
                    rwnx_txq_vif_stop(vif, RWNX_TXQ_STOP_CHAN, rwnx_hw);
            }
        }
    }

    return 0;
}

static inline int rwnx_rx_csa_traffic_ind(struct rwnx_hw *rwnx_hw,
                                          struct rwnx_cmd *cmd,
                                          struct ipc_e2a_msg *msg)
{
    struct mm_csa_traffic_ind *ind = (struct mm_csa_traffic_ind *)msg->param;
    struct rwnx_vif *vif;
    bool found = false;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    // Look for VIF entry
    list_for_each_entry(vif, &rwnx_hw->vifs, list) {
        if (vif->vif_index == ind->vif_index) {
            found=true;
            break;
        }
    }

    if (found) {
        if (ind->enable)
            rwnx_txq_vif_start(vif, RWNX_TXQ_STOP_CSA, rwnx_hw);
        else
            rwnx_txq_vif_stop(vif, RWNX_TXQ_STOP_CSA, rwnx_hw);
    }

    return 0;
}

static inline int rwnx_rx_ps_change_ind(struct rwnx_hw *rwnx_hw,
                                        struct rwnx_cmd *cmd,
                                        struct ipc_e2a_msg *msg)
{
    struct mm_ps_change_ind *ind = (struct mm_ps_change_ind *)msg->param;
    struct rwnx_sta *sta;
    struct rwnx_vif *vif;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    if (ind->sta_idx >= ARRAY_SIZE(rwnx_hw->sta_table)) {
        wiphy_err(rwnx_hw->wiphy, "Invalid sta index reported by fw %d\n",
                  ind->sta_idx);
        return 1;
    }

    sta = &rwnx_hw->sta_table[ind->sta_idx];
    if (sta->vif_idx >= ARRAY_SIZE(rwnx_hw->vif_table) ||
        !(vif = rwnx_hw->vif_table[sta->vif_idx]) || !vif->ndev) {
        wiphy_err(rwnx_hw->wiphy, "Invalid vif index reported for sta %d\n",
                  ind->sta_idx);
        return 1;
    }

    netdev_dbg(vif->ndev,
               "Sta %d, change PS mode to %s", sta->sta_idx,
               ind->ps_state ? "ON" : "OFF");

    if (sta->valid) {
        rwnx_ps_bh_enable(rwnx_hw, sta, ind->ps_state);
    } else if (rwnx_hw->adding_sta) {
        sta->ps.active = ind->ps_state ? true : false;
    } else {
        netdev_err(vif->ndev,
                   "Ignore PS mode change on invalid sta\n");
    }


    return 0;
}


static inline int rwnx_rx_traffic_req_ind(struct rwnx_hw *rwnx_hw,
                                          struct rwnx_cmd *cmd,
                                          struct ipc_e2a_msg *msg)
{
    struct mm_traffic_req_ind *ind = (struct mm_traffic_req_ind *)msg->param;
    struct rwnx_sta *sta;
    struct rwnx_vif *vif;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    if (ind->sta_idx >= ARRAY_SIZE(rwnx_hw->sta_table))
        return 1;
    sta = &rwnx_hw->sta_table[ind->sta_idx];
    if (sta->vif_idx >= ARRAY_SIZE(rwnx_hw->vif_table) ||
        !(vif = rwnx_hw->vif_table[sta->vif_idx]) || !vif->ndev)
        return 1;

    netdev_dbg(vif->ndev,
               "Sta %d, asked for %d pkt", sta->sta_idx, ind->pkt_cnt);

    rwnx_ps_bh_traffic_req(rwnx_hw, sta, ind->pkt_cnt,
                           ind->uapsd ? UAPSD_ID : LEGACY_PS_ID);

    return 0;
}
#endif /* CONFIG_RWNX_FULLMAC */

/***************************************************************************
 * Messages from SCAN task
 **************************************************************************/
#if 0
static inline int rwnx_rx_scan_done_ind(struct rwnx_hw *rwnx_hw,
                                        struct rwnx_cmd *cmd,
                                        struct ipc_e2a_msg *msg)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 8, 0)
    struct cfg80211_scan_info info = {
        .aborted = false,
    };
#endif
    RWNX_DBG(RWNX_FN_ENTRY_STR);

    rwnx_ipc_elem_var_deallocs(rwnx_hw, &rwnx_hw->scan_ie);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 8, 0)
    ieee80211_scan_completed(rwnx_hw->hw, &info);
#else
    ieee80211_scan_completed(rwnx_hw->hw, false);
#endif

    return 0;
}
#endif

/***************************************************************************
 * Messages from SCANU task
 **************************************************************************/
#ifdef CONFIG_RWNX_FULLMAC
extern uint8_t scanning;
static inline int rwnx_rx_scanu_start_cfm(struct rwnx_hw *rwnx_hw,
                                          struct rwnx_cmd *cmd,
                                          struct ipc_e2a_msg *msg)
{

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 8, 0)
			struct cfg80211_scan_info info = {
				.aborted = false,
			};
#endif

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    if (rwnx_hw->scan_request
#ifdef CONFIG_USE_WIRELESS_EXT
		&& !rwnx_hw->wext_scan) {
#else
		){
#endif


#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 8, 0)
        cfg80211_scan_done(rwnx_hw->scan_request, &info);
#else
        cfg80211_scan_done(rwnx_hw->scan_request, false);
#endif
    }


#ifdef CONFIG_USE_WIRELESS_EXT
	else if(rwnx_hw->wext_scan){
    	rwnx_hw->wext_scan = 0;
		AICWFDBG(LOGDEBUG, "%s rwnx_hw->wext_scan done!!\r\n", __func__);
		if(rwnx_hw->scan_request){
			vfree(rwnx_hw->scan_request);
		}
		complete(&rwnx_hw->wext_scan_com);
	}
#endif
	else {
        AICWFDBG(LOGERROR, "%s rwnx_hw->scan_request is NULL!!\r\n", __func__);
    }

    rwnx_hw->scan_request = NULL;
    scanning = 0;

    return 0;
}

static inline int rwnx_rx_scanu_result_ind(struct rwnx_hw *rwnx_hw,
                                           struct rwnx_cmd *cmd,
                                           struct ipc_e2a_msg *msg)
{
    struct cfg80211_bss *bss = NULL;
    struct ieee80211_channel *chan;
    struct scanu_result_ind *ind = (struct scanu_result_ind *)msg->param;
    struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)ind->payload;
    const u8 *ssid_ie;
    const size_t fixed_len = offsetof(struct ieee80211_mgmt,
                                      u.beacon.variable);
	int freq = 0;

#ifdef CONFIG_USE_WIRELESS_EXT
	struct scanu_result_wext *scan_re_wext;
#endif


    RWNX_DBG(RWNX_FN_ENTRY_STR);

    if (ind->length < fixed_len) {
        atomic_inc(&rwnx_hw->runtime_stats.fw_msg_invalid);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "Dropped short scan result length:%u\n",
                             ind->length);
        return 0;
    }

    chan = ieee80211_get_channel(rwnx_hw->wiphy, ind->center_freq);

    if (chan != NULL) {
        #if LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0)
        //ktime_t ts;
        struct timespec ts;
		get_monotonic_boottime(&ts);
        //ts = ktime_get_real();
        mgmt->u.probe_resp.timestamp = ((u64)ts.tv_sec*1000000) + ts.tv_nsec/1000;
        #else
        struct timespec64 ts;
        ktime_get_real_ts64(&ts);
        mgmt->u.probe_resp.timestamp = ((u64)ts.tv_sec*1000000) + ts.tv_nsec/1000;
        #endif
        bss = cfg80211_inform_bss_frame(rwnx_hw->wiphy, chan,
                                        (struct ieee80211_mgmt *)ind->payload,
                                        ind->length, ind->rssi * 100, GFP_ATOMIC);

		ssid_ie = cfg80211_find_ie(WLAN_EID_SSID,
                                      mgmt->u.beacon.variable,
                                      ind->length - fixed_len);
		freq = ind->center_freq;
		AICWFDBG(LOGDEBUG, "%s %pM ssid:%.*s freq:%d timestamp:%lld, %d\r\n",
                 __func__, mgmt->bssid,
                 ssid_ie ? (int)ssid_ie[1] : 0,
                 ssid_ie ? (const char *)&ssid_ie[2] : "",
                 freq, (long long)mgmt->u.probe_resp.timestamp, ind->rssi);

#ifdef CONFIG_USE_WIRELESS_EXT
		if(rwnx_hw->wext_scan){

			scan_re_wext = (struct scanu_result_wext *)vmalloc(sizeof(struct scanu_result_wext));
			scan_re_wext->ind = (struct scanu_result_ind *)vmalloc(sizeof(struct scanu_result_ind));
			scan_re_wext->payload = (u32_l *)vmalloc(sizeof(u32_l) * ind->length);

			memset(scan_re_wext->ind, 0, sizeof(struct scanu_result_ind));
			memset(scan_re_wext->payload, 0, ind->length);

			memcpy(scan_re_wext->ind, ind, sizeof(struct scanu_result_ind));
			memcpy(scan_re_wext->payload, ind->payload, ind->length);

			scan_re_wext->bss = bss;

			INIT_LIST_HEAD(&scan_re_wext->scanu_re_list);
			list_add_tail(&scan_re_wext->scanu_re_list, &rwnx_hw->wext_scanre_list);
			return 0;
		}
#endif
    }

    if (bss != NULL)
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 9, 0)
	cfg80211_put_bss(bss);
#else
        cfg80211_put_bss(rwnx_hw->wiphy, bss);
#endif

    return 0;
}
#endif /* CONFIG_RWNX_FULLMAC */

/***************************************************************************
 * Messages from ME task
 **************************************************************************/
#ifdef CONFIG_RWNX_FULLMAC
static inline int rwnx_rx_me_tkip_mic_failure_ind(struct rwnx_hw *rwnx_hw,
                                                  struct rwnx_cmd *cmd,
                                                  struct ipc_e2a_msg *msg)
{
    struct me_tkip_mic_failure_ind *ind = (struct me_tkip_mic_failure_ind *)msg->param;
    struct rwnx_vif *rwnx_vif;
    struct net_device *dev;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    if (ind->vif_idx >= ARRAY_SIZE(rwnx_hw->vif_table) ||
        !(rwnx_vif = rwnx_hw->vif_table[ind->vif_idx]) ||
        !(dev = rwnx_vif->ndev))
        return 1;

    cfg80211_michael_mic_failure(dev, (u8 *)&ind->addr, (ind->ga?NL80211_KEYTYPE_GROUP:
                                 NL80211_KEYTYPE_PAIRWISE), ind->keyid,
                                 (u8 *)&ind->tsc, GFP_ATOMIC);

    return 0;
}

static inline int rwnx_rx_me_tx_credits_update_ind(struct rwnx_hw *rwnx_hw,
                                                   struct rwnx_cmd *cmd,
                                                   struct ipc_e2a_msg *msg)
{
    struct me_tx_credits_update_ind *ind = (struct me_tx_credits_update_ind *)msg->param;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    if (ind->sta_idx >= ARRAY_SIZE(rwnx_hw->sta_table))
        return 1;

    rwnx_txq_credit_update(rwnx_hw, ind->sta_idx, ind->tid, ind->credits);

    return 0;
}
#endif /* CONFIG_RWNX_FULLMAC */

/***************************************************************************
 * Messages from SM task
 **************************************************************************/
#ifdef CONFIG_RWNX_FULLMAC

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
static inline void cfg80211_chandef_create(struct cfg80211_chan_def *chandef,
                             struct ieee80211_channel *chan,
                             enum nl80211_channel_type chan_type)
{
        if (WARN_ON(!chan))
                return;
        chandef->chan = chan;
        chandef->center_freq2 = 0;
        switch (chan_type) {
        case NL80211_CHAN_NO_HT:
                chandef->width = NL80211_CHAN_WIDTH_20_NOHT;
                chandef->center_freq1 = chan->center_freq;
                break;
        case NL80211_CHAN_HT20:
                chandef->width = NL80211_CHAN_WIDTH_20;
                chandef->center_freq1 = chan->center_freq;
                break;
        case NL80211_CHAN_HT40PLUS:
                chandef->width = NL80211_CHAN_WIDTH_40;
                chandef->center_freq1 = chan->center_freq + 10;
                break;
        case NL80211_CHAN_HT40MINUS:
                chandef->width = NL80211_CHAN_WIDTH_40;
                chandef->center_freq1 = chan->center_freq - 10;
                break;
        default:
                WARN_ON(1);
        }
}
#endif

static atomic_t rwnx_connect_evt_seq = ATOMIC_INIT(0);
static atomic_t rwnx_connect_txn_seq = ATOMIC_INIT(0);

#define RWNX_CONNECT_IND_MAX_AGE_MS 12000
#define RWNX_TRANSIENT_DISC_GUARD_MS 1500
#define RWNX_GUARD_REPORT_INTERVAL_MS 10000

struct rwnx_conn_txn_snapshot {
    u32 id;
    enum rwnx_conn_txn_kind kind;
    enum rwnx_conn_txn_phase phase;
    unsigned long start_jiffies;
    unsigned long old_link_gone_jiffies;
    bool target_valid;
    bool prev_valid;
    u8 old_ap_idx;
    u8 target_bssid[ETH_ALEN];
    u8 prev_bssid[ETH_ALEN];
};

static void rwnx_conn_cleanup_link(struct rwnx_hw *rwnx_hw,
                                   struct rwnx_vif *rwnx_vif,
                                   bool preserve_external_auth);
static void rwnx_conn_cleanup_terminal(
    struct rwnx_hw *rwnx_hw, struct rwnx_vif *rwnx_vif,
    const struct rwnx_conn_txn_snapshot *snapshot);
static void rwnx_conn_tx_abort_pause(struct rwnx_vif *rwnx_vif);

enum rwnx_conn_guard_stat {
    RWNX_GUARD_NONCANONICAL_ROAMED = 0,
    RWNX_GUARD_DROP_STALE_SUCCESS,
    RWNX_GUARD_SYNTH_ROAM,
    RWNX_GUARD_DUP_CONNECTED,
    RWNX_GUARD_DUP_IND,
    RWNX_GUARD_SUPPRESS_PREV_STATE,
    RWNX_GUARD_DUP_TXN_CONNECT,
    RWNX_GUARD_DUP_TXN_ROAM,
    RWNX_GUARD_TRANSIENT_DISC,
    RWNX_GUARD_STAT_MAX,
};

static atomic_t rwnx_conn_guard_stats[RWNX_GUARD_STAT_MAX] = {
    [0 ... RWNX_GUARD_STAT_MAX - 1] = ATOMIC_INIT(0),
};
static int rwnx_conn_guard_stats_last_report[RWNX_GUARD_STAT_MAX];
static unsigned long rwnx_conn_guard_last_report_jiffies;
static DEFINE_SPINLOCK(rwnx_conn_guard_report_lock);

static const char * const rwnx_conn_guard_stat_names[RWNX_GUARD_STAT_MAX] = {
    [RWNX_GUARD_NONCANONICAL_ROAMED] = "noncanonical_roamed",
    [RWNX_GUARD_DROP_STALE_SUCCESS] = "stale_success",
    [RWNX_GUARD_SYNTH_ROAM] = "synth_roam",
    [RWNX_GUARD_DUP_CONNECTED] = "dup_connected",
    [RWNX_GUARD_DUP_IND] = "dup_ind",
    [RWNX_GUARD_SUPPRESS_PREV_STATE] = "prev_state_suppress",
    [RWNX_GUARD_DUP_TXN_CONNECT] = "dup_txn_connect",
    [RWNX_GUARD_DUP_TXN_ROAM] = "dup_txn_roam",
    [RWNX_GUARD_TRANSIENT_DISC] = "transient_disc",
};

int rwnx_conn_guard_stats_format(char *buf, size_t size)
{
    int len = 0;
    int i;

    len += scnprintf(buf + len, size - len, "connect_events=%d\n",
                     atomic_read(&rwnx_connect_evt_seq));
    len += scnprintf(buf + len, size - len, "connect_transactions=%d\n",
                     atomic_read(&rwnx_connect_txn_seq));
    for (i = 0; i < RWNX_GUARD_STAT_MAX && len < size; i++)
        len += scnprintf(buf + len, size - len, "conn_guard_%s=%d\n",
                         rwnx_conn_guard_stat_names[i],
                         atomic_read(&rwnx_conn_guard_stats[i]));

    return len;
}

static inline void rwnx_conn_guard_maybe_report(void)
{
    unsigned long flags;
    unsigned long now;
    int total[RWNX_GUARD_STAT_MAX];
    int delta[RWNX_GUARD_STAT_MAX];
    int i;

    now = jiffies;
    if (rwnx_conn_guard_last_report_jiffies &&
        time_before(now, rwnx_conn_guard_last_report_jiffies +
                          msecs_to_jiffies(RWNX_GUARD_REPORT_INTERVAL_MS))) {
        return;
    }

    spin_lock_irqsave(&rwnx_conn_guard_report_lock, flags);
    now = jiffies;
    if (rwnx_conn_guard_last_report_jiffies &&
        time_before(now, rwnx_conn_guard_last_report_jiffies +
                          msecs_to_jiffies(RWNX_GUARD_REPORT_INTERVAL_MS))) {
        spin_unlock_irqrestore(&rwnx_conn_guard_report_lock, flags);
        return;
    }

    rwnx_conn_guard_last_report_jiffies = now;
    for (i = 0; i < RWNX_GUARD_STAT_MAX; i++) {
        total[i] = atomic_read(&rwnx_conn_guard_stats[i]);
        delta[i] = total[i] - rwnx_conn_guard_stats_last_report[i];
        rwnx_conn_guard_stats_last_report[i] = total[i];
    }
    spin_unlock_irqrestore(&rwnx_conn_guard_report_lock, flags);

    AICWFDBG(LOGINFO,
             "conn_guard_summary(+10s): noncanonical_roamed=%d(total=%d) stale_success=%d(total=%d) synth_roam=%d(total=%d) dup_connected=%d(total=%d) dup_ind=%d(total=%d) prev_state_suppress=%d(total=%d) dup_txn_connect=%d(total=%d) dup_txn_roam=%d(total=%d) transient_disc=%d(total=%d)\r\n",
             delta[RWNX_GUARD_NONCANONICAL_ROAMED], total[RWNX_GUARD_NONCANONICAL_ROAMED],
             delta[RWNX_GUARD_DROP_STALE_SUCCESS], total[RWNX_GUARD_DROP_STALE_SUCCESS],
             delta[RWNX_GUARD_SYNTH_ROAM], total[RWNX_GUARD_SYNTH_ROAM],
             delta[RWNX_GUARD_DUP_CONNECTED], total[RWNX_GUARD_DUP_CONNECTED],
             delta[RWNX_GUARD_DUP_IND], total[RWNX_GUARD_DUP_IND],
             delta[RWNX_GUARD_SUPPRESS_PREV_STATE], total[RWNX_GUARD_SUPPRESS_PREV_STATE],
             delta[RWNX_GUARD_DUP_TXN_CONNECT], total[RWNX_GUARD_DUP_TXN_CONNECT],
             delta[RWNX_GUARD_DUP_TXN_ROAM], total[RWNX_GUARD_DUP_TXN_ROAM],
             delta[RWNX_GUARD_TRANSIENT_DISC], total[RWNX_GUARD_TRANSIENT_DISC]);
}

static inline void rwnx_conn_guard_note(enum rwnx_conn_guard_stat stat)
{
    if (stat >= RWNX_GUARD_STAT_MAX)
        return;

    atomic_inc(&rwnx_conn_guard_stats[stat]);
    rwnx_conn_guard_maybe_report();
}

static void rwnx_conn_txn_clear_locked(struct rwnx_conn_txn *txn)
{
    txn->id = 0;
    txn->kind = RWNX_CONN_TXN_NONE;
    txn->phase = RWNX_CONN_TXN_IDLE;
    txn->start_jiffies = 0;
    txn->old_link_gone_jiffies = 0;
    txn->target_valid = false;
    txn->prev_valid = false;
    txn->old_ap_idx = RWNX_INVALID_STA;
    memset(txn->target_bssid, 0, ETH_ALEN);
    memset(txn->prev_bssid, 0, ETH_ALEN);
}

static bool rwnx_conn_track_snapshot(struct rwnx_vif *rwnx_vif,
                                     struct rwnx_conn_txn_snapshot *snapshot)
{
    struct rwnx_conn_txn *txn = &rwnx_vif->conn_txn;
    unsigned long flags;

    memset(snapshot, 0, sizeof(*snapshot));
    spin_lock_irqsave(&txn->lock, flags);
    if (txn->kind == RWNX_CONN_TXN_NONE) {
        spin_unlock_irqrestore(&txn->lock, flags);
        return false;
    }

    snapshot->id = txn->id;
    snapshot->kind = txn->kind;
    snapshot->phase = txn->phase;
    snapshot->start_jiffies = txn->start_jiffies;
    snapshot->old_link_gone_jiffies = txn->old_link_gone_jiffies;
    snapshot->target_valid = txn->target_valid;
    snapshot->prev_valid = txn->prev_valid;
    snapshot->old_ap_idx = txn->old_ap_idx;
    memcpy(snapshot->target_bssid, txn->target_bssid, ETH_ALEN);
    memcpy(snapshot->prev_bssid, txn->prev_bssid, ETH_ALEN);
    spin_unlock_irqrestore(&txn->lock, flags);
    return true;
}

static bool rwnx_conn_track_finish(struct rwnx_vif *rwnx_vif, u32 txn_id,
                                   bool expect_late_disconnect)
{
    struct rwnx_conn_txn *txn = &rwnx_vif->conn_txn;
    unsigned long flags;
    bool finished = false;

    spin_lock_irqsave(&txn->lock, flags);
    if ((txn->kind != RWNX_CONN_TXN_NONE) && (txn->id == txn_id)) {
        if (expect_late_disconnect) {
            txn->late_disconnect_pending = true;
            txn->late_disconnect_deadline = jiffies +
                msecs_to_jiffies(RWNX_TRANSIENT_DISC_GUARD_MS);
            txn->late_disconnect_txn_id = txn_id;
        }
        rwnx_conn_txn_clear_locked(txn);
        finished = true;
    }
    spin_unlock_irqrestore(&txn->lock, flags);

    if (finished)
        cancel_delayed_work(&txn->timeout_work);
    return finished;
}

static bool rwnx_conn_track_mark_old_gone(struct rwnx_vif *rwnx_vif,
                                          u32 txn_id)
{
    struct rwnx_conn_txn *txn = &rwnx_vif->conn_txn;
    unsigned long flags;
    bool marked = false;

    spin_lock_irqsave(&txn->lock, flags);
    if ((txn->kind == RWNX_CONN_TXN_ROAM) &&
        (txn->id == txn_id) &&
        (txn->phase == RWNX_CONN_TXN_WAIT_RESULT)) {
        txn->phase = RWNX_CONN_TXN_OLD_LINK_GONE;
        txn->old_link_gone_jiffies = jiffies;
        atomic_set(&rwnx_vif->conn_tx_paused, 1);
        marked = true;
    }
    spin_unlock_irqrestore(&txn->lock, flags);
    return marked;
}

static bool rwnx_conn_track_take_late_disconnect(struct rwnx_vif *rwnx_vif,
                                                 u16 reason, u32 *txn_id)
{
    struct rwnx_conn_txn *txn = &rwnx_vif->conn_txn;
    unsigned long flags;
    bool ignore = false;

    spin_lock_irqsave(&txn->lock, flags);
    if (txn->late_disconnect_pending) {
        if ((reason == 0) &&
            time_before_eq(jiffies, txn->late_disconnect_deadline)) {
            *txn_id = txn->late_disconnect_txn_id;
            ignore = true;
        }
        txn->late_disconnect_pending = false;
        txn->late_disconnect_deadline = 0;
        txn->late_disconnect_txn_id = 0;
    }
    spin_unlock_irqrestore(&txn->lock, flags);
    return ignore;
}

static void rwnx_conn_track_arm_late_disconnect(struct rwnx_vif *rwnx_vif,
                                                u32 event_id)
{
    struct rwnx_conn_txn *txn = &rwnx_vif->conn_txn;
    unsigned long flags;

    spin_lock_irqsave(&txn->lock, flags);
    if (txn->kind == RWNX_CONN_TXN_NONE) {
        txn->late_disconnect_pending = true;
        txn->late_disconnect_deadline = jiffies +
            msecs_to_jiffies(RWNX_TRANSIENT_DISC_GUARD_MS);
        txn->late_disconnect_txn_id = event_id;
    }
    spin_unlock_irqrestore(&txn->lock, flags);
}

static void rwnx_conn_report_timeout(struct rwnx_vif *rwnx_vif,
                                     const struct rwnx_conn_txn_snapshot *snapshot,
                                     gfp_t gfp)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
    cfg80211_connect_timeout(rwnx_vif->ndev,
        snapshot->target_valid ? snapshot->target_bssid : NULL,
        NULL, 0, gfp, NL80211_TIMEOUT_UNSPECIFIED);
#else
    cfg80211_connect_result(rwnx_vif->ndev,
        snapshot->target_valid ? snapshot->target_bssid : NULL,
        NULL, 0, NULL, 0, WLAN_STATUS_UNSPECIFIED_FAILURE, gfp);
#endif
}

static void rwnx_conn_timeout_work(struct work_struct *work)
{
    struct rwnx_conn_txn *txn =
        container_of(to_delayed_work(work), struct rwnx_conn_txn, timeout_work);
    struct rwnx_vif *rwnx_vif =
        container_of(txn, struct rwnx_vif, conn_txn);
    struct rwnx_conn_txn_snapshot snapshot;
    struct net_device *dev = rwnx_vif->ndev;

    if (!rwnx_conn_track_snapshot(rwnx_vif, &snapshot))
        return;
    if (!rwnx_conn_track_finish(rwnx_vif, snapshot.id, true))
        return;

    atomic_set(&rwnx_vif->drv_conn_state, RWNX_DRV_STATUS_DISCONNECTED);
    if (dev && rwnx_vif->up) {
        netif_tx_stop_all_queues(dev);
        netif_carrier_off(dev);
    }
    rwnx_conn_tx_abort_pause(rwnx_vif);
    rwnx_conn_cleanup_terminal(rwnx_vif->rwnx_hw, rwnx_vif,
                               &snapshot);
    if (dev && rwnx_vif->up) {
        if (snapshot.kind == RWNX_CONN_TXN_ROAM)
            cfg80211_disconnected(dev, 0, NULL, 0, false, GFP_KERNEL);
        else
            rwnx_conn_report_timeout(rwnx_vif, &snapshot, GFP_KERNEL);
    }
    AICWFDBG(LOGERROR,
             "conn_txn_end id:%u vif:%u kind:%u report:%s status:timeout duration_ms:%u state:%d carrier:%d\r\n",
             snapshot.id, rwnx_vif->vif_index, snapshot.kind,
             snapshot.kind == RWNX_CONN_TXN_ROAM ? "disconnected" : "connect_timeout",
             jiffies_to_msecs(jiffies - snapshot.start_jiffies),
             atomic_read(&rwnx_vif->drv_conn_state),
             dev ? netif_carrier_ok(dev) : 0);
}

void rwnx_conn_track_init(struct rwnx_vif *rwnx_vif)
{
    struct rwnx_conn_txn *txn = &rwnx_vif->conn_txn;

    spin_lock_init(&txn->lock);
    INIT_DELAYED_WORK(&txn->timeout_work, rwnx_conn_timeout_work);
    atomic_set(&rwnx_vif->conn_tx_paused, 0);
    rwnx_conn_txn_clear_locked(txn);
    txn->late_disconnect_pending = false;
    txn->late_disconnect_deadline = 0;
    txn->late_disconnect_txn_id = 0;
}

void rwnx_conn_track_deinit(struct rwnx_vif *rwnx_vif)
{
    struct rwnx_conn_txn *txn = &rwnx_vif->conn_txn;
    unsigned long flags;

    cancel_delayed_work_sync(&txn->timeout_work);
    spin_lock_irqsave(&txn->lock, flags);
    rwnx_conn_txn_clear_locked(txn);
    txn->late_disconnect_pending = false;
    txn->late_disconnect_deadline = 0;
    txn->late_disconnect_txn_id = 0;
    spin_unlock_irqrestore(&txn->lock, flags);
    atomic_set(&rwnx_vif->conn_tx_paused, 0);
}

bool rwnx_conn_tx_paused(struct rwnx_vif *rwnx_vif)
{
    return rwnx_vif && atomic_read(&rwnx_vif->conn_tx_paused);
}

static void rwnx_conn_tx_abort_pause(struct rwnx_vif *rwnx_vif)
{
    atomic_set(&rwnx_vif->conn_tx_paused, 0);
}

enum rwnx_conn_tx_resume_result {
    RWNX_CONN_TX_RESUME_NOT_PAUSED,
    RWNX_CONN_TX_RESUME_IMMEDIATE,
    RWNX_CONN_TX_RESUME_DEFERRED_TBUSY,
    RWNX_CONN_TX_RESUME_BLOCKED_VIF,
    RWNX_CONN_TX_RESUME_BLOCKED_NO_DEVICE,
    RWNX_CONN_TX_RESUME_BLOCKED_BUS_DOWN,
    RWNX_CONN_TX_RESUME_BLOCKED_USB_DOWN,
    RWNX_CONN_TX_RESUME_BLOCKED_CARRIER,
};

static const char *
rwnx_conn_tx_resume_name(enum rwnx_conn_tx_resume_result result)
{
    switch (result) {
    case RWNX_CONN_TX_RESUME_NOT_PAUSED:
        return "not_paused";
    case RWNX_CONN_TX_RESUME_IMMEDIATE:
        return "immediate";
    case RWNX_CONN_TX_RESUME_DEFERRED_TBUSY:
        return "deferred_tbusy";
    case RWNX_CONN_TX_RESUME_BLOCKED_VIF:
        return "blocked_vif";
    case RWNX_CONN_TX_RESUME_BLOCKED_NO_DEVICE:
        return "blocked_no_device";
    case RWNX_CONN_TX_RESUME_BLOCKED_BUS_DOWN:
        return "blocked_bus_down";
    case RWNX_CONN_TX_RESUME_BLOCKED_USB_DOWN:
        return "blocked_usb_down";
    case RWNX_CONN_TX_RESUME_BLOCKED_CARRIER:
        return "blocked_carrier";
    }

    return "blocked_unknown";
}

static enum rwnx_conn_tx_resume_result
rwnx_conn_tx_resume(struct rwnx_vif *rwnx_vif)
{
    struct net_device *dev = rwnx_vif->ndev;
    struct rwnx_runtime_stats *stats = &rwnx_vif->rwnx_hw->runtime_stats;
    enum rwnx_conn_tx_resume_result result;

    if (!atomic_xchg(&rwnx_vif->conn_tx_paused, 0))
        return RWNX_CONN_TX_RESUME_NOT_PAUSED;
    if (!dev || !rwnx_vif->up) {
        result = RWNX_CONN_TX_RESUME_BLOCKED_VIF;
        goto note_result;
    }
    if (!netif_carrier_ok(dev)) {
        result = RWNX_CONN_TX_RESUME_BLOCKED_CARRIER;
        goto note_result;
    }

#ifdef AICWF_USB_SUPPORT
    switch (aicwf_usb_tx_maybe_wake(rwnx_vif->rwnx_hw, dev)) {
    case AICWF_USB_TX_WAKE_IMMEDIATE:
        result = RWNX_CONN_TX_RESUME_IMMEDIATE;
        break;
    case AICWF_USB_TX_WAKE_DEFERRED_TBUSY:
        result = RWNX_CONN_TX_RESUME_DEFERRED_TBUSY;
        break;
    case AICWF_USB_TX_WAKE_BLOCKED_BUS_DOWN:
        result = RWNX_CONN_TX_RESUME_BLOCKED_BUS_DOWN;
        break;
    case AICWF_USB_TX_WAKE_BLOCKED_USB_DOWN:
        result = RWNX_CONN_TX_RESUME_BLOCKED_USB_DOWN;
        break;
    case AICWF_USB_TX_WAKE_BLOCKED_CARRIER:
        result = RWNX_CONN_TX_RESUME_BLOCKED_CARRIER;
        break;
    case AICWF_USB_TX_WAKE_BLOCKED_NO_DEVICE:
    default:
        result = RWNX_CONN_TX_RESUME_BLOCKED_NO_DEVICE;
        break;
    }
#else
    netif_tx_wake_all_queues(dev);
    result = RWNX_CONN_TX_RESUME_IMMEDIATE;
#endif

note_result:
    if (result == RWNX_CONN_TX_RESUME_IMMEDIATE)
        atomic_inc(&stats->roam_tx_resume_immediate);
    else if (result == RWNX_CONN_TX_RESUME_DEFERRED_TBUSY)
        atomic_inc(&stats->roam_tx_resume_deferred_tbusy);
    else
        atomic_inc(&stats->roam_tx_resume_blocked);
    return result;
}

int rwnx_conn_track_start(struct rwnx_vif *rwnx_vif,
                          enum rwnx_conn_txn_kind kind,
                          const u8 *target_bssid, const u8 *prev_bssid)
{
    struct rwnx_conn_txn *txn = &rwnx_vif->conn_txn;
    unsigned long flags;
    u32 txn_id;
    bool log_target_valid;
    bool log_prev_valid;
    u8 log_old_ap;

    if (kind != RWNX_CONN_TXN_INITIAL &&
        kind != RWNX_CONN_TXN_ROAM)
        return -EINVAL;

    spin_lock_irqsave(&txn->lock, flags);
    if (txn->kind != RWNX_CONN_TXN_NONE) {
        spin_unlock_irqrestore(&txn->lock, flags);
        return -EINPROGRESS;
    }

    txn_id = (u32)atomic_inc_return(&rwnx_connect_txn_seq);
    txn->id = txn_id;
    txn->kind = kind;
    txn->phase = RWNX_CONN_TXN_WAIT_RESULT;
    txn->start_jiffies = jiffies;
    txn->target_valid = target_bssid && !is_zero_ether_addr(target_bssid);
    txn->prev_valid = prev_bssid && !is_zero_ether_addr(prev_bssid);
    txn->old_ap_idx = rwnx_vif->sta.ap ? rwnx_vif->sta.ap->sta_idx : RWNX_INVALID_STA;
    if (txn->target_valid)
        memcpy(txn->target_bssid, target_bssid, ETH_ALEN);
    else
        memset(txn->target_bssid, 0, ETH_ALEN);
    if (txn->prev_valid)
        memcpy(txn->prev_bssid, prev_bssid, ETH_ALEN);
    else
        memset(txn->prev_bssid, 0, ETH_ALEN);
    if (txn->late_disconnect_pending &&
        time_after(jiffies, txn->late_disconnect_deadline)) {
        txn->late_disconnect_pending = false;
        txn->late_disconnect_deadline = 0;
        txn->late_disconnect_txn_id = 0;
    }
    log_target_valid = txn->target_valid;
    log_prev_valid = txn->prev_valid;
    log_old_ap = txn->old_ap_idx;
    atomic_set(&rwnx_vif->drv_conn_state, RWNX_DRV_STATUS_CONNECTING);
    spin_unlock_irqrestore(&txn->lock, flags);

    mod_delayed_work(system_wq, &txn->timeout_work,
                     msecs_to_jiffies(RWNX_CONNECT_IND_MAX_AGE_MS));
    AICWFDBG(LOGINFO,
             "conn_txn_begin id:%u vif:%u kind:%u prev_set:%d target_set:%d old_ap:%u state:%d carrier:%d\r\n",
             txn_id, rwnx_vif->vif_index, kind,
             log_prev_valid, log_target_valid, log_old_ap,
             atomic_read(&rwnx_vif->drv_conn_state),
             rwnx_vif->ndev ? netif_carrier_ok(rwnx_vif->ndev) : 0);
    return 0;
}

void rwnx_conn_track_abort(struct rwnx_vif *rwnx_vif, const char *reason)
{
    struct rwnx_conn_txn_snapshot snapshot;

    if (!rwnx_conn_track_snapshot(rwnx_vif, &snapshot))
        return;
    if (!rwnx_conn_track_finish(rwnx_vif, snapshot.id, false))
        return;
    rwnx_conn_tx_abort_pause(rwnx_vif);
    AICWFDBG(LOGINFO,
             "conn_txn_end id:%u vif:%u kind:%u report:none status:abort reason:%s duration_ms:%u state:%d carrier:%d\r\n",
             snapshot.id, rwnx_vif->vif_index, snapshot.kind, reason,
             jiffies_to_msecs(jiffies - snapshot.start_jiffies),
             atomic_read(&rwnx_vif->drv_conn_state),
             rwnx_vif->ndev ? netif_carrier_ok(rwnx_vif->ndev) : 0);
}

enum rwnx_conn_txn_kind rwnx_conn_track_kind(struct rwnx_vif *rwnx_vif)
{
    struct rwnx_conn_txn_snapshot snapshot;

    if (!rwnx_conn_track_snapshot(rwnx_vif, &snapshot))
        return RWNX_CONN_TXN_NONE;
    return snapshot.kind;
}

u32 rwnx_conn_track_id(struct rwnx_vif *rwnx_vif)
{
    struct rwnx_conn_txn_snapshot snapshot;

    if (!rwnx_conn_track_snapshot(rwnx_vif, &snapshot))
        return 0;
    return snapshot.id;
}

void rwnx_conn_cancel(struct rwnx_vif *rwnx_vif, u16 reason,
                      const char *source)
{
    struct rwnx_conn_txn_snapshot snapshot;
    struct net_device *dev = rwnx_vif->ndev;

    if (!rwnx_conn_track_snapshot(rwnx_vif, &snapshot))
        return;
    if (!rwnx_conn_track_finish(rwnx_vif, snapshot.id, true))
        return;

    rwnx_conn_tx_abort_pause(rwnx_vif);
    atomic_set(&rwnx_vif->drv_conn_state, RWNX_DRV_STATUS_DISCONNECTED);
    if (dev && rwnx_vif->up) {
        netif_tx_stop_all_queues(dev);
        netif_carrier_off(dev);
        if (snapshot.kind == RWNX_CONN_TXN_ROAM) {
            cfg80211_disconnected(dev, reason, NULL, 0, true, GFP_ATOMIC);
        } else {
            rwnx_conn_report_timeout(rwnx_vif, &snapshot, GFP_ATOMIC);
        }
        rwnx_conn_cleanup_terminal(rwnx_vif->rwnx_hw, rwnx_vif,
                                   &snapshot);
    }
    AICWFDBG(LOGINFO,
             "conn_txn_end id:%u vif:%u kind:%u report:%s status:cancel source:%s reason:%u duration_ms:%u state:%d carrier:%d\r\n",
             snapshot.id, rwnx_vif->vif_index, snapshot.kind,
             snapshot.kind == RWNX_CONN_TXN_ROAM ? "disconnected" : "connect_timeout",
             source, reason, jiffies_to_msecs(jiffies - snapshot.start_jiffies),
             atomic_read(&rwnx_vif->drv_conn_state),
             dev ? netif_carrier_ok(dev) : 0);
}

static inline int rwnx_rx_sm_connect_ind(struct rwnx_hw *rwnx_hw,
                                         struct rwnx_cmd *cmd,
                                         struct ipc_e2a_msg *msg)
{
    struct sm_connect_ind *ind = (struct sm_connect_ind *)msg->param;
    struct rwnx_conn_txn_snapshot txn;
    struct rwnx_vif *rwnx_vif;
    struct net_device *dev;
    const u8 *req_ie;
    const u8 *rsp_ie;
    const u8 *extcap_ie;
    const struct ieee_types_extcap *extcap;
    struct ieee80211_channel *chan = NULL;
    struct rwnx_sta *sta;
    struct cfg80211_chan_def chandef;
    u16 assoc_req_ie_len;
    u16 assoc_rsp_ie_len;
    unsigned int roamed_raw = 0;
    unsigned long age_ms = 0;
    u32 evt_id;
    int prev_state;
    bool transaction_active;
    bool had_sta_ap;
    bool prev_valid;
    bool bssid_changed;
    bool roamed_hint;
    bool roamed_noncanonical;
    bool roam = false;
    bool synthesized_roam = false;
    bool stale = false;
    bool target_mismatch = false;
    bool malformed_success = false;
    bool arm_late_disconnect = false;
    enum rwnx_conn_tx_resume_result tx_resume =
        RWNX_CONN_TX_RESUME_NOT_PAUSED;
    u8 prev_bssid[ETH_ALEN] = {0};
    u8 txq_status;
    unsigned long pause_ms = 0;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    if (ind->vif_idx >= ARRAY_SIZE(rwnx_hw->vif_table)) {
        AICWFDBG(LOGERROR, "%s invalid vif idx:%u\r\n",
                 __func__, ind->vif_idx);
        return 0;
    }

    rwnx_vif = rwnx_hw->vif_table[ind->vif_idx];
    if (!rwnx_vif || !rwnx_vif->ndev) {
        AICWFDBG(LOGERROR, "%s rwnx_vif is null\r\n", __func__);
        return 0;
    }

    dev = rwnx_vif->ndev;
    evt_id = (u32)atomic_inc_return(&rwnx_connect_evt_seq);
    prev_state = atomic_read(&rwnx_vif->drv_conn_state);
    had_sta_ap = rwnx_vif->sta.ap != NULL;
    if (had_sta_ap)
        memcpy(prev_bssid, rwnx_vif->sta.ap->mac_addr, ETH_ALEN);

    transaction_active = rwnx_conn_track_snapshot(rwnx_vif, &txn);
    if (transaction_active) {
        age_ms = jiffies_to_msecs(jiffies - txn.start_jiffies);
        if (!had_sta_ap && txn.prev_valid)
            memcpy(prev_bssid, txn.prev_bssid, ETH_ALEN);
        roam = txn.kind == RWNX_CONN_TXN_ROAM;
    }

    prev_valid = had_sta_ap || (transaction_active && txn.prev_valid);
    bssid_changed = prev_valid &&
        !is_zero_ether_addr((const u8 *)ind->bssid.array) &&
        memcmp(prev_bssid, ind->bssid.array, ETH_ALEN);
    memcpy(&roamed_raw, &ind->roamed, sizeof(ind->roamed));
    roamed_hint = roamed_raw != 0;
    roamed_noncanonical = roamed_raw > 1;
    if (roamed_noncanonical)
        rwnx_conn_guard_note(RWNX_GUARD_NONCANONICAL_ROAMED);

    if (!transaction_active && ind->status_code == 0) {
        if (roamed_hint && had_sta_ap &&
            prev_state == RWNX_DRV_STATUS_CONNECTED && bssid_changed) {
            roam = true;
        } else if (had_sta_ap &&
                   prev_state == RWNX_DRV_STATUS_CONNECTED &&
                   bssid_changed) {
            roam = true;
            synthesized_roam = true;
            rwnx_conn_guard_note(RWNX_GUARD_SYNTH_ROAM);
        } else {
            rwnx_conn_guard_note(had_sta_ap ? RWNX_GUARD_DUP_CONNECTED :
                                             RWNX_GUARD_SUPPRESS_PREV_STATE);
            AICWFDBG(LOGINFO,
                     "conn_ind evt:%u vif:%u decision:drop_no_txn status:%u raw_roamed:%u noncanonical:%d bssid_changed:%d state:%d had_ap:%d\r\n",
                     evt_id, ind->vif_idx, ind->status_code, roamed_raw,
                     roamed_noncanonical, bssid_changed, prev_state,
                     had_sta_ap);
            return 0;
        }
    } else if (transaction_active && roam && !roamed_hint &&
               ind->status_code == 0) {
        synthesized_roam = true;
        rwnx_conn_guard_note(RWNX_GUARD_SYNTH_ROAM);
    }

    assoc_req_ie_len = ind->assoc_req_ie_len;
    assoc_rsp_ie_len = ind->assoc_rsp_ie_len;
    if ((u32)assoc_req_ie_len + (u32)assoc_rsp_ie_len >
        sizeof(ind->assoc_ie_buf)) {
        AICWFDBG(LOGERROR,
                 "conn_ind evt:%u vif:%u invalid_ie_lengths req:%u rsp:%u max:%zu\r\n",
                 evt_id, ind->vif_idx, assoc_req_ie_len, assoc_rsp_ie_len,
                 sizeof(ind->assoc_ie_buf));
        assoc_req_ie_len = 0;
        assoc_rsp_ie_len = 0;
    }
    req_ie = (const u8 *)ind->assoc_ie_buf;
    rsp_ie = req_ie + assoc_req_ie_len;

    if (transaction_active && ind->status_code == 0) {
        stale = age_ms > RWNX_CONNECT_IND_MAX_AGE_MS;
        target_mismatch = txn.target_valid &&
            memcmp(txn.target_bssid, ind->bssid.array, ETH_ALEN);
        if (stale || target_mismatch)
            rwnx_conn_guard_note(RWNX_GUARD_DROP_STALE_SUCCESS);
    }

    if (ind->status_code == 0) {
        if (ind->ap_idx >= ARRAY_SIZE(rwnx_hw->sta_table)) {
            AICWFDBG(LOGERROR, "conn_ind evt:%u invalid ap_idx:%u\r\n",
                     evt_id, ind->ap_idx);
            malformed_success = true;
        } else if (ind->ch_idx >= NX_CHAN_CTXT_CNT) {
            AICWFDBG(LOGERROR, "conn_ind evt:%u invalid ch_idx:%u\r\n",
                     evt_id, ind->ch_idx);
            malformed_success = true;
        } else {
            chan = ieee80211_get_channel(rwnx_hw->wiphy, ind->center_freq);
            if (!chan) {
                AICWFDBG(LOGERROR,
                         "conn_ind evt:%u invalid center_freq:%u\r\n",
                         evt_id, ind->center_freq);
                malformed_success = true;
            }
        }
    }

    AICWFDBG(LOGINFO,
             "conn_ind evt:%u txn:%u vif:%u kind:%u phase:%u status:%u raw_roamed:%u noncanonical:%d roam:%d synth:%d age_ms:%lu state:%d had_ap:%d prev_set:%d target_set:%d bssid_changed:%d stale:%d mismatch:%d malformed:%d\r\n",
             evt_id, transaction_active ? txn.id : 0, ind->vif_idx,
             transaction_active ? txn.kind : RWNX_CONN_TXN_NONE,
             transaction_active ? txn.phase : RWNX_CONN_TXN_IDLE,
             ind->status_code, roamed_raw, roamed_noncanonical, roam,
             synthesized_roam, age_ms, prev_state, had_sta_ap, prev_valid,
             transaction_active ? txn.target_valid :
                 !is_zero_ether_addr((const u8 *)ind->bssid.array),
             bssid_changed, stale, target_mismatch, malformed_success);

    if (ind->status_code != 0 || stale || target_mismatch ||
        malformed_success) {
        const char *failure = ind->status_code ? "firmware_reject" :
                              stale ? "stale_success" :
                              target_mismatch ? "target_mismatch" :
                                                "malformed_success";

        if (!transaction_active) {
            AICWFDBG(LOGINFO,
                     "conn_ind evt:%u vif:%u decision:drop_late_failure status:%u reason:%s\r\n",
                     evt_id, ind->vif_idx, ind->status_code, failure);
            return 0;
        }
        if (!rwnx_conn_track_finish(rwnx_vif, txn.id, true))
            return 0;

        atomic_set(&rwnx_vif->drv_conn_state,
                   RWNX_DRV_STATUS_DISCONNECTED);
        netif_tx_stop_all_queues(dev);
        netif_carrier_off(dev);
        rwnx_conn_tx_abort_pause(rwnx_vif);
        rwnx_conn_cleanup_terminal(rwnx_hw, rwnx_vif, &txn);

        if (txn.kind == RWNX_CONN_TXN_ROAM) {
            if (rwnx_vif->up)
                cfg80211_disconnected(dev, 0, NULL, 0, false, GFP_ATOMIC);
        } else if (rwnx_vif->up) {
            if (stale || target_mismatch) {
                rwnx_conn_report_timeout(rwnx_vif, &txn, GFP_ATOMIC);
            } else {
                cfg80211_connect_result(dev,
                    (const u8 *)ind->bssid.array, req_ie, assoc_req_ie_len,
                    rsp_ie, assoc_rsp_ie_len,
                    ind->status_code ? ind->status_code :
                                       WLAN_STATUS_UNSPECIFIED_FAILURE,
                    GFP_ATOMIC);
            }
        }
        if (ind->status_code == WLAN_STATUS_NOT_SUPPORTED_AUTH_ALG &&
            rwnx_vif->wep_enabled)
            rwnx_vif->wep_auth_err = true;

        AICWFDBG(LOGINFO,
                 "conn_txn_end id:%u evt:%u vif:%u kind:%u report:%s status:%s fw_status:%u duration_ms:%lu state:%d carrier:%d\r\n",
                 txn.id, evt_id, ind->vif_idx, txn.kind,
                 txn.kind == RWNX_CONN_TXN_ROAM ? "disconnected" :
                 (stale || target_mismatch ? "connect_timeout" :
                                             "connect_result"),
                 failure, ind->status_code, age_ms,
                 atomic_read(&rwnx_vif->drv_conn_state),
                 netif_carrier_ok(dev));
        return 0;
    }

    if (transaction_active) {
        arm_late_disconnect = roam &&
            txn.phase != RWNX_CONN_TXN_OLD_LINK_GONE;
        if (!rwnx_conn_track_finish(rwnx_vif, txn.id,
                                    arm_late_disconnect))
            return 0;
    } else if (roam) {
        arm_late_disconnect = true;
        rwnx_conn_track_arm_late_disconnect(rwnx_vif, evt_id);
    }

    if (roam &&
        (!transaction_active ||
         txn.phase != RWNX_CONN_TXN_OLD_LINK_GONE))
        rwnx_conn_cleanup_link(rwnx_hw, rwnx_vif, true);

    sta = &rwnx_hw->sta_table[ind->ap_idx];
    sta->valid = true;
    sta->sta_idx = ind->ap_idx;
    sta->ch_idx = ind->ch_idx;
    sta->vif_idx = ind->vif_idx;
    sta->vlan_idx = sta->vif_idx;
    sta->qos = ind->qos;
    sta->acm = ind->acm;
    sta->ps.active = false;
    sta->aid = ind->aid;
    sta->band = ind->band;
    sta->width = ind->width;
    sta->center_freq = ind->center_freq;
    sta->center_freq1 = ind->center_freq1;
    sta->center_freq2 = ind->center_freq2;
    memcpy(sta->mac_addr, ind->bssid.array, ETH_ALEN);
    memcpy(sta->ac_param, ind->ac_param, sizeof(sta->ac_param));
    rwnx_vif->sta.ap = sta;
    memcpy(rwnx_vif->sta.bssid, ind->bssid.array, ETH_ALEN);

    cfg80211_chandef_create(&chandef, chan, NL80211_CHAN_NO_HT);
    if (!rwnx_hw->mod_params->ht_on)
        chandef.width = NL80211_CHAN_WIDTH_20_NOHT;
    else if (ind->width < PHY_CHNL_BW_OTHER)
        chandef.width = chnl2bw[ind->width];
    else
        chandef.width = NL80211_CHAN_WIDTH_20;
    chandef.center_freq1 = ind->center_freq1;
    chandef.center_freq2 = ind->center_freq2;
    rwnx_chanctx_link(rwnx_vif, ind->ch_idx, &chandef);

    txq_status = ind->ch_idx == rwnx_hw->cur_chanctx ?
                 0 : RWNX_TXQ_STOP_CHAN;
    rwnx_txq_sta_init(rwnx_hw, sta, txq_status);
    rwnx_txq_tdls_vif_init(rwnx_vif);
#ifdef CONFIG_DEBUG_FS
    rwnx_dbgfs_register_rc_stat(rwnx_hw, sta);
#endif
    rwnx_mu_group_sta_init(sta, NULL);

    extcap_ie = cfg80211_find_ie(WLAN_EID_EXT_CAPABILITY,
                                 rsp_ie, assoc_rsp_ie_len);
    if (extcap_ie && extcap_ie[1] >= 5) {
        extcap = (void *)extcap_ie;
        rwnx_vif->tdls_chsw_prohibited =
            extcap->ext_capab[4] &
            WLAN_EXT_CAPA5_TDLS_CH_SW_PROHIBITED;
    }

#ifdef CONFIG_RWNX_BFMER
    if (rwnx_hw->mod_params->bfmer) {
        const u8 *vht_capa_ie;
        const struct ieee80211_vht_cap *vht_cap;

        vht_capa_ie = cfg80211_find_ie(WLAN_EID_VHT_CAPABILITY,
                                       rsp_ie, assoc_rsp_ie_len);
        if (vht_capa_ie) {
            vht_cap =
                (const struct ieee80211_vht_cap *)(vht_capa_ie + 2);
            rwnx_send_bfmer_enable(rwnx_hw, sta, vht_cap);
        }
    }
#endif

#ifdef CONFIG_RWNX_MON_DATA
    if (rwnx_hw->monitor_vif != RWNX_INVALID_VIF &&
        rwnx_hw->monitor_vif < ARRAY_SIZE(rwnx_hw->vif_table) &&
        rwnx_hw->vif_table[rwnx_hw->monitor_vif]) {
        struct rwnx_vif *rwnx_mon_vif =
            rwnx_hw->vif_table[rwnx_hw->monitor_vif];
        rwnx_chanctx_unlink(rwnx_mon_vif);
        rwnx_chanctx_link(rwnx_mon_vif, ind->ch_idx, NULL);
    }
#endif

    if (rwnx_vif->wep_enabled)
        rwnx_vif->wep_auth_err = false;
    rwnx_external_auth_disable(rwnx_vif);
    atomic_set(&rwnx_vif->drv_conn_state, RWNX_DRV_STATUS_CONNECTED);

    if (roam) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
        struct cfg80211_roam_info info;

        memset(&info, 0, sizeof(info));
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
        info.channel = chan;
        info.bssid = (const u8 *)ind->bssid.array;
#else
        info.links[0].channel = chan;
        info.links[0].bssid = (const u8 *)ind->bssid.array;
#endif
        info.req_ie = req_ie;
        info.req_ie_len = assoc_req_ie_len;
        info.resp_ie = rsp_ie;
        info.resp_ie_len = assoc_rsp_ie_len;
        cfg80211_roamed(dev, &info, GFP_ATOMIC);
#else
        cfg80211_roamed(dev
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 39) || defined(COMPAT_KERNEL_RELEASE)
            , chan
#endif
            , (const u8 *)ind->bssid.array
            , req_ie, assoc_req_ie_len
            , rsp_ie, assoc_rsp_ie_len
            , GFP_ATOMIC);
#endif
    } else {
        cfg80211_connect_result(dev, (const u8 *)ind->bssid.array,
                                req_ie, assoc_req_ie_len,
                                rsp_ie, assoc_rsp_ie_len,
                                WLAN_STATUS_SUCCESS, GFP_ATOMIC);
    }

    if (roam) {
        netif_carrier_on(dev);
        if (transaction_active &&
            txn.phase == RWNX_CONN_TXN_OLD_LINK_GONE) {
            pause_ms = jiffies_to_msecs(
                jiffies - txn.old_link_gone_jiffies);
        }
        tx_resume = rwnx_conn_tx_resume(rwnx_vif);
    } else {
        netif_tx_start_all_queues(dev);
        netif_carrier_on(dev);
    }
    AICWFDBG(LOGINFO,
             "conn_txn_end id:%u evt:%u vif:%u kind:%u phase:%u report:%s status:success duration_ms:%lu pause_ms:%lu tx_resume:%s bssid_changed:%d late_guard:%d state:%d carrier:%d\r\n",
             transaction_active ? txn.id : 0, evt_id, ind->vif_idx,
             roam ? RWNX_CONN_TXN_ROAM : RWNX_CONN_TXN_INITIAL,
             transaction_active ? txn.phase : RWNX_CONN_TXN_IDLE,
             roam ? "roamed" : "connect_result", age_ms, pause_ms,
             rwnx_conn_tx_resume_name(tx_resume),
             bssid_changed,
             arm_late_disconnect,
             atomic_read(&rwnx_vif->drv_conn_state),
             netif_carrier_ok(dev));
    return 0;
}
#if 0
void rwnx_cfg80211_unlink_bss(struct rwnx_hw *rwnx_hw, struct rwnx_vif *rwnx_vif){
	struct wiphy *wiphy = rwnx_hw->wiphy;
	struct cfg80211_bss *bss = NULL;

	RWNX_DBG(RWNX_FN_ENTRY_STR);

	bss = cfg80211_get_bss(wiphy, NULL/*notify_channel*/,
		rwnx_vif->sta.bssid, rwnx_vif->sta.ssid,
		rwnx_vif->sta.ssid_len,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
		IEEE80211_BSS_TYPE_ESS,
		IEEE80211_PRIVACY(true));//temp set true
#else
		WLAN_CAPABILITY_ESS,
		WLAN_CAPABILITY_ESS);
#endif

	if (bss) {
		cfg80211_unlink_bss(wiphy, bss);
		AICWFDBG(LOGINFO, "%s(): cfg80211_unlink %s!!\n", __func__, rwnx_vif->sta.ssid);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0)
		cfg80211_put_bss(wiphy, bss);
#else
		cfg80211_put_bss(bss);
#endif
	}else{
		AICWFDBG(LOGERROR, "%s(): cfg80211_unlink error %s!!\n", __func__, rwnx_vif->sta.ssid);
	}

	memset(rwnx_vif->sta.ssid, 0, rwnx_vif->sta.ssid_len);
	rwnx_vif->sta.ssid_len = 0;
	memset(rwnx_vif->sta.bssid, 0, ETH_ALEN);
}

#endif

extern u8 dhcped;

static void rwnx_conn_cleanup_link(struct rwnx_hw *rwnx_hw,
                                   struct rwnx_vif *rwnx_vif,
                                   bool preserve_external_auth)
{
#ifdef AICWF_RX_REORDER
    struct reord_ctrl_info *reord_info;
    struct reord_ctrl_info *tmp;
    struct aicwf_rx_priv *rx_priv;
    const u8 *macaddr;
#endif

    if (!rwnx_vif)
        return;

#ifdef CONFIG_RWNX_BFMER
    if (rwnx_vif->sta.ap)
        rwnx_bfmer_report_del(rwnx_hw, rwnx_vif->sta.ap);
#endif

#ifdef AICWF_RX_REORDER
#ifdef AICWF_SDIO_SUPPORT
    rx_priv = rwnx_hw->sdiodev->rx_priv;
#else
    rx_priv = rwnx_hw->usbdev->rx_priv;
#endif
    if (rwnx_vif->ndev &&
        (rwnx_vif->wdev.iftype == NL80211_IFTYPE_STATION ||
         rwnx_vif->wdev.iftype == NL80211_IFTYPE_P2P_CLIENT)) {
        macaddr = rwnx_vif->ndev->dev_addr;
        list_for_each_entry_safe(reord_info, tmp,
                                 &rx_priv->stas_reord_list, list) {
            if (!memcmp(reord_info->mac_addr, macaddr, ETH_ALEN)) {
                reord_deinit_sta(rx_priv, reord_info);
                break;
            }
        }
    }
#endif

    if (rwnx_vif->sta.ap) {
        rwnx_txq_sta_deinit(rwnx_hw, rwnx_vif->sta.ap);
        rwnx_dbgfs_unregister_rc_stat(rwnx_hw, rwnx_vif->sta.ap);
        rwnx_vif->sta.ap->valid = false;
        rwnx_vif->sta.ap = NULL;
    }
    rwnx_txq_tdls_vif_deinit(rwnx_vif);
    if (!preserve_external_auth)
        rwnx_external_auth_disable(rwnx_vif);
    rwnx_chanctx_unlink(rwnx_vif);
}

static void rwnx_conn_cleanup_terminal(
    struct rwnx_hw *rwnx_hw, struct rwnx_vif *rwnx_vif,
    const struct rwnx_conn_txn_snapshot *snapshot)
{
    if (snapshot && snapshot->kind == RWNX_CONN_TXN_ROAM &&
        snapshot->phase == RWNX_CONN_TXN_OLD_LINK_GONE) {
        rwnx_external_auth_disable(rwnx_vif);
        return;
    }

    rwnx_conn_cleanup_link(rwnx_hw, rwnx_vif, false);
}

static inline int rwnx_rx_sm_disconnect_ind(struct rwnx_hw *rwnx_hw,
                                            struct rwnx_cmd *cmd,
                                            struct ipc_e2a_msg *msg)
{
    struct sm_disconnect_ind *ind = (struct sm_disconnect_ind *)msg->param;
    struct rwnx_conn_txn_snapshot txn;
    struct rwnx_vif *rwnx_vif;
    struct net_device *dev;
    int prev_state;
    bool transaction_active;
    u32 late_txn_id = 0;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    if (ind->vif_idx >= ARRAY_SIZE(rwnx_hw->vif_table)) {
        AICWFDBG(LOGERROR, "%s invalid vif idx:%u\r\n",
                 __func__, ind->vif_idx);
        return 0;
    }

    rwnx_vif = rwnx_hw->vif_table[ind->vif_idx];
    if (!rwnx_vif || !rwnx_vif->ndev) {
        AICWFDBG(LOGERROR, "%s rwnx_vif is null\r\n", __func__);
        return 0;
    }

    dev = rwnx_vif->ndev;
    prev_state = atomic_read(&rwnx_vif->drv_conn_state);

    if (rwnx_conn_track_take_late_disconnect(rwnx_vif,
                                              ind->reason_code,
                                              &late_txn_id)) {
        rwnx_conn_guard_note(RWNX_GUARD_TRANSIENT_DISC);
        AICWFDBG(LOGINFO,
                 "conn_disconnect_ind txn:%u vif:%u reason:%u decision:ignore_late_old state:%d carrier:%d\r\n",
                 late_txn_id, ind->vif_idx, ind->reason_code,
                 prev_state, netif_carrier_ok(dev));
        return 0;
    }

    transaction_active = rwnx_conn_track_snapshot(rwnx_vif, &txn);
    AICWFDBG(LOGINFO,
             "conn_disconnect_ind txn:%u vif:%u kind:%u phase:%u reason:%u ft_over_ds:%u state:%d up:%d had_ap:%d carrier:%d\r\n",
             transaction_active ? txn.id : 0, ind->vif_idx,
             transaction_active ? txn.kind : RWNX_CONN_TXN_NONE,
             transaction_active ? txn.phase : RWNX_CONN_TXN_IDLE,
             ind->reason_code, ind->ft_over_ds, prev_state,
             rwnx_vif->up, rwnx_vif->sta.ap != NULL,
             netif_carrier_ok(dev));

    if (transaction_active &&
        txn.kind == RWNX_CONN_TXN_ROAM &&
        ind->reason_code == 0) {
        if (!rwnx_conn_track_mark_old_gone(rwnx_vif, txn.id)) {
            rwnx_conn_guard_note(RWNX_GUARD_DUP_IND);
            AICWFDBG(LOGINFO,
                     "conn_disconnect_ind txn:%u vif:%u reason:0 decision:drop_duplicate_old phase:%u state:%d carrier:%d\r\n",
                     txn.id, ind->vif_idx, txn.phase, prev_state,
                     netif_carrier_ok(dev));
            return 0;
        }
        atomic_inc(&rwnx_hw->runtime_stats.roam_tx_pauses);
        netif_tx_stop_all_queues(dev);
        rwnx_conn_cleanup_link(rwnx_hw, rwnx_vif, true);
        atomic_set(&rwnx_vif->drv_conn_state,
                   RWNX_DRV_STATUS_CONNECTING);
        rwnx_conn_guard_note(RWNX_GUARD_TRANSIENT_DISC);
        AICWFDBG(LOGINFO,
                 "conn_disconnect_ind txn:%u vif:%u reason:0 decision:cleanup_old_preserve state:%d tx_paused:%d carrier:%d\r\n",
                 txn.id, ind->vif_idx,
                 atomic_read(&rwnx_vif->drv_conn_state),
                 rwnx_conn_tx_paused(rwnx_vif),
                 netif_carrier_ok(dev));
        return 0;
    }

    if (transaction_active &&
        txn.kind == RWNX_CONN_TXN_INITIAL &&
        ind->reason_code == 0 &&
        !rwnx_vif->sta.ap) {
        rwnx_conn_guard_note(RWNX_GUARD_TRANSIENT_DISC);
        AICWFDBG(LOGINFO,
                 "conn_disconnect_ind txn:%u vif:%u reason:0 decision:ignore_transient_initial state:%d carrier:%d\r\n",
                 txn.id, ind->vif_idx, prev_state,
                 netif_carrier_ok(dev));
        return 0;
    }

    if (transaction_active) {
        if (!rwnx_conn_track_finish(rwnx_vif, txn.id, false))
            return 0;

        dhcped = 0;
        atomic_set(&rwnx_vif->drv_conn_state,
                   RWNX_DRV_STATUS_DISCONNECTED);
        netif_tx_stop_all_queues(dev);
        netif_carrier_off(dev);
        rwnx_conn_tx_abort_pause(rwnx_vif);
        rwnx_conn_cleanup_terminal(rwnx_hw, rwnx_vif, &txn);
        if (rwnx_vif->wdev.iftype == NL80211_IFTYPE_P2P_CLIENT)
            rwnx_hw->is_p2p_connected = 0;
#ifdef CONFIG_BR_SUPPORT
        nat25_db_cleanup(rwnx_vif);
#endif

        if (rwnx_vif->up) {
            if (txn.kind == RWNX_CONN_TXN_ROAM)
                cfg80211_disconnected(dev, ind->reason_code,
                                      NULL, 0,
                                      ind->reason_code <= 1,
                                      GFP_ATOMIC);
            else
                rwnx_conn_report_timeout(rwnx_vif, &txn,
                                         GFP_ATOMIC);
        }

        AICWFDBG(LOGINFO,
                 "conn_txn_end id:%u vif:%u kind:%u report:%s status:disconnect_ind reason:%u duration_ms:%u state:%d carrier:%d\r\n",
                 txn.id, ind->vif_idx, txn.kind,
                 txn.kind == RWNX_CONN_TXN_ROAM ?
                 "disconnected" : "connect_timeout",
                 ind->reason_code,
                 jiffies_to_msecs(jiffies - txn.start_jiffies),
                 atomic_read(&rwnx_vif->drv_conn_state),
                 netif_carrier_ok(dev));
        return 0;
    }

    if (prev_state == RWNX_DRV_STATUS_CONNECTING &&
        ind->reason_code == 0 && !rwnx_vif->sta.ap) {
        rwnx_conn_guard_note(RWNX_GUARD_TRANSIENT_DISC);
        AICWFDBG(LOGINFO,
                 "conn_disconnect_ind txn:0 vif:%u reason:0 decision:drop_unowned_transient state:%d\r\n",
                 ind->vif_idx, prev_state);
        return 0;
    }

    dhcped = 0;
#ifdef CONFIG_BR_SUPPORT
    nat25_db_cleanup(rwnx_vif);
#endif
    if (rwnx_vif->wdev.iftype == NL80211_IFTYPE_P2P_CLIENT)
        rwnx_hw->is_p2p_connected = 0;

    if (rwnx_vif->up) {
        if (!ind->ft_over_ds)
            cfg80211_disconnected(dev, ind->reason_code, NULL, 0,
                                  ind->reason_code <= 1, GFP_ATOMIC);
        netif_tx_stop_all_queues(dev);
        netif_carrier_off(dev);
    }
    rwnx_conn_tx_abort_pause(rwnx_vif);
    rwnx_conn_cleanup_link(rwnx_hw, rwnx_vif, false);
    atomic_set(&rwnx_vif->drv_conn_state,
               RWNX_DRV_STATUS_DISCONNECTED);
    AICWFDBG(LOGINFO,
             "conn_disconnect_ind txn:0 vif:%u decision:full_disconnect reason:%u state:%d carrier:%d\r\n",
             ind->vif_idx, ind->reason_code,
             atomic_read(&rwnx_vif->drv_conn_state),
             netif_carrier_ok(dev));
    return 0;
}
static inline int rwnx_rx_sm_external_auth_required_ind(struct rwnx_hw *rwnx_hw,
                                                        struct rwnx_cmd *cmd,
                                                        struct ipc_e2a_msg *msg)
{
    struct sm_external_auth_required_ind *ind =
        (struct sm_external_auth_required_ind *)msg->param;
    struct rwnx_vif *rwnx_vif;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 17, 0) || defined(CONFIG_WPA3_FOR_OLD_KERNEL)
    struct net_device *dev;
    struct cfg80211_external_auth_params params;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    if (ind->vif_idx >= ARRAY_SIZE(rwnx_hw->vif_table) ||
        !(rwnx_vif = rwnx_hw->vif_table[ind->vif_idx]) ||
        !(dev = rwnx_vif->ndev) || !rwnx_vif->up ||
        RWNX_VIF_TYPE(rwnx_vif) != NL80211_IFTYPE_STATION) {
        wiphy_err(rwnx_hw->wiphy,
                  "Invalid external auth indication on vif %u\n",
                  ind->vif_idx);
        return 0;
    }

    memset(&params, 0, sizeof(params));
    params.action = NL80211_EXTERNAL_AUTH_START;
    memcpy(params.bssid, ind->bssid.array, ETH_ALEN);
    params.ssid.ssid_len = min_t(size_t, ind->ssid.length,
                                 sizeof(params.ssid.ssid));
    memcpy(params.ssid.ssid, ind->ssid.array,
           params.ssid.ssid_len);
    params.key_mgmt_suite = ind->akm;

    if (cfg80211_external_auth_request(dev, &params, GFP_ATOMIC)) {
        wiphy_err(rwnx_hw->wiphy, "Failed to start external auth on vif %d",
                  ind->vif_idx);
        rwnx_send_sm_external_auth_required_rsp(rwnx_hw, rwnx_vif,
                                                WLAN_STATUS_UNSPECIFIED_FAILURE);
        return 0;
    }

    rwnx_external_auth_enable(rwnx_vif);
#else
    if (ind->vif_idx >= ARRAY_SIZE(rwnx_hw->vif_table) ||
        !(rwnx_vif = rwnx_hw->vif_table[ind->vif_idx]))
        return 0;
    rwnx_send_sm_external_auth_required_rsp(rwnx_hw, rwnx_vif,
                                            WLAN_STATUS_UNSPECIFIED_FAILURE);
#endif
    return 0;
}


static inline int rwnx_rx_mesh_path_create_cfm(struct rwnx_hw *rwnx_hw,
                                               struct rwnx_cmd *cmd,
                                               struct ipc_e2a_msg *msg)
{
    struct mesh_path_create_cfm *cfm = (struct mesh_path_create_cfm *)msg->param;
    struct rwnx_vif *rwnx_vif;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    if (cfm->vif_idx >= ARRAY_SIZE(rwnx_hw->vif_table))
        return 1;
    rwnx_vif = rwnx_hw->vif_table[cfm->vif_idx];

    /* Check we well have a Mesh Point Interface */
    if (rwnx_vif && (RWNX_VIF_TYPE(rwnx_vif) == NL80211_IFTYPE_MESH_POINT)) {
        rwnx_vif->ap.create_path = false;
    }

    return 0;
}

static inline int rwnx_rx_mesh_peer_update_ind(struct rwnx_hw *rwnx_hw,
                                               struct rwnx_cmd *cmd,
                                               struct ipc_e2a_msg *msg)
{
    struct mesh_peer_update_ind *ind = (struct mesh_peer_update_ind *)msg->param;
    struct rwnx_vif *rwnx_vif;
    struct rwnx_sta *rwnx_sta;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    if (ind->vif_idx >= ARRAY_SIZE(rwnx_hw->vif_table) ||
        ind->sta_idx >= ARRAY_SIZE(rwnx_hw->sta_table))
        return 1;

    rwnx_vif = rwnx_hw->vif_table[ind->vif_idx];
    rwnx_sta = &rwnx_hw->sta_table[ind->sta_idx];
    if (!rwnx_vif ||
        RWNX_VIF_TYPE(rwnx_vif) != NL80211_IFTYPE_MESH_POINT)
        return 1;

    /* Check we well have a Mesh Point Interface */
    if (!rwnx_vif->user_mpm)
    {
        /* Check if peer link has been established or lost */
        if (ind->estab) {
            if (!rwnx_sta->valid) {
                u8 txq_status;

                rwnx_sta->valid = true;
                rwnx_sta->sta_idx = ind->sta_idx;
                rwnx_sta->ch_idx = rwnx_vif->ch_index;
                rwnx_sta->vif_idx = ind->vif_idx;
                rwnx_sta->vlan_idx = rwnx_sta->vif_idx;
                rwnx_sta->ps.active = false;
                rwnx_sta->qos = true;
                rwnx_sta->aid = ind->sta_idx + 1;
                //rwnx_sta->acm = ind->acm;
                memcpy(rwnx_sta->mac_addr, ind->peer_addr.array, ETH_ALEN);

                rwnx_chanctx_link(rwnx_vif, rwnx_sta->ch_idx, NULL);

                /* Add the station in the list of VIF's stations */
                INIT_LIST_HEAD(&rwnx_sta->list);
                list_add_tail(&rwnx_sta->list, &rwnx_vif->ap.sta_list);

                /* Initialize the TX queues */
                if (rwnx_sta->ch_idx == rwnx_hw->cur_chanctx) {
                    txq_status = 0;
                } else {
                    txq_status = RWNX_TXQ_STOP_CHAN;
                }

                rwnx_txq_sta_init(rwnx_hw, rwnx_sta, txq_status);
#ifdef CONFIG_DEBUG_FS
                rwnx_dbgfs_register_rc_stat(rwnx_hw, rwnx_sta);
#endif
#ifdef CONFIG_RWNX_BFMER
                // TODO: update indication to contains vht capabilties
                if (rwnx_hw->mod_params->bfmer)
                    rwnx_send_bfmer_enable(rwnx_hw, rwnx_sta, NULL);

                rwnx_mu_group_sta_init(rwnx_sta, NULL);
#endif /* CONFIG_RWNX_BFMER */

            } else {
                WARN_ON(0);
            }
        } else {
            if (rwnx_sta->valid) {
                rwnx_sta->ps.active = false;
                rwnx_sta->valid = false;

                /* Remove the station from the list of VIF's station */
                list_del_init(&rwnx_sta->list);

                rwnx_txq_sta_deinit(rwnx_hw, rwnx_sta);
#ifdef CONFIG_DEBUG_FS
                rwnx_dbgfs_unregister_rc_stat(rwnx_hw, rwnx_sta);
#endif
            } else {
                WARN_ON(0);
            }
        }
    } else {
        if (!ind->estab && rwnx_sta->valid) {
            /* There is no way to inform upper layer for lost of peer, still
               clean everything in the driver */
            rwnx_sta->ps.active = false;
            rwnx_sta->valid = false;

            /* Remove the station from the list of VIF's station */
            list_del_init(&rwnx_sta->list);

            rwnx_txq_sta_deinit(rwnx_hw, rwnx_sta);
#ifdef CONFIG_DEBUG_FS
            rwnx_dbgfs_unregister_rc_stat(rwnx_hw, rwnx_sta);
#endif
        } else {
            WARN_ON(0);
        }
    }

    return 0;
}

static inline int rwnx_rx_mesh_path_update_ind(struct rwnx_hw *rwnx_hw,
                                               struct rwnx_cmd *cmd,
                                               struct ipc_e2a_msg *msg)
{
    struct mesh_path_update_ind *ind = (struct mesh_path_update_ind *)msg->param;
    struct rwnx_vif *rwnx_vif;
    struct rwnx_mesh_path *mesh_path;
    bool found = false;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    if (ind->vif_idx >= ARRAY_SIZE(rwnx_hw->vif_table))
        return 1;

    rwnx_vif = rwnx_hw->vif_table[ind->vif_idx];
    if (!rwnx_vif || (RWNX_VIF_TYPE(rwnx_vif) != NL80211_IFTYPE_MESH_POINT))
        return 0;

    if (!ind->delete &&
        ind->nhop_sta_idx >= ARRAY_SIZE(rwnx_hw->sta_table))
        return 1;

    /* Look for path with provided target address */
    list_for_each_entry(mesh_path, &rwnx_vif->ap.mpath_list, list) {
        if (mesh_path->path_idx == ind->path_idx) {
            found = true;
            break;
        }
    }

    /* Check if element has been deleted */
    if (ind->delete) {
        if (found) {
#ifdef CREATE_TRACE_POINTS
            trace_mesh_delete_path(mesh_path);
#endif
            /* Remove element from list */
            list_del_init(&mesh_path->list);
            /* Free the element */
            kfree(mesh_path);
        }
    }
    else {
        if (found) {
            // Update the Next Hop STA
            mesh_path->p_nhop_sta = &rwnx_hw->sta_table[ind->nhop_sta_idx];
#ifdef CREATE_TRACE_POINTS
            trace_mesh_update_path(mesh_path);
#endif
        } else {
            // Allocate a Mesh Path structure
            mesh_path = (struct rwnx_mesh_path *)kmalloc(sizeof(struct rwnx_mesh_path), GFP_ATOMIC);

            if (mesh_path) {
                INIT_LIST_HEAD(&mesh_path->list);

                mesh_path->path_idx = ind->path_idx;
                mesh_path->p_nhop_sta = &rwnx_hw->sta_table[ind->nhop_sta_idx];
                memcpy(&mesh_path->tgt_mac_addr, &ind->tgt_mac_addr, MAC_ADDR_LEN);

                // Insert the path in the list of path
                list_add_tail(&mesh_path->list, &rwnx_vif->ap.mpath_list);
#ifdef CREATE_TRACE_POINTS
                trace_mesh_create_path(mesh_path);
#endif
            }
        }
    }

    return 0;
}

static inline int rwnx_rx_mesh_proxy_update_ind(struct rwnx_hw *rwnx_hw,
                                               struct rwnx_cmd *cmd,
                                               struct ipc_e2a_msg *msg)
{
    struct mesh_proxy_update_ind *ind = (struct mesh_proxy_update_ind *)msg->param;
    struct rwnx_vif *rwnx_vif;
    struct rwnx_mesh_proxy *mesh_proxy;
    bool found = false;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    if (ind->vif_idx >= ARRAY_SIZE(rwnx_hw->vif_table))
        return 1;

    rwnx_vif = rwnx_hw->vif_table[ind->vif_idx];
    if (!rwnx_vif || (RWNX_VIF_TYPE(rwnx_vif) != NL80211_IFTYPE_MESH_POINT))
        return 0;

    /* Look for path with provided external STA address */
    list_for_each_entry(mesh_proxy, &rwnx_vif->ap.proxy_list, list) {
        if (!memcmp(&ind->ext_sta_addr, &mesh_proxy->ext_sta_addr, ETH_ALEN)) {
            found = true;
            break;
        }
    }

    if (ind->delete && found) {
        /* Delete mesh path */
        list_del_init(&mesh_proxy->list);
        kfree(mesh_proxy);
    } else if (!ind->delete && !found) {
        /* Allocate a Mesh Path structure */
        mesh_proxy = (struct rwnx_mesh_proxy *)kmalloc(sizeof(*mesh_proxy),
                                                       GFP_ATOMIC);

        if (mesh_proxy) {
            INIT_LIST_HEAD(&mesh_proxy->list);

            memcpy(&mesh_proxy->ext_sta_addr, &ind->ext_sta_addr, MAC_ADDR_LEN);
            mesh_proxy->local = ind->local;

            if (!ind->local) {
                memcpy(&mesh_proxy->proxy_addr, &ind->proxy_mac_addr, MAC_ADDR_LEN);
            }

            /* Insert the path in the list of path */
            list_add_tail(&mesh_proxy->list, &rwnx_vif->ap.proxy_list);
        }
    }

    return 0;
}
#endif /* CONFIG_RWNX_FULLMAC */

/***************************************************************************
 * Messages from APM task
 **************************************************************************/


/***************************************************************************
 * Messages from DEBUG task
 **************************************************************************/
static inline int rwnx_rx_dbg_error_ind(struct rwnx_hw *rwnx_hw,
                                        struct rwnx_cmd *cmd,
                                        struct ipc_e2a_msg *msg)
{
    RWNX_DBG(RWNX_FN_ENTRY_STR);

    return 0;
}

#ifdef CONFIG_RWNX_FULLMAC

static msg_cb_fct mm_hdlrs[MSG_I(MM_MAX)] = {
    [MSG_I(MM_CHANNEL_SWITCH_IND)]     = rwnx_rx_chan_switch_ind,
    [MSG_I(MM_CHANNEL_PRE_SWITCH_IND)] = rwnx_rx_chan_pre_switch_ind,
    [MSG_I(MM_REMAIN_ON_CHANNEL_EXP_IND)] = rwnx_rx_remain_on_channel_exp_ind,
    [MSG_I(MM_PS_CHANGE_IND)]          = rwnx_rx_ps_change_ind,
    [MSG_I(MM_TRAFFIC_REQ_IND)]        = rwnx_rx_traffic_req_ind,
    [MSG_I(MM_P2P_VIF_PS_CHANGE_IND)]  = rwnx_rx_p2p_vif_ps_change_ind,
    [MSG_I(MM_CSA_COUNTER_IND)]        = rwnx_rx_csa_counter_ind,
    [MSG_I(MM_CSA_FINISH_IND)]         = rwnx_rx_csa_finish_ind,
    [MSG_I(MM_CSA_TRAFFIC_IND)]        = rwnx_rx_csa_traffic_ind,
    [MSG_I(MM_CHANNEL_SURVEY_IND)]     = rwnx_rx_channel_survey_ind,
    [MSG_I(MM_P2P_NOA_UPD_IND)]        = rwnx_rx_p2p_noa_upd_ind,
    [MSG_I(MM_RSSI_STATUS_IND)]        = rwnx_rx_rssi_status_ind,
    [MSG_I(MM_PKTLOSS_IND)]            = rwnx_rx_pktloss_notify_ind,
    [MSG_I(MM_APM_STALOSS_IND)]        = rwnx_apm_staloss_ind,
};

static msg_cb_fct scan_hdlrs[MSG_I(SCANU_MAX)] = {
    [MSG_I(SCANU_START_CFM)]           = rwnx_rx_scanu_start_cfm,
    [MSG_I(SCANU_RESULT_IND)]          = rwnx_rx_scanu_result_ind,
};

static msg_cb_fct me_hdlrs[MSG_I(ME_MAX)] = {
    [MSG_I(ME_TKIP_MIC_FAILURE_IND)] = rwnx_rx_me_tkip_mic_failure_ind,
    [MSG_I(ME_TX_CREDITS_UPDATE_IND)] = rwnx_rx_me_tx_credits_update_ind,
};

static msg_cb_fct sm_hdlrs[MSG_I(SM_MAX)] = {
    [MSG_I(SM_CONNECT_IND)]    = rwnx_rx_sm_connect_ind,
    [MSG_I(SM_DISCONNECT_IND)] = rwnx_rx_sm_disconnect_ind,
    [MSG_I(SM_EXTERNAL_AUTH_REQUIRED_IND)] = rwnx_rx_sm_external_auth_required_ind,
};

static msg_cb_fct apm_hdlrs[MSG_I(APM_MAX)] = {
};

static msg_cb_fct mesh_hdlrs[MSG_I(MESH_MAX)] = {
    [MSG_I(MESH_PATH_CREATE_CFM)]  = rwnx_rx_mesh_path_create_cfm,
    [MSG_I(MESH_PEER_UPDATE_IND)]  = rwnx_rx_mesh_peer_update_ind,
    [MSG_I(MESH_PATH_UPDATE_IND)]  = rwnx_rx_mesh_path_update_ind,
    [MSG_I(MESH_PROXY_UPDATE_IND)] = rwnx_rx_mesh_proxy_update_ind,
};

#endif /* CONFIG_RWNX_FULLMAC */

static msg_cb_fct dbg_hdlrs[MSG_I(DBG_MAX)] = {
    [MSG_I(DBG_ERROR_IND)]                = rwnx_rx_dbg_error_ind,
};

static msg_cb_fct tdls_hdlrs[MSG_I(TDLS_MAX)] = {
    [MSG_I(TDLS_CHAN_SWITCH_CFM)] = rwnx_rx_tdls_chan_switch_cfm,
    [MSG_I(TDLS_CHAN_SWITCH_IND)] = rwnx_rx_tdls_chan_switch_ind,
    [MSG_I(TDLS_CHAN_SWITCH_BASE_IND)] = rwnx_rx_tdls_chan_switch_base_ind,
    [MSG_I(TDLS_PEER_PS_IND)] = rwnx_rx_tdls_peer_ps_ind,
};

static msg_cb_fct rwnx_rx_msg_handler_get(u16 id, bool *valid_id)
{
    unsigned int index = MSG_I(id);

    *valid_id = true;
    switch (MSG_T(id)) {
    case TASK_MM:
        if (index < ARRAY_SIZE(mm_hdlrs))
            return mm_hdlrs[index];
        break;
    case TASK_DBG:
        if (index < ARRAY_SIZE(dbg_hdlrs))
            return dbg_hdlrs[index];
        break;
    case TASK_TDLS:
        if (index < ARRAY_SIZE(tdls_hdlrs))
            return tdls_hdlrs[index];
        break;
#ifdef CONFIG_RWNX_FULLMAC
    case TASK_SCANU:
        if (index < ARRAY_SIZE(scan_hdlrs))
            return scan_hdlrs[index];
        break;
    case TASK_ME:
        if (index < ARRAY_SIZE(me_hdlrs))
            return me_hdlrs[index];
        break;
    case TASK_SM:
        if (index < ARRAY_SIZE(sm_hdlrs))
            return sm_hdlrs[index];
        break;
    case TASK_APM:
        if (index < ARRAY_SIZE(apm_hdlrs))
            return apm_hdlrs[index];
        break;
    case TASK_MESH:
        if (index < ARRAY_SIZE(mesh_hdlrs))
            return mesh_hdlrs[index];
        break;
#endif
    default:
        break;
    }

    *valid_id = false;
    return NULL;
}

static size_t rwnx_rx_msg_min_param_len(u16 id)
{
    switch (id) {
    case MM_CHANNEL_SWITCH_IND:
        return sizeof(struct mm_channel_switch_ind);
    case MM_CHANNEL_PRE_SWITCH_IND:
        return sizeof(struct mm_channel_pre_switch_ind);
    case MM_PS_CHANGE_IND:
        return sizeof(struct mm_ps_change_ind);
    case MM_TRAFFIC_REQ_IND:
        return sizeof(struct mm_traffic_req_ind);
    case MM_P2P_VIF_PS_CHANGE_IND:
        return sizeof(struct mm_p2p_vif_ps_change_ind);
    case MM_CSA_COUNTER_IND:
        return sizeof(struct mm_csa_counter_ind);
    case MM_CSA_FINISH_IND:
        return sizeof(struct mm_csa_finish_ind);
    case MM_CSA_TRAFFIC_IND:
        return sizeof(struct mm_csa_traffic_ind);
    case MM_CHANNEL_SURVEY_IND:
        return sizeof(struct mm_channel_survey_ind);
    case MM_RSSI_STATUS_IND:
        return sizeof(struct mm_rssi_status_ind);
    case MM_PKTLOSS_IND:
        return sizeof(struct mm_pktloss_ind);
    case MM_APM_STALOSS_IND:
        return sizeof(struct mm_apm_staloss_ind);
    case SCANU_RESULT_IND:
        return offsetof(struct scanu_result_ind, payload);
    case ME_TKIP_MIC_FAILURE_IND:
        return sizeof(struct me_tkip_mic_failure_ind);
    case ME_TX_CREDITS_UPDATE_IND:
        return sizeof(struct me_tx_credits_update_ind);
    case SM_CONNECT_IND:
        return sizeof(struct sm_connect_ind);
    case SM_DISCONNECT_IND:
        return sizeof(struct sm_disconnect_ind);
    case SM_EXTERNAL_AUTH_REQUIRED_IND:
        return sizeof(struct sm_external_auth_required_ind);
    case MESH_PATH_CREATE_CFM:
        return sizeof(struct mesh_path_create_cfm);
    case MESH_PEER_UPDATE_IND:
        return sizeof(struct mesh_peer_update_ind);
    case MESH_PATH_UPDATE_IND:
        return sizeof(struct mesh_path_update_ind);
    case MESH_PROXY_UPDATE_IND:
        return sizeof(struct mesh_proxy_update_ind);
    case TDLS_CHAN_SWITCH_BASE_IND:
        return sizeof(struct tdls_chan_switch_base_ind);
    case TDLS_PEER_PS_IND:
        return sizeof(struct tdls_peer_ps_ind);
    default:
        return 0;
    }
}

/**
 *
 */
void rwnx_rx_handle_msg(struct rwnx_hw *rwnx_hw, struct ipc_e2a_msg *msg,
                        size_t msg_len)
{
    const size_t header_len = offsetof(struct ipc_e2a_msg, param);
    msg_cb_fct handler;
    size_t min_param_len;
    bool valid_id;

	//printk("%s(%d) MSG_T(msg->id):%d MSG_I(msg->id):%d cmd:%s\r\n", __func__,
	//	msg->id,
	//	MSG_T(msg->id),
	//	MSG_I(msg->id),
	//	rwnx_id2str[MSG_T(msg->id)][MSG_I(msg->id)]);

    if (!rwnx_hw || !msg || msg_len < header_len) {
        if (rwnx_hw)
            atomic_inc(&rwnx_hw->runtime_stats.fw_msg_invalid);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "Dropped short firmware message len:%zu\n",
                             msg_len);
        return;
    }

    handler = rwnx_rx_msg_handler_get(msg->id, &valid_id);
    min_param_len = rwnx_rx_msg_min_param_len(msg->id);
    if (!valid_id || msg->param_len > sizeof(msg->param) ||
        msg->param_len > msg_len - header_len ||
        msg->param_len < min_param_len) {
        atomic_inc(&rwnx_hw->runtime_stats.fw_msg_invalid);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "Dropped invalid firmware message id:0x%x task:%u index:%u param:%u available:%zu min:%zu\n",
                             msg->id, MSG_T(msg->id), MSG_I(msg->id),
                             msg->param_len, msg_len - header_len,
                             min_param_len);
        return;
    }

    if (msg->id == SCANU_RESULT_IND) {
        struct scanu_result_ind *ind =
            (struct scanu_result_ind *)msg->param;
        size_t payload_len = msg->param_len -
                             offsetof(struct scanu_result_ind, payload);

        if (ind->length > payload_len) {
            atomic_inc(&rwnx_hw->runtime_stats.fw_msg_invalid);
            AICWFDBG_RATELIMITED(LOGERROR,
                                 "Dropped truncated scan result frame:%u available:%zu\n",
                                 ind->length, payload_len);
            return;
        }
    }

    rwnx_hw->cmd_mgr->msgind(rwnx_hw->cmd_mgr, msg, handler);
}

void rwnx_rx_handle_print(struct rwnx_hw *rwnx_hw, u8 *msg, u32 len)
{
    u8 *data_end = NULL;
    (void)data_end;

    if (!msg || !len)
        return;

    if (!rwnx_hw || !rwnx_hw->fwlog_en) {
        pr_err("FWLOG-OVFL: %.*s", (int)len, msg);
        return;
    }

    printk("FWLOG: %.*s", (int)len, msg);

#ifdef CONFIG_RWNX_DEBUGFS
    data_end = rwnx_hw->debugfs.fw_log.buf.dataend;

    if (!rwnx_hw->debugfs.fw_log.buf.data)
        return ;

    if (len > FW_LOG_SIZE) {
        atomic_inc(&rwnx_hw->runtime_stats.fw_log_drops);
        msg += len - FW_LOG_SIZE;
        len = FW_LOG_SIZE;
    }

    //printk("end=%lx, len=%d\n", (unsigned long)rwnx_hw->debugfs.fw_log.buf.end, len);

    spin_lock_bh(&rwnx_hw->debugfs.fw_log.lock);

    if (rwnx_hw->debugfs.fw_log.buf.end + len > data_end) {
        int rem = data_end - rwnx_hw->debugfs.fw_log.buf.end;
        memcpy(rwnx_hw->debugfs.fw_log.buf.end, msg, rem);
        memcpy(rwnx_hw->debugfs.fw_log.buf.data, &msg[rem], len - rem);
        rwnx_hw->debugfs.fw_log.buf.end = rwnx_hw->debugfs.fw_log.buf.data + (len - rem);
    } else {
        memcpy(rwnx_hw->debugfs.fw_log.buf.end, msg, len);
        rwnx_hw->debugfs.fw_log.buf.end += len;
    }

    rwnx_hw->debugfs.fw_log.buf.size += len;
    if (rwnx_hw->debugfs.fw_log.buf.size > FW_LOG_SIZE)
        rwnx_hw->debugfs.fw_log.buf.size = FW_LOG_SIZE;

    spin_unlock_bh(&rwnx_hw->debugfs.fw_log.lock);
#endif
}

/**
 ******************************************************************************
 *
 * rwnx_cmds.c
 *
 * Handles queueing (push to IPC, ack/cfm from IPC) of commands issued to
 * LMAC FW
 *
 * Copyright (C) RivieraWaves 2014-2019
 *
 ******************************************************************************
 */

//#define CREATE_TRACE_POINTS
#include <linux/list.h>

#include "rwnx_cmds.h"
#include "rwnx_defs.h"
#include "rwnx_strs.h"
#include "rwnx_events.h"
#include "rwnx_msg_tx.h"
#include "aicwf_txrxif.h"
#ifdef AICWF_SDIO_SUPPORT
#include "aicwf_sdio.h"
#else
#include "aicwf_usb.h"
#endif
/**
 *
 */
extern int aicwf_sdio_writeb(struct aic_sdio_dev *sdiodev, uint regaddr, u8 val);

static void cmd_complete(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_cmd *cmd);

static struct rwnx_hw *cmd_mgr_rwnx_hw(struct rwnx_cmd_mgr *cmd_mgr)
{
#ifdef AICWF_SDIO_SUPPORT
    struct aic_sdio_dev *sdiodev =
        container_of(cmd_mgr, struct aic_sdio_dev, cmd_mgr);

    return sdiodev->rwnx_hw;
#elif defined(AICWF_USB_SUPPORT)
    struct aic_usb_dev *usbdev =
        container_of(cmd_mgr, struct aic_usb_dev, cmd_mgr);

    return usbdev->rwnx_hw;
#else
    return NULL;
#endif
}

static int cmd_mgr_tx(struct rwnx_cmd_mgr *cmd_mgr, struct lmac_msg *msg)
{
#ifdef AICWF_SDIO_SUPPORT
    struct aic_sdio_dev *sdiodev =
        container_of(cmd_mgr, struct aic_sdio_dev, cmd_mgr);

    return aicwf_set_cmd_tx(sdiodev, msg,
                            sizeof(*msg) + msg->param_len);
#elif defined(AICWF_USB_SUPPORT)
    struct aic_usb_dev *usbdev =
        container_of(cmd_mgr, struct aic_usb_dev, cmd_mgr);

    return aicwf_set_cmd_tx(usbdev, msg,
                            sizeof(*msg) + msg->param_len);
#else
    return -ENODEV;
#endif
}

static void cmd_mgr_record_tx_failure(struct rwnx_cmd_mgr *cmd_mgr)
{
    struct rwnx_hw *rwnx_hw = cmd_mgr_rwnx_hw(cmd_mgr);

    if (rwnx_hw)
        atomic_inc(&rwnx_hw->runtime_stats.cmd_tx_failures);
}

static void cmd_mgr_finish_cmd(struct rwnx_cmd_mgr *cmd_mgr,
                               struct rwnx_cmd *cmd, int result)
{
    spin_lock_bh(&cmd_mgr->lock);
    if (!(cmd->flags & RWNX_CMD_FLAG_DONE)) {
        cmd->result = result;
        cmd->flags &= ~(RWNX_CMD_FLAG_WAIT_PUSH |
                        RWNX_CMD_FLAG_WAIT_ACK |
                        RWNX_CMD_FLAG_WAIT_CFM);
        cmd_complete(cmd_mgr, cmd);
    }
    spin_unlock_bh(&cmd_mgr->lock);
}

static void cmd_dump(const struct rwnx_cmd *cmd)
{
    printk(KERN_CRIT "tkn[%d]  flags:%04x  result:%3d  cmd:%4d-%-24s - reqcfm(%4d-%-s)\n",
           cmd->tkn, cmd->flags, cmd->result, cmd->id, RWNX_ID2STR(cmd->id),
           cmd->reqid, cmd->reqid != (lmac_msg_id_t)-1 ? RWNX_ID2STR(cmd->reqid) : "none");
}

/**
 *
 */
static void cmd_complete(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_cmd *cmd)
{
    //RWNX_DBG(RWNX_FN_ENTRY_STR);
    lockdep_assert_held(&cmd_mgr->lock);

    list_del_init(&cmd->list);
    if (WARN_ON_ONCE(!cmd_mgr->queue_sz))
        cmd_mgr->queue_sz = 0;
    else
        cmd_mgr->queue_sz--;

    cmd->flags |= RWNX_CMD_FLAG_DONE;
    if (RWNX_CMD_WAIT_COMPLETE(cmd->flags)) {
        if (cmd->result == -EINTR)
            cmd->result = 0;
        complete(&cmd->complete);
    }
}

int cmd_mgr_queue_force_defer(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_cmd *cmd)
{
    if (!cmd || !cmd->a2e_msg)
        return -EINVAL;

    RWNX_DBG(RWNX_FN_ENTRY_STR);
#ifdef CREATE_TRACE_POINTS
    trace_msg_send(cmd->id);
#endif
    spin_lock_bh(&cmd_mgr->lock);

    if (cmd_mgr->state == RWNX_CMD_MGR_STATE_CRASHED) {
        printk(KERN_CRIT"cmd queue crashed\n");
        cmd->result = -EPIPE;
        spin_unlock_bh(&cmd_mgr->lock);
        return -EPIPE;
    }

    #ifndef CONFIG_RWNX_FHOST
    if (!list_empty(&cmd_mgr->cmds)) {
        if (cmd_mgr->queue_sz >= cmd_mgr->max_queue_sz) {
            printk(KERN_CRIT"Too many cmds (%d) already queued\n",
                   cmd_mgr->max_queue_sz);
            cmd->result = -ENOMEM;
            spin_unlock_bh(&cmd_mgr->lock);
            return -ENOMEM;
        }
    }
    #endif

    cmd->flags |= RWNX_CMD_FLAG_WAIT_PUSH | RWNX_CMD_FLAG_WORKER_OWNS;
    if (cmd->flags & RWNX_CMD_FLAG_REQ_CFM)
        cmd->flags |= RWNX_CMD_FLAG_WAIT_CFM;

    cmd->tkn    = cmd_mgr->next_tkn++;
    cmd->result = -EINTR;

    init_completion(&cmd->complete);
    init_completion(&cmd->push_complete);

    list_add_tail(&cmd->list, &cmd_mgr->cmds);
    cmd_mgr->queue_sz++;
    spin_unlock_bh(&cmd_mgr->lock);

    WAKE_CMD_WORK(cmd_mgr);
    return 0;
}

void rwnx_msg_free_(struct lmac_msg *msg);


static int cmd_mgr_queue(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_cmd *cmd)
{
    int ret = 0;
    long wait_ret;
#ifdef AICWF_SDIO_SUPPORT
    struct aic_sdio_dev *sdiodev = container_of(cmd_mgr, struct aic_sdio_dev, cmd_mgr);
#endif
    bool defer_push = false;
    bool expects_response;

    if (!cmd || !cmd->a2e_msg)
        return -EINVAL;
    expects_response = cmd->flags & RWNX_CMD_FLAG_REQ_CFM;

    //RWNX_DBG(RWNX_FN_ENTRY_STR);
#ifdef CREATE_TRACE_POINTS
    trace_msg_send(cmd->id);
#endif
    spin_lock_bh(&cmd_mgr->lock);

    if (cmd_mgr->state == RWNX_CMD_MGR_STATE_CRASHED) {
        printk(KERN_CRIT"cmd queue crashed\n");
        cmd->result = -EPIPE;
        spin_unlock_bh(&cmd_mgr->lock);
        return -EPIPE;
    }

    #ifndef CONFIG_RWNX_FHOST
    if (!list_empty(&cmd_mgr->cmds)) {
        struct rwnx_cmd *last;

        if (cmd_mgr->queue_sz >= cmd_mgr->max_queue_sz) {
            printk(KERN_CRIT"Too many cmds (%d) already queued\n",
                   cmd_mgr->max_queue_sz);
            cmd->result = -ENOMEM;
            spin_unlock_bh(&cmd_mgr->lock);
            return -ENOMEM;
        }
        last = list_entry(cmd_mgr->cmds.prev, struct rwnx_cmd, list);
        if (last->flags & (RWNX_CMD_FLAG_WAIT_ACK | RWNX_CMD_FLAG_WAIT_PUSH | RWNX_CMD_FLAG_WAIT_CFM)) {
#if 0 // queue even NONBLOCK command.
            if (cmd->flags & RWNX_CMD_FLAG_NONBLOCK) {
                printk(KERN_CRIT"cmd queue busy\n");
                cmd->result = -EBUSY;
                spin_unlock_bh(&cmd_mgr->lock);
                return -EBUSY;
            }
#endif
            cmd->flags |= RWNX_CMD_FLAG_WAIT_PUSH;
            defer_push = true;
        }
    }
    #endif

#if 0
    cmd->flags |= RWNX_CMD_FLAG_WAIT_ACK;
#endif
    if (cmd->flags & RWNX_CMD_FLAG_REQ_CFM)
        cmd->flags |= RWNX_CMD_FLAG_WAIT_CFM;

    cmd->tkn    = cmd_mgr->next_tkn++;
    cmd->result = -EINTR;

    init_completion(&cmd->complete);
    init_completion(&cmd->push_complete);

    list_add_tail(&cmd->list, &cmd_mgr->cmds);
    cmd_mgr->queue_sz++;

	if(cmd->a2e_msg->id == ME_TRAFFIC_IND_REQ
	#ifdef AICWF_ARP_OFFLOAD
		|| cmd->a2e_msg->id == MM_SET_ARPOFFLOAD_REQ
	#endif
	) {
		defer_push = true;
		cmd->flags |= RWNX_CMD_FLAG_WAIT_PUSH;
		//printk("defer push: tkn=%d\r\n", cmd->tkn);
	}

    spin_unlock_bh(&cmd_mgr->lock);
    if (!defer_push) {
        AICWFDBG(LOGTRACE, "queue:id=%x, param_len=%u\n",
                 cmd->a2e_msg->id, cmd->a2e_msg->param_len);
        ret = cmd_mgr_tx(cmd_mgr, cmd->a2e_msg);
        kfree(cmd->a2e_msg);
        cmd->a2e_msg = NULL;
        if (ret) {
            cmd_mgr_record_tx_failure(cmd_mgr);
            cmd_mgr_finish_cmd(cmd_mgr, cmd, ret);
        } else if (!expects_response)
            cmd_mgr_finish_cmd(cmd_mgr, cmd, 0);
    } else {
        WAKE_CMD_WORK(cmd_mgr);
    }

#ifdef CONFIG_RWNX_FHOST
    wait_ret = wait_for_completion_killable(&cmd->complete);
    if (wait_ret) {
        cmd_mgr_finish_cmd(cmd_mgr, cmd, (int)wait_ret);
        /* TODO: kill the cmd at fw level */
    }
    ret = cmd->result;
#else
        unsigned long tout = msecs_to_jiffies(RWNX_80211_CMD_TIMEOUT_MS/*AIDEN workaround* cmd_mgr->queue_sz*/);
    wait_ret = wait_for_completion_killable_timeout(&cmd->complete, tout);
    if (wait_ret <= 0) {
        ret = wait_ret ? (int)wait_ret : -ETIMEDOUT;
        if (!wait_ret) {
            struct rwnx_hw *rwnx_hw = cmd_mgr_rwnx_hw(cmd_mgr);

            if (rwnx_hw)
                atomic_inc(&rwnx_hw->runtime_stats.cmd_timeouts);
            printk(KERN_CRIT"%s cmd timed-out cmd_mgr->queue_sz:%d\n", __func__,cmd_mgr->queue_sz);
#ifdef AICWF_SDIO_SUPPORT
            if (aicwf_sdio_writeb(sdiodev, SDIOWIFI_WAKEUP_REG, 2) < 0) {
                sdio_err("reg:%d write failed!\n", SDIOWIFI_WAKEUP_REG);
            }
#endif

            cmd_dump(cmd);
            spin_lock_bh(&cmd_mgr->lock);
            cmd_mgr->state = RWNX_CMD_MGR_STATE_CRASHED;
            spin_unlock_bh(&cmd_mgr->lock);
        }
        cmd_mgr_finish_cmd(cmd_mgr, cmd, ret);
    } else {
        ret = cmd->result;
    }
#endif
    if (defer_push)
        wait_for_completion(&cmd->push_complete);
    WAKE_CMD_WORK(cmd_mgr);
    return ret;
}

/**
 *
 */
static int cmd_mgr_llind(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_cmd *cmd)
{
    struct rwnx_cmd *cur, *acked = NULL, *next = NULL;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    spin_lock_bh(&cmd_mgr->lock);
    list_for_each_entry(cur, &cmd_mgr->cmds, list) {
        if (!acked) {
            if (cur->tkn == cmd->tkn) {
                if (WARN_ON_ONCE(cur != cmd)) {
                    cmd_dump(cmd);
                }
                acked = cur;
                continue;
            }
        }
        if (cur->flags & RWNX_CMD_FLAG_WAIT_PUSH) {
                next = cur;
                break;
        }
    }
    if (!acked) {
        printk(KERN_CRIT "Error: acked cmd not found\n");
    } else {
        cmd->flags &= ~RWNX_CMD_FLAG_WAIT_ACK;
        if (RWNX_CMD_WAIT_COMPLETE(cmd->flags))
            cmd_complete(cmd_mgr, cmd);
    }

    if (next) {
	#if 0 //there is no ack
        struct rwnx_hw *rwnx_hw = container_of(cmd_mgr, struct rwnx_hw, cmd_mgr);
        next->flags &= ~RWNX_CMD_FLAG_WAIT_PUSH;
        rwnx_ipc_msg_push(rwnx_hw, next, RWNX_CMD_A2EMSG_LEN(next->a2e_msg));
        kfree(next->a2e_msg);
	#endif
    }
    spin_unlock(&cmd_mgr->lock);

    return 0;
}

static void cmd_mgr_task_process(struct work_struct *work)
{
    struct rwnx_cmd_mgr *cmd_mgr = container_of(work, struct rwnx_cmd_mgr, cmdWork);
    struct rwnx_cmd *cur, *next = NULL;
    struct rwnx_hw *rwnx_hw = cmd_mgr_rwnx_hw(cmd_mgr);
    unsigned long tout;
    long wait_ret;
    int ret;
    bool worker_owns = false;
    bool expects_response = false;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    while (1) {
        next = NULL;
        spin_lock_bh(&cmd_mgr->lock);

        if (cmd_mgr->state == RWNX_CMD_MGR_STATE_CRASHED) {
            spin_unlock_bh(&cmd_mgr->lock);
            break;
        }

        list_for_each_entry(cur, &cmd_mgr->cmds, list) {
            if (cur->flags & RWNX_CMD_FLAG_WAIT_PUSH) { //just judge the first
                next = cur;
                worker_owns = next->flags & RWNX_CMD_FLAG_WORKER_OWNS;
                expects_response = next->flags & RWNX_CMD_FLAG_REQ_CFM;
                next->flags &= ~RWNX_CMD_FLAG_WAIT_PUSH;
                cmd_mgr->active_cmd = next;
            }
            break;
        }
        spin_unlock_bh(&cmd_mgr->lock);

        if (!next)
            break;

        ret = cmd_mgr_tx(cmd_mgr, next->a2e_msg);
        kfree(next->a2e_msg);
        next->a2e_msg = NULL;
        if (ret) {
            cmd_mgr_record_tx_failure(cmd_mgr);
            cmd_mgr_finish_cmd(cmd_mgr, next, ret);
        } else if (!expects_response)
            cmd_mgr_finish_cmd(cmd_mgr, next, 0);

        /* Do not touch a caller-owned command after releasing its push wait. */
        complete_all(&next->push_complete);
        if (!worker_owns) {
            spin_lock_bh(&cmd_mgr->lock);
            if (cmd_mgr->active_cmd == next)
                cmd_mgr->active_cmd = NULL;
            spin_unlock_bh(&cmd_mgr->lock);
            break;
        }

        tout = msecs_to_jiffies(RWNX_80211_CMD_TIMEOUT_MS *
                                 max_t(u32, cmd_mgr->queue_sz, 1));
        wait_ret = wait_for_completion_killable_timeout(&next->complete,
                                                         tout);
        if (wait_ret <= 0) {
            ret = wait_ret ? (int)wait_ret : -ETIMEDOUT;
            if (!wait_ret) {
                if (rwnx_hw)
                    atomic_inc(&rwnx_hw->runtime_stats.cmd_timeouts);
                printk(KERN_CRIT
                       "%s cmd timed-out cmd_mgr->queue_sz:%d\n",
                       __func__, cmd_mgr->queue_sz);
                cmd_dump(next);
                spin_lock_bh(&cmd_mgr->lock);
                cmd_mgr->state = RWNX_CMD_MGR_STATE_CRASHED;
                spin_unlock_bh(&cmd_mgr->lock);
            }
            cmd_mgr_finish_cmd(cmd_mgr, next, ret);
        }
        spin_lock_bh(&cmd_mgr->lock);
        if (cmd_mgr->active_cmd == next)
            cmd_mgr->active_cmd = NULL;
        spin_unlock_bh(&cmd_mgr->lock);
        rwnx_cmd_free(next);
    }
}


static int cmd_mgr_run_callback(struct rwnx_hw *rwnx_hw, struct rwnx_cmd *cmd,
                                struct rwnx_cmd_e2amsg *msg, msg_cb_fct cb)
{
    int res;

    if (! cb){
        return 0;
    }
    //RWNX_DBG(RWNX_FN_ENTRY_STR);
#ifndef CONFIG_DEBUG_ATOMIC_SLEEP
	//spin_lock_bh(&rwnx_hw->cb_lock);
#endif
    res = cb(rwnx_hw, cmd, msg);
#ifndef CONFIG_DEBUG_ATOMIC_SLEEP
	//spin_unlock_bh(&rwnx_hw->cb_lock);
#endif

    return res;
}

/**
 *

 */
static int cmd_mgr_msgind(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_cmd_e2amsg *msg,
                          msg_cb_fct cb)
{
#ifdef AICWF_SDIO_SUPPORT
    struct aic_sdio_dev *sdiodev = container_of(cmd_mgr, struct aic_sdio_dev, cmd_mgr);
    struct rwnx_hw *rwnx_hw = sdiodev->rwnx_hw;
#endif
#ifdef AICWF_USB_SUPPORT
    struct aic_usb_dev *usbdev = container_of(cmd_mgr, struct aic_usb_dev, cmd_mgr);
    struct rwnx_hw *rwnx_hw = usbdev->rwnx_hw;
#endif
    struct rwnx_cmd *cmd, *pos;
    bool found = false;
    size_t param_len = msg->param_len;

   // RWNX_DBG(RWNX_FN_ENTRY_STR);
#ifdef CREATE_TRACE_POINTS
    trace_msg_recv(msg->id);
#endif
    AICWFDBG(LOGTRACE, "%s cmd->id=%d\n", __func__, msg->id);
    spin_lock_bh(&cmd_mgr->lock);
    list_for_each_entry_safe(cmd, pos, &cmd_mgr->cmds, list) {
        if (cmd->reqid == msg->id &&
            (cmd->flags & RWNX_CMD_FLAG_WAIT_CFM)) {

            if (cmd->flags & RWNX_CMD_FLAG_WAIT_PUSH) {
                found = true;
                if (rwnx_hw)
                    atomic_inc(&rwnx_hw->runtime_stats.cmd_cfm_before_push);
                AICWFDBG_RATELIMITED(
                    LOGERROR,
                    "drop CFM 0x%x for command still waiting to be sent\n",
                    msg->id);
                break;
            }

            if (param_len > RWNX_CMD_E2AMSG_LEN_MAX ||
                (cmd->e2a_msg && param_len > cmd->e2a_msg_len)) {
                size_t limit = cmd->e2a_msg ? cmd->e2a_msg_len :
                               RWNX_CMD_E2AMSG_LEN_MAX;

                found = true;
                cmd->flags &= ~RWNX_CMD_FLAG_WAIT_CFM;
                cmd->result = -EMSGSIZE;
                if (rwnx_hw)
                    atomic_inc(&rwnx_hw->runtime_stats.cmd_cfm_oversize);
                AICWFDBG_RATELIMITED(
                    LOGERROR,
                    "CFM 0x%x payload too large: %zu > %zu\n",
                    msg->id, param_len, limit);
                if (RWNX_CMD_WAIT_COMPLETE(cmd->flags))
                    cmd_complete(cmd_mgr, cmd);
                break;
            }

            if (!cmd_mgr_run_callback(rwnx_hw, cmd, msg, cb)) {
                found = true;
                cmd->flags &= ~RWNX_CMD_FLAG_WAIT_CFM;

                if (cmd->e2a_msg && param_len)
                    memcpy(cmd->e2a_msg, &msg->param, param_len);

                if (RWNX_CMD_WAIT_COMPLETE(cmd->flags))
                    cmd_complete(cmd_mgr, cmd);

                break;
            }
        }
    }
    spin_unlock_bh(&cmd_mgr->lock);

    if (!found)
        cmd_mgr_run_callback(rwnx_hw, NULL, msg, cb);

    return 0;
}

/**
 *
 */
static void cmd_mgr_print(struct rwnx_cmd_mgr *cmd_mgr)
{
    struct rwnx_cmd *cur;

    spin_lock_bh(&cmd_mgr->lock);
    RWNX_DBG("q_sz/max: %2d / %2d - next tkn: %d\n",
             cmd_mgr->queue_sz, cmd_mgr->max_queue_sz,
             cmd_mgr->next_tkn);
    list_for_each_entry(cur, &cmd_mgr->cmds, list) {
        cmd_dump(cur);
    }
    spin_unlock_bh(&cmd_mgr->lock);
}

static void cmd_mgr_drain(struct rwnx_cmd_mgr *cmd_mgr)
{
    struct rwnx_cmd *cur, *nxt;

    RWNX_DBG(RWNX_FN_ENTRY_STR);

    spin_lock_bh(&cmd_mgr->lock);
    list_for_each_entry_safe(cur, nxt, &cmd_mgr->cmds, list) {
        bool worker_owns = cur->flags & RWNX_CMD_FLAG_WORKER_OWNS;

        list_del_init(&cur->list);
        cmd_mgr->queue_sz--;
        cur->result = -ESHUTDOWN;
        cur->flags &= ~(RWNX_CMD_FLAG_WAIT_PUSH |
                        RWNX_CMD_FLAG_WAIT_ACK |
                        RWNX_CMD_FLAG_WAIT_CFM);
        cur->flags |= RWNX_CMD_FLAG_DONE;
        kfree(cur->a2e_msg);
        cur->a2e_msg = NULL;
        complete_all(&cur->push_complete);
        if (worker_owns)
            rwnx_cmd_free(cur);
        else
            complete(&cur->complete);
    }
    spin_unlock_bh(&cmd_mgr->lock);
}

void rwnx_cmd_mgr_init(struct rwnx_cmd_mgr *cmd_mgr)
{
    RWNX_DBG(RWNX_FN_ENTRY_STR);

    INIT_LIST_HEAD(&cmd_mgr->cmds);
	cmd_mgr->state = RWNX_CMD_MGR_STATE_CRASHED;
    spin_lock_init(&cmd_mgr->lock);
    cmd_mgr->active_cmd = NULL;
    cmd_mgr->max_queue_sz = RWNX_CMD_MAX_QUEUED;
    cmd_mgr->queue  = &cmd_mgr_queue;
    cmd_mgr->print  = &cmd_mgr_print;
    cmd_mgr->drain  = &cmd_mgr_drain;
    cmd_mgr->llind  = &cmd_mgr_llind;
    cmd_mgr->msgind = &cmd_mgr_msgind;

    INIT_WORK(&cmd_mgr->cmdWork, cmd_mgr_task_process);
    cmd_mgr->cmd_wq = create_singlethread_workqueue("cmd_wq");
    if (!cmd_mgr->cmd_wq) {
        txrx_err("insufficient memory to create cmd workqueue.\n");
        return;
    }
    cmd_mgr->state = RWNX_CMD_MGR_STATE_INITED;
}

void rwnx_cmd_mgr_deinit(struct rwnx_cmd_mgr *cmd_mgr)
{
    struct rwnx_cmd *active_cmd;

    spin_lock_bh(&cmd_mgr->lock);
    cmd_mgr->state = RWNX_CMD_MGR_STATE_CRASHED;
    active_cmd = cmd_mgr->active_cmd;
    if (active_cmd &&
        (active_cmd->flags & RWNX_CMD_FLAG_WORKER_OWNS) &&
        !(active_cmd->flags & RWNX_CMD_FLAG_DONE)) {
        active_cmd->result = -ESHUTDOWN;
        active_cmd->flags &= ~(RWNX_CMD_FLAG_WAIT_PUSH |
                               RWNX_CMD_FLAG_WAIT_ACK |
                               RWNX_CMD_FLAG_WAIT_CFM);
        cmd_complete(cmd_mgr, active_cmd);
    }
    spin_unlock_bh(&cmd_mgr->lock);
    cmd_mgr->print(cmd_mgr);
    if (cmd_mgr->cmd_wq)
        cancel_work_sync(&cmd_mgr->cmdWork);
    cmd_mgr->drain(cmd_mgr);
    cmd_mgr->print(cmd_mgr);
    if (cmd_mgr->cmd_wq)
        destroy_workqueue(cmd_mgr->cmd_wq);
    memset(cmd_mgr, 0, sizeof(*cmd_mgr));
}


int aicwf_set_cmd_tx(void *dev, struct lmac_msg *msg, uint len)
{
    u8 *buffer = NULL;
    u16 index = 0;
    int ret;

    if (!dev || !msg)
        return -EINVAL;
    if (len != sizeof(*msg) + msg->param_len || len > CMD_BUF_MAX - 8)
        return -EMSGSIZE;
#ifdef AICWF_SDIO_SUPPORT
	struct aic_sdio_dev *sdiodev = (struct aic_sdio_dev *)dev;
    struct aicwf_bus *bus = sdiodev->bus_if;
#else
	struct aic_usb_dev *usbdev = (struct aic_usb_dev *)dev;
	struct aicwf_bus *bus = NULL;
    if (!usbdev->state) {
        AICWFDBG_RATELIMITED(LOGERROR, "command rejected: USB is down\n");
        return -ESHUTDOWN;
    }
	bus = usbdev->bus_if;
#endif
    if (!bus || !bus->cmd_buf)
        return -ENODEV;

    mutex_lock(&bus->cmd_buf_lock);
    buffer = bus->cmd_buf;

    memset(buffer, 0, CMD_BUF_MAX);
    buffer[0] = (len+4) & 0x00ff;
    buffer[1] = ((len+4) >> 8) &0x0f;
    buffer[2] = 0x11;
    buffer[3] = 0x0;
    index += 4;
    //there is a dummy word
    index += 4;

	//make sure little endian
    put_u16(&buffer[index], msg->id);
    index += 2;
    put_u16(&buffer[index], msg->dest_id);
    index += 2;
    put_u16(&buffer[index], msg->src_id);
    index += 2;
    put_u16(&buffer[index], msg->param_len);
    index += 2;
    memcpy(&buffer[index], (u8 *)msg->param, msg->param_len);

    ret = aicwf_bus_txmsg(bus, buffer, len + 8);
    mutex_unlock(&bus->cmd_buf_lock);

    return ret;
}

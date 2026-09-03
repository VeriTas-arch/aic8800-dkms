/**
 * aicwf_usb.c
 *
 * USB function declarations
 *
 * Copyright (C) AICSemi 2018-2020
 */

#include <linux/usb.h>
#include <linux/kthread.h>
#include "aicwf_txrxif.h"
#include "aicwf_usb.h"
#include "rwnx_tx.h"
#include "rwnx_defs.h"
#include "rwnx_msg_rx.h"
#include "usb_host.h"
#include "rwnx_platform.h"

#ifdef CONFIG_GPIO_WAKEUP
#ifdef CONFIG_PLATFORM_ROCKCHIP
#include <linux/rfkill-wlan.h>
#endif
static int wakeup_enable;
static u32 hostwake_irq_num;
atomic_t irq_count;
spinlock_t irq_lock;
#endif

#include <linux/semaphore.h>
extern struct semaphore aicwf_deinit_sem;
extern atomic_t aicwf_deinit_atomic;
#define SEM_TIMOUT 2000


#ifdef CONFIG_TXRX_THREAD_PRIO

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0)
#include "linux/sched/types.h"
#else
//#include "linux/sched/rt.h"
//#include "uapi/linux/sched/types.h"
#endif

int bustx_thread_prio = 1;
module_param(bustx_thread_prio, int, 0);
int busrx_thread_prio = 1;
module_param(busrx_thread_prio, int, 0);
#endif

atomic_t rx_urb_cnt;

#define AICWF_USB_STAT_INC(usb_dev, field) do { \
    if ((usb_dev) && (usb_dev)->rwnx_hw) \
        atomic_inc(&(usb_dev)->rwnx_hw->runtime_stats.field); \
} while (0)

#define AICWF_USB_RX_RETRY_DELAY_MS 100

static bool aicwf_usb_status_terminal(int status)
{
    return status == -ENOENT || status == -ECONNRESET ||
           status == -ESHUTDOWN || status == -ENODEV;
}

static void aicwf_usb_schedule_rx_refill(struct aic_usb_dev *usb_dev,
                                         unsigned long delay_ms)
{
    if (usb_dev && usb_dev->state == USB_UP_ST)
        mod_delayed_work(system_wq, &usb_dev->rx_urb_work,
                         msecs_to_jiffies(delay_ms));
}

#ifdef CONFIG_USB_MSG_IN_EP
static void aicwf_usb_schedule_msg_rx_refill(struct aic_usb_dev *usb_dev,
                                             unsigned long delay_ms)
{
    if (usb_dev && usb_dev->state == USB_UP_ST)
        mod_delayed_work(system_wq, &usb_dev->msg_rx_urb_work,
                         msecs_to_jiffies(delay_ms));
}
#endif

void aicwf_usb_tx_flowctrl(struct rwnx_hw *rwnx_hw, bool state)
{
    struct rwnx_vif *rwnx_vif;

    list_for_each_entry(rwnx_vif, &rwnx_hw->vifs, list) {
        if (!rwnx_vif || !rwnx_vif->ndev || !rwnx_vif->up)
            continue;
        if (state) {
            netif_tx_stop_all_queues(rwnx_vif->ndev);//netif_stop_queue(rwnx_vif->ndev);
        } else if (netif_carrier_ok(rwnx_vif->ndev) &&
                   !rwnx_conn_tx_paused(rwnx_vif)) {
            netif_tx_wake_all_queues(rwnx_vif->ndev);//netif_wake_queue(rwnx_vif->ndev);
		}
	}
}

enum aicwf_usb_tx_wake_result
aicwf_usb_tx_maybe_wake(struct rwnx_hw *rwnx_hw,
                        struct net_device *ndev)
{
    struct aic_usb_dev *usb_dev;
    unsigned long flags;
    enum aicwf_usb_tx_wake_result result;

    if (!rwnx_hw || !ndev)
        return AICWF_USB_TX_WAKE_BLOCKED_NO_DEVICE;

    usb_dev = rwnx_hw->usbdev;
    if (!usb_dev || !usb_dev->bus_if)
        return AICWF_USB_TX_WAKE_BLOCKED_NO_DEVICE;

    spin_lock_irqsave(&usb_dev->tx_flow_lock, flags);
    if (usb_dev->bus_if->state == BUS_DOWN_ST) {
        result = AICWF_USB_TX_WAKE_BLOCKED_BUS_DOWN;
    } else if (usb_dev->state == USB_DOWN_ST) {
        result = AICWF_USB_TX_WAKE_BLOCKED_USB_DOWN;
    } else if (!netif_carrier_ok(ndev)) {
        result = AICWF_USB_TX_WAKE_BLOCKED_CARRIER;
    } else if (usb_dev->tbusy) {
        result = AICWF_USB_TX_WAKE_DEFERRED_TBUSY;
    } else {
        netif_tx_wake_all_queues(ndev);
        result = AICWF_USB_TX_WAKE_IMMEDIATE;
    }
    spin_unlock_irqrestore(&usb_dev->tx_flow_lock, flags);

    return result;
}

static struct aicwf_usb_buf *aicwf_usb_tx_dequeue(struct aic_usb_dev *usb_dev,
    struct list_head *q, int *counter, spinlock_t *qlock)
{
    unsigned long flags;
    struct aicwf_usb_buf *usb_buf;

    spin_lock_irqsave(qlock, flags);
    if (list_empty(q)) {
        usb_buf = NULL;
    } else {
        usb_buf = list_first_entry(q, struct aicwf_usb_buf, list);
        list_del_init(&usb_buf->list);
        if (counter)
            (*counter)--;
    }
    spin_unlock_irqrestore(qlock, flags);
    return usb_buf;
}

static void aicwf_usb_tx_queue(struct aic_usb_dev *usb_dev,
    struct list_head *q, struct aicwf_usb_buf *usb_buf, int *counter,
    spinlock_t *qlock)
{
    unsigned long flags;

    spin_lock_irqsave(qlock, flags);
    list_add_tail(&usb_buf->list, q);
    (*counter)++;
    spin_unlock_irqrestore(qlock, flags);
}

static struct aicwf_usb_buf *aicwf_usb_rx_buf_get(struct aic_usb_dev *usb_dev)
{
    unsigned long flags;
    struct aicwf_usb_buf *usb_buf;

    spin_lock_irqsave(&usb_dev->rx_free_lock, flags);
    if (list_empty(&usb_dev->rx_free_list)) {
        usb_buf = NULL;
    } else {
        usb_buf = list_first_entry(&usb_dev->rx_free_list, struct aicwf_usb_buf, list);
        list_del_init(&usb_buf->list);
    }
    spin_unlock_irqrestore(&usb_dev->rx_free_lock, flags);
    return usb_buf;
}

static void aicwf_usb_rx_buf_put(struct aic_usb_dev *usb_dev, struct aicwf_usb_buf *usb_buf)
{
    unsigned long flags;

    spin_lock_irqsave(&usb_dev->rx_free_lock, flags);
    list_add_tail(&usb_buf->list, &usb_dev->rx_free_list);
    spin_unlock_irqrestore(&usb_dev->rx_free_lock, flags);
}

#ifdef CONFIG_USB_MSG_IN_EP
static struct aicwf_usb_buf *aicwf_usb_msg_rx_buf_get(struct aic_usb_dev *usb_dev)
{
    unsigned long flags;
    struct aicwf_usb_buf *usb_buf;

    spin_lock_irqsave(&usb_dev->msg_rx_free_lock, flags);
    if (list_empty(&usb_dev->msg_rx_free_list)) {
        usb_buf = NULL;
    } else {
        usb_buf = list_first_entry(&usb_dev->msg_rx_free_list, struct aicwf_usb_buf, list);
        list_del_init(&usb_buf->list);
    }
    spin_unlock_irqrestore(&usb_dev->msg_rx_free_lock, flags);
    return usb_buf;
}

static void aicwf_usb_msg_rx_buf_put(struct aic_usb_dev *usb_dev, struct aicwf_usb_buf *usb_buf)
{
    unsigned long flags;

    spin_lock_irqsave(&usb_dev->msg_rx_free_lock, flags);
    list_add_tail(&usb_buf->list, &usb_dev->msg_rx_free_list);
    spin_unlock_irqrestore(&usb_dev->msg_rx_free_lock, flags);
}
#endif

#ifdef CONFIG_PER_STA_FC
static void rwnx_stop_sta_all_queues(struct rwnx_sta *sta,
                                     struct rwnx_hw *rwnx_hw)
{
        u8 tid;
         struct rwnx_txq *txq;
         for(tid=0; tid<8; tid++) {
                 txq = rwnx_txq_sta_get(sta, tid, rwnx_hw);
                 netif_stop_subqueue(txq->ndev, txq->ndev_idx);
         }
 }

static void rwnx_wake_sta_all_queues(struct rwnx_sta *sta,
                                     struct rwnx_hw *rwnx_hw)
{
        u8 tid;
         struct rwnx_txq *txq;
         for(tid=0; tid<8; tid++) {
                 txq = rwnx_txq_sta_get(sta, tid, rwnx_hw);
                 netif_wake_subqueue(txq->ndev, txq->ndev_idx);
         }
 }
#endif

static void usb_txc_sta_flowctrl(struct aicwf_usb_buf *usb_buf, struct aic_usb_dev *usb_dev)
{
#ifdef CONFIG_PER_STA_FC
    unsigned long flags;
	struct rwnx_sta *sta;
	struct txdesc_api *hostdesc;
	u8 sta_idx;
	if(usb_buf->cfm)
		hostdesc = (struct txdesc_api *)((u8 *)usb_buf->skb + 4);
	else
		hostdesc = (struct txdesc_api *)((u8 *)usb_buf->skb->data + 4);
	//printk("txcpl: sta %d\n", hostdesc->host.staid);
	sta_idx = hostdesc->host.staid;
	if(sta_idx < NX_REMOTE_STA_MAX && !(hostdesc->host.flags & TXU_CNTRL_MGMT)) {
		struct rwnx_vif *vif = NULL;
		sta = &usb_dev->rwnx_hw->sta_table[sta_idx];
		if (sta->vif_idx >= ARRAY_SIZE(usb_dev->rwnx_hw->vif_table))
			return;
		vif = usb_dev->rwnx_hw->vif_table[sta->vif_idx];
		if (!vif)
			return;
		spin_lock_irqsave(&usb_dev->tx_flow_lock, flags);
		atomic_dec(&usb_dev->rwnx_hw->sta_flowctrl[sta_idx].tx_pending_cnt);
		//printk("sta:%d, pending:%d, flowctrl=%d\n", sta->sta_idx, sta->tx_pending_cnt, sta->flowctrl);
		if(RWNX_VIF_TYPE(vif) == NL80211_IFTYPE_AP) {
			if(atomic_read(&usb_dev->rwnx_hw->sta_flowctrl[sta_idx].tx_pending_cnt) < AICWF_USB_FC_PERSTA_LOW_WATER &&
							usb_dev->rwnx_hw->sta_flowctrl[sta_idx].flowctrl) {
				//AICWFDBG(LOGDEBUG, "sta 0x%x:0x%x, %d pending %d, wake\n", sta->mac_addr[4], sta->mac_addr[5], sta->sta_idx, atomic_read(&usb_dev->rwnx_hw->sta_flowctrl[sta_idx].tx_pending_cnt));
				if(!usb_dev->tbusy)
					rwnx_wake_sta_all_queues(sta, usb_dev->rwnx_hw);
				usb_dev->rwnx_hw->sta_flowctrl[sta_idx].flowctrl = 0;
			}
		}
		spin_unlock_irqrestore(&usb_dev->tx_flow_lock, flags);
	}
#endif
}

static void aicwf_usb_tx_payload_free(struct aicwf_usb_buf *usb_buf)
{
#ifdef CONFIG_USB_ALIGN_DATA
    kfree(usb_buf->usb_align_data);
    usb_buf->usb_align_data = NULL;
#endif
#ifndef CONFIG_USB_TX_AGGR
    if (!usb_buf->skb)
        return;

    if (!usb_buf->cfm)
        dev_kfree_skb_any(usb_buf->skb);
#if !defined CONFIG_USB_NO_TRANS_DMA_MAP
    else
        kfree((u8 *)usb_buf->skb);
#endif
    usb_buf->skb = NULL;
#endif
}

static void aicwf_usb_tx_complete(struct urb *urb)
{
    unsigned long flags;
    struct aicwf_usb_buf *usb_buf = (struct aicwf_usb_buf *) urb->context;
    struct aic_usb_dev *usb_dev = usb_buf->usbdev;

    if (urb->status) {
        AICWF_USB_STAT_INC(usb_dev, usb_tx_completion_errors);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "USB TX completion failed:%d\n", urb->status);
    }

    usb_txc_sta_flowctrl(usb_buf, usb_dev);
    aicwf_usb_tx_payload_free(usb_buf);

#ifdef CONFIG_USB_TX_AGGR
    AICWFDBG(LOGDEBUG,"tx com %d\n", usb_buf->aggr_cnt);
    usb_buf->aggr_cnt = 0;
#endif//CONFIG_USB_TX_AGGR
    aicwf_usb_tx_queue(usb_dev, &usb_dev->tx_free_list, usb_buf,
                    &usb_dev->tx_free_count, &usb_dev->tx_free_lock);

    spin_lock_irqsave(&usb_dev->tx_flow_lock, flags);
    if (usb_dev->tx_free_count > AICWF_USB_TX_HIGH_WATER) {
        if (usb_dev->tbusy) {
            usb_dev->tbusy = false;
            AICWF_USB_STAT_INC(usb_dev, usb_flow_wakes);
            aicwf_usb_tx_flowctrl(usb_dev->rwnx_hw, false);
        }
    }
    spin_unlock_irqrestore(&usb_dev->tx_flow_lock, flags);
}

void aicwf_usb_rx_submit_all_urb_(struct aic_usb_dev *usb_dev);

#ifdef CONFIG_PREALLOC_RX_SKB
static void aicwf_usb_rx_complete(struct urb *urb)
    {
        struct aicwf_usb_buf *usb_buf = (struct aicwf_usb_buf *) urb->context;
        struct aic_usb_dev *usb_dev = usb_buf->usbdev;
        struct aicwf_rx_priv* rx_priv = usb_dev->rx_priv;
        struct rx_buff *rx_buff = NULL;
        unsigned long flags = 0;
    
        rx_buff = usb_buf->rx_buff;
        usb_buf->rx_buff = NULL;
    
        atomic_dec(&rx_urb_cnt);
        if(atomic_read(&rx_urb_cnt) < 10){
            AICWFDBG(LOGDEBUG, "%s %d \r\n", __func__, atomic_read(&rx_urb_cnt));
            //printk("%s %d \r\n", __func__, atomic_read(&rx_urb_cnt));
        }

        if(!usb_dev->rwnx_hw){
            aicwf_prealloc_rxbuff_free(rx_buff, &rx_priv->rxbuff_lock);
            aicwf_usb_rx_buf_put(usb_dev, usb_buf);
            AICWFDBG(LOGERROR, "usb_dev->rwnx_hw is not ready \r\n");
            aicwf_usb_schedule_rx_refill(usb_dev,
                                         AICWF_USB_RX_RETRY_DELAY_MS);
            return;
        }
    
        if (urb->actual_length > urb->transfer_buffer_length) {
            aicwf_prealloc_rxbuff_free(rx_buff, &rx_priv->rxbuff_lock);
            aicwf_usb_rx_buf_put(usb_dev, usb_buf);
            AICWF_USB_STAT_INC(usb_dev, usb_rx_invalid_lengths);
            aicwf_usb_schedule_rx_refill(usb_dev, 0);
            return;
        }
    
        if (urb->status != 0 || !urb->actual_length) {
            aicwf_prealloc_rxbuff_free(rx_buff, &rx_priv->rxbuff_lock);
            aicwf_usb_rx_buf_put(usb_dev, usb_buf);
            if (urb->status) {
                AICWF_USB_STAT_INC(usb_dev, usb_rx_completion_errors);
                if (aicwf_usb_status_terminal(urb->status)) {
                    AICWF_USB_STAT_INC(usb_dev, usb_rx_terminal_errors);
                    return;
                }
                AICWFDBG_RATELIMITED(LOGERROR,
                                     "USB RX completion failed:%d\n",
                                     urb->status);
                aicwf_usb_schedule_rx_refill(
                    usb_dev, AICWF_USB_RX_RETRY_DELAY_MS);
            } else {
                aicwf_usb_schedule_rx_refill(usb_dev, 0);
            }
            return;
        }

        rx_buff->len = urb->actual_length;
    
    if (usb_dev->state == USB_UP_ST) {
        spin_lock_irqsave(&rx_priv->rxqlock, flags);

        if(!aicwf_rxbuff_enqueue(usb_dev->dev, &rx_priv->rxq, rx_buff)){
            spin_unlock_irqrestore(&rx_priv->rxqlock, flags);
            AICWF_USB_STAT_INC(usb_dev, usb_rx_queue_overflows);
            AICWFDBG_RATELIMITED(LOGERROR,
                                 "USB RX queue overflow\n");
            aicwf_prealloc_rxbuff_free(rx_buff, &rx_priv->rxbuff_lock);
            aicwf_usb_rx_buf_put(usb_dev, usb_buf);
            aicwf_usb_schedule_rx_refill(usb_dev, 0);
            return;
        }
        spin_unlock_irqrestore(&rx_priv->rxqlock, flags);
        atomic_inc(&rx_priv->rx_cnt);
            
        if(atomic_read(&rx_priv->rx_cnt) == 1){
            complete(&rx_priv->usbdev->bus_if->busrx_trgg);
        }

        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
        aicwf_usb_schedule_rx_refill(usb_dev, 0);
    } else {
        aicwf_prealloc_rxbuff_free(rx_buff, &rx_priv->rxbuff_lock);
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
    }
}

#else
static void aicwf_usb_rx_complete(struct urb *urb)
{
    struct aicwf_usb_buf *usb_buf = (struct aicwf_usb_buf *) urb->context;
    struct aic_usb_dev *usb_dev = usb_buf->usbdev;
    struct aicwf_rx_priv* rx_priv = usb_dev->rx_priv;
    struct sk_buff *skb = NULL;
    unsigned long flags = 0;

    skb = usb_buf->skb;
    usb_buf->skb = NULL;

	atomic_dec(&rx_urb_cnt);
	if(atomic_read(&rx_urb_cnt) < 10){
		AICWFDBG(LOGDEBUG, "%s %d \r\n", __func__, atomic_read(&rx_urb_cnt));
		//printk("%s %d \r\n", __func__, atomic_read(&rx_urb_cnt));
	}

	if(!usb_dev->rwnx_hw){
		aicwf_dev_skb_free(skb);
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
		AICWFDBG(LOGERROR, "usb_dev->rwnx_hw is not ready \r\n");
		aicwf_usb_schedule_rx_refill(usb_dev, AICWF_USB_RX_RETRY_DELAY_MS);
		return;
	}

    if (urb->actual_length > urb->transfer_buffer_length) {
        aicwf_dev_skb_free(skb);
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
        AICWF_USB_STAT_INC(usb_dev, usb_rx_invalid_lengths);
        aicwf_usb_schedule_rx_refill(usb_dev, 0);
        return;
    }

    if (urb->status != 0 || !urb->actual_length) {
        aicwf_dev_skb_free(skb);
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
		if (urb->status) {
            AICWF_USB_STAT_INC(usb_dev, usb_rx_completion_errors);
            if (aicwf_usb_status_terminal(urb->status)) {
                AICWF_USB_STAT_INC(usb_dev, usb_rx_terminal_errors);
                return;
            }
            AICWFDBG_RATELIMITED(LOGERROR,
                                 "USB RX completion failed:%d\n",
                                 urb->status);
            aicwf_usb_schedule_rx_refill(usb_dev,
                                         AICWF_USB_RX_RETRY_DELAY_MS);
		} else {
            aicwf_usb_schedule_rx_refill(usb_dev, 0);
        }
        return;
    }
    #ifdef CONFIG_USB_RX_AGGR
    if (urb->actual_length > 1600 * 30) {
        printk("r%d\n", urb->actual_length);
    }
    #endif

    if (usb_dev->state == USB_UP_ST) {

        skb_put(skb, urb->actual_length);

        spin_lock_irqsave(&rx_priv->rxqlock, flags);
        #ifdef CONFIG_USB_RX_AGGR
        skb->len = urb->actual_length;
        #endif
        if(!aicwf_rxframe_enqueue(usb_dev->dev, &rx_priv->rxq, skb)){
            spin_unlock_irqrestore(&rx_priv->rxqlock, flags);
            AICWF_USB_STAT_INC(usb_dev, usb_rx_queue_overflows);
            AICWFDBG_RATELIMITED(LOGERROR,
                                 "USB RX queue overflow\n");
            aicwf_dev_skb_free(skb);
            aicwf_usb_rx_buf_put(usb_dev, usb_buf);
            aicwf_usb_schedule_rx_refill(usb_dev, 0);
            return;
        }
        spin_unlock_irqrestore(&rx_priv->rxqlock, flags);
        atomic_inc(&rx_priv->rx_cnt);
		
#ifndef CONFIG_RX_TASKLET 
		//if(!rx_priv->rx_thread_working && (atomic_read(&rx_priv->rx_cnt)>0)){
		if(atomic_read(&rx_priv->rx_cnt) == 1){
        	complete(&rx_priv->usbdev->bus_if->busrx_trgg);
		}
#else
        tasklet_schedule(&rx_priv->usbdev->recv_tasklet);
#endif
		
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
        aicwf_usb_schedule_rx_refill(usb_dev, 0);
    } else {
        aicwf_dev_skb_free(skb);
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
    }
}
#endif

#ifdef CONFIG_USB_MSG_IN_EP
void aicwf_usb_msg_rx_submit_all_urb_(struct aic_usb_dev *usb_dev);

static void aicwf_usb_msg_rx_complete(struct urb *urb)
{
    struct aicwf_usb_buf *usb_buf = (struct aicwf_usb_buf *) urb->context;
    struct aic_usb_dev *usb_dev = usb_buf->usbdev;
    struct aicwf_rx_priv* rx_priv = usb_dev->rx_priv;
    struct sk_buff *skb = NULL;
    unsigned long flags = 0;

    skb = usb_buf->skb;
    usb_buf->skb = NULL;

    if (!usb_dev->rwnx_hw) {
        aicwf_dev_skb_free(skb);
        aicwf_usb_msg_rx_buf_put(usb_dev, usb_buf);
        aicwf_usb_schedule_msg_rx_refill(
            usb_dev, AICWF_USB_RX_RETRY_DELAY_MS);
        return;
    }

    if (urb->actual_length > urb->transfer_buffer_length) {
        aicwf_dev_skb_free(skb);
        aicwf_usb_msg_rx_buf_put(usb_dev, usb_buf);
        AICWF_USB_STAT_INC(usb_dev, usb_msg_rx_invalid_lengths);
        aicwf_usb_schedule_msg_rx_refill(usb_dev, 0);
        return;
    }

    if (urb->status != 0 || !urb->actual_length) {
        aicwf_dev_skb_free(skb);
        aicwf_usb_msg_rx_buf_put(usb_dev, usb_buf);

		if (urb->status) {
            AICWF_USB_STAT_INC(usb_dev, usb_msg_rx_completion_errors);
            if (aicwf_usb_status_terminal(urb->status)) {
                AICWF_USB_STAT_INC(usb_dev, usb_msg_rx_terminal_errors);
                return;
            }
            AICWFDBG_RATELIMITED(LOGERROR,
                                 "USB message RX completion failed:%d\n",
                                 urb->status);
            aicwf_usb_schedule_msg_rx_refill(
                usb_dev, AICWF_USB_RX_RETRY_DELAY_MS);
		} else {
            aicwf_usb_schedule_msg_rx_refill(usb_dev, 0);
        }
        return;
    }

    if (usb_dev->state == USB_UP_ST) {
        skb_put(skb, urb->actual_length);

        spin_lock_irqsave(&rx_priv->msg_rxqlock, flags);
        if(!aicwf_rxframe_enqueue(usb_dev->dev, &rx_priv->msg_rxq, skb)){
            spin_unlock_irqrestore(&rx_priv->msg_rxqlock, flags);
            AICWF_USB_STAT_INC(usb_dev, usb_msg_rx_queue_overflows);
            AICWFDBG_RATELIMITED(LOGERROR,
                                 "USB message RX queue overflow\n");
            aicwf_dev_skb_free(skb);
            aicwf_usb_msg_rx_buf_put(usb_dev, usb_buf);
            aicwf_usb_schedule_msg_rx_refill(usb_dev, 0);
            return;
        }
        spin_unlock_irqrestore(&rx_priv->msg_rxqlock, flags);
        atomic_inc(&rx_priv->msg_rx_cnt);
        complete(&rx_priv->usbdev->bus_if->msg_busrx_trgg);
        aicwf_usb_msg_rx_buf_put(usb_dev, usb_buf);
        aicwf_usb_schedule_msg_rx_refill(usb_dev, 0);
    } else {
        aicwf_dev_skb_free(skb);
        aicwf_usb_msg_rx_buf_put(usb_dev, usb_buf);
    }
}
#endif

#ifdef CONFIG_PREALLOC_RX_SKB
//extern int aic_rxbuff_size;
static int aicwf_usb_submit_rx_urb(struct aic_usb_dev *usb_dev,
                struct aicwf_usb_buf *usb_buf)
{
    int ret;
    struct rx_buff *rx_buff;

    if (!usb_buf || !usb_dev)
        return -EINVAL;

    if (usb_dev->state != USB_UP_ST) {
        AICWF_USB_STAT_INC(usb_dev, usb_rx_state_rejects);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "USB RX rejected in state:%d\n",
                             usb_dev->state);
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
        return -ESHUTDOWN;
    }
    rx_buff =  aicwf_prealloc_rxbuff_alloc(&usb_dev->rx_priv->rxbuff_lock);
	if (rx_buff == NULL) {
		AICWFDBG(LOGERROR, "failed to alloc rxbuff\r\n");
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
        return -ENOMEM;
    }
	rx_buff->len = 0;
	rx_buff->start = rx_buff->data;
	rx_buff->read = rx_buff->start;
	rx_buff->end = rx_buff->data + aicwf_rxbuff_size_get();

    usb_buf->rx_buff = rx_buff;

    usb_fill_bulk_urb(usb_buf->urb,
        usb_dev->udev,
        usb_dev->bulk_in_pipe,
        rx_buff->data, aicwf_rxbuff_size_get(), aicwf_usb_rx_complete, usb_buf);

    usb_buf->usbdev = usb_dev;

    usb_anchor_urb(usb_buf->urb, &usb_dev->rx_submitted);
    ret = usb_submit_urb(usb_buf->urb, GFP_ATOMIC);
    if (ret) {
        AICWF_USB_STAT_INC(usb_dev, usb_rx_submit_failures);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "USB RX submit failed:%d\n", ret);
        usb_unanchor_urb(usb_buf->urb);
        aicwf_prealloc_rxbuff_free(rx_buff, &usb_dev->rx_priv->rxbuff_lock);
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);

    }else{
    	atomic_inc(&rx_urb_cnt);
	}
    return ret;
}

#else
static int aicwf_usb_submit_rx_urb(struct aic_usb_dev *usb_dev,
                struct aicwf_usb_buf *usb_buf)
{
    struct sk_buff *skb;
    int ret;

    if (!usb_buf || !usb_dev)
        return -EINVAL;

    if (usb_dev->state != USB_UP_ST) {
        AICWF_USB_STAT_INC(usb_dev, usb_rx_state_rejects);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "USB RX rejected in state:%d\n",
                             usb_dev->state);
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
        return -ESHUTDOWN;
    }

    skb = __dev_alloc_skb(AICWF_USB_MAX_PKT_SIZE, GFP_ATOMIC/*GFP_KERNEL*/);
    if (!skb) {
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);
        return -ENOMEM;
    }

    usb_buf->skb = skb;

    usb_fill_bulk_urb(usb_buf->urb,
        usb_dev->udev,
        usb_dev->bulk_in_pipe,
        skb->data, skb_tailroom(skb), aicwf_usb_rx_complete, usb_buf);

    usb_buf->usbdev = usb_dev;

    usb_anchor_urb(usb_buf->urb, &usb_dev->rx_submitted);
    ret = usb_submit_urb(usb_buf->urb, GFP_ATOMIC);
    if (ret) {
        AICWF_USB_STAT_INC(usb_dev, usb_rx_submit_failures);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "USB RX submit failed:%d\n", ret);
        usb_unanchor_urb(usb_buf->urb);
        aicwf_dev_skb_free(usb_buf->skb);
        usb_buf->skb = NULL;
        aicwf_usb_rx_buf_put(usb_dev, usb_buf);

    }else{
    	atomic_inc(&rx_urb_cnt);
	}
    return ret;
}
#endif

static void aicwf_usb_rx_submit_all_urb(struct aic_usb_dev *usb_dev)
{
    struct aicwf_usb_buf *usb_buf;
//	int i = 0;

    if (usb_dev->state != USB_UP_ST) {
        AICWF_USB_STAT_INC(usb_dev, usb_rx_state_rejects);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "USB RX refill rejected in state:%d\n",
                             usb_dev->state);
        return;
    }

    while((usb_buf = aicwf_usb_rx_buf_get(usb_dev)) != NULL) {
        if (aicwf_usb_submit_rx_urb(usb_dev, usb_buf)) {
            AICWF_USB_STAT_INC(usb_dev, usb_rx_refill_failures);
            AICWFDBG_RATELIMITED(LOGERROR,
                                 "USB RX refill failed\n");
            if (usb_dev->state != USB_UP_ST)
                return;
            aicwf_usb_schedule_rx_refill(usb_dev,
                                         AICWF_USB_RX_RETRY_DELAY_MS);
            return;
        }
    }
}

#ifdef CONFIG_USB_MSG_IN_EP
static int aicwf_usb_submit_msg_rx_urb(struct aic_usb_dev *usb_dev,
                struct aicwf_usb_buf *usb_buf)
{
    struct sk_buff *skb;
    int ret;

    if (!usb_buf || !usb_dev)
        return -EINVAL;

    if (usb_dev->state != USB_UP_ST) {
        AICWF_USB_STAT_INC(usb_dev, usb_msg_rx_state_rejects);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "USB message RX rejected in state:%d\n",
                             usb_dev->state);
        aicwf_usb_msg_rx_buf_put(usb_dev, usb_buf);
        return -ESHUTDOWN;
    }

    skb = __dev_alloc_skb(AICWF_USB_MAX_PKT_SIZE, GFP_ATOMIC);
    if (!skb) {
        aicwf_usb_msg_rx_buf_put(usb_dev, usb_buf);
        return -ENOMEM;
    }

    usb_buf->skb = skb;

    usb_fill_bulk_urb(usb_buf->urb,
        usb_dev->udev,
        usb_dev->msg_in_pipe,
        skb->data, skb_tailroom(skb), aicwf_usb_msg_rx_complete, usb_buf);

    usb_buf->usbdev = usb_dev;

    usb_anchor_urb(usb_buf->urb, &usb_dev->msg_rx_submitted);
    ret = usb_submit_urb(usb_buf->urb, GFP_ATOMIC);
    if (ret) {
        AICWF_USB_STAT_INC(usb_dev, usb_msg_rx_submit_failures);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "USB message RX submit failed:%d\n", ret);
        usb_unanchor_urb(usb_buf->urb);
        aicwf_dev_skb_free(usb_buf->skb);
        usb_buf->skb = NULL;
        aicwf_usb_msg_rx_buf_put(usb_dev, usb_buf);

    }
    return ret;
}


static void aicwf_usb_msg_rx_submit_all_urb(struct aic_usb_dev *usb_dev)
{
    struct aicwf_usb_buf *usb_buf;

    if (usb_dev->state != USB_UP_ST) {
        AICWF_USB_STAT_INC(usb_dev, usb_msg_rx_state_rejects);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "USB message RX refill rejected in state:%d\n",
                             usb_dev->state);
        return;
    }

    while((usb_buf = aicwf_usb_msg_rx_buf_get(usb_dev)) != NULL) {
        if (aicwf_usb_submit_msg_rx_urb(usb_dev, usb_buf)) {
            AICWF_USB_STAT_INC(usb_dev, usb_msg_rx_refill_failures);
            AICWFDBG_RATELIMITED(LOGERROR,
                                 "USB message RX refill failed\n");
            if (usb_dev->state != USB_UP_ST)
                return;
            aicwf_usb_schedule_msg_rx_refill(
                usb_dev, AICWF_USB_RX_RETRY_DELAY_MS);
            return;
        }
    }
}
#endif

#ifdef CONFIG_USB_MSG_IN_EP
void aicwf_usb_msg_rx_submit_all_urb_(struct aic_usb_dev *usb_dev){
	aicwf_usb_msg_rx_submit_all_urb(usb_dev);
}
#endif

void aicwf_usb_rx_submit_all_urb_(struct aic_usb_dev *usb_dev){
	aicwf_usb_rx_submit_all_urb(usb_dev);
}


static void aicwf_usb_rx_prepare(struct aic_usb_dev *usb_dev)
{
    aicwf_usb_rx_submit_all_urb(usb_dev);
}

#ifdef CONFIG_USB_MSG_IN_EP
static void aicwf_usb_msg_rx_prepare(struct aic_usb_dev *usb_dev)
{
    aicwf_usb_msg_rx_submit_all_urb(usb_dev);
}
#endif


static void aicwf_usb_tx_prepare(struct aic_usb_dev *usb_dev)
{
    struct aicwf_usb_buf *usb_buf;

    while(!list_empty(&usb_dev->tx_post_list)){
        usb_buf = aicwf_usb_tx_dequeue(usb_dev, &usb_dev->tx_post_list,
            &usb_dev->tx_post_count, &usb_dev->tx_post_lock);
        usb_txc_sta_flowctrl(usb_buf, usb_dev);
        aicwf_usb_tx_payload_free(usb_buf);
#ifdef CONFIG_USB_TX_AGGR
        usb_buf->aggr_cnt = 0;
#endif
        aicwf_usb_tx_queue(usb_dev, &usb_dev->tx_free_list, usb_buf,
                &usb_dev->tx_free_count, &usb_dev->tx_free_lock);
    }
}

#ifdef CONFIG_USB_TX_AGGR
int aicwf_usb_send_pkt(struct aic_usb_dev *usb_dev, u8 *buf, uint buf_len)
{
    int ret = 0;
    struct aicwf_usb_buf *usb_buf;
    unsigned long flags;
    bool need_cfm = false;

    if (usb_dev->state != USB_UP_ST) {
        AICWF_USB_STAT_INC(usb_dev, usb_tx_state_rejects);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "USB TX rejected in state:%d\n",
                             usb_dev->state);
        return -EIO;
    }

    usb_buf = aicwf_usb_tx_dequeue(usb_dev, &usb_dev->tx_free_list,
                        &usb_dev->tx_free_count, &usb_dev->tx_free_lock);
    if (!usb_buf) {
        AICWF_USB_STAT_INC(usb_dev, usb_tx_no_buffers);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "USB TX buffer exhausted free:%d post:%d\n",
                             usb_dev->tx_free_count,
                             usb_dev->tx_post_count);
        ret = -ENOMEM;
        goto flow_ctrl;
    }

    usb_buf->skb = (struct sk_buff *)buf;
    usb_buf->usbdev = usb_dev;
    if (need_cfm)
        usb_buf->cfm = true;
    else
        usb_buf->cfm = false;
    AICWFDBG(LOGDEBUG, "%s len:%d\n", __func__, buf_len);
    if (READ_ONCE(aicwf_dbg_level) & LOGDATA)
        print_hex_dump(KERN_DEBUG, "aic8800 tx: ", DUMP_PREFIX_NONE,
                       16, 1, &buf[0], min_t(uint, buf_len, 32), false);
    usb_fill_bulk_urb(usb_buf->urb, usb_dev->udev, usb_dev->bulk_out_pipe,
                buf, buf_len, aicwf_usb_tx_complete, usb_buf);
    usb_buf->urb->transfer_flags |= URB_ZERO_PACKET;

    aicwf_usb_tx_queue(usb_dev, &usb_dev->tx_post_list, usb_buf,
                    &usb_dev->tx_post_count, &usb_dev->tx_post_lock);
    ret = 0;

    flow_ctrl:
    spin_lock_irqsave(&usb_dev->tx_flow_lock, flags);
    if (usb_dev->tx_free_count < AICWF_USB_TX_LOW_WATER) {
        if (!usb_dev->tbusy)
            AICWF_USB_STAT_INC(usb_dev, usb_flow_stops);
        usb_dev->tbusy = true;
        aicwf_usb_tx_flowctrl(usb_dev->rwnx_hw, true);
    }
    spin_unlock_irqrestore(&usb_dev->tx_flow_lock, flags);

    return ret;
}

int aicwf_usb_aggr(struct aicwf_tx_priv *tx_priv, struct sk_buff *pkt)
{
    struct rwnx_txhdr *txhdr = (struct rwnx_txhdr *)pkt->data;
    u8 usb_header[8];
    u8 adjust_str[4] = {0, 0, 0, 0};
    u32 curr_len = 0;
    int allign_len = 0;
    u32 data_len = (pkt->len - sizeof(struct rwnx_txhdr) + sizeof(struct txdesc_api)) + 4;

    usb_header[0] =(data_len & 0xff);
    usb_header[1] =((data_len >> 8)&0x0f);
    usb_header[2] =(data_len & 0xff);
    usb_header[3] =((data_len >> 8)&0x0f);

    usb_header[4] =(data_len & 0xff);
    usb_header[5] =((data_len >> 8)&0x0f);
    usb_header[6] = 0x01; //data
    usb_header[7] = 0; //reserved

    memcpy(tx_priv->tail, (u8 *)&usb_header, sizeof(usb_header));
    tx_priv->tail += sizeof(usb_header);
    //payload
    memcpy(tx_priv->tail, (u8 *)(long)&txhdr->sw_hdr->desc, sizeof(struct txdesc_api));
    tx_priv->tail += sizeof(struct txdesc_api); //hostdesc
    memcpy(tx_priv->tail, (u8 *)((u8 *)txhdr + txhdr->sw_hdr->headroom), pkt->len-txhdr->sw_hdr->headroom);
    tx_priv->tail += (pkt->len - txhdr->sw_hdr->headroom);

    //word alignment
    curr_len = tx_priv->tail - tx_priv->head;
    if (curr_len & (TX_ALIGNMENT - 1)) {
        allign_len = roundup(curr_len, TX_ALIGNMENT)-curr_len;
        memcpy(tx_priv->tail, adjust_str, allign_len);
        tx_priv->tail += allign_len;
    }

    tx_priv->aggr_buf->dev = pkt->dev;

    if(!txhdr->sw_hdr->need_cfm) {
        kmem_cache_free(txhdr->sw_hdr->rwnx_vif->rwnx_hw->sw_txhdr_cache, txhdr->sw_hdr);
        skb_pull(pkt, txhdr->sw_hdr->headroom);
        consume_skb(pkt);
    }

    atomic_inc(&tx_priv->aggr_count);
    return 0;
}

int aicwf_usb_send(struct aicwf_tx_priv *tx_priv)
{
    struct sk_buff *pkt;
    struct sk_buff *tx_buf;
    struct aic_usb_dev *usbdev = tx_priv->usbdev;
    struct aicwf_usb_buf *usb_buf;
    u8* buf;
    int ret = 0;
    int curr_len = 0;

    if (aicwf_is_framequeue_empty(&tx_priv->txq)) {
        ret = -1;
        AICWFDBG_RATELIMITED(LOGDEBUG, "USB TX queue is empty\n");
        return ret;
    }

    if (usbdev->state != USB_UP_ST) {
        AICWF_USB_STAT_INC(usbdev, usb_tx_state_rejects);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "USB TX rejected in state:%d\n",
                             usbdev->state);
        ret = -ENODEV;
        return ret;
    }
    usb_buf = aicwf_usb_tx_dequeue(usbdev, &usbdev->tx_free_list,
                        &usbdev->tx_free_count, &usbdev->tx_free_lock);
    if (!usb_buf) {
        AICWF_USB_STAT_INC(usbdev, usb_tx_no_buffers);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "USB TX buffer exhausted free:%d post:%d\n",
                             usbdev->tx_free_count,
                             usbdev->tx_post_count);
        ret = -ENOMEM;
        return ret;
    }

    usb_buf->aggr_cnt = 0;
    spin_lock_bh(&usbdev->tx_priv->txdlock);
    tx_priv->head = usb_buf->skb->data;
    tx_priv->tail = usb_buf->skb->data;

    while (!aicwf_is_framequeue_empty(&usbdev->tx_priv->txq)) {
        if (usbdev->state != USB_UP_ST) {
            AICWF_USB_STAT_INC(usbdev, usb_tx_state_rejects);
            AICWFDBG_RATELIMITED(LOGERROR,
                                 "USB aggregate TX stopped in state:%d\n",
                                 usbdev->state);
            ret = -ENODEV;
            break;
        }

        if (usb_buf->aggr_cnt == 10) {
            break;
        }
        spin_lock_bh(&usbdev->tx_priv->txqlock);
        pkt = aicwf_frame_dequeue(&usbdev->tx_priv->txq);
        if (pkt == NULL) {
            AICWFDBG_RATELIMITED(LOGDEBUG,
                                 "USB aggregate TX queue drained\n");
            spin_unlock_bh(&usbdev->tx_priv->txqlock);
            ret = -1;
            return ret;
        }
        atomic_dec(&usbdev->tx_priv->tx_pktcnt);
        spin_unlock_bh(&usbdev->tx_priv->txqlock);
        if(tx_priv==NULL || tx_priv->tail==NULL || pkt==NULL) {
            AICWFDBG(LOGERROR, "null error\n");
        }
        aicwf_usb_aggr(tx_priv, pkt);
        usb_buf->aggr_cnt++;
    }

    tx_buf = usb_buf->skb;
    buf = tx_buf->data;

    curr_len = tx_priv->tail - tx_priv->head;

    AICWFDBG(LOGDEBUG, "%s len:%d count:%d\n",
             __func__, curr_len, usb_buf->aggr_cnt);
    tx_buf->len = tx_priv->tail - tx_priv->head;
    spin_unlock_bh(&usbdev->tx_priv->txdlock);
    usb_fill_bulk_urb(usb_buf->urb, usbdev->udev, usbdev->bulk_out_pipe,
                buf, curr_len, aicwf_usb_tx_complete, usb_buf);
    usb_buf->urb->transfer_flags |= URB_ZERO_PACKET;

    aicwf_usb_tx_queue(usbdev, &usbdev->tx_post_list, usb_buf,
                    &usbdev->tx_post_count, &usbdev->tx_post_lock);
/*
    flow_ctrl:
    spin_lock_irqsave(&usbdev->tx_flow_lock, flags);
    if (usbdev->tx_free_count < AICWF_USB_TX_LOW_WATER) {
        usbdev->tbusy = true;
        aicwf_usb_tx_flowctrl(usbdev->rwnx_hw, true);
    }
    spin_unlock_irqrestore(&usbdev->tx_flow_lock, flags);
*/
    return ret;
}

#endif

static void aicwf_usb_tx_process(struct aic_usb_dev *usb_dev)
{
    struct aicwf_usb_buf *usb_buf;
    int ret = 0;

#ifdef CONFIG_USB_TX_AGGR
    if (!aicwf_is_framequeue_empty(&usb_dev->tx_priv->txq)) {
        if (aicwf_usb_send(usb_dev->tx_priv)) {
            AICWFDBG_RATELIMITED(LOGDEBUG,
                                 "%s deferred aggregate TX\n", __func__);
        }
    }
#endif

    while(!list_empty(&usb_dev->tx_post_list)) {

        if (usb_dev->state != USB_UP_ST) {
            AICWF_USB_STAT_INC(usb_dev, usb_tx_state_rejects);
            AICWFDBG_RATELIMITED(LOGERROR,
                                 "USB TX process stopped in state:%d\n",
                                 usb_dev->state);
            return;
        }

        usb_buf = aicwf_usb_tx_dequeue(usb_dev, &usb_dev->tx_post_list,
                        &usb_dev->tx_post_count, &usb_dev->tx_post_lock);
        if(!usb_buf) {
            usb_err("can not get usb_buf from tx_post_list!\n");
            return;
        }
        usb_anchor_urb(usb_buf->urb, &usb_dev->tx_submitted);
        ret = usb_submit_urb(usb_buf->urb, GFP_ATOMIC);
        if (ret) {
            usb_unanchor_urb(usb_buf->urb);
            AICWF_USB_STAT_INC(usb_dev, usb_tx_submit_failures);
            AICWFDBG_RATELIMITED(LOGERROR,
                                 "USB TX submit failed:%d\n", ret);
            #ifdef CONFIG_USB_TX_AGGR
            aicwf_usb_tx_queue(usb_dev, &usb_dev->tx_post_list, usb_buf,
                    &usb_dev->tx_post_count, &usb_dev->tx_post_lock);
            break;
            #else
            goto fail;
            #endif
        }

        continue;
#ifndef CONFIG_USB_TX_AGGR
fail:
        usb_txc_sta_flowctrl(usb_buf, usb_dev);
        aicwf_usb_tx_payload_free(usb_buf);
        aicwf_usb_tx_queue(usb_dev, &usb_dev->tx_free_list, usb_buf,
                    &usb_dev->tx_free_count, &usb_dev->tx_free_lock);
#endif
    }
}

#ifdef CONFIG_TX_TASKLET
void aicwf_tasklet_tx_process(unsigned long data)
{
	struct aic_usb_dev *usb_dev = (struct aic_usb_dev *)data;

	aicwf_usb_tx_process(usb_dev);
}
#endif

static inline void aic_thread_wait_stop(void)
{
#if 1// PLATFORM_LINUX
	#if 0
	while (!kthread_should_stop())
		rtw_msleep_os(10);
	#else
	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		schedule();
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
	#endif
#endif
}


int usb_bustx_thread(void *data)
{
    struct aicwf_bus *bus = (struct aicwf_bus *)data;
    struct aic_usb_dev *usbdev = bus->bus_priv.usb;

#ifdef CONFIG_TXRX_THREAD_PRIO
	if (bustx_thread_prio > 0) {
			struct sched_param param;
			param.sched_priority = (bustx_thread_prio < MAX_RT_PRIO)?bustx_thread_prio:(MAX_RT_PRIO-1);
			sched_setscheduler(current, SCHED_FIFO, &param);
	}
#endif
	AICWFDBG(LOGINFO, "%s the policy of current thread is:%d\n", __func__, current->policy);
	AICWFDBG(LOGINFO, "%s the rt_priority of current thread is:%d\n", __func__, current->rt_priority);
	AICWFDBG(LOGINFO, "%s the current pid is:%d\n", __func__, current->pid);


    while (1) {
		#if 0
        if(kthread_should_stop()) {
            usb_err("usb bustx thread stop 2\n");
            break;
        }
		#endif
        if (!wait_for_completion_interruptible(&bus->bustx_trgg)) {
            if(usbdev->bus_if->state == BUS_DOWN_ST){
				AICWFDBG(LOGINFO, "usb bustx thread will to stop\n");
                break;
			}
            #ifdef CONFIG_USB_TX_AGGR
            if ((usbdev->tx_post_count > 0) || !aicwf_is_framequeue_empty(&usbdev->tx_priv->txq))
            #else
            if (usbdev->tx_post_count > 0)
            #endif
                aicwf_usb_tx_process(usbdev);
        }
    }

	aic_thread_wait_stop();
	AICWFDBG(LOGINFO, "usb bustx thread stop\n");

    return 0;
}

int usb_busrx_thread(void *data)
{
    struct aicwf_rx_priv *rx_priv = (struct aicwf_rx_priv *)data;
    struct aicwf_bus *bus_if = rx_priv->usbdev->bus_if;

#ifdef CONFIG_TXRX_THREAD_PRIO
	if (busrx_thread_prio > 0) {
			struct sched_param param;
			param.sched_priority = (busrx_thread_prio < MAX_RT_PRIO)?busrx_thread_prio:(MAX_RT_PRIO-1);
			sched_setscheduler(current, SCHED_FIFO, &param);
	}
#endif
	AICWFDBG(LOGINFO, "%s the policy of current thread is:%d\n", __func__, current->policy);
	AICWFDBG(LOGINFO, "%s the rt_priority of current thread is:%d\n", __func__, current->rt_priority);
	AICWFDBG(LOGINFO, "%s the current pid is:%d\n", __func__, current->pid);

    while (1) {
#if 0
        if(kthread_should_stop()) {
            usb_err("usb busrx thread stop 2\n");
            break;
        }
#endif
		//rx_priv->rx_thread_working = 0;//AIDEN
        if (!wait_for_completion_interruptible(&bus_if->busrx_trgg)) {
            if(bus_if->state == BUS_DOWN_ST){
				AICWFDBG(LOGINFO, "usb busrx thread will to stop\n");
				break;
            }
			//rx_priv->rx_thread_working = 1;//AIDEN
            aicwf_process_rxframes(rx_priv);
        }
    }

	aic_thread_wait_stop();
	AICWFDBG(LOGINFO, "usb busrx thread stop\n");

    return 0;
}

#ifdef CONFIG_USB_MSG_IN_EP
int usb_msg_busrx_thread(void *data)
{
    struct aicwf_rx_priv *rx_priv = (struct aicwf_rx_priv *)data;
    struct aicwf_bus *bus_if = rx_priv->usbdev->bus_if;

#ifdef CONFIG_TXRX_THREAD_PRIO
			if (busrx_thread_prio > 0) {
					struct sched_param param;
					param.sched_priority = (busrx_thread_prio < MAX_RT_PRIO)?busrx_thread_prio:(MAX_RT_PRIO-1);
					sched_setscheduler(current, SCHED_FIFO, &param);
			}
#endif
			AICWFDBG(LOGINFO, "%s the policy of current thread is:%d\n", __func__, current->policy);
			AICWFDBG(LOGINFO, "%s the rt_priority of current thread is:%d\n", __func__, current->rt_priority);
			AICWFDBG(LOGINFO, "%s the current pid is:%d\n", __func__, current->pid);



    while (1) {
        if(kthread_should_stop()) {
            usb_err("usb msg busrx thread stop\n");
            break;
        }
        if (!wait_for_completion_interruptible(&bus_if->msg_busrx_trgg)) {
            if(bus_if->state == BUS_DOWN_ST)
                break;
            aicwf_process_msg_rxframes(rx_priv);
        }
    }

    return 0;
}
#endif


static void aicwf_usb_send_msg_complete(struct urb *urb)
{
    struct aic_usb_dev *usb_dev = (struct aic_usb_dev *) urb->context;

    usb_dev->msg_status = urb->status;
    if (urb->status) {
        AICWF_USB_STAT_INC(usb_dev, usb_msg_tx_completion_errors);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "USB control TX completion failed:%d\n",
                             urb->status);
    }
    usb_dev->msg_finished = true;
    if (waitqueue_active(&usb_dev->msg_wait))
        wake_up(&usb_dev->msg_wait);
}

static int aicwf_usb_bus_txmsg(struct device *dev, u8 *buf, u32 len)
{
    int ret = 0;
    struct aicwf_bus *bus_if = dev_get_drvdata(dev);
    struct aic_usb_dev *usb_dev = bus_if->bus_priv.usb;

    if (buf == NULL || len == 0 || usb_dev->msg_out_urb == NULL)
        return -EINVAL;

    mutex_lock(&usb_dev->msg_tx_lock);

    if (usb_dev->state != USB_UP_ST) {
        AICWF_USB_STAT_INC(usb_dev, usb_msg_tx_state_rejects);
        ret = -ESHUTDOWN;
        goto exit;
    }

    usb_dev->msg_finished = false;
    usb_dev->msg_status = -EINPROGRESS;

#ifdef CONFIG_USB_MSG_OUT_EP
    if (usb_dev->msg_out_pipe) {
        usb_fill_bulk_urb(usb_dev->msg_out_urb,
            usb_dev->udev,
            usb_dev->msg_out_pipe,
            buf, len, (usb_complete_t) aicwf_usb_send_msg_complete, usb_dev);
    } else {
        usb_fill_bulk_urb(usb_dev->msg_out_urb,
            usb_dev->udev,
            usb_dev->bulk_out_pipe,
            buf, len, (usb_complete_t) aicwf_usb_send_msg_complete, usb_dev);
    }
#else
    usb_fill_bulk_urb(usb_dev->msg_out_urb,
        usb_dev->udev,
        usb_dev->bulk_out_pipe,
        buf, len, (usb_complete_t) aicwf_usb_send_msg_complete, usb_dev);
#endif
    #if defined CONFIG_USB_NO_TRANS_DMA_MAP
    usb_dev->msg_out_urb->transfer_dma = usb_dev->cmd_dma_trans_addr;
    usb_dev->msg_out_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
    #endif
    usb_dev->msg_out_urb->transfer_flags |= URB_ZERO_PACKET;

    ret = usb_submit_urb(usb_dev->msg_out_urb, GFP_ATOMIC);
    if (ret) {
        AICWF_USB_STAT_INC(usb_dev, usb_msg_tx_submit_failures);
        usb_err("usb_submit_urb failed %d\n", ret);
        goto exit;
    }

    ret = wait_event_timeout(usb_dev->msg_wait,
        usb_dev->msg_finished, msecs_to_jiffies(CMD_TX_TIMEOUT));
    if (!ret) {
        if (usb_dev->msg_out_urb)
            usb_kill_urb(usb_dev->msg_out_urb);
        usb_err("Txmsg wait timed out\n");
        ret = -ETIMEDOUT;
        goto exit;
    }
    ret = usb_dev->msg_status;
exit:
    mutex_unlock(&usb_dev->msg_tx_lock);
    return ret;
}


static void aicwf_usb_free_urb(struct list_head *q, spinlock_t *qlock)
{
    struct aicwf_usb_buf *usb_buf, *tmp;
    unsigned long flags;

    spin_lock_irqsave(qlock, flags);
    list_for_each_entry_safe(usb_buf, tmp, q, list) {
    spin_unlock_irqrestore(qlock, flags);
        if (!usb_buf->urb) {
            usb_err("bad usb_buf\n");
            spin_lock_irqsave(qlock, flags);
            break;
        }
        #ifdef CONFIG_USB_TX_AGGR
        if (usb_buf->skb) {
            dev_kfree_skb(usb_buf->skb);
        }
        #endif
        usb_free_urb(usb_buf->urb);
        #if defined CONFIG_USB_NO_TRANS_DMA_MAP
        // free dma buf if needed
        if (usb_buf->data_buf) {
            #if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35))
            usb_free_coherent(usb_buf->usbdev->udev, DATA_BUF_MAX, usb_buf->data_buf, usb_buf->data_dma_trans_addr);
            #else
            usb_buffer_free(usb_buf->usbdev->udev, DATA_BUF_MAX, usb_buf->data_buf, usb_buf->data_dma_trans_addr);
            #endif
            usb_buf->data_buf = NULL;
            usb_buf->data_dma_trans_addr = 0x0;
        }
        #endif
        list_del_init(&usb_buf->list);
        spin_lock_irqsave(qlock, flags);
    }
    spin_unlock_irqrestore(qlock, flags);
}

static int aicwf_usb_alloc_rx_urb(struct aic_usb_dev *usb_dev)
{
    int i;

	AICWFDBG(LOGINFO, "%s AICWF_USB_RX_URBS:%d \r\n", __func__, AICWF_USB_RX_URBS);
    for (i = 0; i < AICWF_USB_RX_URBS; i++) {
        struct aicwf_usb_buf *usb_buf = &usb_dev->usb_rx_buf[i];

        usb_buf->usbdev = usb_dev;
        usb_buf->urb = usb_alloc_urb(0, GFP_KERNEL);
        if (!usb_buf->urb) {
            usb_err("could not allocate rx data urb\n");
            goto err;
        }
        #if defined CONFIG_USB_NO_TRANS_DMA_MAP
        // dma buf unused
        usb_buf->data_buf = NULL;
        usb_buf->data_dma_trans_addr = 0x0;
        #endif
        list_add_tail(&usb_buf->list, &usb_dev->rx_free_list);
    }
    return 0;

err:
    aicwf_usb_free_urb(&usb_dev->rx_free_list, &usb_dev->rx_free_lock);
    return -ENOMEM;
}

static int aicwf_usb_alloc_tx_urb(struct aic_usb_dev *usb_dev)
{
    int i;

	AICWFDBG(LOGINFO, "%s AICWF_USB_TX_URBS:%d \r\n", __func__, AICWF_USB_TX_URBS);
    for (i = 0; i < AICWF_USB_TX_URBS; i++) {
        struct aicwf_usb_buf *usb_buf = &usb_dev->usb_tx_buf[i];

        usb_buf->usbdev = usb_dev;
        usb_buf->urb = usb_alloc_urb(0, GFP_KERNEL);
        if (!usb_buf->urb) {
            usb_err("could not allocate tx data urb\n");
            goto err;
        }
        #ifdef CONFIG_USB_TX_AGGR
        usb_buf->skb = dev_alloc_skb(MAX_USB_AGGR_TXPKT_LEN);
        #endif
        #if defined CONFIG_USB_NO_TRANS_DMA_MAP
        // alloc dma buf
        #if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35))
        usb_buf->data_buf = usb_alloc_coherent(usb_dev->udev, DATA_BUF_MAX, (in_interrupt() ? GFP_ATOMIC : GFP_KERNEL), &usb_buf->data_dma_trans_addr);
        #else
        usb_buf->data_buf = usb_buffer_alloc(usb_dev->udev, DATA_BUF_MAX, (in_interrupt() ? GFP_ATOMIC : GFP_KERNEL), &usb_buf->data_dma_trans_addr);
        #endif
        if (usb_buf->data_buf == NULL) {
            usb_err("could not allocate tx data dma buf\n");
            goto err;
        }
        #endif
        list_add_tail(&usb_buf->list, &usb_dev->tx_free_list);
        (usb_dev->tx_free_count)++;
    }
    return 0;

err:
    aicwf_usb_free_urb(&usb_dev->tx_free_list, &usb_dev->tx_free_lock);
    return -ENOMEM;
}

#ifdef CONFIG_USB_MSG_IN_EP
static int aicwf_usb_alloc_msg_rx_urb(struct aic_usb_dev *usb_dev)
{
    int i;
    
    AICWFDBG(LOGINFO, "%s AICWF_USB_MSG_RX_URBS:%d \r\n", __func__, AICWF_USB_MSG_RX_URBS);

    for (i = 0; i < AICWF_USB_MSG_RX_URBS; i++) {
        struct aicwf_usb_buf *usb_buf = &usb_dev->usb_msg_rx_buf[i];

        usb_buf->usbdev = usb_dev;
        usb_buf->urb = usb_alloc_urb(0, GFP_KERNEL);
        if (!usb_buf->urb) {
            usb_err("could not allocate rx data urb\n");
            goto err;
        }
        list_add_tail(&usb_buf->list, &usb_dev->msg_rx_free_list);
    }
    return 0;

err:
    aicwf_usb_free_urb(&usb_dev->msg_rx_free_list, &usb_dev->msg_rx_free_lock);
    return -ENOMEM;
}
#endif

static void aicwf_usb_state_change(struct aic_usb_dev *usb_dev, int state)
{
    if (usb_dev->state == state)
        return;

    usb_dev->state = state;

    if (state == USB_DOWN_ST) {
        usb_dev->bus_if->state = BUS_DOWN_ST;
    }
    if (state == USB_UP_ST) {
        usb_dev->bus_if->state = BUS_UP_ST;
    }
}

int align_param = 8;
module_param(align_param, int, 0660);

#ifndef CONFIG_USB_TX_AGGR
static void usb_tx_flow_ctrl(struct rwnx_sw_txhdr *sw_hdr,
                            struct aic_usb_dev *usb_dev,
                            struct rwnx_hw *rwnx_hw)
{
#ifdef CONFIG_PER_STA_FC
	struct rwnx_sta *sta;
	u8 sta_idx;
	unsigned long flags;

	//printk("txdata: sta %d\n", sw_hdr->desc.host.staid);
	sta_idx = sw_hdr->desc.host.staid;
	if(sta_idx < NX_REMOTE_STA_MAX && !(sw_hdr->desc.host.flags & TXU_CNTRL_MGMT)) {
		struct rwnx_vif *vif = NULL;
		sta = &rwnx_hw->sta_table[sta_idx];
		if (sta->vif_idx >= ARRAY_SIZE(rwnx_hw->vif_table))
			return;
		vif = rwnx_hw->vif_table[sta->vif_idx];
		if (!vif)
			return;
		spin_lock_irqsave(&usb_dev->tx_flow_lock, flags);
		atomic_inc(&rwnx_hw->sta_flowctrl[sta_idx].tx_pending_cnt);
		//printk("sta %d pending %d >= 64, flowctrl=%d\n", sta->sta_idx, sta->tx_pending_cnt, sta->flowctrl);
		if(RWNX_VIF_TYPE(vif) == NL80211_IFTYPE_AP) {
			if((atomic_read(&rwnx_hw->sta_flowctrl[sta_idx].tx_pending_cnt) >= AICWF_USB_FC_PERSTA_HIGH_WATER && rwnx_hw->sta_flowctrl[sta_idx].flowctrl == 0) ||
										rwnx_hw->sta_flowctrl[sta_idx].flowctrl) {
				//AICWFDBG(LOGDEBUG, "sta 0x%x:0x%x, %d pending %d, stop\n", sta->mac_addr[4], sta->mac_addr[5], sta->sta_idx, atomic_read(&rwnx_hw->sta_flowctrl[sta_idx].tx_pending_cnt));
				if(!usb_dev->tbusy)
					rwnx_stop_sta_all_queues(sta, usb_dev->rwnx_hw);
				rwnx_hw->sta_flowctrl[sta_idx].flowctrl = 1;
			}
		}
		spin_unlock_irqrestore(&usb_dev->tx_flow_lock, flags);
	}
#endif
}
#endif

#ifdef CONFIG_USB_TX_AGGR
static int aicwf_usb_bus_txdata(struct device *dev, struct sk_buff *pkt)
{
    uint prio;
    int ret = -EBADE;
    struct aicwf_bus *bus_if = dev_get_drvdata(dev);
    struct aic_usb_dev *usbdev = bus_if->bus_priv.usb;

    //printk("%s\n", __func__);
    prio = (pkt->priority & 0x7);
    spin_lock_bh(&usbdev->tx_priv->txqlock);
    if (!aicwf_frame_enq(usbdev->dev, &usbdev->tx_priv->txq, pkt, prio)) {
        aicwf_dev_skb_free(pkt);
        spin_unlock_bh(&usbdev->tx_priv->txqlock);
        return -ENOSR;
    } else {
        ret = 0;
    }

    if (bus_if->state != BUS_UP_ST) {
        usb_err("bus_if stopped\n");
        spin_unlock_bh(&usbdev->tx_priv->txqlock);
        return -1;
    }

    atomic_inc(&usbdev->tx_priv->tx_pktcnt);
    spin_unlock_bh(&usbdev->tx_priv->txqlock);
    complete(&bus_if->bustx_trgg);

    return ret;
}

#else
static int aicwf_usb_bus_txdata(struct device *dev, struct sk_buff *skb)
{
    u8 *buf;
    u16 buf_len = 0;
    u16 adjust_len = 0;
    struct aicwf_usb_buf *usb_buf;
    int ret = 0;
    unsigned long flags;
    struct aicwf_bus *bus_if = dev_get_drvdata(dev);
    struct aic_usb_dev *usb_dev = bus_if->bus_priv.usb;
    struct rwnx_txhdr *txhdr = (struct rwnx_txhdr *)skb->data;
    struct rwnx_sw_txhdr *sw_hdr = txhdr->sw_hdr;
    struct rwnx_hw *rwnx_hw = usb_dev->rwnx_hw;
    u8 usb_header[4];
    u8 adj_buf[4] = {0};
    u16 index = 0;
    bool need_cfm = false;
#ifdef CONFIG_USB_ALIGN_DATA//AIDEN
	int align;
#endif

    if (usb_dev->state != USB_UP_ST) {
        AICWF_USB_STAT_INC(usb_dev, usb_tx_state_rejects);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "USB TX rejected in state:%d\n",
                             usb_dev->state);
        kmem_cache_free(rwnx_hw->sw_txhdr_cache, sw_hdr);
        dev_kfree_skb_any(skb);
        return -EIO;
    }

    usb_buf = aicwf_usb_tx_dequeue(usb_dev, &usb_dev->tx_free_list,
                        &usb_dev->tx_free_count, &usb_dev->tx_free_lock);
    if (!usb_buf) {
        AICWF_USB_STAT_INC(usb_dev, usb_tx_no_buffers);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "USB TX buffer exhausted free:%d post:%d\n",
                             usb_dev->tx_free_count,
                             usb_dev->tx_post_count);
        kmem_cache_free(rwnx_hw->sw_txhdr_cache, sw_hdr);
        dev_kfree_skb_any(skb);
        ret = -ENOMEM;
        goto flow_ctrl;
    }

    if (sw_hdr->headroom > skb->len) {
        AICWFDBG_RATELIMITED(LOGERROR,
                             "USB TX invalid headroom:%u len:%u\n",
                             sw_hdr->headroom, skb->len);
        kmem_cache_free(rwnx_hw->sw_txhdr_cache, sw_hdr);
        dev_kfree_skb_any(skb);
        aicwf_usb_tx_queue(usb_dev, &usb_dev->tx_free_list, usb_buf,
                           &usb_dev->tx_free_count,
                           &usb_dev->tx_free_lock);
        ret = -EINVAL;
        goto flow_ctrl;
    }

    if (sw_hdr->need_cfm) {
        need_cfm = true;
        #if defined CONFIG_USB_NO_TRANS_DMA_MAP
        buf = usb_buf->data_buf;
        #else
        buf = kmalloc(roundup((size_t)skb->len + 1, TX_ALIGNMENT) + 1,
                      GFP_ATOMIC);
        #endif
        if (!buf) {
            AICWF_USB_STAT_INC(usb_dev, usb_tx_no_buffers);
            AICWFDBG_RATELIMITED(LOGERROR,
                                 "USB TX confirmation buffer allocation failed\n");
            kmem_cache_free(rwnx_hw->sw_txhdr_cache, sw_hdr);
            dev_kfree_skb_any(skb);
            aicwf_usb_tx_queue(usb_dev, &usb_dev->tx_free_list, usb_buf,
                               &usb_dev->tx_free_count,
                               &usb_dev->tx_free_lock);
            ret = -ENOMEM;
            goto flow_ctrl;
        }
        index += sizeof(usb_header);
        memcpy(&buf[index], (u8 *)(long)&sw_hdr->desc, sizeof(struct txdesc_api));
        index += sizeof(struct txdesc_api);
        memcpy(&buf[index], &skb->data[sw_hdr->headroom],
               skb->len - sw_hdr->headroom);
        index += skb->len - sw_hdr->headroom;
        buf_len = index;
        if (buf_len & (TX_ALIGNMENT - 1)) {
            adjust_len = roundup(buf_len, TX_ALIGNMENT)-buf_len;
            memcpy(&buf[buf_len], adj_buf, adjust_len);
            buf_len += adjust_len;
        }
        usb_header[0] =((buf_len) & 0xff);
        usb_header[1] =(((buf_len) >> 8)&0x0f);
        usb_header[2] = 0x01; //data
        usb_header[3] = 0; //reserved
        memcpy(&buf[0], usb_header, sizeof(usb_header));
        usb_buf->skb = (struct sk_buff *)buf;
    } else {
        skb_pull(skb, sw_hdr->headroom);
        skb_push(skb, sizeof(struct txdesc_api));
        memcpy(&skb->data[0], (u8 *)(long)&sw_hdr->desc,
               sizeof(struct txdesc_api));

        skb_push(skb, sizeof(usb_header));
        usb_header[0] =((skb->len) & 0xff);
        usb_header[1] =(((skb->len) >> 8)&0x0f);
        usb_header[2] = 0x01; //data
        usb_header[3] = 0; //reserved
        memcpy(&skb->data[0], usb_header, sizeof(usb_header));

        #if defined CONFIG_USB_NO_TRANS_DMA_MAP
        buf = usb_buf->data_buf;
        memcpy(&buf[0], skb->data, skb->len);
        #else
        buf = skb->data;
        #endif
        buf_len = skb->len;

        usb_buf->skb = skb;
    }
    usb_buf->usbdev = usb_dev;
    if (need_cfm)
        usb_buf->cfm = true;
    else
        usb_buf->cfm = false;


#ifndef CONFIG_USE_USB_ZERO_PACKET
	if((buf_len % 512) == 0){
		AICWFDBG(LOGDEBUG, "%s append zero-packet byte len:%d\r\n",
		         __func__, buf_len);
		if(need_cfm){
			buf[buf_len] = 0x00;
			buf_len = buf_len + 1;
		}else{
			skb_put(skb, 1);
			skb->data[buf_len] = 0x00;
			buf = skb->data;
       		buf_len = skb->len;
		}
	}
#endif

#ifdef CONFIG_USB_ALIGN_DATA
    #if defined CONFIG_USB_NO_TRANS_DMA_MAP
    #error "CONFIG_USB_NO_TRANS_DMA_MAP not supported"
    #endif
	usb_buf->usb_align_data = (u8*)kmalloc(sizeof(u8) * buf_len + align_param, GFP_ATOMIC);
	if (!usb_buf->usb_align_data) {
        AICWF_USB_STAT_INC(usb_dev, usb_tx_no_buffers);
        AICWFDBG_RATELIMITED(LOGERROR,
                             "USB TX alignment buffer allocation failed\n");
        aicwf_usb_tx_payload_free(usb_buf);
        kmem_cache_free(rwnx_hw->sw_txhdr_cache, sw_hdr);
        if (need_cfm)
            dev_kfree_skb_any(skb);
        aicwf_usb_tx_queue(usb_dev, &usb_dev->tx_free_list, usb_buf,
                           &usb_dev->tx_free_count,
                           &usb_dev->tx_free_lock);
        ret = -ENOMEM;
        goto flow_ctrl;
    }

	align = ((unsigned long)(usb_buf->usb_align_data)) & (align_param - 1);
	memcpy(usb_buf->usb_align_data + (align_param - align), buf, buf_len);

    usb_fill_bulk_urb(usb_buf->urb, usb_dev->udev, usb_dev->bulk_out_pipe,
                usb_buf->usb_align_data + (align_param - align), buf_len, aicwf_usb_tx_complete, usb_buf);
#else
	usb_fill_bulk_urb(usb_buf->urb, usb_dev->udev, usb_dev->bulk_out_pipe,
			buf, buf_len, aicwf_usb_tx_complete, usb_buf);
#endif

    usb_tx_flow_ctrl(sw_hdr, usb_dev, rwnx_hw);
    if (!need_cfm)
        kmem_cache_free(rwnx_hw->sw_txhdr_cache, sw_hdr);

    #if defined CONFIG_USB_NO_TRANS_DMA_MAP
    usb_buf->urb->transfer_dma = usb_buf->data_dma_trans_addr;
    usb_buf->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
    #endif
#ifdef CONFIG_USE_USB_ZERO_PACKET
    usb_buf->urb->transfer_flags |= URB_ZERO_PACKET;
#endif

    aicwf_usb_tx_queue(usb_dev, &usb_dev->tx_post_list, usb_buf,
                    &usb_dev->tx_post_count, &usb_dev->tx_post_lock);

#ifdef CONFIG_TX_TASKLET
	tasklet_schedule(&usb_dev->xmit_tasklet);
#else
	complete(&bus_if->bustx_trgg);
#endif

    ret = 0;

    flow_ctrl:
    spin_lock_irqsave(&usb_dev->tx_flow_lock, flags);
    if (usb_dev->tx_free_count < AICWF_USB_TX_LOW_WATER) {
		AICWFDBG(LOGDEBUG, "usb_dev->tx_free_count < AICWF_USB_TX_LOW_WATER:%d\r\n",
			usb_dev->tx_free_count);
        if (!usb_dev->tbusy)
            AICWF_USB_STAT_INC(usb_dev, usb_flow_stops);
        usb_dev->tbusy = true;
        aicwf_usb_tx_flowctrl(usb_dev->rwnx_hw, true);
    }
    spin_unlock_irqrestore(&usb_dev->tx_flow_lock, flags);

    return ret;
}
#endif
static int aicwf_usb_bus_start(struct device *dev)
{
    struct aicwf_bus *bus_if = dev_get_drvdata(dev);
    struct aic_usb_dev *usb_dev = bus_if->bus_priv.usb;

    if (usb_dev->state == USB_UP_ST)
        return 0;

    aicwf_usb_state_change(usb_dev, USB_UP_ST);
    aicwf_usb_rx_prepare(usb_dev);
    aicwf_usb_tx_prepare(usb_dev);
#ifdef CONFIG_USB_MSG_IN_EP
	if(usb_dev->chipid != PRODUCT_ID_AIC8801){
		aicwf_usb_msg_rx_prepare(usb_dev);
	}
#endif

    return 0;
}

static void aicwf_usb_cancel_all_urbs_(struct aic_usb_dev *usb_dev)
{
    if (usb_dev->msg_out_urb)
        usb_kill_urb(usb_dev->msg_out_urb);

    usb_kill_anchored_urbs(&usb_dev->tx_submitted);
    usb_kill_anchored_urbs(&usb_dev->rx_submitted);
#ifdef CONFIG_USB_MSG_IN_EP
	if(usb_dev->chipid != PRODUCT_ID_AIC8801){
   		usb_kill_anchored_urbs(&usb_dev->msg_rx_submitted);
	}
#endif
    aicwf_usb_tx_prepare(usb_dev);
}

void aicwf_usb_cancel_all_urbs(struct aic_usb_dev *usb_dev){
	aicwf_usb_cancel_all_urbs_(usb_dev);
}


static void aicwf_usb_bus_stop(struct device *dev)
{
    struct aicwf_bus *bus_if = dev_get_drvdata(dev);
    struct aic_usb_dev *usb_dev = bus_if->bus_priv.usb;

	AICWFDBG(LOGINFO, "%s\r\n", __func__);
    if (usb_dev == NULL)
        return;

    if (usb_dev->state == USB_DOWN_ST)
        return;

    if (g_rwnx_plat && g_rwnx_plat->wait_disconnect_cb) {
            atomic_set(&aicwf_deinit_atomic, 1);
            up(&aicwf_deinit_sem);
    }
    aicwf_usb_state_change(usb_dev, USB_DOWN_ST);
    //aicwf_usb_cancel_all_urbs(usb_dev);//AIDEN
}

static void aicwf_usb_deinit(struct aic_usb_dev *usbdev)
{
    cancel_delayed_work_sync(&usbdev->rx_urb_work);
    aicwf_usb_free_urb(&usbdev->rx_free_list, &usbdev->rx_free_lock);
    aicwf_usb_free_urb(&usbdev->tx_free_list, &usbdev->tx_free_lock);
#ifdef CONFIG_USB_MSG_IN_EP
	if(usbdev->chipid != PRODUCT_ID_AIC8801){
		cancel_delayed_work_sync(&usbdev->msg_rx_urb_work);
		aicwf_usb_free_urb(&usbdev->msg_rx_free_list, &usbdev->msg_rx_free_lock);
	}
#endif

    usb_free_urb(usbdev->msg_out_urb);
}

static void aicwf_usb_rx_urb_work(struct work_struct *work)
{
    struct aic_usb_dev *usb_dev =
        container_of(to_delayed_work(work), struct aic_usb_dev,
                     rx_urb_work);

    aicwf_usb_rx_submit_all_urb(usb_dev);
}

#ifdef CONFIG_USB_MSG_IN_EP
static void aicwf_usb_msg_rx_urb_work(struct work_struct *work)
{
    struct aic_usb_dev *usb_dev =
        container_of(to_delayed_work(work), struct aic_usb_dev,
                     msg_rx_urb_work);

    aicwf_usb_msg_rx_submit_all_urb(usb_dev);
}
#endif

static int aicwf_usb_init(struct aic_usb_dev *usb_dev)
{
    int ret = 0;

    usb_dev->tbusy = false;
    usb_dev->state = USB_DOWN_ST;

    INIT_DELAYED_WORK(&usb_dev->rx_urb_work, aicwf_usb_rx_urb_work);
#ifdef CONFIG_USB_MSG_IN_EP
	if(usb_dev->chipid != PRODUCT_ID_AIC8801)
		INIT_DELAYED_WORK(&usb_dev->msg_rx_urb_work,
                          aicwf_usb_msg_rx_urb_work);
#endif

    init_waitqueue_head(&usb_dev->msg_wait);
    mutex_init(&usb_dev->msg_tx_lock);
    init_usb_anchor(&usb_dev->rx_submitted);
    init_usb_anchor(&usb_dev->tx_submitted);
#ifdef CONFIG_USB_MSG_IN_EP
	if(usb_dev->chipid != PRODUCT_ID_AIC8801){
		init_usb_anchor(&usb_dev->msg_rx_submitted);
	}
#endif

    spin_lock_init(&usb_dev->tx_free_lock);
    spin_lock_init(&usb_dev->tx_post_lock);
    spin_lock_init(&usb_dev->rx_free_lock);
    spin_lock_init(&usb_dev->tx_flow_lock);
#ifdef CONFIG_USB_MSG_IN_EP
	if(usb_dev->chipid != PRODUCT_ID_AIC8801){
		spin_lock_init(&usb_dev->msg_rx_free_lock);
	}
#endif

    INIT_LIST_HEAD(&usb_dev->rx_free_list);
    INIT_LIST_HEAD(&usb_dev->tx_free_list);
    INIT_LIST_HEAD(&usb_dev->tx_post_list);
#ifdef CONFIG_USB_MSG_IN_EP
	if(usb_dev->chipid != PRODUCT_ID_AIC8801){
		INIT_LIST_HEAD(&usb_dev->msg_rx_free_list);
	}
#endif

	atomic_set(&rx_urb_cnt, 0);

    usb_dev->tx_free_count = 0;
    usb_dev->tx_post_count = 0;

    ret =  aicwf_usb_alloc_rx_urb(usb_dev);
    if (ret) {
        goto error;
    }
    ret =  aicwf_usb_alloc_tx_urb(usb_dev);
    if (ret) {
        goto error;
    }
#ifdef CONFIG_USB_MSG_IN_EP
	if(usb_dev->chipid != PRODUCT_ID_AIC8801){
		ret =  aicwf_usb_alloc_msg_rx_urb(usb_dev);
		if (ret) {
			goto error;
		}
	}
#endif


    usb_dev->msg_out_urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!usb_dev->msg_out_urb) {
        usb_err("usb_alloc_urb (msg out) failed\n");
        ret = -ENOMEM;
        goto error;
    }

    return ret;
    error:
    usb_err("failed!\n");
    aicwf_usb_deinit(usb_dev);
    return ret;
}


static int aicwf_parse_usb(struct aic_usb_dev *usb_dev, struct usb_interface *interface)
{
    struct usb_interface_descriptor *interface_desc;
    struct usb_host_interface *host_interface;
    struct usb_endpoint_descriptor *endpoint;
    struct usb_device *usb = usb_dev->udev;
    int i, endpoints;
    u8 endpoint_num;
    int ret = 0;

    usb_dev->bulk_in_pipe = 0;
    usb_dev->bulk_out_pipe = 0;
#ifdef CONFIG_USB_MSG_OUT_EP
    usb_dev->msg_out_pipe = 0;
#endif
#ifdef CONFIG_USB_MSG_IN_EP
	usb_dev->msg_in_pipe = 0;
#endif

    host_interface = &interface->altsetting[0];
    interface_desc = &host_interface->desc;
    endpoints = interface_desc->bNumEndpoints;
	AICWFDBG(LOGINFO, "%s endpoints = %d\n", __func__, endpoints);

    /* Check device configuration */
    if (usb->descriptor.bNumConfigurations != 1) {
        usb_err("Number of configurations: %d not supported\n",
                        usb->descriptor.bNumConfigurations);
        ret = -ENODEV;
        goto exit;
    }

    /* Check deviceclass */
#ifndef CONFIG_USB_BT
    if (usb->descriptor.bDeviceClass != 0x00) {
        usb_err("DeviceClass %d not supported\n",
            usb->descriptor.bDeviceClass);
        ret = -ENODEV;
        goto exit;
    }
#endif

    /* Check interface number */
#ifdef CONFIG_USB_BT
    if (usb->actconfig->desc.bNumInterfaces != 3) {
#else
    if (usb->actconfig->desc.bNumInterfaces != 1) {
#endif
		AICWFDBG(LOGINFO,
		         "USB interface layout count:%d selecting compatible chip mode\n",
		         usb->actconfig->desc.bNumInterfaces);
		if(usb_dev->chipid == PRODUCT_ID_AIC8800DC){
			AICWFDBG(LOGINFO, "AIC8800DC compatible mode: AIC8800DW\n");
			usb_dev->chipid = PRODUCT_ID_AIC8800DW;
        }else if (usb_dev->chipid == PRODUCT_ID_AIC8800DW) {
			AICWFDBG(LOGDEBUG, "AIC8800DW interface layout confirmed\n");
		}
    }

    if ((interface_desc->bInterfaceClass != USB_CLASS_VENDOR_SPEC) ||
        (interface_desc->bInterfaceSubClass != 0xff) ||
        (interface_desc->bInterfaceProtocol != 0xff)) {
        usb_err("non WLAN interface %d: 0x%x:0x%x:0x%x\n",
            interface_desc->bInterfaceNumber, interface_desc->bInterfaceClass,
            interface_desc->bInterfaceSubClass, interface_desc->bInterfaceProtocol);
        ret = -ENODEV;
        goto exit;
    }

    for (i = 0; i < endpoints; i++) {
        endpoint = &host_interface->endpoint[i].desc;
        endpoint_num = usb_endpoint_num(endpoint);

        if (usb_endpoint_dir_in(endpoint) &&
            usb_endpoint_xfer_bulk(endpoint)) {
            if (!usb_dev->bulk_in_pipe) {
                usb_dev->bulk_in_pipe = usb_rcvbulkpipe(usb, endpoint_num);
            }
#ifdef CONFIG_USB_MSG_IN_EP
            else if (!usb_dev->msg_in_pipe) {
				if(usb_dev->chipid != PRODUCT_ID_AIC8801){
                	usb_dev->msg_in_pipe = usb_rcvbulkpipe(usb, endpoint_num);
				}
            }
#endif
        }

        if (usb_endpoint_dir_out(endpoint) &&
            usb_endpoint_xfer_bulk(endpoint)) {
            if (!usb_dev->bulk_out_pipe)
            {
                usb_dev->bulk_out_pipe = usb_sndbulkpipe(usb, endpoint_num);
            }
#ifdef CONFIG_USB_MSG_OUT_EP
             else if (!usb_dev->msg_out_pipe) {
                usb_dev->msg_out_pipe = usb_sndbulkpipe(usb, endpoint_num);
            }
#endif

        }
    }

    if (usb_dev->bulk_in_pipe == 0) {
        usb_err("No RX (in) Bulk EP found\n");
        ret = -ENODEV;
        goto exit;
    }
    if (usb_dev->bulk_out_pipe == 0) {
        usb_err("No TX (out) Bulk EP found\n");
        ret = -ENODEV;
        goto exit;
    }
#ifdef CONFIG_USB_MSG_OUT_EP
    if (usb_dev->msg_out_pipe == 0) {
        usb_err("No TX Msg (out) Bulk EP found\n");
    }
#endif
#ifdef CONFIG_USB_MSG_IN_EP
		if(usb_dev->chipid != PRODUCT_ID_AIC8801){
			if (usb_dev->msg_in_pipe == 0) {
				usb_err("No RX Msg (in) Bulk EP found\n");
			}
		}
#endif

    if (usb->speed == USB_SPEED_HIGH) {
        AICWFDBG(LOGINFO, "Aic high speed USB device detected\n");
    } else {
        AICWFDBG(LOGINFO,
                 "Aic non-high-speed USB device detected (speed=%u)\n",
                 usb->speed);
    }

    exit:
    return ret;
}



static struct aicwf_bus_ops aicwf_usb_bus_ops = {
    .start = aicwf_usb_bus_start,
    .stop = aicwf_usb_bus_stop,
    .txdata = aicwf_usb_bus_txdata,
    .txmsg = aicwf_usb_bus_txmsg,
};


#ifdef CONFIG_GPIO_WAKEUP

static irqreturn_t rwnx_irq_handler(int irq, void *para)
{
	unsigned long irqflags;
    spin_lock_irqsave(&irq_lock, irqflags);
    disable_irq_nosync(hostwake_irq_num);
    //do something
    printk("%s gpio irq trigger\r\n", __func__);
    spin_unlock_irqrestore(&irq_lock, irqflags);
    atomic_dec(&irq_count);
	return IRQ_HANDLED;
}


static int rwnx_register_hostwake_irq(struct device *dev)
{
	int ret = 0;
	uint irq_flags = 0;

	spin_lock_init(&irq_lock);

//Setting hostwake gpio for platform
//For Rockchip
#ifdef CONFIG_PLATFORM_ROCKCHIP
	hostwake_irq_num = rockchip_wifi_get_oob_irq();
	printk("%s hostwake_irq_num:%d \r\n", __func__, hostwake_irq_num);
	irq_flags = (IORESOURCE_IRQ | IORESOURCE_IRQ_HIGHLEVEL | IORESOURCE_IRQ_SHAREABLE) & IRQF_TRIGGER_MASK;
	printk("%s irq_flags:%d \r\n", __func__, irq_flags);
	wakeup_enable = 1;
#endif //CONFIG_PLATFORM_ROCKCHIP

//For Allwinner
#ifdef CONFIG_PLATFORM_ALLWINNER
		int irq_flags;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
	hostwake_irq_num = sunxi_wlan_get_oob_irq(&irq_flags, &wakeup_enable);
#else
	hostwake_irq_num = sunxi_wlan_get_oob_irq();
	irq_flags = sunxi_wlan_get_oob_irq_flags();
	wakeup_enable = 1;
#endif
#endif //CONFIG_PLATFORM_ALLWINNER


	ret = request_irq(hostwake_irq_num,
				rwnx_irq_handler, IRQF_TRIGGER_RISING | IRQF_NO_SUSPEND,
				"rwnx_irq_handler", NULL);

	enable_irq_wake(hostwake_irq_num);

	return ret;
}

static int rwnx_unregister_hostwake_irq(struct device *dev)
{
	wakeup_enable = 0;

	printk("%s hostwake_irq_num:%d \r\n", __func__, hostwake_irq_num);
	disable_irq_wake(hostwake_irq_num);
	free_irq(hostwake_irq_num, NULL);

	return 0;
}

#endif //CONFIG_GPIO_WAKEUP

static int aicwf_usb_chipmatch(struct aic_usb_dev *usb_dev, u16_l vid, u16_l pid){

	if(pid == USB_PRODUCT_ID_AIC8801){
		usb_dev->chipid = PRODUCT_ID_AIC8801;
		AICWFDBG(LOGINFO, "%s USE AIC8801\r\n", __func__);
		return 0;
	}else if(pid == USB_PRODUCT_ID_AIC8800DC){
		usb_dev->chipid = PRODUCT_ID_AIC8800DC;
		AICWFDBG(LOGINFO, "%s USE AIC8800DC\r\n", __func__);
		return 0;
	}else if(pid == USB_PRODUCT_ID_AIC8800DW || pid == USB_PRODUCT_ID_TP
	 || pid == USB_PRODUCT_ID_AIC8800FC){
        usb_dev->chipid = PRODUCT_ID_AIC8800DW;
		AICWFDBG(LOGINFO, "%s USE AIC8800DW\r\n", __func__);
        return 0;
    }else{
		return -1;
	}
}


static int aicwf_usb_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
    int ret = 0;
    struct usb_device *usb = interface_to_usbdev(intf);
    struct aicwf_bus *bus_if = NULL;
    struct device *dev = NULL;
    struct aicwf_rx_priv* rx_priv = NULL;
    struct aic_usb_dev *usb_dev = NULL;
    #ifdef CONFIG_USB_TX_AGGR
    struct aicwf_tx_priv *tx_priv = NULL;
    #endif

    usb_dev = kzalloc(sizeof(struct aic_usb_dev), GFP_KERNEL);
    if (!usb_dev) {
        return -ENOMEM;
    }

    usb_dev->udev = usb;
    usb_dev->dev = &usb->dev;
    usb_set_intfdata(intf, usb_dev);
	
	ret = aicwf_usb_chipmatch(usb_dev, id->idVendor, id->idProduct);
	
	if (ret < 0) {
        AICWFDBG(LOGERROR, "%s pid:0x%04X vid:0x%04X unsupport\n", 
			__func__, id->idVendor, id->idProduct);
        goto out_free;
    }

    ret = aicwf_parse_usb(usb_dev, intf);
    if (ret) {
        AICWFDBG(LOGERROR, "aicwf_parse_usb err %d\n", ret);
        goto out_free;
    }

    ret = aicwf_usb_init(usb_dev);
    if (ret) {
        AICWFDBG(LOGERROR, "aicwf_usb_init err %d\n", ret);
        goto out_free;
    }

    bus_if = kzalloc(sizeof(struct aicwf_bus), GFP_KERNEL);
    if (!bus_if) {
        ret = -ENOMEM;
        goto out_free_usb;
    }

    dev = usb_dev->dev;
    bus_if->dev = dev;
    usb_dev->bus_if = bus_if;
    bus_if->bus_priv.usb = usb_dev;
    dev_set_drvdata(dev, bus_if);

    bus_if->ops = &aicwf_usb_bus_ops;

    rx_priv = aicwf_rx_init(usb_dev);
    if(!rx_priv) {
       AICWFDBG(LOGERROR, "rx init failed\n");
        ret = -ENOMEM;
        goto out_free_privs;
    }
    usb_dev->rx_priv = rx_priv;

#ifdef CONFIG_USB_TX_AGGR
    tx_priv = aicwf_tx_init(usb_dev);
    if(!tx_priv) {
        usb_err("tx init fail\n");
        ret = -ENOMEM;
        goto out_free_privs;
    }
    usb_dev->tx_priv = tx_priv;
    aicwf_frame_queue_init(&tx_priv->txq, 8, TXQLEN);
    spin_lock_init(&tx_priv->txqlock);
    spin_lock_init(&tx_priv->txdlock);
#endif

    ret = aicwf_bus_init(0, dev);
    if (ret < 0) {
        AICWFDBG(LOGERROR, "aicwf_bus_init err %d\n", ret);
        goto out_free_privs;
    }

    ret = aicwf_bus_start(bus_if);
    if (ret < 0) {
        AICWFDBG(LOGERROR, "aicwf_bus_start err %d\n", ret);
        goto out_deinit_bus;
    }

    ret = aicwf_rwnx_usb_platform_init(usb_dev);
	if (ret < 0) {
        AICWFDBG(LOGERROR, "aicwf_rwnx_usb_platform_init err %d\n", ret);
        goto out_deinit_bus;
    }
    aicwf_hostif_ready();

#ifdef CONFIG_GPIO_WAKEUP
	rwnx_register_hostwake_irq(usb_dev->dev);
#endif

    return 0;

out_deinit_bus:
    aicwf_bus_deinit(dev);
out_free_privs:
#ifdef CONFIG_USB_TX_AGGR
    if (tx_priv)
        aicwf_tx_deinit(tx_priv);
#endif
    if (rx_priv)
        aicwf_rx_deinit(rx_priv);
    dev_set_drvdata(dev, NULL);
    usb_dev->bus_if = NULL;
    kfree(bus_if);
out_free_usb:
    aicwf_usb_deinit(usb_dev);
out_free:
    usb_err("failed with errno %d\n", ret);
    kfree(usb_dev);
    usb_set_intfdata(intf, NULL);
    return ret;
}

static void aicwf_usb_log_runtime_end(struct aic_usb_dev *usb_dev)
{
    struct rwnx_hw *rwnx_hw = usb_dev->rwnx_hw;
    struct rwnx_runtime_stats *stats;
    u64 uptime_ms;

    if (!rwnx_hw)
        return;
    stats = &rwnx_hw->runtime_stats;
    uptime_ms = jiffies64_to_msecs(get_jiffies_64() -
                                   stats->started_jiffies);
    AICWFDBG(LOGINFO,
             "runtime_instance_end generation:%u uptime_ms:%llu "
             "conn_reject:%d cqm_low:%d cqm_high:%d "
             "cmd_alloc:%d cmd_high:%d cmd_atomic:%d cmd_down:%d "
             "cmd_tx:%d cmd_timeout:%d cmd_early_cfm:%d cmd_big_cfm:%d\n",
             stats->device_generation, (unsigned long long)uptime_ms,
             atomic_read(&stats->conn_firmware_rejects),
             atomic_read(&stats->cqm_rssi_low_events),
             atomic_read(&stats->cqm_rssi_high_events),
             atomic_read(&stats->cmd_alloc_failures),
             atomic_read(&stats->cmd_pool_high_water),
             atomic_read(&stats->cmd_atomic_rejects),
             atomic_read(&stats->cmd_bus_down_rejects),
             atomic_read(&stats->cmd_tx_failures),
             atomic_read(&stats->cmd_timeouts),
             atomic_read(&stats->cmd_cfm_before_push),
             atomic_read(&stats->cmd_cfm_oversize));
    AICWFDBG(LOGINFO,
             "runtime_instance_end generation:%u "
             "rx_submit:%d rx_refill:%d rx_state:%d rx_queue:%d "
             "rx_complete:%d rx_terminal:%d rx_short:%d rx_invalid:%d\n",
             stats->device_generation,
             atomic_read(&stats->usb_rx_submit_failures),
             atomic_read(&stats->usb_rx_refill_failures),
             atomic_read(&stats->usb_rx_state_rejects),
             atomic_read(&stats->usb_rx_queue_overflows),
             atomic_read(&stats->usb_rx_completion_errors),
             atomic_read(&stats->usb_rx_terminal_errors),
             atomic_read(&stats->usb_rx_short_frames),
             atomic_read(&stats->usb_rx_invalid_lengths));
    AICWFDBG(LOGINFO,
             "runtime_instance_end generation:%u "
             "msg_rx_submit:%d msg_rx_refill:%d msg_rx_state:%d "
             "msg_rx_queue:%d msg_rx_complete:%d msg_rx_terminal:%d "
             "msg_rx_invalid:%d msg_tx_submit:%d msg_tx_complete:%d "
             "msg_tx_state:%d\n",
             stats->device_generation,
             atomic_read(&stats->usb_msg_rx_submit_failures),
             atomic_read(&stats->usb_msg_rx_refill_failures),
             atomic_read(&stats->usb_msg_rx_state_rejects),
             atomic_read(&stats->usb_msg_rx_queue_overflows),
             atomic_read(&stats->usb_msg_rx_completion_errors),
             atomic_read(&stats->usb_msg_rx_terminal_errors),
             atomic_read(&stats->usb_msg_rx_invalid_lengths),
             atomic_read(&stats->usb_msg_tx_submit_failures),
             atomic_read(&stats->usb_msg_tx_completion_errors),
             atomic_read(&stats->usb_msg_tx_state_rejects));
    AICWFDBG(LOGINFO,
             "runtime_instance_end generation:%u "
             "tx_submit:%d tx_complete:%d tx_nobuf:%d tx_state:%d "
             "flow_stop:%d flow_wake:%d fw_invalid:%d fw_log_drop:%d "
             "amsdu_invalid:%d radiotap_invalid:%d\n",
             stats->device_generation,
             atomic_read(&stats->usb_tx_submit_failures),
             atomic_read(&stats->usb_tx_completion_errors),
             atomic_read(&stats->usb_tx_no_buffers),
             atomic_read(&stats->usb_tx_state_rejects),
             atomic_read(&stats->usb_flow_stops),
             atomic_read(&stats->usb_flow_wakes),
             atomic_read(&stats->fw_msg_invalid),
             atomic_read(&stats->fw_log_drops),
             atomic_read(&stats->amsdu_invalid),
             atomic_read(&stats->radiotap_invalid_rates));
}

static void aicwf_usb_disconnect(struct usb_interface *intf)
{
    struct aic_usb_dev *usb_dev =
            (struct aic_usb_dev *) usb_get_intfdata(intf);
        AICWFDBG(LOGINFO, "%s Enter\r\n", __func__);

    if (!usb_dev){
		AICWFDBG(LOGERROR, "%s usb_dev is null \r\n", __func__);
        return;
    }

	if (g_rwnx_plat && !g_rwnx_plat->wait_disconnect_cb) {
		atomic_set(&aicwf_deinit_atomic, 0);
		down(&aicwf_deinit_sem);
	}

    usb_set_intfdata(intf, NULL);
    aicwf_usb_log_runtime_end(usb_dev);

#if 0
	if(timer_pending(&usb_dev->rwnx_hw->p2p_alive_timer) && usb_dev->rwnx_hw->is_p2p_alive == 1){
		printk("%s del timer rwnx_hw->p2p_alive_timer \r\n", __func__);
		rwnx_del_timer(&usb_dev->rwnx_hw->p2p_alive_timer);
	}
#endif
    aicwf_bus_deinit(usb_dev->dev);
    aicwf_usb_deinit(usb_dev);

#ifdef CONFIG_GPIO_WAKEUP
	rwnx_unregister_hostwake_irq(usb_dev->dev);
#endif

    if (usb_dev->rx_priv)
        aicwf_rx_deinit(usb_dev->rx_priv);
#ifdef CONFIG_USB_TX_AGGR
    if (usb_dev->tx_priv)
        aicwf_tx_deinit(usb_dev->tx_priv);
#endif

    dev_set_drvdata(usb_dev->dev, NULL);
    kfree(usb_dev->bus_if);
    kfree(usb_dev);
	AICWFDBG(LOGINFO, "%s exit\r\n", __func__);
	up(&aicwf_deinit_sem);
	atomic_set(&aicwf_deinit_atomic, 1);
}

static int aicwf_usb_suspend(struct usb_interface *intf, pm_message_t state)
{
    struct aic_usb_dev *usb_dev =
        (struct aic_usb_dev *) usb_get_intfdata(intf);
#ifdef CONFIG_GPIO_WAKEUP
	struct rwnx_vif *rwnx_vif, *tmp;
	//unsigned long irqflags;
#endif

	printk("%s enter\r\n", __func__);

#ifdef CONFIG_GPIO_WAKEUP
//	spin_lock_irqsave(&irq_lock, irqflags);
//	rwnx_enable_hostwake_irq();
//    spin_unlock_irqrestore(&irq_lock, irqflags);
    atomic_inc(&irq_count);

	list_for_each_entry_safe(rwnx_vif, tmp, &usb_dev->rwnx_hw->vifs, list) {
		if (rwnx_vif->ndev)
			netif_device_detach(rwnx_vif->ndev);
	}
#endif

	aicwf_usb_state_change(usb_dev, USB_SLEEP_ST);
    aicwf_bus_stop(usb_dev->bus_if);


    return 0;
}

static int aicwf_usb_resume(struct usb_interface *intf)
{
    struct aic_usb_dev *usb_dev =
        (struct aic_usb_dev *) usb_get_intfdata(intf);
#ifdef CONFIG_GPIO_WAKEUP
	struct rwnx_vif *rwnx_vif, *tmp;
//	unsigned long irqflags;
#endif
	printk("%s enter\r\n", __func__);

#ifdef CONFIG_GPIO_WAKEUP
//	spin_lock_irqsave(&irq_lock, irqflags);
//	rwnx_disable_hostwake_irq();
//	spin_unlock_irqrestore(&irq_lock, irqflags);
	atomic_dec(&irq_count);

	list_for_each_entry_safe(rwnx_vif, tmp, &usb_dev->rwnx_hw->vifs, list) {
		if (rwnx_vif->ndev)
			netif_device_attach(rwnx_vif->ndev);
	}
#endif

    if (usb_dev->state == USB_UP_ST)
        return 0;

    aicwf_bus_start(usb_dev->bus_if);
    return 0;
}

static int aicwf_usb_reset_resume(struct usb_interface *intf)
{
    return aicwf_usb_resume(intf);
}

static struct usb_device_id aicwf_usb_id_table[] = {
#ifndef CONFIG_USB_BT
    {USB_DEVICE(USB_VENDOR_ID_AIC, USB_PRODUCT_ID_AIC8800)},
#else
    {USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_AIC, USB_PRODUCT_ID_AIC8801, 0xff, 0xff, 0xff)},
    {USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_AIC, USB_PRODUCT_ID_AIC8800DC, 0xff, 0xff, 0xff)},
    {USB_DEVICE(USB_VENDOR_ID_AIC, USB_PRODUCT_ID_AIC8800DW)},
    {USB_DEVICE(USB_VENDOR_ID_AIC_V2, USB_PRODUCT_ID_AIC8800FC)},
    //{USB_DEVICE(USB_VENDOR_ID_TENDA, USB_PRODUCT_ID_TENDA)},
    //{USB_DEVICE(USB_VENDOR_ID_TENDA, USB_PRODUCT_ID_TENDA_U2)},
    {USB_DEVICE(USB_VENDOR_ID_TP, USB_PRODUCT_ID_TP)},
#endif
    {}
};

MODULE_DEVICE_TABLE(usb, aicwf_usb_id_table);

static struct usb_driver aicwf_usbdrvr = {
    .name = KBUILD_MODNAME,
    .probe = aicwf_usb_probe,
    .disconnect = aicwf_usb_disconnect,
    .id_table = aicwf_usb_id_table,
    .suspend = aicwf_usb_suspend,
    .resume = aicwf_usb_resume,
    .reset_resume = aicwf_usb_reset_resume,
#ifdef ANDROID_PLATFORM
    .supports_autosuspend = 1,
#else
    .supports_autosuspend = 0,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
    .disable_hub_initiated_lpm = 1,
#endif
};

void aicwf_usb_register(void)
{
    if (usb_register(&aicwf_usbdrvr) < 0) {
        usb_err("usb_register failed\n");
    }
}

void aicwf_usb_exit(void)
{
	//RWNX_DBG(RWNX_FN_ENTRY_STR);
	AICWFDBG(LOGDEBUG, "%s in_interrupt:%d in_softirq:%d in_atomic:%d\r\n", __func__, (int)in_interrupt(), (int)in_softirq(), (int)in_atomic());
	atomic_set(&aicwf_deinit_atomic, 0);
	if(down_timeout(&aicwf_deinit_sem, msecs_to_jiffies(SEM_TIMOUT)) != 0){
		AICWFDBG(LOGERROR, "%s semaphore waiting timeout\r\n", __func__);
	}

	if(g_rwnx_plat){
		g_rwnx_plat->wait_disconnect_cb = false;
	}
	
	AICWFDBG(LOGINFO, "%s Enter\r\n", __func__);

	if(!g_rwnx_plat || !g_rwnx_plat->enabled){
		AICWFDBG(LOGINFO, "g_rwnx_plat is not ready. waiting for 500ms\r\n");
		mdelay(500);
	}

	up(&aicwf_deinit_sem);
	atomic_set(&aicwf_deinit_atomic, 1);

	AICWFDBG(LOGINFO, "%s usb_deregister \r\n", __func__);

    usb_deregister(&aicwf_usbdrvr);
	//mdelay(500);
	if(g_rwnx_plat){
    	kfree(g_rwnx_plat);
	}
	
	AICWFDBG(LOGINFO, "%s exit\r\n", __func__);

}

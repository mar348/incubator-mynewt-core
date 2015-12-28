/**
 * Copyright (c) 2015 Runtime Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "bsp/bsp.h"
#include "os/os.h"
#include "nimble/ble.h"
#include "nimble/hci_common.h"
#include "controller/ble_ll.h"
#include "controller/ble_ll_hci.h"
#include "controller/ble_ll_conn.h"
#include "controller/ble_ll_ctrl.h"
#include "controller/ble_ll_scan.h"
#include "ble_ll_conn_priv.h"

/* 
 * Used to limit the rate at which we send the number of completed packets
 * event to the host. This is the os time at which we can send an event.
 */
static uint32_t g_ble_ll_next_num_comp_pkt_evt;

#define BLE_LL_NUM_COMP_PKT_RATE    \
    ((BLE_LL_CFG_NUM_COMP_PKT_RATE * OS_TICKS_PER_SEC) / 1000)

/**
 * Make a connect request PDU 
 * 
 * @param connsm 
 */
static void
ble_ll_conn_req_pdu_make(struct ble_ll_conn_sm *connsm)
{
    uint8_t pdu_type;
    uint8_t *addr;
    uint8_t *dptr;
    struct os_mbuf *m;

    m = ble_ll_scan_get_pdu();
    assert(m != NULL);
    m->om_len = BLE_CONNECT_REQ_LEN + BLE_LL_PDU_HDR_LEN;
    OS_MBUF_PKTHDR(m)->omp_len = m->om_len;

    /* Construct first PDU header byte */
    pdu_type = BLE_ADV_PDU_TYPE_CONNECT_REQ;

    /* Get pointer to our device address */
    if (connsm->own_addr_type == BLE_HCI_ADV_OWN_ADDR_PUBLIC) {
        addr = g_dev_addr;
    } else if (connsm->own_addr_type == BLE_HCI_ADV_OWN_ADDR_RANDOM) {
        pdu_type |= BLE_ADV_PDU_HDR_TXADD_RAND;
        addr = g_random_addr;
    } else {
        /* XXX: unsupported for now  */
        addr = NULL;
        assert(0);
    }

    /* Construct the connect request */
    dptr = m->om_data;
    dptr[0] = pdu_type;
    dptr[1] = BLE_CONNECT_REQ_LEN;
    memcpy(dptr + BLE_LL_PDU_HDR_LEN, addr, BLE_DEV_ADDR_LEN);

    /* Skip the advertiser's address as we dont know that yet */
    dptr += (BLE_LL_CONN_REQ_ADVA_OFF + BLE_DEV_ADDR_LEN);

    /* Access address */
    htole32(dptr, connsm->access_addr);
    dptr[4] = (uint8_t)connsm->crcinit;
    dptr[5] = (uint8_t)(connsm->crcinit >> 8);
    dptr[6] = (uint8_t)(connsm->crcinit >> 16);
    dptr[7] = connsm->tx_win_size;
    htole16(dptr + 8, connsm->tx_win_off);
    htole16(dptr + 10, connsm->conn_itvl);
    htole16(dptr + 12, connsm->slave_latency);
    htole16(dptr + 14, connsm->supervision_tmo);
    memcpy(dptr + 16, &connsm->chanmap, BLE_LL_CONN_CHMAP_LEN);
    dptr[21] = connsm->hop_inc | connsm->master_sca;
}

/**
 * Send a connection complete event 
 * 
 * @param status The BLE error code associated with the event
 */
void
ble_ll_conn_comp_event_send(struct ble_ll_conn_sm *connsm, uint8_t status)
{
    uint8_t *evbuf;

    if (ble_ll_hci_is_le_event_enabled(BLE_HCI_LE_SUBEV_CONN_COMPLETE - 1)) {
        evbuf = os_memblock_get(&g_hci_cmd_pool);
        if (evbuf) {
            evbuf[0] = BLE_HCI_EVCODE_LE_META;
            evbuf[1] = BLE_HCI_LE_CONN_COMPLETE_LEN;
            evbuf[2] = BLE_HCI_LE_SUBEV_CONN_COMPLETE;
            evbuf[3] = status;
            if (status == BLE_ERR_SUCCESS) {
                htole16(evbuf + 4, connsm->conn_handle);
                evbuf[6] = connsm->conn_role - 1;
                evbuf[7] = connsm->peer_addr_type;
                memcpy(evbuf + 8, connsm->peer_addr, BLE_DEV_ADDR_LEN);
                htole16(evbuf + 14, connsm->conn_itvl);
                htole16(evbuf + 16, connsm->slave_latency);
                htole16(evbuf + 18, connsm->supervision_tmo);
                evbuf[20] = connsm->master_sca;
            }
            ble_ll_hci_event_send(evbuf);
        }
    }
}

/**
 * Called to create and send the number of completed packets event to the 
 * host. 
 *  
 * Because of the ridiculous spec, all the connection handles are contiguous and
 * then all the completed packets are contiguous. In order to avoid multiple 
 * passes through the connection list or allocating a large stack variable or 
 * malloc, I just use the event buffer and place the completed packets after 
 * the last possible handle. I then copy the completed packets to make it 
 * contiguous with the handles. 
 *  
 * @param connsm 
 */
void
ble_ll_conn_num_comp_pkts_event_send(void)
{
    int event_sent;
    uint8_t *evbuf;
    uint8_t *handle_ptr;
    uint8_t *comp_pkt_ptr;
    uint8_t handles;
    struct ble_ll_conn_sm *connsm;

    /* Check rate limit */
    if ((uint32_t)(g_ble_ll_next_num_comp_pkt_evt - os_time_get()) < 
        BLE_LL_NUM_COMP_PKT_RATE) {
        return;
    }

    /* Iterate through all the active, created connections */
    evbuf = NULL;
    handles = 0;
    handle_ptr = NULL;
    comp_pkt_ptr = NULL;
    event_sent = 0;
    SLIST_FOREACH(connsm, &g_ble_ll_conn_active_list, act_sle) {
        /* 
         * Only look at connections that we have sent a connection complete
         * event and that either has packets enqueued or has completed packets.
         */ 
        if ((connsm->conn_state != BLE_LL_CONN_STATE_IDLE) &&
            (connsm->completed_pkts || !STAILQ_EMPTY(&connsm->conn_txq))) {
            /* If no buffer, get one, If cant get one, leave. */
            if (!evbuf) {
                evbuf = os_memblock_get(&g_hci_cmd_pool);
                if (!evbuf) {
                    break;
                }
                handles = 0;
                handle_ptr = evbuf + 3;
                comp_pkt_ptr = handle_ptr + (sizeof(uint16_t) * 60);
            }

            /* Add handle and complete packets */
            htole16(handle_ptr, connsm->conn_handle);
            htole16(comp_pkt_ptr, connsm->completed_pkts);
            connsm->completed_pkts = 0;
            handle_ptr += sizeof(uint16_t);
            comp_pkt_ptr += sizeof(uint16_t);
            ++handles;

            /* 
             * The event buffer should fit at least 255 bytes so this means we
             * can fit up to 60 handles per event (a little more but who cares).
             */ 
            if (handles == 60) {
                evbuf[0] = BLE_HCI_EVCODE_NUM_COMP_PKTS;
                evbuf[1] = (handles * 2 * sizeof(uint16_t)) + 1;
                evbuf[2] = handles;
                ble_ll_hci_event_send(evbuf);
                evbuf = NULL;
                handles = 0;
                event_sent = 1;
            }
        }
    }

    /* Send event if there is an event to send */
    if (evbuf) {
        evbuf[0] = BLE_HCI_EVCODE_NUM_COMP_PKTS;
        evbuf[1] = (handles * 2 * sizeof(uint16_t)) + 1;
        evbuf[2] = handles;
        if (handles < 60) {
            /* Make the pkt counts contiguous with handles */
            memmove(handle_ptr, evbuf + 3 + (60 * 2), handles * 2);
        }
        ble_ll_hci_event_send(evbuf);
        event_sent = 1;
    }

    if (event_sent) {
        g_ble_ll_next_num_comp_pkt_evt = os_time_get() + 
            BLE_LL_NUM_COMP_PKT_RATE;
    }
}


/**
 * Send a disconnection complete event. 
 *  
 * NOTE: we currently only send this event when we have a reason to send it; 
 * not when it fails. 
 * 
 * @param reason The BLE error code to send as a disconnect reason
 */
void
ble_ll_disconn_comp_event_send(struct ble_ll_conn_sm *connsm, uint8_t reason)
{
    uint8_t *evbuf;

    if (ble_ll_hci_is_event_enabled(BLE_HCI_EVCODE_DISCONN_CMP - 1)) {
        evbuf = os_memblock_get(&g_hci_cmd_pool);
        if (evbuf) {
            evbuf[0] = BLE_HCI_EVCODE_DISCONN_CMP;
            evbuf[1] = BLE_HCI_EVENT_DISCONN_COMPLETE_LEN;
            evbuf[2] = BLE_ERR_SUCCESS;
            htole16(evbuf + 3, connsm->conn_handle);
            evbuf[5] = reason;
            ble_ll_hci_event_send(evbuf);
        }
    }
}

/**
 * Process the HCI command to create a connection. 
 *  
 * Context: Link Layer task (HCI command processing) 
 * 
 * @param cmdbuf 
 * 
 * @return int 
 */
int
ble_ll_conn_create(uint8_t *cmdbuf)
{
    int rc;
    uint32_t spvn_tmo_usecs;
    uint32_t min_spvn_tmo_usecs;
    struct hci_create_conn ccdata;
    struct hci_create_conn *hcc;
    struct ble_ll_conn_sm *connsm;

    /* If we are already creating a connection we should leave */
    if (g_ble_ll_conn_create_sm) {
        return BLE_ERR_CMD_DISALLOWED;
    }

    /* If already enabled, we return an error */
    if (ble_ll_scan_enabled()) {
        return BLE_ERR_CMD_DISALLOWED;
    }

    /* Retrieve command data */
    hcc = &ccdata;
    hcc->scan_itvl = le16toh(cmdbuf);
    hcc->scan_window = le16toh(cmdbuf + 2);

    /* Check interval and window */
    if ((hcc->scan_itvl < BLE_HCI_SCAN_ITVL_MIN) || 
        (hcc->scan_itvl > BLE_HCI_SCAN_ITVL_MAX) ||
        (hcc->scan_window < BLE_HCI_SCAN_WINDOW_MIN) ||
        (hcc->scan_window > BLE_HCI_SCAN_WINDOW_MAX) ||
        (hcc->scan_itvl < hcc->scan_window)) {
        return BLE_ERR_INV_HCI_CMD_PARMS;
    }

    /* Check filter policy */
    hcc->filter_policy = cmdbuf[4];
    if (hcc->filter_policy > BLE_HCI_INITIATOR_FILT_POLICY_MAX) {
        return BLE_ERR_INV_HCI_CMD_PARMS;
    }

    /* Get peer address type and address only if no whitelist used */
    if (hcc->filter_policy == 0) {
        hcc->peer_addr_type = cmdbuf[5];
        if (hcc->peer_addr_type > BLE_HCI_CONN_PEER_ADDR_MAX) {
            return BLE_ERR_INV_HCI_CMD_PARMS;
        }
        memcpy(&hcc->peer_addr, cmdbuf + 6, BLE_DEV_ADDR_LEN);
    }

    /* Get own address type (used in connection request) */
    hcc->own_addr_type = cmdbuf[12];
    if (hcc->own_addr_type > BLE_HCI_ADV_OWN_ADDR_MAX) {
        return BLE_ERR_INV_HCI_CMD_PARMS;
    }

    /* Check connection interval */
    hcc->conn_itvl_min = le16toh(cmdbuf + 13);
    hcc->conn_itvl_max = le16toh(cmdbuf + 15);
    hcc->conn_latency = le16toh(cmdbuf + 17);
    if ((hcc->conn_itvl_min > hcc->conn_itvl_max)       ||
        (hcc->conn_itvl_min < BLE_HCI_CONN_ITVL_MIN)    ||
        (hcc->conn_itvl_min > BLE_HCI_CONN_ITVL_MAX)    ||
        (hcc->conn_latency > BLE_HCI_CONN_LATENCY_MAX)) {
        return BLE_ERR_INV_HCI_CMD_PARMS;
    }

    /* Check supervision timeout */
    hcc->supervision_timeout = le16toh(cmdbuf + 19);
    if ((hcc->supervision_timeout < BLE_HCI_CONN_SPVN_TIMEOUT_MIN) ||
        (hcc->supervision_timeout > BLE_HCI_CONN_SPVN_TIMEOUT_MAX))
    {
        return BLE_ERR_INV_HCI_CMD_PARMS;
    }

    /* 
     * Supervision timeout (in msecs) must be more than:
     *  (1 + connLatency) * connIntervalMax * 1.25 msecs * 2.
     */
    spvn_tmo_usecs = hcc->supervision_timeout;
    spvn_tmo_usecs *= (BLE_HCI_CONN_SPVN_TMO_UNITS * 1000);
    min_spvn_tmo_usecs = (uint32_t)hcc->conn_itvl_max * 2 * 
        BLE_LL_CONN_ITVL_USECS;
    min_spvn_tmo_usecs *= (1 + hcc->conn_latency);
    if (spvn_tmo_usecs <= min_spvn_tmo_usecs) {
        return BLE_ERR_INV_HCI_CMD_PARMS;
    }

    /* Min/max connection event lengths */
    hcc->min_ce_len = le16toh(cmdbuf + 21);
    hcc->max_ce_len = le16toh(cmdbuf + 23);
    if (hcc->min_ce_len > hcc->max_ce_len) {
        return BLE_ERR_INV_HCI_CMD_PARMS;
    }

    /* Make sure we can accept a connection! */
    connsm = ble_ll_conn_sm_get();
    if (connsm == NULL) {
        return BLE_ERR_CONN_LIMIT;
    }

    /* Initialize state machine in master role and start state machine */
    ble_ll_conn_master_init(connsm, hcc);
    ble_ll_conn_sm_start(connsm);

    /* Create the connection request */
    ble_ll_conn_req_pdu_make(connsm);

    /* Start scanning */
    rc = ble_ll_scan_initiator_start(hcc);
    if (rc) {
        SLIST_REMOVE(&g_ble_ll_conn_active_list,connsm,ble_ll_conn_sm,act_sle);
        STAILQ_INSERT_TAIL(&g_ble_ll_conn_free_list, connsm, free_stqe);
    } else {
        /* Set the connection state machine we are trying to create. */
        g_ble_ll_conn_create_sm = connsm;
    }

    return rc;
}

/**
 * Called when HCI command to cancel a create connection command has been 
 * received. 
 *  
 * Context: Link Layer (HCI command parser) 
 * 
 * @return int 
 */
int
ble_ll_conn_create_cancel(void)
{
    int rc;
    struct ble_ll_conn_sm *connsm;

    /* 
     * If we receive this command and we have not got a connection
     * create command, we have to return disallowed. The spec does not say
     * what happens if the connection has already been established. We
     * return disallowed as well
     */
    connsm = g_ble_ll_conn_create_sm;
    if (connsm && (connsm->conn_state == BLE_LL_CONN_STATE_IDLE)) {
        /* stop scanning and end the connection event */
        g_ble_ll_conn_create_sm = NULL;
        ble_ll_scan_sm_stop(ble_ll_scan_sm_get(), 0);
        ble_ll_conn_end(connsm, BLE_ERR_UNK_CONN_ID);
        rc = BLE_ERR_SUCCESS;
    } else {
        /* If we are not attempting to create a connection*/
        rc = BLE_ERR_CMD_DISALLOWED;
    }

    return rc;
}

/**
 * Called to process a HCI disconnect command 
 *  
 * Context: Link Layer task (HCI command parser). 
 * 
 * @param cmdbuf 
 * 
 * @return int 
 */
int
ble_ll_conn_hci_disconnect_cmd(uint8_t *cmdbuf)
{
    int rc;
    uint8_t reason;
    uint16_t handle;
    struct ble_ll_conn_sm *connsm;

    /* Check for valid parameters */
    handle = le16toh(cmdbuf);
    reason = cmdbuf[2];

    rc = BLE_ERR_INV_HCI_CMD_PARMS;
    if (handle <= BLE_LL_CONN_MAX_CONN_HANDLE) {
        /* Make sure reason is valid */
        switch (reason) {
        case BLE_ERR_AUTH_FAIL:
        case BLE_ERR_REM_USER_CONN_TERM:
        case BLE_ERR_RD_CONN_TERM_RESRCS:
        case BLE_ERR_RD_CONN_TERM_PWROFF:
        case BLE_ERR_UNSUPP_FEATURE:
        case BLE_ERR_UNIT_KEY_PAIRING:
        case BLE_ERR_CONN_PARMS:
            connsm = ble_ll_conn_find_active_conn(handle);
            if (connsm) {
                /* Do not allow command if we are in process of disconnecting */
                if (connsm->disconnect_reason) {
                    rc = BLE_ERR_CMD_DISALLOWED;
                } else {
                    /* This control procedure better not be pending! */
                    assert(!IS_PENDING_CTRL_PROC_M(connsm,
                                                   BLE_LL_CTRL_PROC_TERMINATE));

                    /* Record the disconnect reason */
                    connsm->disconnect_reason = reason;

                    /* Start this control procedure */
                    ble_ll_ctrl_terminate_start(connsm);

                    rc = BLE_ERR_SUCCESS;
                }
            } else {
                rc = BLE_ERR_UNK_CONN_ID;
            }
            break;
        default:
            break;
        }
    }

    return rc;
}
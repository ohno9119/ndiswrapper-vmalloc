/*
 * WPA Supplicant / EAPOL state machines
 * Copyright (c) 2004, Jouni Malinen <jkmaline@cc.hut.fi>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Alternatively, this software may be distributed under the terms of BSD
 * license.
 *
 * See README and COPYING for more details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>

#include "common.h"
#include "eapol_sm.h"
#include "eap.h"
#include "eloop.h"
#include "wpa_supplicant.h"
#include "l2_packet.h"
#include "wpa.h"
#include "md5.h"
#include "rc4.h"


static void eapol_sm_txLogoff(struct eapol_sm *sm);
static void eapol_sm_txStart(struct eapol_sm *sm);
static void eapol_sm_processKey(struct eapol_sm *sm);
static void eapol_sm_getSuppRsp(struct eapol_sm *sm);
static void eapol_sm_txSuppRsp(struct eapol_sm *sm);
static void eapol_sm_abortSupp(struct eapol_sm *sm);
static void eapol_sm_abort_cached(struct eapol_sm *sm);
static void eapol_sm_step_timeout(void *eloop_ctx, void *timeout_ctx);


/* Definitions for clarifying state machine implementation */
#define SM_STATE(machine, state) \
static void sm_ ## machine ## _ ## state ## _Enter(struct eapol_sm *sm, \
	int global)

#define SM_ENTRY(machine, state) \
if (!global || sm->machine ## _state != machine ## _ ## state) { \
	sm->changed = TRUE; \
	wpa_printf(MSG_DEBUG, "EAPOL: " #machine " entering state " #state); \
} \
sm->machine ## _state = machine ## _ ## state;

#define SM_ENTER(machine, state) \
sm_ ## machine ## _ ## state ## _Enter(sm, 0)
#define SM_ENTER_GLOBAL(machine, state) \
sm_ ## machine ## _ ## state ## _Enter(sm, 1)

#define SM_STEP(machine) \
static void sm_ ## machine ## _Step(struct eapol_sm *sm)

#define SM_STEP_RUN(machine) sm_ ## machine ## _Step(sm)


/* Port Timers state machine - implemented as a function that will be called
 * once a second as a registered event loop timeout */
static void eapol_port_timers_tick(void *eloop_ctx, void *timeout_ctx)
{
	struct eapol_sm *sm = timeout_ctx;

	if (sm->authWhile > 0)
		sm->authWhile--;
	if (sm->heldWhile > 0)
		sm->heldWhile--;
	if (sm->startWhen > 0)
		sm->startWhen--;
	if (sm->idleWhile > 0)
		sm->idleWhile--;
	wpa_printf(MSG_MSGDUMP, "EAPOL: Port Timers tick - authWhile=%d "
		   "heldWhile=%d startWhen=%d idleWhile=%d",
		   sm->authWhile, sm->heldWhile, sm->startWhen, sm->idleWhile);

	eapol_sm_step(sm);

	eloop_register_timeout(1, 0, eapol_port_timers_tick, eloop_ctx, sm);
}


SM_STATE(SUPP_PAE, LOGOFF)
{
	SM_ENTRY(SUPP_PAE, LOGOFF);
	eapol_sm_txLogoff(sm);
	sm->logoffSent = TRUE;
	sm->suppPortStatus = Unauthorized;
}


SM_STATE(SUPP_PAE, DISCONNECTED)
{
	SM_ENTRY(SUPP_PAE, DISCONNECTED);
	sm->sPortMode = Auto;
	sm->startCount = 0;
	sm->logoffSent = FALSE;
	sm->suppPortStatus = Unauthorized;
	sm->suppAbort = TRUE;

	sm->unicast_key_received = FALSE;
	sm->broadcast_key_received = FALSE;
}


SM_STATE(SUPP_PAE, CONNECTING)
{
	SM_ENTRY(SUPP_PAE, CONNECTING);
	sm->startWhen = sm->startPeriod;
	sm->startCount++;
	sm->eapolEap = FALSE;
	eapol_sm_txStart(sm);
}


SM_STATE(SUPP_PAE, AUTHENTICATING)
{
	SM_ENTRY(SUPP_PAE, AUTHENTICATING);
	sm->startCount = 0;
	sm->suppSuccess = FALSE;
	sm->suppFail = FALSE;
	sm->suppTimeout = FALSE;
	sm->keyRun = FALSE;
	sm->keyDone = FALSE;
	sm->suppStart = TRUE;
}


SM_STATE(SUPP_PAE, HELD)
{
	SM_ENTRY(SUPP_PAE, HELD);
	sm->heldWhile = sm->heldPeriod;
	sm->suppPortStatus = Unauthorized;
	sm->cb_status = EAPOL_CB_FAILURE;
}


SM_STATE(SUPP_PAE, AUTHENTICATED)
{
	SM_ENTRY(SUPP_PAE, AUTHENTICATED);
	sm->suppPortStatus = Authorized;
	sm->cb_status = EAPOL_CB_SUCCESS;
}


SM_STATE(SUPP_PAE, RESTART)
{
	SM_ENTRY(SUPP_PAE, RESTART);
	sm->eapRestart = TRUE;
}


SM_STATE(SUPP_PAE, S_FORCE_AUTH)
{
	SM_ENTRY(SUPP_PAE, S_FORCE_AUTH);
	sm->suppPortStatus = Authorized;
	sm->sPortMode = ForceAuthorized;
}


SM_STATE(SUPP_PAE, S_FORCE_UNAUTH)
{
	SM_ENTRY(SUPP_PAE, S_FORCE_UNAUTH);
	sm->suppPortStatus = Unauthorized;
	sm->sPortMode = ForceUnauthorized;
	eapol_sm_txLogoff(sm);
}


SM_STEP(SUPP_PAE)
{
	if ((sm->userLogoff && !sm->logoffSent) &&
	    !(sm->initialize || !sm->portEnabled))
		SM_ENTER_GLOBAL(SUPP_PAE, LOGOFF);
	else if (((sm->portControl == Auto) &&
		  (sm->sPortMode != sm->portControl)) ||
		 sm->initialize || !sm->portEnabled)
		SM_ENTER_GLOBAL(SUPP_PAE, DISCONNECTED);
	else if ((sm->portControl == ForceAuthorized) &&
		 (sm->sPortMode != sm->portControl) &&
		 !(sm->initialize || !sm->portEnabled))
		SM_ENTER_GLOBAL(SUPP_PAE, S_FORCE_AUTH);
	else if ((sm->portControl == ForceUnauthorized) &&
		 (sm->sPortMode != sm->portControl) &&
		 !(sm->initialize || !sm->portEnabled))
		SM_ENTER_GLOBAL(SUPP_PAE, S_FORCE_UNAUTH);
	else switch (sm->SUPP_PAE_state) {
	case SUPP_PAE_UNKNOWN:
		break;
	case SUPP_PAE_LOGOFF:
		if (!sm->userLogoff)
			SM_ENTER(SUPP_PAE, DISCONNECTED);
		break;
	case SUPP_PAE_DISCONNECTED:
		SM_ENTER(SUPP_PAE, CONNECTING);
		break;
	case SUPP_PAE_CONNECTING:
		if (sm->startWhen == 0 && sm->startCount < sm->maxStart)
			SM_ENTER(SUPP_PAE, CONNECTING);
		else if (sm->startWhen == 0 &&
			 sm->startCount >= sm->maxStart &&
			 sm->portValid)
			SM_ENTER(SUPP_PAE, AUTHENTICATED);
		else if (sm->eapSuccess || sm->eapFail)
			SM_ENTER(SUPP_PAE, AUTHENTICATING);
		else if (sm->eapolEap)
			SM_ENTER(SUPP_PAE, RESTART);
		else if (sm->startWhen == 0 &&
			 sm->startCount >= sm->maxStart &&
			 !sm->portValid)
			SM_ENTER(SUPP_PAE, HELD);
		break;
	case SUPP_PAE_AUTHENTICATING:
		if (sm->eapSuccess && !sm->portValid &&
		    sm->conf.accept_802_1x_keys &&
		    sm->conf.required_keys == 0) {
			wpa_printf(MSG_DEBUG, "EAPOL: IEEE 802.1X for "
				   "plaintext connection; no EAPOL-Key frames "
				   "required");
			sm->portValid = TRUE;
			if (sm->ctx->eapol_done_cb)
				sm->ctx->eapol_done_cb(sm->ctx->ctx);
		}
		if (sm->eapSuccess && sm->portValid)
			SM_ENTER(SUPP_PAE, AUTHENTICATED);
		else if (sm->eapFail || (sm->keyDone && !sm->portValid))
			SM_ENTER(SUPP_PAE, HELD);
		else if (sm->suppTimeout)
			SM_ENTER(SUPP_PAE, CONNECTING);
		break;
	case SUPP_PAE_HELD:
		if (sm->heldWhile == 0)
			SM_ENTER(SUPP_PAE, CONNECTING);
		else if (sm->eapolEap)
			SM_ENTER(SUPP_PAE, RESTART);
		break;
	case SUPP_PAE_AUTHENTICATED:
		if (sm->eapolEap && sm->portValid)
			SM_ENTER(SUPP_PAE, RESTART);
		else if (!sm->portValid)
			SM_ENTER(SUPP_PAE, DISCONNECTED);
		break;
	case SUPP_PAE_RESTART:
		if (!sm->eapRestart)
			SM_ENTER(SUPP_PAE, AUTHENTICATING);
		break;
	case SUPP_PAE_S_FORCE_AUTH:
		break;
	case SUPP_PAE_S_FORCE_UNAUTH:
		break;
	}
}


SM_STATE(KEY_RX, NO_KEY_RECEIVE)
{
	SM_ENTRY(KEY_RX, NO_KEY_RECEIVE);
}


SM_STATE(KEY_RX, KEY_RECEIVE)
{
	SM_ENTRY(KEY_RX, KEY_RECEIVE);
	eapol_sm_processKey(sm);
	sm->rxKey = FALSE;
}


SM_STEP(KEY_RX)
{
	if (sm->initialize || !sm->portEnabled)
		SM_ENTER_GLOBAL(KEY_RX, NO_KEY_RECEIVE);
	switch (sm->KEY_RX_state) {
	case KEY_RX_UNKNOWN:
		break;
	case KEY_RX_NO_KEY_RECEIVE:
		if (sm->rxKey)
			SM_ENTER(KEY_RX, KEY_RECEIVE);
		break;
	case KEY_RX_KEY_RECEIVE:
		if (sm->rxKey)
			SM_ENTER(KEY_RX, KEY_RECEIVE);
		break;
	}
}


SM_STATE(SUPP_BE, REQUEST)
{
	SM_ENTRY(SUPP_BE, REQUEST);
	sm->authWhile = 0;
	sm->eapReq = TRUE;
	eapol_sm_getSuppRsp(sm);
}


SM_STATE(SUPP_BE, RESPONSE)
{
	SM_ENTRY(SUPP_BE, RESPONSE);
	eapol_sm_txSuppRsp(sm);
	sm->eapResp = FALSE;
}


SM_STATE(SUPP_BE, SUCCESS)
{
	SM_ENTRY(SUPP_BE, SUCCESS);
	sm->keyRun = TRUE;
	sm->suppSuccess = TRUE;

	if (sm->eap && sm->eap->eapKeyAvailable) {
		/* New key received - clear IEEE 802.1X EAPOL-Key replay
		 * counter */
		sm->replay_counter_valid = FALSE;
	}
}


SM_STATE(SUPP_BE, FAIL)
{
	SM_ENTRY(SUPP_BE, FAIL);
	sm->suppFail = TRUE;
}


SM_STATE(SUPP_BE, TIMEOUT)
{
	SM_ENTRY(SUPP_BE, TIMEOUT);
	sm->suppTimeout = TRUE;
}


SM_STATE(SUPP_BE, IDLE)
{
	SM_ENTRY(SUPP_BE, IDLE);
	sm->suppStart = FALSE;
	sm->initial_req = TRUE;
}


SM_STATE(SUPP_BE, INITIALIZE)
{
	SM_ENTRY(SUPP_BE, INITIALIZE);
	eapol_sm_abortSupp(sm);
	sm->suppAbort = FALSE;
}


SM_STATE(SUPP_BE, RECEIVE)
{
	SM_ENTRY(SUPP_BE, RECEIVE);
	sm->authWhile = sm->authPeriod;
	sm->eapolEap = FALSE;
	sm->eapNoResp = FALSE;
	sm->initial_req = FALSE;
}


SM_STEP(SUPP_BE)
{
	if (sm->initialize || sm->suppAbort)
		SM_ENTER_GLOBAL(SUPP_BE, INITIALIZE);
	else switch (sm->SUPP_BE_state) {
	case SUPP_BE_UNKNOWN:
		break;
	case SUPP_BE_REQUEST:
		if (sm->eapResp && sm->eapNoResp) {
			wpa_printf(MSG_DEBUG, "EAPOL: SUPP_BE REQUEST: both "
				   "eapResp and eapNoResp set?!");
		}
		if (sm->eapResp)
			SM_ENTER(SUPP_BE, RESPONSE);
		else if (sm->eapNoResp)
			SM_ENTER(SUPP_BE, RECEIVE);
		break;
	case SUPP_BE_RESPONSE:
		SM_ENTER(SUPP_BE, RECEIVE);
		break;
	case SUPP_BE_SUCCESS:
		SM_ENTER(SUPP_BE, IDLE);
		break;
	case SUPP_BE_FAIL:
		SM_ENTER(SUPP_BE, IDLE);
		break;
	case SUPP_BE_TIMEOUT:
		SM_ENTER(SUPP_BE, IDLE);
		break;
	case SUPP_BE_IDLE:
		if (sm->eapFail && sm->suppStart)
			SM_ENTER(SUPP_BE, FAIL);
		else if (sm->eapolEap && sm->suppStart)
			SM_ENTER(SUPP_BE, REQUEST);
		else if (sm->eapSuccess && sm->suppStart)
			SM_ENTER(SUPP_BE, SUCCESS);
		break;
	case SUPP_BE_INITIALIZE:
		SM_ENTER(SUPP_BE, IDLE);
		break;
	case SUPP_BE_RECEIVE:
		if (sm->eapolEap)
			SM_ENTER(SUPP_BE, REQUEST);
		else if (sm->eapFail)
			SM_ENTER(SUPP_BE, FAIL);
		else if (sm->authWhile == 0)
			SM_ENTER(SUPP_BE, TIMEOUT);
		else if (sm->eapSuccess)
			SM_ENTER(SUPP_BE, SUCCESS);
		break;
	}
}


static void eapol_sm_txLogoff(struct eapol_sm *sm)
{
	wpa_printf(MSG_DEBUG, "EAPOL: txLogoff");
	sm->ctx->eapol_send(sm->ctx->ctx, IEEE802_1X_TYPE_EAPOL_LOGOFF, "", 0);
	sm->dot1xSuppEapolLogoffFramesTx++;
	sm->dot1xSuppEapolFramesTx++;
}


static void eapol_sm_txStart(struct eapol_sm *sm)
{
	wpa_printf(MSG_DEBUG, "EAPOL: txStart");
	sm->ctx->eapol_send(sm->ctx->ctx, IEEE802_1X_TYPE_EAPOL_START, "", 0);
	sm->dot1xSuppEapolStartFramesTx++;
	sm->dot1xSuppEapolFramesTx++;
}


#define IEEE8021X_ENCR_KEY_LEN 32
#define IEEE8021X_SIGN_KEY_LEN 32

struct eap_key_data {
	u8 encr_key[IEEE8021X_ENCR_KEY_LEN];
	u8 sign_key[IEEE8021X_SIGN_KEY_LEN];
};


static void eapol_sm_processKey(struct eapol_sm *sm)
{
	struct ieee802_1x_hdr *hdr;
	struct ieee802_1x_eapol_key *key;
	struct eap_key_data keydata;
	u8 orig_key_sign[IEEE8021X_KEY_SIGN_LEN], datakey[32];
	u8 ekey[IEEE8021X_KEY_IV_LEN + IEEE8021X_ENCR_KEY_LEN];
	int key_len, res, sign_key_len, encr_key_len;

	wpa_printf(MSG_DEBUG, "EAPOL: processKey");
	if (sm->last_rx_key == NULL)
		return;

	if (!sm->conf.accept_802_1x_keys) {
		wpa_printf(MSG_WARNING, "EAPOL: Received IEEE 802.1X EAPOL-Key"
			   " even though this was not accepted - "
			   "ignoring this packet");
		return;
	}

	hdr = (struct ieee802_1x_hdr *) sm->last_rx_key;
	key = (struct ieee802_1x_eapol_key *) (hdr + 1);
	if (sizeof(*hdr) + htons(hdr->length) > sm->last_rx_key_len) {
		wpa_printf(MSG_WARNING, "EAPOL: Too short EAPOL-Key frame");
		return;
	}
	wpa_printf(MSG_DEBUG, "EAPOL: RX IEEE 802.1X ver=%d type=%d len=%d "
		   "EAPOL-Key: type=%d key_length=%d key_index=0x%x",
		   hdr->version, hdr->type, htons(hdr->length),
		   key->type, ntohs(key->key_length), key->key_index);

	sign_key_len = IEEE8021X_SIGN_KEY_LEN;
	encr_key_len = IEEE8021X_ENCR_KEY_LEN;
	res = eapol_sm_get_key(sm, (u8 *) &keydata, sizeof(keydata));
	if (res < 0) {
		wpa_printf(MSG_DEBUG, "EAPOL: Could not get master key for "
			   "decrypting EAPOL-Key keys");
		return;
	}
	if (res == 16) {
		/* LEAP derives only 16 bytes of keying material. */
		res = eapol_sm_get_key(sm, (u8 *) &keydata, 16);
		if (res) {
			wpa_printf(MSG_DEBUG, "EAPOL: Could not get LEAP "
				   "master key for decrypting EAPOL-Key keys");
			return;
		}
		sign_key_len = 16;
		encr_key_len = 16;
		memcpy(keydata.sign_key, keydata.encr_key, 16);
	} else if (res) {
		wpa_printf(MSG_DEBUG, "EAPOL: Could not get enough master key "
			   "data for decrypting EAPOL-Key keys (res=%d)", res);
		return;
	}

	/* The key replay_counter must increase when same master key */
	if (sm->replay_counter_valid &&
	    memcmp(sm->last_replay_counter, key->replay_counter,
		   IEEE8021X_REPLAY_COUNTER_LEN) >= 0) {
		wpa_printf(MSG_WARNING, "EAPOL: EAPOL-Key replay counter did "
			   "not increase - ignoring key");
		wpa_hexdump(MSG_DEBUG, "EAPOL: last replay counter",
			    sm->last_replay_counter,
			    IEEE8021X_REPLAY_COUNTER_LEN);
		wpa_hexdump(MSG_DEBUG, "EAPOL: received replay counter",
			    key->replay_counter, IEEE8021X_REPLAY_COUNTER_LEN);
		return;
	}

	/* Verify key signature (HMAC-MD5) */
	memcpy(orig_key_sign, key->key_signature, IEEE8021X_KEY_SIGN_LEN);
	memset(key->key_signature, 0, IEEE8021X_KEY_SIGN_LEN);
	hmac_md5(keydata.sign_key, sign_key_len,
		 sm->last_rx_key, sizeof(*hdr) + htons(hdr->length),
		 key->key_signature);
	if (memcmp(orig_key_sign, key->key_signature, IEEE8021X_KEY_SIGN_LEN)
	    != 0) {
		wpa_printf(MSG_DEBUG, "EAPOL: Invalid key signature in "
			   "EAPOL-Key packet");
		memcpy(key->key_signature, orig_key_sign,
		       IEEE8021X_KEY_SIGN_LEN);
		return;
	}
	wpa_printf(MSG_DEBUG, "EAPOL: EAPOL-Key key signature verified");

	key_len = ntohs(hdr->length) - sizeof(*key);
	if (key_len > 32 || ntohs(key->key_length) > 32) {
		wpa_printf(MSG_WARNING, "EAPOL: Too long key data length %d",
			   key_len ? key_len : ntohs(key->key_length));
		return;
	}
	if (key_len == ntohs(key->key_length)) {
		memcpy(ekey, key->key_iv, IEEE8021X_KEY_IV_LEN);
		memcpy(ekey + IEEE8021X_KEY_IV_LEN, keydata.encr_key,
		       encr_key_len);
		memcpy(datakey, key + 1, key_len);
		rc4(datakey, key_len, ekey,
		    IEEE8021X_KEY_IV_LEN + encr_key_len);
		wpa_hexdump(MSG_DEBUG, "EAPOL: Decrypted(RC4) key",
			    datakey, key_len);
	} else if (key_len == 0) {
		/* IEEE 802.1X-REV specifies that least significant Key Length
		 * octets from MS-MPPE-Send-Key are used as the key if the key
		 * data is not present. This seems to be meaning the beginning
		 * of the MS-MPPE-Send-Key. In addition, MS-MPPE-Send-Key in
		 * Supplicant corresponds to MS-MPPE-Recv-Key in Authenticator.
		 * Anyway, taking the beginning of the keying material from EAP
		 * seems to interoperate with Authenticators. */
		key_len = ntohs(key->key_length);
		memcpy(datakey, keydata.encr_key, key_len);
		wpa_hexdump(MSG_DEBUG, "EAPOL: using part of EAP keying "
			    "material data encryption key", datakey, key_len);
	} else {
		wpa_printf(MSG_DEBUG, "EAPOL: Invalid key data length %d "
			   "(key_length=%d)", key_len, ntohs(key->key_length));
		return;
	}

	sm->replay_counter_valid = TRUE;
	memcpy(sm->last_replay_counter, key->replay_counter,
	       IEEE8021X_REPLAY_COUNTER_LEN);

	wpa_printf(MSG_DEBUG, "EAPOL: Setting dynamic WEP key: %s keyidx %d "
		   "len %d",
		   key->key_index & IEEE8021X_KEY_INDEX_FLAG ?
		   "unicast" : "broadcast",
		   key->key_index & IEEE8021X_KEY_INDEX_MASK, key_len);

	if (sm->ctx->set_wep_key &&
	    sm->ctx->set_wep_key(sm->ctx->ctx,
				 key->key_index & IEEE8021X_KEY_INDEX_FLAG,
				 key->key_index & IEEE8021X_KEY_INDEX_MASK,
				 datakey, key_len) < 0) {
		wpa_printf(MSG_WARNING, "EAPOL: Failed to set WEP key to the "
			   " driver.");
	} else {
		if (key->key_index & IEEE8021X_KEY_INDEX_FLAG)
			sm->unicast_key_received = TRUE;
		else
			sm->broadcast_key_received = TRUE;

		if ((sm->unicast_key_received ||
		     !(sm->conf.required_keys & EAPOL_REQUIRE_KEY_UNICAST)) &&
		    (sm->broadcast_key_received ||
		     !(sm->conf.required_keys & EAPOL_REQUIRE_KEY_BROADCAST)))
		{
			wpa_printf(MSG_DEBUG, "EAPOL: all required EAPOL-Key "
				   "frames received");
			sm->portValid = TRUE;
			if (sm->ctx->eapol_done_cb)
				sm->ctx->eapol_done_cb(sm->ctx->ctx);
		}
	}
}


static void eapol_sm_getSuppRsp(struct eapol_sm *sm)
{
	wpa_printf(MSG_DEBUG, "EAPOL: getSuppRsp");
	/* FIX: EAP layer processing */
}


static void eapol_sm_txSuppRsp(struct eapol_sm *sm)
{
	wpa_printf(MSG_DEBUG, "EAPOL: txSuppRsp");
	if (sm->eap->eapRespData == NULL) {
		wpa_printf(MSG_WARNING, "EAPOL: txSuppRsp - EAP response data "
			   "not available");
		return;
	}

	/* Send EAP-Packet from the EAP layer to the Authenticator */
	sm->ctx->eapol_send(sm->ctx->ctx, IEEE802_1X_TYPE_EAP_PACKET,
			    sm->eap->eapRespData, sm->eap->eapRespDataLen);

	/* eapRespData is not used anymore, so free it here */
	free(sm->eap->eapRespData);
	sm->eap->eapRespData = NULL;

	if (sm->initial_req)
		sm->dot1xSuppEapolReqIdFramesRx++;
	else
		sm->dot1xSuppEapolReqFramesRx++;
	sm->dot1xSuppEapolRespFramesTx++;
	sm->dot1xSuppEapolFramesTx++;
}


static void eapol_sm_abortSupp(struct eapol_sm *sm)
{
	/* release system resources that may have been allocated for the
	 * authentication session */
	free(sm->last_rx_key);
	sm->last_rx_key = NULL;
	free(sm->eapReqData);
	sm->eapReqData = NULL;
	eap_sm_abort(sm->eap);
}


struct eapol_sm *eapol_sm_init(struct eapol_ctx *ctx)
{
	struct eapol_sm *sm;
	sm = malloc(sizeof(*sm));
	if (sm == NULL)
		return NULL;
	memset(sm, 0, sizeof(*sm));
	sm->ctx = ctx;

	sm->portControl = Auto;

	/* Supplicant PAE state machine */
	sm->heldPeriod = 60;
	sm->startPeriod = 30;
	sm->maxStart = 3;

	/* Supplicant Backend state machine */
	sm->authPeriod = 30;

	sm->eap = eap_sm_init(sm);
	if (sm->eap == NULL) {
		free(sm);
		return NULL;
	}

	/* Initialize EAPOL state machines */
	sm->initialize = TRUE;
	eapol_sm_step(sm);
	sm->initialize = FALSE;
	eapol_sm_step(sm);

	eloop_register_timeout(1, 0, eapol_port_timers_tick, NULL, sm);

	return sm;
}


void eapol_sm_deinit(struct eapol_sm *sm)
{
	if (sm == NULL)
		return;
	eloop_cancel_timeout(eapol_sm_step_timeout, NULL, sm);
	eloop_cancel_timeout(eapol_port_timers_tick, NULL, sm);
	eap_sm_deinit(sm->eap);
	free(sm->last_rx_key);
	free(sm->eapReqData);
	free(sm->ctx);
	free(sm);
}


static void eapol_sm_step_timeout(void *eloop_ctx, void *timeout_ctx)
{
	eapol_sm_step(timeout_ctx);
}


void eapol_sm_step(struct eapol_sm *sm)
{
	sm->changed = FALSE;
	SM_STEP_RUN(SUPP_PAE);
	SM_STEP_RUN(KEY_RX);
	SM_STEP_RUN(SUPP_BE);
	if (eap_sm_step(sm->eap))
		sm->changed = TRUE;
	if (sm->changed) {
		/* restart EAPOL state machine step from timeout call in order
		 * to allow other events to be processed. */
		eloop_cancel_timeout(eapol_sm_step_timeout, NULL, sm);
		eloop_register_timeout(0, 0, eapol_sm_step_timeout, NULL, sm);
	}

	if (sm->ctx->cb && sm->cb_status != EAPOL_CB_IN_PROGRESS) {
		int success = sm->cb_status == EAPOL_CB_SUCCESS ? 1 : 0;
		sm->cb_status = EAPOL_CB_IN_PROGRESS;
		sm->ctx->cb(sm, success, sm->ctx->cb_ctx);
	}

}


static const char *eapol_supp_pae_state(int state)
{
	switch (state) {
	case SUPP_PAE_LOGOFF:
		return "LOGOFF";
	case SUPP_PAE_DISCONNECTED:
		return "DISCONNECTED";
	case SUPP_PAE_CONNECTING:
		return "CONNECTING";
	case SUPP_PAE_AUTHENTICATING:
		return "AUTHENTICATING";
	case SUPP_PAE_HELD:
		return "HELD";
	case SUPP_PAE_AUTHENTICATED:
		return "AUTHENTICATED";
	case SUPP_PAE_RESTART:
		return "RESTART";
	default:
		return "UNKNOWN";
	}
}


static const char *eapol_supp_be_state(int state)
{
	switch (state) {
	case SUPP_BE_REQUEST:
		return "REQUEST";
	case SUPP_BE_RESPONSE:
		return "RESPONSE";
	case SUPP_BE_SUCCESS:
		return "SUCCESS";
	case SUPP_BE_FAIL:
		return "FAIL";
	case SUPP_BE_TIMEOUT:
		return "TIMEOUT";
	case SUPP_BE_IDLE:
		return "IDLE";
	case SUPP_BE_INITIALIZE:
		return "INITIALIZE";
	case SUPP_BE_RECEIVE:
		return "RECEIVE";
	default:
		return "UNKNOWN";
	}
}


static const char * eapol_port_status(PortStatus status)
{
	if (status == Authorized)
		return "Authorized";
	else
		return "Unauthorized";
}


static const char * eapol_port_control(PortControl ctrl)
{
	switch (ctrl) {
	case Auto:
		return "Auto";
	case ForceUnauthorized:
		return "ForceUnauthorized";
	case ForceAuthorized:
		return "ForceAuthorized";
	default:
		return "Unknown";
	}
}


void eapol_sm_configure(struct eapol_sm *sm, int heldPeriod, int authPeriod,
			int startPeriod, int maxStart)
{
	if (sm == NULL)
		return;
	if (heldPeriod >= 0)
		sm->heldPeriod = heldPeriod;
	if (authPeriod >= 0)
		sm->authPeriod = authPeriod;
	if (startPeriod >= 0)
		sm->startPeriod = startPeriod;
	if (maxStart >= 0)
		sm->maxStart = maxStart;
}


int eapol_sm_get_status(struct eapol_sm *sm, char *buf, size_t buflen)
{
	int len;
	if (sm == NULL)
		return 0;

	len = snprintf(buf, buflen,
		       "Supplicant PAE state=%s\n"
		       "heldPeriod=%d\n"
		       "authPeriod=%d\n"
		       "startPeriod=%d\n"
		       "maxStart=%d\n"
		       "suppPortStatus=%s\n"
		       "portControl=%s\n"
		       "Supplicant Backend state=%s\n",
		       eapol_supp_pae_state(sm->SUPP_PAE_state),
		       sm->heldPeriod,
		       sm->authPeriod,
		       sm->startPeriod,
		       sm->maxStart,
		       eapol_port_status(sm->suppPortStatus),
		       eapol_port_control(sm->portControl),
		       eapol_supp_be_state(sm->SUPP_BE_state));
	len += eap_sm_get_status(sm->eap, buf + len, buflen - len);
	return len;
}


int eapol_sm_get_mib(struct eapol_sm *sm, char *buf, size_t buflen)
{
	int len;
	if (sm == NULL)
		return 0;
	len = snprintf(buf, buflen,
		       "dot1xSuppPaeState=%d\n"
		       "dot1xSuppHeldPeriod=%d\n"
		       "dot1xSuppAuthPeriod=%d\n"
		       "dot1xSuppStartPeriod=%d\n"
		       "dot1xSuppMaxStart=%d\n"
		       "dot1xSuppSuppControlledPortStatus=%s\n"
		       "dot1xSuppBackendPaeState=%d\n"
		       "dot1xSuppEapolFramesRx=%d\n"
		       "dot1xSuppEapolFramesTx=%d\n"
		       "dot1xSuppEapolStartFramesTx=%d\n"
		       "dot1xSuppEapolLogoffFramesTx=%d\n"
		       "dot1xSuppEapolRespFramesTx=%d\n"
		       "dot1xSuppEapolReqIdFramesRx=%d\n"
		       "dot1xSuppEapolReqFramesRx=%d\n"
		       "dot1xSuppInvalidEapolFramesRx=%d\n"
		       "dot1xSuppEapLengthErrorFramesRx=%d\n"
		       "dot1xSuppLastEapolFrameVersion=%d\n"
		       "dot1xSuppLastEapolFrameSource=" MACSTR "\n",
		       sm->SUPP_PAE_state,
		       sm->heldPeriod,
		       sm->authPeriod,
		       sm->startPeriod,
		       sm->maxStart,
		       sm->suppPortStatus == Authorized ?
		       "Authorized" : "Unauthorized",
		       sm->SUPP_BE_state,
		       sm->dot1xSuppEapolFramesRx,
		       sm->dot1xSuppEapolFramesTx,
		       sm->dot1xSuppEapolStartFramesTx,
		       sm->dot1xSuppEapolLogoffFramesTx,
		       sm->dot1xSuppEapolRespFramesTx,
		       sm->dot1xSuppEapolReqIdFramesRx,
		       sm->dot1xSuppEapolReqFramesRx,
		       sm->dot1xSuppInvalidEapolFramesRx,
		       sm->dot1xSuppEapLengthErrorFramesRx,
		       sm->dot1xSuppLastEapolFrameVersion,
		       MAC2STR(sm->dot1xSuppLastEapolFrameSource));
	return len;
}


void eapol_sm_rx_eapol(struct eapol_sm *sm, u8 *src, u8 *buf, size_t len)
{
	struct ieee802_1x_hdr *hdr;
	struct ieee802_1x_eapol_key *key;
	int plen, data_len;

	if (sm == NULL)
		return;
	sm->dot1xSuppEapolFramesRx++;
	if (len < sizeof(*hdr)) {
		sm->dot1xSuppInvalidEapolFramesRx++;
		return;
	}
	hdr = (struct ieee802_1x_hdr *) buf;
	sm->dot1xSuppLastEapolFrameVersion = hdr->version;
	memcpy(sm->dot1xSuppLastEapolFrameSource, src, ETH_ALEN);
	if (hdr->version < EAPOL_VERSION) {
		/* TODO: backwards compatibility */
	}
	plen = ntohs(hdr->length);
	if (plen > len - sizeof(*hdr)) {
		sm->dot1xSuppEapLengthErrorFramesRx++;
		return;
	}
	data_len = plen + sizeof(*hdr);

	switch (hdr->type) {
	case IEEE802_1X_TYPE_EAP_PACKET:
		if (sm->cached_pmk) {
			/* Trying to use PMKSA caching, but Authenticator did
			 * not seem to have a matching entry. Need to restart
			 * EAPOL state machines.
			 */
			eapol_sm_abort_cached(sm);
		}
		free(sm->eapReqData);
		sm->eapReqDataLen = plen;
		sm->eapReqData = malloc(sm->eapReqDataLen);
		if (sm->eapReqData) {
			wpa_printf(MSG_DEBUG, "EAPOL: Received EAP-Packet "
				   "frame");
			memcpy(sm->eapReqData, (u8 *) (hdr + 1),
			       sm->eapReqDataLen);
			sm->eapolEap = TRUE;
			eapol_sm_step(sm);
		}
		break;
	case IEEE802_1X_TYPE_EAPOL_KEY:
		if (plen < sizeof(*key)) {
			wpa_printf(MSG_DEBUG, "EAPOL: Too short EAPOL-Key "
				   "frame received");
			break;
		}
		key = (struct ieee802_1x_eapol_key *) (hdr + 1);
		if (key->type == EAPOL_KEY_TYPE_WPA ||
		    key->type == EAPOL_KEY_TYPE_RSN) {
			/* WPA Supplicant takes care of this frame. */
			wpa_printf(MSG_DEBUG, "EAPOL: Ignoring WPA EAPOL-Key "
				   "frame in EAPOL state machines");
			break;
		}
		if (key->type != EAPOL_KEY_TYPE_RC4) {
			wpa_printf(MSG_DEBUG, "EAPOL: Ignored unknown "
				   "EAPOL-Key type %d", key->type);
			break;
		}
		free(sm->last_rx_key);
		sm->last_rx_key = malloc(data_len);
		if (sm->last_rx_key) {
			wpa_printf(MSG_DEBUG, "EAPOL: Received EAPOL-Key "
				   "frame");
			memcpy(sm->last_rx_key, buf, data_len);
			sm->last_rx_key_len = data_len;
			sm->rxKey = TRUE;
			eapol_sm_step(sm);
		}
		break;
	default:
		wpa_printf(MSG_DEBUG, "EAPOL: Received unknown EAPOL type %d",
			   hdr->type);
		sm->dot1xSuppInvalidEapolFramesRx++;
		break;
	}
}


void eapol_sm_notify_portEnabled(struct eapol_sm *sm, Boolean enabled)
{
	if (sm == NULL)
		return;
	wpa_printf(MSG_DEBUG, "EAPOL: External notification - "
		   "portEnabled=%d", enabled);
	sm->portEnabled = enabled;
	eapol_sm_step(sm);
}


void eapol_sm_notify_portValid(struct eapol_sm *sm, Boolean valid)
{
	if (sm == NULL)
		return;
	wpa_printf(MSG_DEBUG, "EAPOL: External notification - "
		   "portValid=%d", valid);
	sm->portValid = valid;
	eapol_sm_step(sm);
}


void eapol_sm_notify_eap_success(struct eapol_sm *sm, Boolean success)
{
	if (sm == NULL)
		return;
	wpa_printf(MSG_DEBUG, "EAPOL: External notification - "
		   "EAP success=%d", success);
	sm->eapSuccess = success;
	sm->altAccept = success;
	if (success)
		sm->eap->decision = DECISION_COND_SUCC;
	eapol_sm_step(sm);
}


void eapol_sm_notify_eap_fail(struct eapol_sm *sm, Boolean fail)
{
	if (sm == NULL)
		return;
	wpa_printf(MSG_DEBUG, "EAPOL: External notification - "
		   "EAP fail=%d", fail);
	sm->eapFail = fail;
	sm->altReject = fail;
	eapol_sm_step(sm);
}


void eapol_sm_notify_config(struct eapol_sm *sm, struct wpa_ssid *config,
			    struct eapol_config *conf)
{
	if (sm == NULL)
		return;

	sm->config = config;

	if (conf == NULL)
		return;

	sm->conf.accept_802_1x_keys = conf->accept_802_1x_keys;
	sm->conf.required_keys = conf->required_keys;
}


int eapol_sm_get_key(struct eapol_sm *sm, u8 *key, size_t len)
{
	if (sm == NULL || sm->eap == NULL || !sm->eap->eapKeyAvailable ||
	    sm->eap->eapKeyData == NULL)
		return -1;
	if (len > sm->eap->eapKeyDataLen)
		return sm->eap->eapKeyDataLen;
	memcpy(key, sm->eap->eapKeyData, len);
	return 0;
}


void eapol_sm_notify_logoff(struct eapol_sm *sm, Boolean logoff)
{
	if (sm) {
		sm->userLogoff = logoff;
		eapol_sm_step(sm);
	}
}


void eapol_sm_notify_cached(struct eapol_sm *sm)
{
	if (sm == NULL)
		return;
	sm->SUPP_PAE_state = SUPP_PAE_AUTHENTICATED;
	sm->suppPortStatus = Authorized;
	if (sm->eap) {
		sm->eap->decision = DECISION_COND_SUCC;
		sm->eap->EAP_state = EAP_SUCCESS;
	}
}


void eapol_sm_notify_pmkid_attempt(struct eapol_sm *sm)
{
	if (sm == NULL)
		return;
	wpa_printf(MSG_DEBUG, "RSN: Trying to use cached PMKSA");
	sm->cached_pmk = TRUE;
}


static void eapol_sm_abort_cached(struct eapol_sm *sm)
{
	wpa_printf(MSG_DEBUG, "RSN: Authenticator did not accept PMKID, "
		   "doing full EAP authentication");
	if (sm == NULL)
		return;
	sm->cached_pmk = FALSE;
	sm->SUPP_PAE_state = SUPP_PAE_CONNECTING;
	sm->suppPortStatus = Unauthorized;
	sm->eapRestart= TRUE;
}


void eapol_sm_register_scard_ctx(struct eapol_sm *sm, void *ctx)
{
	if (sm)
		sm->ctx->scard_ctx = ctx;
}


void eapol_sm_notify_portControl(struct eapol_sm *sm, PortControl portControl)
{
	if (sm == NULL)
		return;
	wpa_printf(MSG_DEBUG, "EAPOL: External notification - "
		   "portControl=%s", eapol_port_control(portControl));
	sm->portControl = portControl;
	eapol_sm_step(sm);
}


void eapol_sm_notify_ctrl_attached(struct eapol_sm *sm)
{
	if (sm == NULL)
		return;
	eap_sm_notify_ctrl_attached(sm->eap);
}


void eapol_sm_notify_ctrl_response(struct eapol_sm *sm)
{
	if (sm == NULL)
		return;
	if (sm->eapReqData && !sm->eapReq) {
		wpa_printf(MSG_DEBUG, "EAPOL: received control response (user "
			   "input) notification - retrying pending EAP "
			   "Request");
		sm->eapolEap = TRUE;
		eapol_sm_step(sm);
	}
}

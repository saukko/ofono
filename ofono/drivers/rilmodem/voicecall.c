/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012 Canonical Ltd.
 *  Copyright (C) 2013 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/voicecall.h>

#include "gril.h"
#include "grilutil.h"

#include "common.h"
#include "rilmodem.h"

/* Amount of ms we wait between CLCC calls */
#define POLL_CLCC_INTERVAL 300

/* When +VTD returns 0, an unspecified manufacturer-specific delay is used */
#define TONE_DURATION 1000

#define FLAG_NEED_CLIP 1

struct voicecall_data {
	GSList *calls;
	unsigned int local_release;
	unsigned int clcc_source;
	GRil *ril;
	unsigned int vendor;
	unsigned int tone_duration;
	guint vts_source;
	unsigned int vts_delay;
	unsigned char flags;
	ofono_voicecall_cb_t cb;
	void *data;
};

struct release_id_req {
	struct ofono_voicecall *vc;
	ofono_voicecall_cb_t cb;
	void *data;
	int id;
};

struct change_state_req {
	struct ofono_voicecall *vc;
	ofono_voicecall_cb_t cb;
	void *data;
	int affected_types;
};

struct lastcause_req {
	struct ofono_voicecall *vc;
	int id;
};

static void lastcause_cb(struct ril_msg *message, gpointer user_data)
{
	struct lastcause_req *reqdata = user_data;
	struct ofono_voicecall *vc = reqdata->vc;
	int id = reqdata->id;

	enum ofono_disconnect_reason reason = OFONO_DISCONNECT_REASON_ERROR;
	int last_cause = CALL_FAIL_ERROR_UNSPECIFIED;
	struct parcel rilp;
	ril_util_init_parcel(message, &rilp);
	if (parcel_r_int32(&rilp) > 0)
		last_cause = parcel_r_int32(&rilp);

	DBG("Call %d ended with RIL cause %d", id, last_cause);
	if (last_cause == CALL_FAIL_NORMAL || last_cause == CALL_FAIL_BUSY) {
		reason = OFONO_DISCONNECT_REASON_REMOTE_HANGUP;
	}

	ofono_voicecall_disconnected(vc, id, reason, NULL);
}

static void clcc_poll_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GSList *calls;
	GSList *n, *o;
	struct ofono_call *nc, *oc;
	struct ofono_error error;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("We are polling CLCC and received an error");
		ofono_error("All bets are off for call management");
		return;
	}

	calls = ril_util_parse_clcc(vd->ril, message);

	n = calls;
	o = vd->calls;

	while (n || o) {
		nc = n ? n->data : NULL;
		oc = o ? o->data : NULL;

		if (oc && (nc == NULL || (nc->id > oc->id))) {
			if (vd->local_release & (1 << oc->id)) {
				ofono_voicecall_disconnected(vc, oc->id,
					OFONO_DISCONNECT_REASON_LOCAL_HANGUP, NULL);
			} else {
				/* Get disconnect cause before informing oFono core */
				struct lastcause_req *reqdata =
						g_try_new0(struct lastcause_req, 1);
				if (reqdata) {
					reqdata->vc = user_data;
					reqdata->id = oc->id;
					g_ril_send(vd->ril, RIL_REQUEST_LAST_CALL_FAIL_CAUSE,
						NULL, 0, lastcause_cb, reqdata, g_free);
				}
			}

			o = o->next;
		} else if (nc && (oc == NULL || (nc->id < oc->id))) {
			/* new call, signal it */
			if (nc->type) {
				ofono_voicecall_notify(vc, nc);
				if (vd->cb) {
					ofono_voicecall_cb_t cb = vd->cb;
					decode_ril_error(&error, "OK");
					cb(&error, vd->data);
					vd->cb = NULL;
					vd->data = NULL;
				}
			}

			n = n->next;
		} else {
			/*
			 * Always use the clip_validity from old call
			 * the only place this is truly told to us is
			 * in the CLIP notify, the rest are fudged
			 * anyway.  Useful when RING, CLIP is used,
			 * and we're forced to use CLCC and clip_validity
			 * is 1
			 */
			if (oc->clip_validity == 1)
				nc->clip_validity = oc->clip_validity;

			nc->cnap_validity = oc->cnap_validity;

			/*
			 * CDIP doesn't arrive as part of CLCC, always
			 * re-use from the old call
			 */
			memcpy(&nc->called_number, &oc->called_number,
					sizeof(oc->called_number));

			/*
			 * If the CLIP is not provided and the CLIP never
			 * arrives, or RING is used, then signal the call
			 * here
			 */
			if (nc->status == CALL_STATUS_INCOMING &&
					(vd->flags & FLAG_NEED_CLIP)) {
				if (nc->type)
					ofono_voicecall_notify(vc, nc);

				vd->flags &= ~FLAG_NEED_CLIP;
			} else if (memcmp(nc, oc, sizeof(*nc)) && nc->type)
				ofono_voicecall_notify(vc, nc);

			n = n->next;
			o = o->next;
		}
	}

	g_slist_foreach(vd->calls, (GFunc) g_free, NULL);
	g_slist_free(vd->calls);

	vd->calls = calls;
	vd->local_release = 0;
}

static gboolean poll_clcc(gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	int request = RIL_REQUEST_GET_CURRENT_CALLS;
	int ret;

	ret = g_ril_send(vd->ril, request, NULL,
			0, clcc_poll_cb, vc, NULL);

	g_ril_print_request_no_args(vd->ril, ret, request);

	vd->clcc_source = 0;

	return FALSE;
}

static void generic_cb(struct ril_msg *message, gpointer user_data)
{
	struct change_state_req *req = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(req->vc);
	struct ofono_error error;
	int request = RIL_REQUEST_GET_CURRENT_CALLS;
	int ret;

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		decode_ril_error(&error, "FAIL");
		goto out;
	}

	g_ril_print_response_no_args(vd->ril, message);

	if (req->affected_types) {
		GSList *l;
		struct ofono_call *call;

		for (l = vd->calls; l; l = l->next) {
			call = l->data;

			if (req->affected_types & (1 << call->status))
				vd->local_release |= (1 << call->id);
		}
	}

out:
	ret = g_ril_send(vd->ril, request, NULL,
			0, clcc_poll_cb, req->vc, NULL);

	g_ril_print_request_no_args(vd->ril, ret, request);

	/* We have to callback after we schedule a poll if required */
	if (req->cb)
		req->cb(&error, req->data);
}

static int ril_template(const guint rreq, struct ofono_voicecall *vc,
			GRilResponseFunc func, unsigned int affected_types,
			gpointer pdata, const gsize psize,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct change_state_req *req = g_try_new0(struct change_state_req, 1);
	int ret;

	if (req == NULL)
		goto error;

	req->vc = vc;
	req->cb = cb;
	req->data = data;
	req->affected_types = affected_types;

	ret = g_ril_send(vd->ril, rreq, pdata, psize, func, req, g_free);
	if (ret > 0)
		return ret;
error:
	g_free(req);

	if (cb)
		CALLBACK_WITH_FAILURE(cb, data);

	return 0;
}

static void rild_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_voicecall *vc = cbd->user;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	ofono_voicecall_cb_t cb = cbd->cb;
	struct ofono_error error;
	struct ofono_call *call;
	GSList *l;

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
	} else {
		decode_ril_error(&error, "FAIL");
		goto out;
	}

	g_ril_print_response_no_args(vd->ril, message);

	/* On a success, make sure to put all active calls on hold */
	for (l = vd->calls; l; l = l->next) {
		call = l->data;

		if (call->status != CALL_STATUS_ACTIVE)
			continue;

		call->status = CALL_STATUS_HELD;
		ofono_voicecall_notify(vc, call);
	}

	/* CLCC will update the oFono call list with proper ids  */
	if (!vd->clcc_source)
		vd->clcc_source = g_timeout_add(POLL_CLCC_INTERVAL,
						poll_clcc, vc);

	/* we cannot answer just yet since we don't know the
	 * call id */
	vd->cb = cb;
	vd->data = cbd->data;
	return;
out:
	cb(&error, cbd->data);
}

static void ril_dial(struct ofono_voicecall *vc,
			const struct ofono_phone_number *ph,
			enum ofono_clir_option clir, ofono_voicecall_cb_t cb,
			void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct parcel rilp;
	int request = RIL_REQUEST_DIAL;
	int ret;

	cbd->user = vc;

	parcel_init(&rilp);

	/* Number to dial */
        parcel_w_string(&rilp, (char *) phone_number_to_string(ph));
	/* CLIR mode */
	parcel_w_int32(&rilp, clir);
	/* USS, need it twice for absent */
	/* TODO: Deal with USS properly */
	parcel_w_int32(&rilp, 0);
	parcel_w_int32(&rilp, 0);

	/* Send request to RIL */
	ret = g_ril_send(vd->ril, request, rilp.data,
				rilp.size, rild_cb, cbd, g_free);

	g_ril_append_print_buf(vd->ril, "(%s,%d,0,0)",
				phone_number_to_string(ph),
				clir);

	g_ril_print_request(vd->ril, ret, request);

	parcel_free(&rilp);

	/* In case of error free cbd and return the cb with failure */
	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	}
}

static void ril_hangup_all(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct parcel rilp;
	struct ofono_error error;
	struct ofono_call *call;
	GSList *l;
	int request = RIL_REQUEST_HANGUP;
	int ret;

	for (l = vd->calls; l; l = l->next) {
		call = l->data;
		/* TODO: Hangup just the active ones once we have call
		 * state tracking (otherwise it can't handle ringing) */
		DBG("Hanging up call with id %d", call->id);
		parcel_init(&rilp);
		parcel_w_int32(&rilp, 1); /* Always 1 - AT+CHLD=1x */
		parcel_w_int32(&rilp, call->id);

		/* Send request to RIL */
		ret = ril_template(request, vc, generic_cb, 0x3f,
					rilp.data, rilp.size, NULL, NULL);

		g_ril_append_print_buf(vd->ril, "(%d)", call->id);
		g_ril_print_request(vd->ril, ret, request);

		parcel_free(&rilp);
	}

	/* TODO: Deal in case of an error at hungup */
	decode_ril_error(&error, "OK");
	cb(&error, data);
}

static void ril_call_state_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;

	if (message->req != RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED)
		goto error;

	/* Just need to request the call list again */
	poll_clcc(vc);

	return;

error:
	ofono_error("Unable to notify about call state changes");
}

static void ril_ss_notify(struct ril_msg *message, gpointer user_data)
{
	struct parcel rilp;
	struct ofono_voicecall *vc = user_data;
	struct ofono_phone_number number;
	int notification_type = 0;
	int code = 0;
	int index = 0;
	char *tmp_number = NULL;

	ril_util_init_parcel(message, &rilp);

	switch (message->req) {
		case RIL_UNSOL_SUPP_SVC_NOTIFICATION: {
			notification_type = parcel_r_int32(&rilp);
			code = parcel_r_int32(&rilp);
			index = parcel_r_int32(&rilp);
			parcel_r_int32(&rilp);
			tmp_number = parcel_r_string(&rilp);

			if (tmp_number != NULL)
				strncpy(number.number, tmp_number,
					OFONO_MAX_PHONE_NUMBER_LENGTH);

			DBG("RIL data: MT/MO: %i, code: %i, index: %i",
				notification_type, code, index);
		break;
		}
	default:
		goto error;
	}

	/* 0 stands for MO intermediate (support TBD), 1 for MT unsolicited */
	if (notification_type == 1) {
		ofono_voicecall_ssn_mt_notify(
			vc, 0, code, index, &number);
	} else
		goto error;

	return;

error:
	ofono_error("Unknown SS notification");
}

static void ril_answer(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	int request = RIL_REQUEST_ANSWER;
	int ret;

	DBG("Answering current call");

	/* Send request to RIL */
	ret = ril_template(request, vc, generic_cb, 0,
				NULL, 0, cb, data);

	g_ril_print_request_no_args(vd->ril, ret, request);
}

static void ril_send_dtmf(struct ofono_voicecall *vc, const char *dtmf,
		ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	int len = strlen(dtmf);
	struct parcel rilp;
	struct ofono_error error;
	char *ril_dtmf = g_try_malloc(sizeof(char) * 2);
	int request = RIL_REQUEST_DTMF;
	int i, ret;

	DBG("");

	/* Ril wants just one character, but we need to send as string */
	ril_dtmf[1] = '\0';

	for (i = 0; i < len; i++) {
		parcel_init(&rilp);
		ril_dtmf[0] = dtmf[i];
		parcel_w_string(&rilp, ril_dtmf);

		ret = g_ril_send(vd->ril, request, rilp.data,
				rilp.size, NULL, NULL, NULL);

		g_ril_append_print_buf(vd->ril, "(%s)", ril_dtmf);
		g_ril_print_request(vd->ril, ret, request);
		parcel_free(&rilp);

		/* TODO: should we break out of look on failure? */
		if (ret <= 0)
			ofono_error("send REQUEST_DTMF failed");
	}

	free(ril_dtmf);

	/* We don't really care about errors here */
	decode_ril_error(&error, "OK");
	cb(&error, data);
}

static void multiparty_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_error error;

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
		/* Need to update call statuses */
		struct ofono_voicecall *vc = user_data;
		struct voicecall_data *vd = ofono_voicecall_get_data(vc);
		g_ril_send(vd->ril, RIL_REQUEST_GET_CURRENT_CALLS, NULL,
				0, clcc_poll_cb, vc, NULL);
	} else {
		decode_ril_error(&error, "FAIL");
	}
}

static void ril_create_multiparty(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	g_ril_send(vd->ril, RIL_REQUEST_CONFERENCE, NULL,
			0, multiparty_cb, vc, NULL);

	struct ofono_error error = { .type = 0, .error = 0 };
	if (cb)
		cb(&error, data);
}

static void private_chat_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_error error;

	if (message->error == RIL_E_SUCCESS) {
		decode_ril_error(&error, "OK");
		/* Need to update call statuses */
		struct ofono_voicecall *vc = user_data;
		struct voicecall_data *vd = ofono_voicecall_get_data(vc);
		g_ril_send(vd->ril, RIL_REQUEST_GET_CURRENT_CALLS, NULL,
				0, clcc_poll_cb, vc, NULL);
	} else {
		decode_ril_error(&error, "FAIL");
	}
}

static void ril_private_chat(struct ofono_voicecall *vc, int id,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct parcel rilp;
	parcel_init(&rilp);
	parcel_w_int32(&rilp, 1);
	parcel_w_int32(&rilp, id);
	g_ril_send(vd->ril, RIL_REQUEST_SEPARATE_CONNECTION, rilp.data,
			rilp.size, private_chat_cb, vc, NULL);

	struct ofono_error error = { .type = 0, .error = 0 };
	if (cb)
		cb(&error, data);
}

static void ril_swap_without_accept(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	ril_template(RIL_REQUEST_SWITCH_HOLDING_AND_ACTIVE, vc, generic_cb, 0,
				NULL, 0, cb, data);
}

static void ril_release_all_held(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	ril_template(RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND, vc,
		    generic_cb, 0,
			NULL, 0, cb, data);
}

static gboolean enable_supp_svc(gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	int request = RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION;
	int ret;
	struct parcel rilp;

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 1); /* size of array */
	parcel_w_int32(&rilp, 1); /* notifications enabled */

	ret = g_ril_send(vd->ril, request, rilp.data,
			rilp.size, NULL, vc, NULL);

	g_ril_print_request(vd->ril, ret, request);

	parcel_free(&rilp);

	/* Makes this a single shot */
	return FALSE;
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	ofono_voicecall_register(vc);

	/* Initialize call list */
	poll_clcc(vc);

	/* Unsol when call state changes */
	g_ril_register(vd->ril, RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
			ril_call_state_notify, vc);

	/* Unsol when call set in hold */
	g_ril_register(vd->ril, RIL_UNSOL_SUPP_SVC_NOTIFICATION,
			ril_ss_notify, vc);

	/* request supplementary service notifications*/
	enable_supp_svc(vc);

	/* This makes the timeout a single-shot */
	return FALSE;
}

static int ril_voicecall_probe(struct ofono_voicecall *vc, unsigned int vendor,
				void *data)
{
	GRil *ril = data;
	struct voicecall_data *vd;

	vd = g_try_new0(struct voicecall_data, 1);
	if (vd == NULL)
		return -ENOMEM;

	vd->ril = g_ril_clone(ril);
	vd->vendor = vendor;
	vd->tone_duration = TONE_DURATION;
	vd->cb = NULL;
	vd->data = NULL;

	ofono_voicecall_set_data(vc, vd);

	/*
	 * TODO: analyze if capability check is needed
	 * and/or timer should be adjusted.
	 *
	 * ofono_voicecall_register() needs to be called after
	 * the driver has been set in ofono_voicecall_create(),
	 * which calls this function.  Most other drivers make
	 * some kind of capabilities query to the modem, and then
	 * call register in the callback; we use a timer instead.
	 */
	g_timeout_add_seconds(2, ril_delayed_register, vc);

	return 0;
}

static void ril_voicecall_remove(struct ofono_voicecall *vc)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	if (vd->clcc_source)
		g_source_remove(vd->clcc_source);

	if (vd->vts_source)
		g_source_remove(vd->vts_source);

	g_slist_foreach(vd->calls, (GFunc) g_free, NULL);
	g_slist_free(vd->calls);

	ofono_voicecall_set_data(vc, NULL);

	g_ril_unref(vd->ril);
	g_free(vd);
}

static struct ofono_voicecall_driver driver = {
	.name			= "rilmodem",
	.probe			= ril_voicecall_probe,
	.remove			= ril_voicecall_remove,
	.dial			= ril_dial,
	.answer			= ril_answer,
	.hangup_all		= ril_hangup_all,
	.send_tones		= ril_send_dtmf,
	.create_multiparty	= ril_create_multiparty,
	.private_chat		= ril_private_chat,
	.swap_without_accept = ril_swap_without_accept,
	.release_all_held = ril_release_all_held,
};

void ril_voicecall_init(void)
{
	ofono_voicecall_driver_register(&driver);
}

void ril_voicecall_exit(void)
{
	ofono_voicecall_driver_unregister(&driver);
}


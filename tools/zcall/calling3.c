/*
* Wire
* Copyright (C) 2016 Wire Swiss GmbH
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <re.h>
#include <avs.h>
#include <avs_wcall.h>
#include <avs_msystem.h>
#include <avs_audio_io.h>
#include "cli.h"
#include "view.h"
#include "options.h"

#define SFT_URL "http://3.126.116.55:8282"
//#define SFT_URL "http://18.185.29.166:8282"
//#define SFT_URL "http://18.185.29.166:8585"
//#define SFT_URL "http://127.0.0.1:8282"

WUSER_HANDLE calling3_get_wuser(void);

static struct {
	bool initialized;
	bool ready;
	int video_send_state;
	struct list postponel;
	WUSER_HANDLE wuser;
	struct http_cli *http_cli;
} calling3 = {
	.initialized = false,
	.ready = false,
	.video_send_state = WCALL_VIDEO_STATE_STOPPED,
	.wuser = WUSER_INVALID_HANDLE,
	.http_cli = NULL,
};

enum postpone_event {
	POSTPONE_EVENT_START,
	POSTPONE_EVENT_ANSWER,
	POSTPONE_EVENT_RECV_MSG,
};

struct postpone_recv_msg {
	char *convid;
	char *fromuid;
	char *fromcid;
	struct ztime ts;
	char *data;
};

struct postpone_elem {
	enum postpone_event ev;
	void *arg;
	
	struct le le;
};


static void rmsg_destructor(void *arg)
{
	struct postpone_recv_msg *rmsg = arg;

	mem_deref(rmsg->convid);
	mem_deref(rmsg->fromuid);
	mem_deref(rmsg->fromcid);
	mem_deref(rmsg->data);
}

#ifdef HAVE_CRYPTOBOX
static void econn_otr_resp_handler(int err, void *arg)
{
	struct engine_conv *conv = arg;
	// todo: add request and request_id, pass error call

	if (err == ECONNABORTED)
		return;

	if (err) {
		warning("calling: OTR Request failed (%m)\n", err);
		wcall_end(calling3.wuser, conv->id);
	}
}
#endif


static void wcall_incoming_handler(const char *convid, uint32_t msg_time,
				   const char *userid, const char *clientid,
				   int video_call, int should_ring,
				   void *arg)
{
	struct engine_conv *conv = NULL;
	struct engine_user *ruser;
	const char *dname = userid;
	int err;

	(void)arg;

	err = engine_lookup_conv(&conv, zcall_engine, convid);
	if (err) {
		warning("calling: cannot find conversation: %s: %m\n",
			convid, err);
	}

	if (0 == engine_lookup_user(&ruser, zcall_engine, userid, false)) {
		dname = ruser->display_name;
	}

	output("calling: incoming %s call in conv: %H from \"%s:%s\""
	       " ring: %s ts: %u\n",
	       video_call ? "video" : "audio",
	       engine_print_conv_name, conv,
	       dname, clientid, should_ring ? "yes" : "no",
	       msg_time);

	calling_incoming();
}


static void wcall_missed_handler(const char *convid, uint32_t msg_time,
				 const char *userid, const char *clientid,
				 int video_call /*bool*/,
				 void *arg)
{
	time_t when = msg_time;
	(void)arg;

	output("calling: missed %s call in conv: %s from \"%s:%s\" at %H\n",
	       video_call ? "video" : "audio", convid, userid, clientid,
	       fmt_gmtime, &when);
}


static void wcall_answered_handler(const char *convid, void *arg)
{
	(void)arg;

	output("call answered in conv %s\n", convid);
}


static void wcall_estab_handler(const char *convid,
				const char *userid,
				const char *clientid,
				void *arg)
{
	struct engine_user *ruser;
	const char *dname = userid;

	(void)arg;

	if (0 == engine_lookup_user(&ruser, zcall_engine, userid, false)) {
		dname = ruser->display_name;
	}

	output("call established with user \"%s\"\n", dname);

	wcall_set_video_send_state(calling3.wuser, convid,calling3.video_send_state);
}


static void wcall_close_handler(int reason,
				const char *convid,
				uint32_t msg_time,
				const char *userid,
				const char *clientid,
				void *arg)
{
	output("calling: call in convid=%s closed reason=\"%s\" time=%u\n",
	       convid, wcall_reason_name(reason), msg_time);
}


static void wcall_metrics_handler(const char *convid,
                                const char *metrics_json, void *arg)
{
	if(metrics_json){
		output("calling: metrics json : %s \n", metrics_json);
	}
}

static const char *audio_state_name(int astate)
{
	switch(astate) {
	case WCALL_AUDIO_STATE_CONNECTING:
		return "CONNECTING";
		
	case WCALL_AUDIO_STATE_ESTABLISHED:
		return "ESTABLISHED";
		
	case WCALL_AUDIO_STATE_NETWORK_PROBLEM:
		return "NETWORK_PROBLEM";

	default:
		return "???";
	}
}


static const char *video_state_name(int vstate)
{
	switch(vstate) {
	case WCALL_VIDEO_STATE_STOPPED:
		return "STOPPED";
	case WCALL_VIDEO_STATE_STARTED:
		return "STARTED";
	case WCALL_VIDEO_STATE_BAD_CONN:
		return "BAD_CONN";
	case WCALL_VIDEO_STATE_PAUSED:
		return "PAUSED";
	case WCALL_VIDEO_STATE_SCREENSHARE:
		return "SCREENSHARE";
	default:
		return "???";
	}
}

static void wcall_participant_changed_handler(const char *convid,
					      const char *mjson,
					      void *arg)
{
	output("Member list<json> changed for conv %s %s\n", convid, mjson);

	info("member_list: %s\n", mjson);
}


static void wcall_group_changed_handler(const char *convid, void *arg)
{
	struct wcall_members *members;
	size_t i;

	members = wcall_get_members(calling3.wuser, convid);
	if (!members) {
		return;
	}

	output("Member list changed for conv %s\n", convid);

	for (i = 0; i < members->membc; i++) {
		struct wcall_member *m = &(members->membv[i]);
		output("Member %s audio: %s video: %s\n",
		       m->userid,
		       audio_state_name(m->audio_state),
		       video_state_name(m->video_recv));
	}
	wcall_free_members(members);
}


static void wcall_media_stopped_handler(const char *convid, void *arg)
{
	output("Media stopped for conv %s\n", convid);
}

static const char *cbr_banner =
	"   ****    ******   ****** \n"
	"  **   **  **   **  **   **\n"
	"  **       **   **  **   **\n"
	"  **       ******   ****** \n"
	"  **       **   **  ** **  \n"
	"  **   **  **   **  **  ** \n"
	"   *****   ******   **   **\n";


static void wcall_audio_cbr_change_handler(const char *userid,
					   const char *clientid,
					   int active,
					   void *arg)
{
	output("calling: audio CBR is %sabled\n", active > 0 ? "en" : "dis");

	if (active) {

		output("\n"
		       "\x1b[32m" /* Green */
		       "%s"
		       "\x1b[;m"
		       "\n", cbr_banner);
	}
}

static void run_postponed(void)
{
	struct postpone_elem *pe;
	struct le *le;

	LIST_FOREACH(&calling3.postponel, le) {
		pe = le->data;

		switch(pe->ev) {
		case POSTPONE_EVENT_START: {
			struct engine_conv *conv = pe->arg;

			calling3_start(conv);
			break;
		}

		case POSTPONE_EVENT_ANSWER: {
			struct engine_conv *conv = pe->arg;

			calling3_answer(conv);
			break;
		}
			
		case POSTPONE_EVENT_RECV_MSG: {
			struct postpone_recv_msg *rmsg = pe->arg;
			
			calling3_recv_msg(rmsg->convid,
					  rmsg->fromuid,
					  rmsg->fromcid,
					  &rmsg->ts,
					  rmsg->data);
			mem_deref(rmsg);
			break;
		}
		default:
			warning("calling3: unknown postponed event: %d\n",
				pe->ev);
			break;
		}
				
	}

	list_flush(&calling3.postponel);
}

static void shutdown_handler(void *arg)
{
	calling3_close();
}


static void wcall_shutdown_handler(WUSER_HANDLE wuser, void *arg)
{
	calling3.ready = false;

	wcall_close();

	engine_call_shutdown(zcall_engine);
}

static void wcall_ready_handler(int version, void *arg)
{
	(void)arg;

	info("calling: ready with version=%d\n", version);
	calling3.ready = true;
	run_postponed();
	
	output("calling: wcall ready with version %d\n", version);

	engine_call_set_shutdown_handler(zcall_engine,
					 shutdown_handler,
					 NULL);
}

struct c3_req_ctx {
	WUSER_HANDLE wuser;
	void *arg;
	struct mbuf *mb_body;
	struct http_req *http_req;
};

static void ctx_destructor(void *arg)
{
	struct c3_req_ctx *ctx = arg;

	mem_deref(ctx->mb_body);
	mem_deref(ctx->http_req);
}
	
static void cfg_resp_handler(int err, const struct http_msg *msg,
			     struct mbuf *mb, struct json_object *jobj,
			     void *arg)
{
	struct c3_req_ctx *c3ctx = arg;
	char *json_str = NULL;

	if (err == ECONNABORTED)
		goto error;

#if ENABLE_CONFERENCE_CALLS
	if (!jzon_str(jobj, "sft_url")) {
		jzon_add_str(jobj, "sft_url", "%s", SFT_URL);
	}
#endif

	if (!err && jobj) {
		err = jzon_encode(&json_str, jobj);
		if (err)
			goto out;
	}
 out:
	wcall_config_update(c3ctx->wuser, err, json_str);

 error:
	mem_deref(json_str);
	mem_deref(c3ctx);
}
	

static int wcall_cfg_req_handler(WUSER_HANDLE wuser, void *arg)
{
	
	struct c3_req_ctx *ctx;

	(void) arg;
	ctx = mem_zalloc(sizeof(*ctx), ctx_destructor);
	if (!ctx)
		return ENOMEM;
	ctx->wuser = wuser;
	
	return rest_request(NULL, engine_get_restcli(zcall_engine), 0,
			    "GET", cfg_resp_handler, ctx,
			    "/calls/config/v2", NULL);

	return 0;
}


static int wcall_send_handler(void *ctx, const char *convid,
			      const char *userid_self,
			      const char *clientid_self,
			      const char *userid_dest,
			      const char *clientid_dest,
			      const uint8_t *data, size_t len,
			      int transient,
			      void *arg)
{
	struct engine_conv *conv;
	uint8_t *pbuf = NULL;
	size_t pbuf_len = 8192;
	char lclientid[64];
	int err;

	pbuf = mem_zalloc(pbuf_len, NULL);

#if defined (HAVE_PROTOBUF)
	err = protobuf_encode_calling(pbuf, &pbuf_len, (const char *)data);
	if (err) {
		warning("transp_send: protobuf_encode_calling failed (%m)\n",
			err);
		goto out;
	}
#else
	warning("transp_send: compiled without HAVE_PROTOBUF\n");
	err = ENOSYS;
	goto out;
#endif

	err = engine_lookup_conv(&conv, zcall_engine, convid);
	if (err) {
		warning("calling3: transp_send: conv lookup failed: %m\n",
			err);
		goto out;
	}

#if 0
	err = engine_lookup_user(&user, zcall_engine, userid, false);
	if (err) {
		warning("calling: transp_send: user lookup failed: %m\n",
			err);
		goto out;
	}
#endif

	err = client_id_load(lclientid, sizeof(lclientid));
	if (err) {
		warning("calling: could not load Clientid (%m)\n", err);
		goto out;
	}

#ifdef HAVE_CRYPTOBOX
	err = engine_send_otr_message(zcall_engine, g_cryptobox, conv,
		userid_dest, clientid_dest, lclientid, pbuf, pbuf_len,
		transient != 0, userid_dest, econn_otr_resp_handler, conv);
	if (err) {
		warning("calling: transp_send: otr_encrypt_and_send %zu bytes"
			" failed (%m)\n", pbuf_len, err);
		goto out;
	}
#else
	warning("transp_send: compiled without HAVE_CRYPTOBOX\n");
	err = ENOSYS;
	goto out;
#endif

 out:
	wcall_resp(calling3.wuser, 200, "", ctx);
	mem_deref(pbuf);

	return err;
}

static int sft_data_handler(const uint8_t *buf, size_t size,
			    const struct http_msg *msg, void *arg)
{
	struct c3_req_ctx *c3ctx = arg;
	bool chunked;
	int err = 0;

	chunked = http_msg_hdr_has_value(msg, HTTP_HDR_TRANSFER_ENCODING,
					 "chunked");
	if (!c3ctx->mb_body) {
		c3ctx->mb_body = mbuf_alloc(1024);
		if (!c3ctx->mb_body) {
			err = ENOMEM;
			goto out;
		}
	}

	/* append data to the body-buffer */
	err = mbuf_write_mem(c3ctx->mb_body, buf, size);
	if (err)
		return err;


 out:
	return err;
}


static void sft_resp_handler(int err, const struct http_msg *msg,
			     void *arg)
{
	struct c3_req_ctx *c3ctx = arg;
	char *json_str = NULL;
	const uint8_t *buf = NULL;
	int sz = 0;

	info("sft_resp: done err %d, %d bytes to send\n",
		err, c3ctx->mb_body ? mbuf_get_left(c3ctx->mb_body) : 0);
	if (err == ECONNABORTED)
		goto error;

	if (c3ctx->mb_body) {
		mbuf_write_u8(c3ctx->mb_body, 0);
		c3ctx->mb_body->pos = 0;

		buf = mbuf_buf(c3ctx->mb_body);
		sz = mbuf_get_left(c3ctx->mb_body);
	}

	wcall_sft_resp(c3ctx->wuser, err,
		       buf, sz,
		       c3ctx->arg);
 error:
	mem_deref(json_str);
	mem_deref(c3ctx);
}


static int wcall_sft_handler(void* ctx, const char *url,
			     const uint8_t *data, size_t len,
			     void *arg)
{
	
	struct c3_req_ctx *c3ctx;
	int err = 0;

	info("wcall_sft_handler: url: %s\n", url);
	c3ctx = mem_zalloc(sizeof(*c3ctx), ctx_destructor);
	if (!c3ctx)
		return ENOMEM;
	c3ctx->arg = ctx;
	c3ctx->wuser = calling3.wuser;

	if (!calling3.http_cli) {
		err = http_client_alloc(&calling3.http_cli, engine_dnsc(zcall_engine));
		if (err) {
			warning("HTTP client init failed: %m.\n", err);
			goto out;
		}
	}

	err = http_request(&c3ctx->http_req, calling3.http_cli,
			   "POST", url, sft_resp_handler, sft_data_handler, c3ctx, 
			   "Accept: application/json\r\n"
			   "Content-Type: application/json\r\n"
			   "Content-Length: %zu\r\n"
			   "User-Agent: zcall\r\n"
			   "\r\n"
			   "%b",
			   len, data, len);

out:
	if (err) {
		mem_deref(c3ctx);
	}

	return err;
}


static int postpone_event(enum postpone_event ev, void *arg)
{
	struct postpone_elem *pe;

	pe = mem_zalloc(sizeof(*pe), NULL);
	if (!pe)
		return ENOMEM;

	pe->ev = ev;
	pe->arg = arg;
	list_append(&calling3.postponel, &pe->le, pe);

	return 0;
}


void calling3_recv_msg(const char *convid,
		       const char *from_userid,
		       const char *from_clientid,
		       const struct ztime *timestamp,
		       const char *data)
{
	int err;
	struct ztime now = {0, 0};
	
	if (!calling3.initialized)
		calling3_init();

	if (!calling3.ready) {
		struct postpone_recv_msg *rmsg;

		rmsg = mem_zalloc(sizeof(*rmsg), rmsg_destructor);
		if (!rmsg) {
			warning("calling3: recv_msg: cannot allocate rmsg\n");
			return;
		}
		str_dup(&rmsg->convid, convid);
		str_dup(&rmsg->fromuid, from_userid);
		str_dup(&rmsg->fromcid, from_clientid);
		str_dup(&rmsg->data, data);
		rmsg->ts = *timestamp;
		
		postpone_event(POSTPONE_EVENT_RECV_MSG, rmsg);
		return;
	}

	err = ztime_get(&now);
	if (err) {
		warning("could not get current time (%m)\n", err);
	}

	err = wcall_recv_msg(calling3.wuser, (uint8_t *)data, strlen(data),
		       now.sec, timestamp->sec,
		       convid, from_userid, from_clientid);

	if (err == WCALL_ERROR_UNKNOWN_PROTOCOL) {
		warning("calling3: recv_msg: unknown protocol, please update your client!\n");
	}
	else if (err) {
		warning("calling3: recv_msg: error returned %d\n", err);
	}
}

struct client_ctx {
	struct json_object *jclients;
	char *convid;
};

static void client_ctx_destructor(void *arg)
{
	struct client_ctx *ctx = arg;

	ctx->convid = mem_deref(ctx->convid);
	ctx->jclients = mem_deref(ctx->jclients);
}
	

static void get_clients_missing_handler(const char *userid,
					const char *clientid,
					void *arg)
{
	struct client_ctx *ctx = arg;
	struct json_object *jcli;

	jcli = jzon_alloc_object();
	jzon_add_str(jcli, "userid", "%s", userid);
	jzon_add_str(jcli, "clientid", "%s", clientid);

	json_object_array_add(ctx->jclients, jcli);
}


static void get_clients_response_handler(int err, void *arg)
{
	struct client_ctx *ctx = arg;
	struct json_object *jobj;
	char *json;

	jobj = jzon_alloc_object();
	jzon_add_str(jobj, "convid", "%s", ctx->convid);
	json_object_object_add(jobj, "clients", ctx->jclients);
	ctx->jclients = NULL;

	jzon_encode(&json, jobj);

	if (json) {
		wcall_set_clients_for_conv(calling3.wuser,
					   ctx->convid,
					   json);
	}

	mem_deref(ctx);
	mem_deref(jobj);
	mem_deref(json);
}

static void set_clients_for_conv(struct engine_conv *conv)
{
	struct client_ctx *ctx;

	ctx = mem_zalloc(sizeof(struct client_ctx), client_ctx_destructor);
	if (!ctx) {
		return;
	}

	str_dup(&ctx->convid, conv->id);
	ctx->jclients = json_object_new_array();
	if (!ctx->jclients)
		return;

	struct list msgl = LIST_INIT;
	char lclientid[64];
	int err = 0;

	err = client_id_load(lclientid, sizeof(lclientid));
	if (err) {
		warning("calling: could not load Clientid (%m)\n", err);
		goto out;
	}
	err = engine_send_message(conv,
			      lclientid,
			      &msgl,
			      false, // transient,
			      false, // ignore_missing,
			      get_clients_response_handler,
			      get_clients_missing_handler,
			      ctx);

out:
	if (err) {
		mem_deref(ctx);
	}
}


static void wcall_req_clients_handler(const char *convid, void *arg)
{
	struct engine_conv *conv = NULL;
	int err;

	(void)arg;

	err = engine_lookup_conv(&conv, zcall_engine, convid);
	if (err) {
		warning("calling: cannot find conversation: %s: %m\n",
			convid, err);
	}

	set_clients_for_conv(conv);
}

int calling3_start(struct engine_conv *conv)
{
	struct engine_conv_member *mbr;
	int err = 0;
	int call_type, conv_type;
	int ret;

	if (!conv) {
		warning("calling3: start: no conversation!\n");
		return EINVAL;
	}
	
	if (!calling3.initialized) {
		info("calling3: not initialized\n");
		err = calling3_init();
		if (err)
			return err;
	}

	if (!calling3.ready) {
		postpone_event(POSTPONE_EVENT_START, conv);
		return 0;
	}

	call_type = zcall_force_audio ? WCALL_CALL_TYPE_FORCED_AUDIO :
		calling3.video_send_state != WCALL_VIDEO_STATE_STOPPED ? WCALL_CALL_TYPE_VIDEO :
		WCALL_CALL_TYPE_NORMAL;

	switch (conv->type) {
	case ENGINE_CONV_REGULAR:
		conv_type = g_use_conference ? WCALL_CONV_TYPE_CONFERENCE : WCALL_CONV_TYPE_GROUP;;
		break;
	case ENGINE_CONV_ONE:
		conv_type = WCALL_CONV_TYPE_ONEONONE;
		break;
	default:
		output("only regular conversations supported.\n");
		return EINVAL;
	}

	mbr = list_ledata(conv->memberl.head);
	if (!mbr) {
		output("No user members in this conversation\n");
		return EINVAL;
	}

	output("starting call in %sconversation \"%H\"\n",
	       conv_type == WCALL_CONV_TYPE_GROUP ? "group-" : "", engine_print_conv_name, conv);

	if (avs_get_flags() & AVS_FLAG_AUDIO_TEST)
		audio_io_enable_sine();
			
	ret = wcall_start(calling3.wuser, conv->id,
			  call_type, conv_type,
			  zcall_audio_cbr);
	if (ret < 0) {
		warning("start: wcall_start failed\n");
		goto out;
	}

 out:
	return err;
}


/* Answer the first PENDING and INCOMING call */
void calling3_answer(struct engine_conv *conv)
{
	int ret;
	bool isgrp;
	int call_type;

	if (!calling3.initialized)
		calling3_init();

	if (!calling3.ready) {
		postpone_event(POSTPONE_EVENT_ANSWER, conv);
		return;
	}
	
	isgrp = conv->type == ENGINE_CONV_REGULAR;
	if (!isgrp && conv->type != ENGINE_CONV_ONE) {
		output("only regular conversations supported.\n");
		return;
	}

	call_type = zcall_force_audio ? WCALL_CALL_TYPE_FORCED_AUDIO :
		calling3.video_send_state != WCALL_VIDEO_STATE_STOPPED ? WCALL_CALL_TYPE_VIDEO :
		WCALL_CALL_TYPE_NORMAL;
	if (avs_get_flags() & AVS_FLAG_AUDIO_TEST)
		audio_io_enable_sine();
	ret = wcall_answer(calling3.wuser, conv->id,
			   call_type, zcall_audio_cbr);
	if (ret < 0)
		output("Answering call failed\n");
	else
		output("Answering incoming call\n");
}


/* Reject the first PENDING and INCOMING call */
void calling3_reject(struct engine_conv *conv)
{
	int ret;
	bool isgrp;

	isgrp = conv->type == ENGINE_CONV_REGULAR;
	if (!isgrp && conv->type != ENGINE_CONV_ONE) {
		output("only regular conversations supported.\n");
		return;
	}
		
	ret = wcall_reject(calling3.wuser, conv->id);
	if (ret < 0)
		output("Rejecting call failed\n");
	else
		output("Rejecting incoming call\n");
}

void calling3_end(struct engine_conv *conv)
{
	bool isgrp;

	isgrp = conv->type == ENGINE_CONV_REGULAR;
	if (!isgrp && conv->type != ENGINE_CONV_ONE) {
		output("only regular conversations supported.\n");
		return;
	}
	
	output("ending call in conversation `%H'\n",
	       engine_print_conv_name, conv);
	
	if (conv->id)
		wcall_end(calling3.wuser, conv->id);
	else
		output("no call to end.\n");
}


static void register_dummy_sounds(void)
{
	static const char * const soundv[] = {
		"ringing_from_me",
		"ready_to_talk",
		"talk_later",
	};
	size_t i;

	for (i=0; i<ARRAY_SIZE(soundv); i++) {
		mediamgr_register_media(wcall_mediamgr(calling3.wuser),
					soundv[i],
					0,
					false,
					false,
					0,
					0,
					true);
	}
}

static const char *quality2name(int quality)
{
	switch (quality) {
	case WCALL_QUALITY_NORMAL:
		return "NORMAL";

	case WCALL_QUALITY_MEDIUM:
		return "MEDIUM";

	case WCALL_QUALITY_POOR:
		return "POOR";

	default:
		return "???";
	}
}

static void wcall_quality_handler(const char *convid,
				  const char *userid,
				  const char *clientid,
				  int quality, /*  WCALL_QUALITY_ */
				  int rtt, /* round trip time in ms */
				  int uploss, /* upstream pkt loss % */
				  int downloss, /* dnstream pkt loss */
				  void *arg)
{
	(void)arg;

	info("call_quality: convid=%s userid=%s quality=%s "
	     "rtt=%d up=%d dn=%d\n",
	     convid, userid, quality2name(quality),
	     rtt, uploss, downloss);
}



int calling3_init(void)
{
	struct engine_user *self;
	char clientid[64];
	int err	= 0;

	info("calling3_init: initialized=%d\n", calling3.initialized);
	
	if (calling3.initialized)
		return EALREADY;

	

	err = client_id_load(clientid, sizeof(clientid));
	if (err) {
		warning("calling: could not load Clientid (%m)\n", err);
		return err;
	}

	self = engine_get_self(zcall_engine);
	if (!self) {
		warning("Self not loaded yet.\n");
		return EINVAL;
	}

	err = wcall_init(WCALL_ENV_DEFAULT);
	if (err) {
		warning("calling3: wcall_init failed (%m)\n", err);
		goto out;
	}

	calling3.wuser = wcall_create_ex(self->id,
					 clientid,
					 1,
					 "voe",
					 wcall_ready_handler,
					 wcall_send_handler,
					 wcall_sft_handler,
					 wcall_incoming_handler,
					 wcall_missed_handler,
					 wcall_answered_handler,
					 wcall_estab_handler,
					 wcall_close_handler,
					 wcall_metrics_handler,
					 wcall_cfg_req_handler,
					 wcall_audio_cbr_change_handler,
					 wcall_vidstate_handler,
					 NULL);
	if (calling3.wuser == WUSER_INVALID_HANDLE) {
		warning("calling3: wcall_create failed (%m)\n", err);
		goto out;
	}

	wcall_set_shutdown_handler(calling3.wuser,
				   wcall_shutdown_handler,
				   NULL);
	wcall_set_trace(calling3.wuser, g_trace);
	wcall_set_participant_changed_handler(calling3.wuser,
					wcall_participant_changed_handler,
					NULL);
	wcall_set_group_changed_handler(calling3.wuser,
					wcall_group_changed_handler,
					NULL);
	wcall_set_media_stopped_handler(calling3.wuser,
					wcall_media_stopped_handler);

	wcall_set_req_clients_handler(calling3.wuser,
				   wcall_req_clients_handler);
	if (g_ice_privacy)
		wcall_enable_privacy(calling3.wuser, 1);

	/* NOTE: must be done after wcall_create */
	if (!g_use_kase)
		msystem_enable_kase(flowmgr_msystem(), false);

	wcall_set_network_quality_handler(calling3.wuser,
					  wcall_quality_handler,
					  5,
					  NULL);

	view_set_local_user(self->id, clientid);
	register_dummy_sounds();

	calling3.initialized = true;

 out:
	return err;
}

WUSER_HANDLE calling3_get_wuser(void)
{
	return calling3.wuser;
}

void calling3_close(void)
{
	if (!calling3.initialized)
		return;

	if (calling3.wuser) {
		wcall_destroy(calling3.wuser);
		calling3.wuser = WUSER_INVALID_HANDLE;
	}

	calling3.http_cli = mem_deref(calling3.http_cli);
}


void calling3_set_video_send_state(struct engine_conv *conv, int state)
{
	calling3.video_send_state = state;

	if (conv && conv->id) {
		wcall_set_video_send_state(calling3.wuser, conv->id, state);
	}
}


void calling3_propsync(struct engine_conv *conv)
{
	// TODO: readd this
	/*
	if (conv && conv->id) {
		wcall_propsync_request(calling3.wuser, conv->id);
	}*/
}


void calling3_dump(void)
{
	re_fprintf(stderr, "********** CALLING3 DUMP **********\n");
	re_fprintf(stderr, "%H\n", wcall_debug, calling3.wuser);
	re_fprintf(stderr, "***********************************\n");

	output("********** CALLING3 DUMP **********\n");
	output("%H\n", wcall_debug, calling3.wuser);
	output("***********************************\n");
}


void calling3_stats(void)
{
	re_fprintf(stderr, "********** CALLING3 STATS **********\n");
	re_fprintf(stderr, "%H\n", wcall_stats, calling3.wuser);
	re_fprintf(stderr, "***********************************\n");

	output("%H\n", wcall_stats, calling3.wuser);
}

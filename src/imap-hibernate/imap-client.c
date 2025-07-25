/* Copyright (c) 2014-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "ioloop.h"
#include "fdpass.h"
#include "hostpid.h"
#include "connection.h"
#include "iostream.h"
#include "istream.h"
#include "ostream.h"
#include "ostream-multiplex.h"
#include "llist.h"
#include "priorityq.h"
#include "base64.h"
#include "str.h"
#include "strescape.h"
#include "time-util.h"
#include "var-expand.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "imap-keepalive.h"
#include "imap-master-connection.h"
#include "imap-client.h"

#include <unistd.h>

#define IMAP_MASTER_SOCKET_NAME "imap-master"

/* we only need enough for "DONE\r\n<tag> IDLE\r\n" */
#define IMAP_MAX_INBUF 12 + 1 + 128 /* DONE\r\nIDLE\r\n + ' ' + <tag> */
#define IMAP_MAX_OUTBUF 1024

/* If client has sent input and we can't recreate imap process in this
   many seconds, disconnect the client. */
#define IMAP_CLIENT_MOVE_BACK_WITH_INPUT_TIMEOUT_SECS 10
/* If there's a change notification and we can't recreate imap process in this
   many seconds, disconnect the client. */
#define IMAP_CLIENT_MOVE_BACK_WITHOUT_INPUT_TIMEOUT_SECS (60*5)

/* How often to try to unhibernate clients. */
#define IMAP_UNHIBERNATE_RETRY_MSECS 100

#define IMAP_CLIENT_BUFFER_FULL_ERROR "Client output buffer is full"
#define IMAP_CLIENT_UNHIBERNATE_ERROR "Failed to unhibernate client"

enum imap_client_input_state {
	IMAP_CLIENT_INPUT_STATE_UNKNOWN,
	IMAP_CLIENT_INPUT_STATE_BAD,
	IMAP_CLIENT_INPUT_STATE_DONE_LF,
	IMAP_CLIENT_INPUT_STATE_DONE_CRLF,
	IMAP_CLIENT_INPUT_STATE_DONEIDLE
};

struct imap_client_notify {
	int fd;
	struct io *io;
};

struct imap_client {
	struct priorityq_item item;

	struct imap_client *prev, *next;
	pool_t pool;
	struct event *event;
	struct imap_client_state state;
	ARRAY(struct imap_client_notify) notifys;

	time_t move_back_start;
	struct timeout *to_move_back;

	int fd;
	struct io *io;
	struct istream *input;
	struct ostream *output;
	struct timeout *to_keepalive;
	struct imap_master_connection *master_conn;
	struct ioloop_context *ioloop_ctx;
	const char *log_prefix;
	unsigned int next_read_threshold;
	bool bad_done, idle_done;
	bool unhibernate_queued;
	bool input_pending;
	bool shutdown_fd_on_destroy;
};

static struct imap_client *imap_clients;
static struct priorityq *unhibernate_queue;
static struct timeout *to_unhibernate;
static const char imap_still_here_text[] = "* OK Still here\r\n";

static struct event_category event_category_imap = {
	.name = "imap",
};
static struct event_category event_category_imap_hibernate = {
	.name = "imap-hibernate",
	.parent = &event_category_imap,
};

static void imap_client_stop(struct imap_client *client);
void imap_client_destroy(struct imap_client **_client, const char *reason);
static void imap_client_add_idle_keepalive_timeout(struct imap_client *client);
static void imap_clients_unhibernate(void *context);
static void imap_client_stop_notify_listening(struct imap_client *client);

static void imap_client_disconnected(struct imap_client **_client)
{
	struct imap_client *client = *_client;
	const char *reason;

	reason = io_stream_get_disconnect_reason(client->input, client->output);
	imap_client_destroy(_client, reason);
}

static void
imap_client_unhibernate_failed(struct imap_client **_client, const char *error)
{
	struct imap_client *client = *_client;
	struct timeval created;
	event_get_create_time(client->event, &created);

	struct event_passthrough *e =
		event_create_passthrough(client->event)->
		set_name("imap_client_unhibernated")->
		add_int("hibernation_usecs",
			timeval_diff_usecs(&ioloop_timeval, &created))->
		add_str("error", error);
	e_error(e->event(), IMAP_CLIENT_UNHIBERNATE_ERROR": %s", error);
	imap_client_destroy(_client, IMAP_CLIENT_UNHIBERNATE_ERROR);
}

static void
imap_client_parse_userdb_fields(struct imap_client *client,
				const char **auth_user_r)
{
	const char *const *field;
	unsigned int i;

	*auth_user_r = NULL;

	if (client->state.userdb_fields == NULL)
		return;

	field = t_strsplit_tabescaped(client->state.userdb_fields);
	for (i = 0; field[i] != NULL; i++) {
		if (str_begins(field[i], "auth_user=", auth_user_r))
			break;
	}
}

static void
imap_client_move_back_send_callback(void *context, struct ostream *output)
{
	struct imap_client *client = context;
	const struct imap_client_state *state = &client->state;
	string_t *str = t_str_new(256);
	struct timeval created;
	const unsigned char *input_data;
	size_t input_size;
	ssize_t ret;

	str_append_tabescaped(str, state->username);
	event_get_create_time(client->event, &created);
	str_printfa(str, "\thibernation_started=%"PRIdTIME_T".%06u",
		    created.tv_sec, (unsigned int)created.tv_usec);

	if (state->session_id != NULL) {
		str_append(str, "\tsession=");
		str_append_tabescaped(str, state->session_id);
	}
	if (state->session_created != 0) {
		str_printfa(str, "\tsession_created=%s",
			    dec2str(state->session_created));
	}
	if (state->tag != NULL)
		str_printfa(str, "\ttag=%s", client->state.tag);
	if (state->local_ip.family != 0)
		str_printfa(str, "\tlip=%s", net_ip2addr(&state->local_ip));
	if (state->local_port != 0)
		str_printfa(str, "\tlport=%u", state->local_port);
	if (state->remote_ip.family != 0)
		str_printfa(str, "\trip=%s", net_ip2addr(&state->remote_ip));
	if (state->remote_port != 0)
		str_printfa(str, "\trport=%u", state->remote_port);
	if (state->multiplex_ostream)
		str_append(str, "\tmultiplex_ostream");
	if (state->userdb_fields != NULL) {
		str_append(str, "\tuserdb_fields=");
		str_append_tabescaped(str, state->userdb_fields);
	}
	if (major(state->peer_dev) != 0 || minor(state->peer_dev) != 0) {
		str_printfa(str, "\tpeer_dev_major=%lu\tpeer_dev_minor=%lu",
			    (unsigned long)major(state->peer_dev),
			    (unsigned long)minor(state->peer_dev));
	}
	if (state->peer_ino != 0)
		str_printfa(str, "\tpeer_ino=%llu", (unsigned long long)state->peer_ino);
	if (state->state_size > 0) {
		str_append(str, "\tstate=");
		base64_encode(state->state, state->state_size, str);
	}
	input_data = i_stream_get_data(client->input, &input_size);
	if (input_size > 0) {
		str_append(str, "\tclient_input=");
		base64_encode(input_data, input_size, str);
	}
	i_assert(o_stream_get_buffer_used_size(client->output) == 0);
	if (client->idle_done) {
		if (client->bad_done)
			str_append(str, "\tbad-done");
	} else if (client->state.idle_cmd) {
		/* IDLE continues after sending changes */
		str_append(str, "\tidle-continue");
	}
	/* For imap_logout_format statistics: */
	str_printfa(str,
		   "\tfetch_hdr_count=%u\tfetch_hdr_bytes=%"PRIu64
		   "\tfetch_body_count=%u\tfetch_body_bytes=%"PRIu64
		   "\tdeleted_count=%u\texpunged_count=%u\ttrashed_count=%u"
		   "\tautoexpunged_count=%u\tappend_count=%u"
		   "\tinput_bytes_extra=%"PRIuUOFF_T
		   "\toutput_bytes_extra=%"PRIuUOFF_T,
		   client->state.logout_stats.fetch_hdr_count,
		   client->state.logout_stats.fetch_hdr_bytes,
		   client->state.logout_stats.fetch_body_count,
		   client->state.logout_stats.fetch_body_bytes,
		   client->state.logout_stats.deleted_count,
		   client->state.logout_stats.expunged_count,
		   client->state.logout_stats.trashed_count,
		   client->state.logout_stats.autoexpunged_count,
		   client->state.logout_stats.append_count,
		   i_stream_get_absolute_offset(client->input) + client->state.logout_stats.input_bytes_extra,
		   client->output->offset + client->state.logout_stats.output_bytes_extra);
	str_append_c(str, '\n');

	/* send the fd first */
	ret = fd_send(o_stream_get_fd(output), client->fd, str_data(str), 1);
	if (ret < 0) {
		const char *error = t_strdup_printf(
			"fd_send(%s) failed: %m", o_stream_get_name(output));
		imap_client_unhibernate_failed(&client, error);
		return;
	}
	/* If unhibernation fails after this, shutdown() the fd to make sure
	   the imap process won't later on finish unhibernation after all and
	   cause confusion. */
	client->shutdown_fd_on_destroy = TRUE;
	i_assert(ret > 0);
	o_stream_nsend(output, str_data(str) + 1, str_len(str) - 1);
}

static void
imap_client_move_back_read_callback(void *context, const char *line)
{
	struct imap_client *client = context;

	if (line[0] != '+') {
		/* failed - FIXME: retry later? */
		imap_client_unhibernate_failed(&client, line+1);
	} else {
		client->shutdown_fd_on_destroy = FALSE;
		imap_client_destroy(&client, NULL);
	}
}

static bool imap_move_has_reached_timeout(struct imap_client *client)
{
	int max_secs = client->input_pending ?
		IMAP_CLIENT_MOVE_BACK_WITH_INPUT_TIMEOUT_SECS :
		IMAP_CLIENT_MOVE_BACK_WITHOUT_INPUT_TIMEOUT_SECS;
	return client->move_back_start != 0 &&
		ioloop_time - client->move_back_start > max_secs;
}

static bool imap_client_try_move_back(struct imap_client *client)
{
	const struct master_service_settings *master_set;
	const char *path, *error;
	int ret;

	if (o_stream_get_buffer_used_size(client->output) > 0) {
		/* there is data buffered, so we have to disconnect you */
		imap_client_destroy(&client, IMAP_CLIENT_BUFFER_FULL_ERROR);
		return TRUE;
	}

	master_set = master_service_get_service_settings(master_service);
	path = t_strconcat(master_set->base_dir,
			   "/"IMAP_MASTER_SOCKET_NAME, NULL);
	ret = imap_master_connection_init(path,
					  imap_client_move_back_send_callback,
					  imap_client_move_back_read_callback,
					  client, &client->master_conn, &error);
	if (ret > 0) {
		/* success */
		imap_client_stop(client);
		return TRUE;
	} else if (ret < 0 || imap_move_has_reached_timeout(client)) {
		/* failed to connect to the imap-master socket */
		imap_client_unhibernate_failed(&client, error);
		return TRUE;
	}

	e_debug(event_create_passthrough(client->event)->
		set_name("imap_client_unhibernate_retried")->
		add_str("error", error)->event(),
		"Unhibernation failed: %s - retrying", error);
	/* Stop listening for client's IOs while waiting for the next
	   reconnection attempt. However if we got here because of an external
	   notification keep waiting to see if client sends any IO, since that
	   will cause the unhibernation to be aborted earlier. */
	if (client->input_pending)
		io_remove(&client->io);
	imap_client_stop_notify_listening(client);
	return FALSE;
}

static void imap_client_move_back(struct imap_client *client)
{
	if (imap_client_try_move_back(client))
		return;

	/* imap-master socket is busy. retry in a while. */
	if (client->move_back_start == 0)
		client->move_back_start = ioloop_time;
	if (!client->unhibernate_queued) {
		client->unhibernate_queued = TRUE;
		priorityq_add(unhibernate_queue, &client->item);
	}
	if (to_unhibernate == NULL) {
		to_unhibernate = timeout_add_short(IMAP_UNHIBERNATE_RETRY_MSECS,
						   imap_clients_unhibernate, NULL);
	}
}

static enum imap_client_input_state
imap_client_input_parse(const unsigned char *data, size_t size, const char **tag_r)
{
	const unsigned char *tag_start, *tag_end;

	enum imap_client_input_state state = IMAP_CLIENT_INPUT_STATE_DONE_LF;

	/* skip over DONE[\r]\n */
	if (i_memcasecmp(data, "DONE", I_MIN(size, 4)) != 0)
		return IMAP_CLIENT_INPUT_STATE_BAD;
	if (size <= 4)
		return IMAP_CLIENT_INPUT_STATE_UNKNOWN;
	data += 4; size -= 4;

	if (data[0] == '\r') {
		state = IMAP_CLIENT_INPUT_STATE_DONE_CRLF;
		data++; size--;
	}
	if (size == 0)
		return IMAP_CLIENT_INPUT_STATE_UNKNOWN;
	if (data[0] != '\n')
		return IMAP_CLIENT_INPUT_STATE_BAD;
	data++; size--;
	if (size == 0)
		return state;

	tag_start = data;

	/* skip over tag */
	while(data[0] != ' ' &&
	      data[0] != '\r' &&
	      data[0] != '\t' ) { data++; size--; }

	tag_end = data;

	if (size == 0)
		return state;
	if (data[0] != ' ')
		return IMAP_CLIENT_INPUT_STATE_BAD;
	data++; size--;

	/* skip over IDLE[\r]\n - checking this assumes that the DONE and IDLE
	   are sent in the same IP packet, otherwise we'll unnecessarily
	   recreate the imap process and immediately resume IDLE there. if this
	   becomes an issue we could add a small delay to the imap process
	   creation and wait for the IDLE command during it. */
	if (size <= 4 || i_memcasecmp(data, "IDLE", 4) != 0)
		return state;
	data += 4; size -= 4;

	if (data[0] == '\r') {
		data++; size--;
	}
	if (size == 1 && data[0] == '\n') {
		*tag_r = t_strdup_until(tag_start, tag_end);
		return IMAP_CLIENT_INPUT_STATE_DONEIDLE;
	}
	return state;
}

static void imap_client_input_idle_cmd(struct imap_client *client)
{
	char *old_tag;
	const char *new_tag;
	const char *output;
	const unsigned char *data;
	size_t size;
	bool done = TRUE;
	int ret;

	/* we should read either DONE or disconnection. also handle if client
	   sends DONE\nIDLE simply to recreate the IDLE. */
	ret = i_stream_read_bytes(client->input, &data, &size,
				  client->next_read_threshold + 1);
	if (size == 0) {
		if (ret < 0)
			imap_client_disconnected(&client);
		return;
	}
	client->next_read_threshold = 0;
	switch (imap_client_input_parse(data, size, &new_tag)) {
	case IMAP_CLIENT_INPUT_STATE_UNKNOWN:
		/* we haven't received a full DONE[\r]\n yet - wait */
		client->next_read_threshold = size;
		return;
	case IMAP_CLIENT_INPUT_STATE_BAD:
		/* invalid input - return this to the imap process */
		client->bad_done = TRUE;
		break;
	case IMAP_CLIENT_INPUT_STATE_DONE_LF:
		i_stream_skip(client->input, 4+1);
		break;
	case IMAP_CLIENT_INPUT_STATE_DONE_CRLF:
		i_stream_skip(client->input, 4+2);
		break;
	case IMAP_CLIENT_INPUT_STATE_DONEIDLE:
		/* we received DONE+IDLE, so the client simply wanted to notify
		   us that it's still there. continue hibernation. */
		old_tag = client->state.tag;
		client->state.tag = i_strdup(new_tag);
		output = t_strdup_printf("%s OK Idle completed.\r\n+ idling\r\n", old_tag);
		i_free(old_tag);
		ret = o_stream_flush(client->output);
		if (ret > 0)
			ret = o_stream_send_str(client->output, output);
		if (ret < 0) {
			imap_client_disconnected(&client);
			return;
		}
		if ((size_t)ret != strlen(output)) {
			/* disconnect */
			imap_client_destroy(&client, IMAP_CLIENT_BUFFER_FULL_ERROR);
			return;
		} else {
			done = FALSE;
			i_stream_skip(client->input, size);
		}
		break;
	}

	if (done) {
		client->idle_done = TRUE;
		client->input_pending = TRUE;
		imap_client_move_back(client);
	} else
		imap_client_add_idle_keepalive_timeout(client);
}

static void imap_client_input_nonidle(struct imap_client *client)
{
	if (i_stream_read(client->input) < 0)
		imap_client_disconnected(&client);
	else {
		client->input_pending = TRUE;
		imap_client_move_back(client);
	}
}

static void imap_client_input_notify(struct imap_client *client)
{
	imap_client_move_back(client);
}

static void keepalive_timeout(struct imap_client *client)
{
	ssize_t ret;

	/* do not send this if there is data buffered */
	if ((ret = o_stream_flush(client->output)) < 0) {
		imap_client_disconnected(&client);
		return;
	} else if (ret == 0)
		return;

	ret = o_stream_send_str(client->output, imap_still_here_text);
	if (ret < 0) {
		imap_client_disconnected(&client);
		return;
	}
	/* ostream buffer size is definitely large enough for this text */
	i_assert((size_t)ret == strlen(imap_still_here_text));
	imap_client_add_idle_keepalive_timeout(client);
}

static void imap_client_add_idle_keepalive_timeout(struct imap_client *client)
{
	unsigned int interval = client->state.imap_idle_notify_interval;

	if (interval == 0)
		return;

	interval = imap_keepalive_interval_msecs(client->state.username,
						 &client->state.remote_ip,
						 interval);

	timeout_remove(&client->to_keepalive);
	client->to_keepalive = timeout_add(interval, keepalive_timeout, client);
}

static const struct var_expand_table *
imap_client_get_var_expand_table(struct imap_client *client)
{
	const char *local_ip = client->state.local_ip.family == 0 ? NULL :
		net_ip2addr(&client->state.local_ip);
	const char *remote_ip = client->state.remote_ip.family == 0 ? NULL :
		net_ip2addr(&client->state.remote_ip);

	const char *auth_user;
	imap_client_parse_userdb_fields(client, &auth_user);
	if (auth_user == NULL)
		auth_user = client->state.username;


	const char *local_port = "";
	const char *remote_port = "";

	if (client->state.local_port != 0)
		local_port = dec2str(client->state.local_port);
	if (client->state.remote_port != 0)
		remote_port = dec2str(client->state.remote_port);

	const struct var_expand_table stack_tab[] = {
		{ .key = "user", .value = client->state.username },
		{ .key = "service", .value = "imap-hibernate" },
		{ .key = "home", .value = NULL /* we shouldn't need this */ },
		{ .key = "local_ip", .value = local_ip },
		{ .key = "remote_ip", .value = remote_ip },
		{ .key = "local_port", .value = local_port },
		{ .key = "remote_port", .value = remote_port },
		{ .key = "uid", .value = dec2str(client->state.uid) },
		{ .key = "gid", .value = dec2str(client->state.gid) },
		{ .key = "session", .value = client->state.session_id },
		{ .key = "auth_user", .value = auth_user },

		/* NOTE: keep this synced with lib-storage's
		   mail_user_var_expand_params() */
		VAR_EXPAND_TABLE_END
	};
	struct var_expand_table *tab;

	tab = t_malloc_no0(sizeof(stack_tab));
	memcpy(tab, stack_tab, sizeof(stack_tab));
	return tab;
}

static int
imap_client_var_expand_func_userdb(const char *data, const char **value_r,
				   void *context, const char **error_r ATTR_UNUSED)
{
	const char *const *fields = context;
	const char *field_name = t_strdup_printf("%s=",t_strcut(data, ':'));
	const char *default_value = i_strchr_to_next(data, ':');
	const char *value = NULL;

	for(;*fields != NULL; fields++) {
		if (str_begins(*fields, field_name, &value))
			break;
	}

	*value_r = value != NULL ? value : default_value;

	return 0;
}

static void imap_client_io_activate_user(struct imap_client *client)
{
	i_set_failure_prefix("%s", client->log_prefix);
}

static void imap_client_io_deactivate_user(struct imap_client *client ATTR_UNUSED)
{
	i_set_failure_prefix("imap-hibernate: ");
}

static const char *const *
userdb_fields_get_alt_usernames(char *const *userdb_fields)
{
	ARRAY_TYPE(const_string) alt_usernames;
	t_array_init(&alt_usernames, 4);
	for (unsigned int i = 0; userdb_fields[i] != NULL; i++) {
		const char *key, *value;
		if (t_split_key_value_eq(userdb_fields[i], &key, &value) &&
		    *value != '\0' && str_begins_with(key, "user_")) {
			array_push_back(&alt_usernames, &key);
			array_push_back(&alt_usernames, &value);
		}
	}
	if (array_count(&alt_usernames) == 0)
		return NULL;

	array_append_zero(&alt_usernames);
	return array_front(&alt_usernames);
}

struct imap_client *
imap_client_create(int fd, const struct imap_client_state *state)
{
	const struct var_expand_provider funcs[] = {
		{ "userdb", imap_client_var_expand_func_userdb },
		{ NULL, NULL }
	};
	struct imap_client *client;
	pool_t pool = pool_alloconly_create("imap client", 256);
	void *statebuf;
	const char *error;

	i_assert(state->username != NULL);
	i_assert(state->mail_log_prefix != NULL);

	fd_set_nonblock(fd, TRUE); /* it should already be, but be sure */

	client = p_new(pool, struct imap_client, 1);
	client->pool = pool;
	client->fd = fd;
	client->input = i_stream_create_fd(fd, IMAP_MAX_INBUF);
	client->output = o_stream_create_fd(fd, IMAP_MAX_OUTBUF);
	o_stream_set_no_error_handling(client->output, TRUE);
	if (state->multiplex_ostream) {
		struct ostream *output =
			o_stream_create_multiplex(client->output,
				IMAP_MAX_OUTBUF,
				OSTREAM_MULTIPLEX_FORMAT_STREAM_CONTINUE);
		o_stream_unref(&client->output);
		client->output = output;
	}
	client->state = *state;
	client->state.username = p_strdup(pool, state->username);
	client->state.session_id = p_strdup(pool, state->session_id);
	client->state.userdb_fields = p_strdup(pool, state->userdb_fields);
	client->state.stats = p_strdup(pool, state->stats);
	client->state.tag = i_strdup(state->tag);

	client->event = event_create(NULL);
	event_add_category(client->event, &event_category_imap_hibernate);
	event_add_str(client->event, "user", state->username);
	event_add_str(client->event, "session", state->session_id);
	if (state->mailbox_vname != NULL)
		event_add_str(client->event, "mailbox", state->mailbox_vname);
	if (state->local_ip.family != 0)
		event_add_ip(client->event, "local_ip", &state->local_ip);
	if (state->local_port != 0)
		event_add_int(client->event, "local_port", state->local_port);
	if (state->remote_ip.family != 0)
		event_add_ip(client->event, "remote_ip", &state->remote_ip);
	if (state->remote_port != 0)
		event_add_int(client->event, "remote_port", state->remote_port);

	if (state->state_size > 0) {
		client->state.state = statebuf = p_malloc(pool, state->state_size);
		memcpy(statebuf, state->state, state->state_size);
		client->state.state_size = state->state_size;
	}

	struct master_service_anvil_session anvil_session = {
		.username = client->state.username,
		.service_name = master_service_get_name(master_service),
		.ip = client->state.remote_ip,
	};
	T_BEGIN {
		char **fields = p_strsplit_tabescaped(unsafe_data_stack_pool,
						      client->state.userdb_fields);
		const struct var_expand_params params = {
			.table = imap_client_get_var_expand_table(client),
			.providers = funcs,
			.context = fields,
			.event = client->event,
		};
		string_t *str;

		str = t_str_new(256);
		if (var_expand(str, state->mail_log_prefix, &params, &error) < 0) {
			e_error(client->event,
				"Failed to expand mail_log_prefix=%s: %s",
				state->mail_log_prefix, error);
		}
		client->log_prefix = p_strdup(pool, str_c(str));
		anvil_session.alt_usernames =
			userdb_fields_get_alt_usernames(fields);
		if (master_service_anvil_connect(master_service, &anvil_session,
						 TRUE, client->state.anvil_conn_guid))
			client->state.anvil_sent = TRUE;
	} T_END;

	p_array_init(&client->notifys, pool, 2);
	DLLIST_PREPEND(&imap_clients, client);
	return client;
}

static void imap_client_stop_notify_listening(struct imap_client *client)
{
	struct imap_client_notify *notify;

	array_foreach_modifiable(&client->notifys, notify) {
		io_remove(&notify->io);
		i_close_fd(&notify->fd);
	}
}

static void imap_client_stop(struct imap_client *client)
{
	if (client->unhibernate_queued) {
		priorityq_remove(unhibernate_queue, &client->item);
		client->unhibernate_queued = FALSE;
	}
	io_remove(&client->io);
	timeout_remove(&client->to_keepalive);
	imap_client_stop_notify_listening(client);
}

void imap_client_destroy(struct imap_client **_client, const char *reason)
{
	struct imap_client *client = *_client;

	*_client = NULL;

	if (reason != NULL) {
		/* the client input/output bytes don't count the DONE+IDLE by
		   imap-hibernate, but that shouldn't matter much. */
		e_info(client->event, "Disconnected: %s %s",
		       reason, client->state.stats);
	}

	if (client->state.anvil_sent) {
		struct master_service_anvil_session anvil_session = {
			.username = client->state.username,
			.service_name = master_service_get_name(master_service),
			.ip = client->state.remote_ip,
		};
		master_service_anvil_disconnect(master_service, &anvil_session,
						client->state.anvil_conn_guid);
	}

	if (client->master_conn != NULL)
		imap_master_connection_free(&client->master_conn);
	if (client->ioloop_ctx != NULL) {
		io_loop_context_remove_callbacks(client->ioloop_ctx,
						 imap_client_io_activate_user,
						 imap_client_io_deactivate_user, client);
		imap_client_io_deactivate_user(client);
		io_loop_context_unref(&client->ioloop_ctx);
	}

	if (client->state.tag != NULL)
		i_free(client->state.tag);

	if (client->shutdown_fd_on_destroy) {
		if (shutdown(client->fd, SHUT_RDWR) < 0)
			e_error(client->event, "shutdown() failed: %m");
	}

	DLLIST_REMOVE(&imap_clients, client);
	imap_client_stop(client);
	i_stream_destroy(&client->input);
	o_stream_destroy(&client->output);
	i_close_fd(&client->fd);
	event_unref(&client->event);
	pool_unref(&client->pool);

	master_service_client_connection_destroyed(master_service);
}

void imap_client_add_notify_fd(struct imap_client *client, int fd)
{
	struct imap_client_notify *notify;

	notify = array_append_space(&client->notifys);
	notify->fd = fd;
}

void imap_client_create_finish(struct imap_client *client)
{
	struct imap_client_notify *notify;

	client->ioloop_ctx = io_loop_context_new(current_ioloop);
	io_loop_context_add_callbacks(client->ioloop_ctx,
				      imap_client_io_activate_user,
				      imap_client_io_deactivate_user, client);
	io_loop_context_switch(client->ioloop_ctx);

	if (client->state.idle_cmd) {
		client->io = io_add(client->fd, IO_READ,
				    imap_client_input_idle_cmd, client);
	} else {
		client->io = io_add(client->fd, IO_READ,
				    imap_client_input_nonidle, client);
	}
	imap_client_add_idle_keepalive_timeout(client);

	array_foreach_modifiable(&client->notifys, notify) {
		notify->io = io_add(notify->fd, IO_READ,
				    imap_client_input_notify, client);
	}
}

static int client_unhibernate_cmp(const void *p1, const void *p2)
{
	const struct imap_client *c1 = p1, *c2 = p2;
	time_t t1, t2;

	t1 = c1->move_back_start +
		(c1->input_pending ?
		 IMAP_CLIENT_MOVE_BACK_WITH_INPUT_TIMEOUT_SECS :
		 IMAP_CLIENT_MOVE_BACK_WITHOUT_INPUT_TIMEOUT_SECS);
	t2 = c2->move_back_start +
		(c2->input_pending ?
		 IMAP_CLIENT_MOVE_BACK_WITH_INPUT_TIMEOUT_SECS :
		 IMAP_CLIENT_MOVE_BACK_WITHOUT_INPUT_TIMEOUT_SECS);
	if (t1 < t2)
		return -1;
	if (t1 > t2)
		return 1;
	return 0;
}

static void imap_clients_unhibernate(void *context ATTR_UNUSED)
{
	struct priorityq_item *item;

	while ((item = priorityq_peek(unhibernate_queue)) != NULL) {
		struct imap_client *client = (struct imap_client *)item;

		if (!imap_client_try_move_back(client))
			return;
	}
	timeout_remove(&to_unhibernate);
}

static void imap_client_kick(struct imap_client *client, bool shutdown)
{
	imap_client_io_activate_user(client);
	o_stream_nsend_str(client->output,
			   "* BYE "MASTER_SERVICE_SHUTTING_DOWN_MSG".\r\n");
	imap_client_destroy(&client, shutdown ?
			    MASTER_SERVICE_SHUTTING_DOWN_MSG :
			    MASTER_SERVICE_USER_KICKED_MSG);
}

unsigned int imap_clients_kick(const char *user, const guid_128_t conn_guid)
{
	struct imap_client *client, *next;
	unsigned int count = 0;

	for (client = imap_clients; client != NULL; client = next) {
		next = client->next;
		if (strcmp(client->state.username, user) == 0 &&
		    (guid_128_is_empty(conn_guid) ||
		     guid_128_cmp(client->state.anvil_conn_guid, conn_guid) == 0))
			imap_client_kick(client, FALSE);
	}
	return count;
}

void imap_clients_init(void)
{
	unhibernate_queue = priorityq_init(client_unhibernate_cmp, 64);
}

void imap_clients_deinit(void)
{
	while (imap_clients != NULL)
		imap_client_kick(imap_clients, TRUE);

	timeout_remove(&to_unhibernate);
	priorityq_deinit(&unhibernate_queue);
}

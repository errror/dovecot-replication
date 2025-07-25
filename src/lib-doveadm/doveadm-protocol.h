#ifndef DOVEADM_PROTOCOL_H
#define DOVEADM_PROTOCOL_H

#include "module-dir.h"

#define DOVEADM_SERVER_PROTOCOL_VERSION_MAJOR 1
#define DOVEADM_SERVER_PROTOCOL_VERSION_MINOR 3
#define DOVEADM_SERVER_PROTOCOL_VERSION_LINE "VERSION\tdoveadm-server\t1\t3"
#define DOVEADM_CLIENT_PROTOCOL_VERSION_LINE "VERSION\tdoveadm-client\t1\t3"
#define DOVEADM_TCP_CONNECT_TIMEOUT_SECS 30
#define DOVEADM_HANDSHAKE_TIMEOUT_SECS 5

#define DOVEADM_PROTOCOL_MIN_VERSION_MULTIPLEX 1
#define DOVEADM_PROTOCOL_MIN_VERSION_STARTTLS 2
#define DOVEADM_PROTOCOL_MIN_VERSION_LOG_PASSTHROUGH 3
#define DOVEADM_PROTOCOL_MIN_VERSION_EXTRA_FIELDS 3

#define DOVEADM_EX_NOTFOUND EX_NOHOST
#define DOVEADM_EX_NOTPOSSIBLE EX_DATAERR
#define DOVEADM_EX_UNKNOWN -1

#define DOVEADM_EX_CHANGED 2
#define DOVEADM_EX_REFERRAL 1002
#define DOVEADM_EX_EXPIRED 1003

enum doveadm_protocol_cmd_flag {
	DOVEADM_PROTOCOL_CMD_FLAG_DEBUG = 'D',
	DOVEADM_PROTOCOL_CMD_FLAG_VERBOSE = 'v',
	DOVEADM_PROTOCOL_CMD_FLAG_EXTRA_FIELDS = 'x',
};

void doveadm_exit_code_deinit(void);

/* use this function in your doveadm plugin if you need to register
   additional exit codes. The code is the exit code you will use in
   your plugin and the str is the string representation to be used
   when transfering the exit code from server to client over the
   doveadm protocol. */
void doveadm_exit_code_add(const struct module *module,
                           const int code, const char *str);
/* in case your plugin registered an additional exit code, you can
   unregister it using this function. */
void doveadm_exit_code_remove(const int code);

const char *doveadm_exit_code_to_str(int code);
int doveadm_str_to_exit_code(const char *reason);

char doveadm_log_type_to_char(enum log_type type) ATTR_PURE;
bool doveadm_log_type_from_char(char c, enum log_type *type_r);

#endif

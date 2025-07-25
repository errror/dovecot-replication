doveadm_moduledir = $(moduledir)/doveadm
pkglibexecdir = $(libexecdir)/dovecot

SUBDIRS = dsync

bin_PROGRAMS = doveadm
pkglibexec_PROGRAMS = doveadm-server

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-test \
	-I$(top_srcdir)/src/lib-settings \
	-I$(top_srcdir)/src/lib-json \
	-I$(top_srcdir)/src/lib-auth \
	-I$(top_srcdir)/src/lib-auth-client \
	-I$(top_srcdir)/src/lib-compression \
	-I$(top_srcdir)/src/lib-dict \
	-I$(top_srcdir)/src/lib-doveadm \
	-I$(top_srcdir)/src/lib-fs \
	-I$(top_srcdir)/src/lib-ssl-iostream \
	-I$(top_srcdir)/src/lib-master \
	-I$(top_srcdir)/src/lib-login \
	-I$(top_srcdir)/src/lib-mail \
	-I$(top_srcdir)/src/lib-imap \
	-I$(top_srcdir)/src/lib-index \
	-I$(top_srcdir)/src/lib-storage \
	-I$(top_srcdir)/src/lib-imap-storage \
	-I$(top_srcdir)/src/lib-http \
	-I$(top_srcdir)/src/lib-dcrypt \
	-I$(top_srcdir)/src/lib-sasl \
	-I$(top_srcdir)/src/stats \
	-I$(top_srcdir)/src/lib-var-expand \
	-I$(top_srcdir)/src/doveadm/dsync \
	-DMODULEDIR=\""$(moduledir)"\" \
	-DAUTH_MODULE_DIR=\""$(moduledir)/auth"\" \
	-DDOVEADM_MODULEDIR=\""$(doveadm_moduledir)"\" \
	-DPKG_RUNDIR=\""$(rundir)"\" \
	-DPKG_STATEDIR=\""$(statedir)"\" \
	-DPKG_LIBEXECDIR=\""$(pkglibexecdir)"\" \
	-DBINDIR=\""$(bindir)"\" \
	-DMANDIR=\""$(mandir)"\" \
	$(BINARY_CFLAGS)


libs = \
       ../lib-auth/libauth-crypt.la \
	dsync/libdsync.la \
	../lib-compression/libcompression.la

doveadm_LDADD = \
	$(libs) \
	$(CRYPT_LIBS) \
	$(LIBDOVECOT_STORAGE) \
	$(LIBDOVECOT) \
	$(LIBSODIUM_LIBS) \
	$(BINARY_LDFLAGS) \
	-lm

doveadm_DEPENDENCIES = \
	$(libs) \
	$(LIBDOVECOT_STORAGE_DEPS) \
	$(LIBDOVECOT_DEPS)

doveadm_server_LDADD = \
	$(libs) \
	$(LIBDOVECOT_STORAGE) \
	$(LIBDOVECOT) \
	$(BINARY_LDFLAGS) \
	-lm

doveadm_server_DEPENDENCIES = \
	$(libs) \
	$(LIBDOVECOT_STORAGE_DEPS) \
	$(LIBDOVECOT_DEPS)

doveadm_common_cmds = \
	doveadm-auth.c \
	doveadm-dict.c \
	doveadm-fs.c \
	doveadm-indexer.c \
	doveadm-instance.c \
	doveadm-kick.c \
	doveadm-log.c \
	doveadm-master.c \
	doveadm-mutf7.c \
	doveadm-penalty.c \
	doveadm-sis.c \
	doveadm-stats.c \
	doveadm-who.c

doveadm_common_mail_cmds = \
	doveadm-dsync.c \
	doveadm-mail.c \
	doveadm-mail-altmove.c \
	doveadm-mail-deduplicate.c \
	doveadm-mail-dict.c \
	doveadm-mail-expunge.c \
	doveadm-mail-fetch.c \
	doveadm-mail-flags.c \
	doveadm-mail-fs.c \
	doveadm-mail-import.c \
	doveadm-mail-index.c \
	doveadm-mail-iter.c \
	doveadm-mail-mailbox.c \
	doveadm-mail-mailbox-metadata.c \
	doveadm-mail-mailbox-status.c \
	doveadm-mail-copymove.c \
	doveadm-mailbox-list-iter.c \
	doveadm-mail-save.c \
	doveadm-mail-search.c \
	doveadm-mail-server.c \
	doveadm-mail-mailbox-cache.c \
	doveadm-mail-rebuild.c

# these aren't actually useful in doveadm-server, but plugins may implement
# both dumping and some other commands inside a single plugin. not having the
# dump functions in doveadm-server fails to load such plugins.
doveadm_common_dump_cmds = \
	doveadm-dump.c \
	doveadm-dump-dbox.c \
	doveadm-dump-index.c \
	doveadm-dump-log.c \
	doveadm-dump-mailboxlog.c \
	doveadm-dump-thread.c \
	doveadm-dump-dcrypt-file.c \
	doveadm-dump-dcrypt-key.c \
	doveadm-compress.c

common = \
	$(doveadm_common_cmds) \
	$(doveadm_common_mail_cmds) \
	$(doveadm_common_dump_cmds) \
	doveadm-cmd.c \
	doveadm-cmd-parse.c \
	doveadm-print.c \
	doveadm-settings.c \
	doveadm-util.c \
	doveadm-print-formatted.c

doveadm_SOURCES = \
	$(common) \
	doveadm.c \
	doveadm-print-flow.c \
	doveadm-print-pager.c \
	doveadm-print-tab.c \
	doveadm-print-table.c \
	doveadm-print-json.c \
	doveadm-pw.c

doveadm_server_SOURCES = \
	$(common) \
	doveadm-auth-server.c \
	client-connection.c \
	client-connection-tcp.c \
	client-connection-http.c \
	doveadm-print-server.c \
	doveadm-print-json.c \
	main.c

pkginc_libdir = $(pkgincludedir)
pkginc_lib_HEADERS = \
	doveadm.h \
	doveadm-cmd.h \
	doveadm-cmd-parse.h \
	doveadm-dsync.h \
	doveadm-dump.h \
	doveadm-mail.h \
	doveadm-mail-iter.h \
	doveadm-mailbox-list-iter.h \
	doveadm-print.h \
	doveadm-print-private.h \
	doveadm-settings.h \
	doveadm-util.h

noinst_HEADERS = \
	client-connection.h \
	client-connection-private.h \
	doveadm-dict.h \
	doveadm-fs.h \
	doveadm-who.h

test_programs = \
	test-doveadm-cmd \
	test-doveadm-util
noinst_PROGRAMS = $(test_programs)

test_libs = \
	../lib-test/libtest.la \
	../lib/liblib.la
test_deps = $(noinst_LTLIBRARIES) $(test_libs)

test_doveadm_cmd_SOURCES = doveadm-cmd-parse.c test-doveadm-cmd-parse.c
test_doveadm_cmd_LDADD = $(test_libs) $(MODULE_LIBS) $(LIBDOVECOT)
test_doveadm_cmd_DEPENDENCIES = $(test_deps)

test_doveadm_util_SOURCES = doveadm-util.c test-doveadm-util.c
test_doveadm_util_LDADD = $(test_libs) $(MODULE_LIBS)
test_doveadm_util_DEPENDENCIES = $(test_deps)

check-local:
	for bin in $(test_programs); do \
	  if ! $(RUN_TEST) ./$$bin; then exit 1; fi; \
	done

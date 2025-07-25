pkglibexecdir = $(libexecdir)/dovecot

pkglibexec_PROGRAMS = imap

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-test \
	-I$(top_srcdir)/src/lib-settings \
	-I$(top_srcdir)/src/lib-dict \
	-I$(top_srcdir)/src/lib-master \
	-I$(top_srcdir)/src/lib-login \
	-I$(top_srcdir)/src/lib-mail \
	-I$(top_srcdir)/src/lib-smtp \
	-I$(top_srcdir)/src/lib-imap \
	-I$(top_srcdir)/src/lib-imap-urlauth \
	-I$(top_srcdir)/src/lib-imap-storage \
	-I$(top_srcdir)/src/lib-index \
	-I$(top_srcdir)/src/lib-storage \
	-I$(top_srcdir)/src/lib-compression \
	-I$(top_srcdir)/src/lib-var-expand \
	$(BINARY_CFLAGS)

imap_LDFLAGS = -export-dynamic \
	$(BINARY_LDFLAGS)

imap_LDADD = \
	../lib-imap-urlauth/libimap-urlauth.la \
	../lib-compression/libcompression.la \
	$(LIBDOVECOT_STORAGE) \
	$(LIBDOVECOT)
imap_DEPENDENCIES = \
	../lib-imap-urlauth/libimap-urlauth.la \
	../lib-compression/libcompression.la \
	$(LIBDOVECOT_STORAGE_DEPS) \
	$(LIBDOVECOT_DEPS)

cmds = \
	cmd-append.c \
	cmd-capability.c \
	cmd-cancelupdate.c \
	cmd-check.c \
	cmd-close.c \
	cmd-copy.c \
	cmd-create.c \
	cmd-delete.c \
	cmd-enable.c \
	cmd-examine.c \
	cmd-expunge.c \
	cmd-fetch.c \
	cmd-genurlauth.c \
	cmd-getmetadata.c \
	cmd-id.c \
	cmd-idle.c \
	cmd-list.c \
	cmd-logout.c \
	cmd-lsub.c \
	cmd-namespace.c \
	cmd-noop.c \
	cmd-notify.c \
	cmd-rename.c \
	cmd-resetkey.c \
	cmd-search.c \
	cmd-select.c \
	cmd-setmetadata.c \
	cmd-sort.c \
	cmd-status.c \
	cmd-store.c \
	cmd-subscribe.c \
	cmd-thread.c \
	cmd-unselect.c \
	cmd-unsubscribe.c \
	cmd-urlfetch.c \
	cmd-compress.c

common_sources = \
	$(cmds) \
	imap-client.c \
	imap-client-hibernate.c \
	imap-commands.c \
	imap-commands-util.c \
	imap-expunge.c \
	imap-feature.c \
	imap-fetch.c \
	imap-fetch-body.c \
	imap-list.c \
	imap-master-client.c \
	imap-notify.c \
	imap-search.c \
	imap-search-args.c \
	imap-settings.c \
	imap-status.c \
	imap-state.c \
	imap-sync.c \
	imap-storage-callbacks.c

imap_SOURCES = \
	$(common_sources) \
	main.c

headers = \
	imap-client.h \
	imap-commands.h \
	imap-commands-util.h \
	imap-common.h \
	imap-expunge.h \
	imap-feature.h \
	imap-fetch.h \
	imap-list.h \
	imap-master-client.h \
	imap-notify.h \
	imap-search.h \
	imap-search-args.h \
	imap-settings.h \
	imap-status.h \
	imap-state.h \
	imap-sync.h \
	imap-sync-private.h \
	imap-storage-callbacks.h

pkginc_libdir=$(pkgincludedir)
pkginc_lib_HEADERS = $(headers)

if BUILD_IMAP_HIBERNATE
TEST_IMAP_CLIENT_HIBERNATE=test-imap-client-hibernate
endif

test_programs = \
	test-imap-storage-callbacks \
	$(TEST_IMAP_CLIENT_HIBERNATE)
noinst_PROGRAMS = $(test_programs)

test_imap_storage_callbacks_SOURCES = \
	test-imap-storage-callbacks.c \
	imap-storage-callbacks.c
test_imap_storage_callbacks_LDADD = $(imap_LDADD)
test_imap_storage_callbacks_DEPENDENCIES = $(imap_DEPENDENCIES)

test_imap_client_hibernate_SOURCES = \
	test-imap-client-hibernate.c $(common_sources)
test_imap_client_hibernate_LDADD = $(imap_LDADD)
test_imap_client_hibernate_DEPENDENCIES = $(imap_DEPENDENCIES)

check-local:
	for bin in $(test_programs); do \
	  if ! $(RUN_TEST) ./$$bin; then exit 1; fi; \
	done

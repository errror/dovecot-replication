pkglibexecdir = $(libexecdir)/dovecot

pkglibexec_PROGRAMS = imap-login

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-settings \
	-I$(top_srcdir)/src/lib-auth-client \
	-I$(top_srcdir)/src/lib-compression \
	-I$(top_srcdir)/src/lib-sasl \
	-I$(top_srcdir)/src/lib-ssl-iostream \
	-I$(top_srcdir)/src/lib-imap \
	-I$(top_srcdir)/src/lib-master \
	-I$(top_srcdir)/src/lib-login \
	-I$(top_srcdir)/src/lib-mail \
	-I$(top_srcdir)/src/lib-var-expand \
	-I$(top_srcdir)/src/lib-ssl-iostream \
	-I$(top_srcdir)/src/login-common \
	$(BINARY_CFLAGS)

imap_login_LDADD = \
	../lib-compression/libcompression.la \
	$(LIBDOVECOT_LOGIN) \
	$(LIBDOVECOT) \
	$(SSL_LIBS) \
	$(BINARY_LDFLAGS)

imap_login_DEPENDENCIES = \
	../lib-compression/libcompression.la \
	$(LIBDOVECOT_LOGIN) \
	$(LIBDOVECOT_DEPS)

imap_login_SOURCES = \
	imap-login-client.c \
	client-authenticate.c \
	imap-login-cmd-id.c \
	imap-login-commands.c \
	imap-login-settings.c \
	imap-proxy.c

noinst_HEADERS = \
	client-authenticate.h \
	imap-proxy.h

pkginc_libdir=$(pkgincludedir)
pkginc_lib_HEADERS = \
	imap-login-client.h \
	imap-login-commands.h \
	imap-login-settings.h

#	$NetBSD: Makefile,v 1.14 2008/10/26 14:55:17 apb Exp $

PROG=	wsconsctl
SRCS=	display.c keyboard.c keysym.c map_parse.y map_scan.l \
		mouse.c util.c wsconsctl.c
MAN=	wsconsctl.8

YHEADER=	1
CPPFLAGS+=	-I. -I${.CURDIR}
DPSRCS+=	keysym.h
CLEANFILES+=	keysym.h

.include <bsd.prog.mk>

# Environment for scripts executed during build.
SCRIPT_ENV= \
	AWK=${TOOL_AWK:Q} \
	SED=${TOOL_SED:Q}

keysym.h: mkkeysym.sh ${DESTDIR}/usr/include/dev/wscons/wsksymdef.h
	${_MKTARGET_CREATE}
	${SCRIPT_ENV} ${HOST_SH} ${.CURDIR}/mkkeysym.sh \
	  ${DESTDIR}/usr/include/dev/wscons/wsksymdef.h > keysym.h

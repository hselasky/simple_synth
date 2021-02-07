#
# $FreeBSD: $
#
# Makefile for Simple Synth
#

PROG=		simple_synth
CFLAGS+=	-Wall -O2
LDADD+=		-lpthread -lm
SRCS=		simple_synth.c
MAN=		# no manual pages at the moment

.include <bsd.prog.mk>

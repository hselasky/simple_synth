#
# Makefile for Simple Synth
#

PROG=		simple_synth
PREFIX?=	/usr/local
CFLAGS+=	-Wall -O2
LDADD+=		-lpthread -lm
SRCS=		simple_synth.c
MAN=		# no manual pages at the moment
BINDIR?=	${PREFIX}/bin

.include <bsd.prog.mk>

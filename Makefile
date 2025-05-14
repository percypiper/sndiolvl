PREFIX = /usr/local
BIN_DIR = ${PREFIX}/bin
MAN1_DIR = ${PREFIX}/man/man1
CFLAGS = `pkg-config --cflags --libs gstreamer-1.0` -lm -Wall -Werror

all:		sndiolvl

sndiolvl:	sndiolvl.c
		${CC} ${CFLAGS} -o sndiolvl sndiolvl.c

install:	sndiolvl
		mkdir -p ${DESTDIR}${BIN_DIR} ${DESTDIR}${MAN1_DIR}
		cp sndiolvl ${DESTDIR}${BIN_DIR}
		cp sndiolvl.1 ${DESTDIR}${MAN1_DIR}

strip:		strip ./sndiolvl

clean:
		rm -f -- sndiolvl *.o *.tgz

tgz:
		tar -czf sndiolvl.tgz Makefile sndiolvl.c sndiolvl.1

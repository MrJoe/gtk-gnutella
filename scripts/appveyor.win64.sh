#!/bin/sh

case "$1" in
'configure')
	CPPFLAGS="-DMINGW32"
	CPPFLAGS="$CPPFLAGS `pkg-config --cflags gnutls`"
	CPPFLAGS="$CPPFLAGS `pkg-config --cflags glib-2.0`"
	CPPFLAGS="$CPPFLAGS `pkg-config --cflags gtk+-win32-2.0`"
	CPPFLAGS="$CPPFLAGS `pkg-config --cflags zlib`"
	
	./Configure -Oder -D "ccflags=$CPPFLAGS" -D "libs=-lbfd -liberty -lintl -lpthread -lwsock32 -lws2_32 -liconv -limagehlp -liphlpapi -lws2_32 -lpowrprof -lpsapi -lkernel32"
	;;
esac
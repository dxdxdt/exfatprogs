if ENABLE_FTS
AM_CFLAGS = -Wall -Wextra -include $(top_builddir)/config.h -I$(top_srcdir)/include -fno-common

sbin_PROGRAMS = debug.exfat

debug_exfat_SOURCES = debug.c
debug_exfat_LDADD = $(top_builddir)/lib/libexfat.a
endif

/*
 * Copyright (c) 2001-2002, Raphael Manfredi
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

#include <sys/types.h>
#include <sys/stat.h>

#include "gnutella.h"
#include "matching.h"
#include "downloads.h"

#define MAX_STRINGS 1024
#define LINELEN 256

static time_t auto_last_mtime = 0;
static GSList *auto_strings = NULL;

int use_autodownload = 0;
gchar *auto_download_file = "auto-downloads.txt";

void autodownload_init()
{
	cpattern_t *pattern;
	char buf[LINELEN];
	FILE *f;
	int i;
	struct stat sbuf;
	GSList *l;

	if (!use_autodownload)
		return;

	/*
	 * Check whether file has changed since last initialization.
	 */

	if (-1 == stat(auto_download_file, &sbuf))
		return;

	if (auto_last_mtime && auto_last_mtime >= sbuf.st_mtime)
		return;
	auto_last_mtime = sbuf.st_mtime;

	for (l = auto_strings; l; l = l->next) {
		pattern = (cpattern_t *) l->data;
		pattern_free(pattern);
	}
	g_slist_free(auto_strings);
	auto_strings = NULL;

	f = fopen(auto_download_file, "r");
	if (f == NULL) {
		g_warning("could not open %s: %s",
			auto_download_file, g_strerror(errno));
		auto_last_mtime = 0;
		return;
	}

	if (dbg)
		printf("*** reloading %s\n", auto_download_file);

	i = 0;
	while (fgets(buf, LINELEN, f) != NULL) {
		buf[strlen(buf)-1] = 0;			/* Zap the trailing newline */
		if (buf[0] == '#')				/* Comment, ignore line */
			continue;
		pattern = pattern_compile(buf);
		auto_strings = g_slist_append(auto_strings, pattern);
	}

	fclose(f);
}

void autodownload_notify(gchar *file, guint32 size,
						 guint32 record_index, guint32 ip,
						 guint16 port, gchar *guid, gchar *sha1, gboolean push)
{
	GSList* l;

	g_assert(use_autodownload);

	for (l = auto_strings; l; l = l->next) {
		cpattern_t *pattern;
		gchar *result;

		pattern = (cpattern_t *) l->data;
		result = pattern_qsearch(pattern, file, 0, 0, qs_any);

		if (result != NULL) {
			if (dbg > 3)
				printf("*** AUTO-MATCHED '%s' pattern '%s'\n",
					file, pattern->pattern);
			auto_download_new(file, size, record_index, ip, port,
				guid, sha1, push);
			return;
		}
	}

	return;
}


/*
 * $Id$
 *
 * Copyright (c) 2002, Raphael Manfredi
 *
 * Background task management.
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

#ifndef __bg_h__
#define __bg_h__

#include <glib.h>

/*
 * Return values for processing steps.
 */
typedef enum {
	BGR_NEXT = 0,					/* OK, move to next step */
	BGR_MORE,						/* OK, still more work for this step */
	BGR_DONE,						/* OK, end processing */
	BGR_ERROR,						/* Error, abort processing */
} bgret_t;

/*
 * Status codes for final "done" callback.
 */
typedef enum {
	BGS_OK = 0,						/* OK, terminated normally */
	BGS_ERROR,						/* Terminated with error */
	BGS_KILLED,						/* Was killed by signal */
} bgstatus_t;

/*
 * Signals that a task can receive.
 */

typedef enum {
	BG_SIG_ZERO = 0,			/* No signal actually delivered */
	BG_SIG_KILL,				/* Task is being killed (not trappable) */
	BG_SIG_TERM,				/* Task is being terminated */
	BG_SIG_USR,					/* User-defined signal */
	BG_SIG_COUNT,
} bgsig_t;

/*
 * Signatures.
 *
 * `bgstep_cb_t' is a processing step callback.
 * `bgsig_cb_t' is a signal processing handler.
 * `bgclean_cb_t' is the context cleanup handler, called upon task destruction.
 * `bgdone_cb_t' is the final callback called when task is finished.
 * `bgstart_cb_t' is the initial callback when daemon starts working.
 * `bgend_cb_t' is the final callback when daemon ends working.
 * `bgnotify_cb_t' is the start/stop callback when daemon starts/stops working.
 */

typedef bgret_t (*bgstep_cb_t)(gpointer h, gpointer ctx, gint ticks);
typedef void (*bgsig_cb_t)(gpointer h, gpointer ctx, bgsig_t sig);
typedef void (*bgclean_cb_t)(gpointer ctx);
typedef void (*bgdone_cb_t)(gpointer h, gpointer ctx,
	bgstatus_t status, gpointer arg);
typedef void (*bgstart_cb_t)(gpointer h, gpointer ctx, gpointer item);
typedef void (*bgend_cb_t)(gpointer h, gpointer ctx, gpointer item);
typedef void (*bgnotify_cb_t)(gpointer h, gboolean on);

/*
 * Public interface.
 */

void bg_close(void);
void bg_sched_timer(void);

gpointer bg_task_create(
	gchar *name,						/* Task name (for tracing) */
	bgstep_cb_t *steps, gint stepcnt,	/* Work to perform (copied) */
	gpointer ucontext,					/* User context */
	bgclean_cb_t ucontext_free,			/* Free routine for context */
	bgdone_cb_t done_cb,				/* Notification callback when done */
	gpointer done_arg);					/* Callback argument */

gpointer bg_daemon_create(
	gchar *name,						/* Task name (for tracing) */
	bgstep_cb_t *steps, gint stepcnt,	/* Work to perform (copied) */
	gpointer ucontext,					/* User context */
	bgclean_cb_t ucontext_free,			/* Free routine for context */
	bgstart_cb_t start_cb,				/* Starting working on an item */
	bgend_cb_t end_cb,					/* Done working on an item */
	bgclean_cb_t item_free,				/* Free routine for work queue items */
	bgnotify_cb_t notify);				/* Start/Stop notify (optional) */

void bg_daemon_enqueue(gpointer h, gpointer item);

void bg_task_cancel(gpointer h);
void bg_task_exit(gpointer h, gint code);
void bg_task_ticks_used(gpointer h, gint used);

gint bg_task_seqno(gpointer h);
gpointer bg_task_context(gpointer h);

#endif	/* __bg_h__ */

/* vi: set ts=4: */


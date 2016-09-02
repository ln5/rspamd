/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "config.h"
#include "fuzzy_backend.h"
#include "fuzzy_backend_sqlite.h"

#define DEFAULT_EXPIRE 172800L

enum rspamd_fuzzy_backend_type {
	RSPAMD_FUZZY_BACKEND_SQLITE = 0,
	// RSPAMD_FUZZY_BACKEND_REDIS
};

static void* rspamd_fuzzy_backend_init_sqlite (struct rspamd_fuzzy_backend *bk,
		const ucl_object_t *obj, GError **err);
static void rspamd_fuzzy_backend_check_sqlite (struct rspamd_fuzzy_backend *bk,
		const struct rspamd_fuzzy_cmd *cmd,
		rspamd_fuzzy_check_cb cb, void *ud,
		void *subr_ud);
static void rspamd_fuzzy_backend_update_sqlite (struct rspamd_fuzzy_backend *bk,
		GQueue *updates, const gchar *src,
		rspamd_fuzzy_update_cb cb, void *ud,
		void *subr_ud);
static void rspamd_fuzzy_backend_count_sqlite (struct rspamd_fuzzy_backend *bk,
		rspamd_fuzzy_count_cb cb, void *ud,
		void *subr_ud);
static void rspamd_fuzzy_backend_version_sqlite (struct rspamd_fuzzy_backend *bk,
		const gchar *src,
		rspamd_fuzzy_version_cb cb, void *ud,
		void *subr_ud);
static const gchar* rspamd_fuzzy_backend_id_sqlite (struct rspamd_fuzzy_backend *bk,
		void *subr_ud);
static void rspamd_fuzzy_backend_expire_sqlite (struct rspamd_fuzzy_backend *bk,
		void *subr_ud);
static void rspamd_fuzzy_backend_close_sqlite (struct rspamd_fuzzy_backend *bk,
		void *subr_ud);

struct rspamd_fuzzy_backend_subr {
	void* (*init) (struct rspamd_fuzzy_backend *bk, const ucl_object_t *obj,
			GError **err);
	void (*check) (struct rspamd_fuzzy_backend *bk,
			const struct rspamd_fuzzy_cmd *cmd,
			rspamd_fuzzy_check_cb cb, void *ud,
			void *subr_ud);
	void (*update) (struct rspamd_fuzzy_backend *bk,
			GQueue *updates, const gchar *src,
			rspamd_fuzzy_update_cb cb, void *ud,
			void *subr_ud);
	void (*count) (struct rspamd_fuzzy_backend *bk,
			rspamd_fuzzy_count_cb cb, void *ud,
			void *subr_ud);
	void (*version) (struct rspamd_fuzzy_backend *bk,
			const gchar *src,
			rspamd_fuzzy_version_cb cb, void *ud,
			void *subr_ud);
	const gchar* (*id) (struct rspamd_fuzzy_backend *bk, void *subr_ud);
	void (*periodic) (struct rspamd_fuzzy_backend *bk, void *subr_ud);
	void (*close) (struct rspamd_fuzzy_backend *bk, void *subr_ud);
};

static const struct rspamd_fuzzy_backend_subr fuzzy_subrs[] = {
	[RSPAMD_FUZZY_BACKEND_SQLITE] = {
		.init = rspamd_fuzzy_backend_init_sqlite,
		.check = rspamd_fuzzy_backend_check_sqlite,
		.update = rspamd_fuzzy_backend_update_sqlite,
		.count = rspamd_fuzzy_backend_count_sqlite,
		.version = rspamd_fuzzy_backend_version_sqlite,
		.id = rspamd_fuzzy_backend_id_sqlite,
		.periodic = rspamd_fuzzy_backend_expire_sqlite,
		.close = rspamd_fuzzy_backend_close_sqlite,
	}
};

struct rspamd_fuzzy_backend {
	enum rspamd_fuzzy_backend_type type;
	gdouble expire;
	gdouble sync;
	struct event_base *ev_base;
	rspamd_fuzzy_periodic_cb periodic_cb;
	void *periodic_ud;
	const struct rspamd_fuzzy_backend_subr *subr;
	void *subr_ud;
	struct event periodic_event;
};

static GQuark
rspamd_fuzzy_backend_quark (void)
{
	return g_quark_from_static_string ("fuzzy-backend");
}

static void*
rspamd_fuzzy_backend_init_sqlite (struct rspamd_fuzzy_backend *bk,
		const ucl_object_t *obj, GError **err)
{
	const ucl_object_t *elt;

	elt = ucl_object_lookup_any (obj, "hashfile", "hash_file", "file",
			"database", NULL);

	if (elt == NULL || ucl_object_type (elt) != UCL_STRING) {
		g_set_error (err, rspamd_fuzzy_backend_quark (),
				EINVAL, "missing sqlite3 path");
		return NULL;
	}

	return rspamd_fuzzy_backend_sqlite_open (ucl_object_tostring (elt),
			FALSE, err);
}

static void
rspamd_fuzzy_backend_check_sqlite (struct rspamd_fuzzy_backend *bk,
		const struct rspamd_fuzzy_cmd *cmd,
		rspamd_fuzzy_check_cb cb, void *ud,
		void *subr_ud)
{
	struct rspamd_fuzzy_backend_sqlite *sq = subr_ud;
	struct rspamd_fuzzy_reply rep;

	rep = rspamd_fuzzy_backend_sqlite_check (sq, cmd, bk->expire);

	if (cb) {
		cb (&rep, ud);
	}
}

static void
rspamd_fuzzy_backend_update_sqlite (struct rspamd_fuzzy_backend *bk,
		GQueue *updates, const gchar *src,
		rspamd_fuzzy_update_cb cb, void *ud,
		void *subr_ud)
{
	struct rspamd_fuzzy_backend_sqlite *sq = subr_ud;
	gboolean success = FALSE;
	GList *cur;
	struct fuzzy_peer_cmd *io_cmd;
	struct rspamd_fuzzy_cmd *cmd;
	gpointer ptr;
	guint nupdates = 0;

	if (rspamd_fuzzy_backend_sqlite_prepare_update (sq, src)) {
		cur = updates->head;

		while (cur) {
			io_cmd = cur->data;

			if (io_cmd->is_shingle) {
				cmd = &io_cmd->cmd.shingle.basic;
				ptr = &io_cmd->cmd.shingle;
			}
			else {
				cmd = &io_cmd->cmd.normal;
				ptr = &io_cmd->cmd.normal;
			}

			if (cmd->cmd == FUZZY_WRITE) {
				rspamd_fuzzy_backend_sqlite_add (sq, ptr);
			}
			else {
				rspamd_fuzzy_backend_sqlite_del (sq, ptr);
			}

			nupdates ++;
			cur = g_list_next (cur);
		}

		if (rspamd_fuzzy_backend_sqlite_finish_update (sq, src,
				nupdates > 0)) {
			success = TRUE;
		}
	}

	if (cb) {
		cb (success, ud);
	}
}

static void
rspamd_fuzzy_backend_count_sqlite (struct rspamd_fuzzy_backend *bk,
		rspamd_fuzzy_count_cb cb, void *ud,
		void *subr_ud)
{
	struct rspamd_fuzzy_backend_sqlite *sq = subr_ud;
	guint64 nhashes;

	nhashes = rspamd_fuzzy_backend_sqlite_count (sq);

	if (cb) {
		cb (nhashes, ud);
	}
}

static void
rspamd_fuzzy_backend_version_sqlite (struct rspamd_fuzzy_backend *bk,
		const gchar *src,
		rspamd_fuzzy_version_cb cb, void *ud,
		void *subr_ud)
{
	struct rspamd_fuzzy_backend_sqlite *sq = subr_ud;
	guint64 rev;

	rev = rspamd_fuzzy_backend_sqlite_version (sq, src);

	if (cb) {
		cb (rev, ud);
	}
}

static const gchar*
rspamd_fuzzy_backend_id_sqlite (struct rspamd_fuzzy_backend *bk,
		void *subr_ud)
{
	struct rspamd_fuzzy_backend_sqlite *sq = subr_ud;

	return rspamd_fuzzy_sqlite_backend_id (sq);
}
static void
rspamd_fuzzy_backend_expire_sqlite (struct rspamd_fuzzy_backend *bk,
		void *subr_ud)
{
	struct rspamd_fuzzy_backend_sqlite *sq = subr_ud;

	rspamd_fuzzy_backend_sqlite_sync (sq, bk->expire, TRUE);
}

static void
rspamd_fuzzy_backend_close_sqlite (struct rspamd_fuzzy_backend *bk,
		void *subr_ud)
{
	struct rspamd_fuzzy_backend_sqlite *sq = subr_ud;

	rspamd_fuzzy_backend_sqlite_close (sq);
}


struct rspamd_fuzzy_backend *
rspamd_fuzzy_backend_create (struct event_base *ev_base,
		const ucl_object_t *config, GError **err)
{
	struct rspamd_fuzzy_backend *bk;
	enum rspamd_fuzzy_backend_type type = RSPAMD_FUZZY_BACKEND_SQLITE;
	const ucl_object_t *elt;
	gdouble expire = DEFAULT_EXPIRE;

	if (config != NULL) {
		elt = ucl_object_lookup (config, "type");

		if (elt != NULL && ucl_object_type (elt) == UCL_STRING) {
			if (strcmp (ucl_object_tostring (elt), "sqlite") == 0) {
				type = RSPAMD_FUZZY_BACKEND_SQLITE;
			}
			else {
				g_set_error (err, rspamd_fuzzy_backend_quark (),
						EINVAL, "invalid backend type: %s",
						ucl_object_tostring (elt));
				return NULL;
			}
		}

		elt = ucl_object_lookup (config, "expire");

		if (elt != NULL) {
			expire = ucl_object_todouble (elt);
		}
	}

	bk = g_slice_alloc0 (sizeof (*bk));
	bk->ev_base = ev_base;
	bk->expire = expire;
	bk->type = type;
	bk->subr = &fuzzy_subrs[type];

	if ((bk->subr_ud = bk->subr->init (bk, config, err)) == NULL) {
		g_slice_free1 (sizeof (*bk), bk);
	}

	return bk;
}


void
rspamd_fuzzy_backend_check (struct rspamd_fuzzy_backend *bk,
		const struct rspamd_fuzzy_cmd *cmd,
		rspamd_fuzzy_check_cb cb, void *ud)
{
	g_assert (bk != NULL);

	bk->subr->check (bk, cmd, cb, ud, bk->subr_ud);
}

void
rspamd_fuzzy_backend_process_updates (struct rspamd_fuzzy_backend *bk,
		GQueue *updates, const gchar *src, rspamd_fuzzy_update_cb cb,
		void *ud)
{
	g_assert (bk != NULL);
	g_assert (updates != NULL);

	if (g_queue_get_length (updates) > 0) {
		bk->subr->update (bk, updates, src, cb, ud, bk->subr_ud);
	}
	else if (cb) {
		cb (TRUE, ud);
	}
}


void
rspamd_fuzzy_backend_count (struct rspamd_fuzzy_backend *bk,
		rspamd_fuzzy_count_cb cb, void *ud)
{
	g_assert (bk != NULL);

	bk->subr->count (bk, cb, ud, bk->subr_ud);
}


void
rspamd_fuzzy_backend_version (struct rspamd_fuzzy_backend *bk,
		const gchar *src,
		rspamd_fuzzy_version_cb cb, void *ud)
{
	g_assert (bk != NULL);

	bk->subr->version (bk, src, cb, ud, bk->subr_ud);
}

const gchar *
rspamd_fuzzy_backend_id (struct rspamd_fuzzy_backend *bk)
{
	g_assert (bk != NULL);

	if (bk->subr->id) {
		return bk->subr->id (bk, bk->subr_ud);
	}

	return NULL;
}

static inline void
rspamd_fuzzy_backend_periodic_sync (struct rspamd_fuzzy_backend *bk)
{
	if (bk->periodic_cb) {
		if (bk->periodic_cb (bk->periodic_ud)) {
			if (bk->subr->periodic) {
				bk->subr->periodic (bk, bk->subr_ud);
			}
		}
	}
	else {
		if (bk->subr->periodic) {
			bk->subr->periodic (bk, bk->subr_ud);
		}
	}
}

static void
rspamd_fuzzy_backend_periodic_cb (gint fd, short what, void *ud)
{
	struct rspamd_fuzzy_backend *bk = ud;
	gdouble jittered;
	struct timeval tv;

	jittered = rspamd_time_jitter (bk->sync, bk->sync / 2.0);
	double_to_tv (jittered, &tv);
	event_del (&bk->periodic_event);
	rspamd_fuzzy_backend_periodic_sync (bk);
	event_add (&bk->periodic_event, &tv);
}

void
rspamd_fuzzy_backend_start_update (struct rspamd_fuzzy_backend *bk,
		gdouble timeout,
		rspamd_fuzzy_periodic_cb cb,
		void *ud)
{
	gdouble jittered;
	struct timeval tv;

	g_assert (bk != NULL);

	if (bk->subr->periodic) {
		if (bk->sync > 0.0) {
			event_del (&bk->periodic_event);
		}

		if (cb) {
			bk->periodic_cb = cb;
			bk->periodic_ud = ud;
		}

		rspamd_fuzzy_backend_periodic_sync (bk);
		bk->sync = timeout;
		jittered = rspamd_time_jitter (timeout, timeout / 2.0);
		double_to_tv (jittered, &tv);
		event_set (&bk->periodic_event, -1, EV_TIMEOUT,
				rspamd_fuzzy_backend_periodic_cb, bk);
		event_base_set (bk->ev_base, &bk->periodic_event);
		event_add (&bk->periodic_event, &tv);
	}
}

void
rspamd_fuzzy_backend_close (struct rspamd_fuzzy_backend *bk)
{
	g_assert (bk != NULL);

	if (bk->sync > 0.0) {
		rspamd_fuzzy_backend_periodic_sync (bk);
		event_del (&bk->periodic_event);
	}

	bk->subr->close (bk, bk->subr_ud);

	g_slice_free1 (sizeof (*bk), bk);
}

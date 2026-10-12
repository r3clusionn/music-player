/* music-player: a foobar2000-style music player. */
#include "app.h"

#include "icons.h"
#include "tags.h"

#include <X11/Xlib.h>
#include <gdk/gdkx.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

App *app;

typedef struct {
	gint64 playlist;
	int index;
} HistoryItem;

/* ---- small helpers -------------------------------------------------------- */

static Playlist *playlist_by_id(gint64 id)
{
	for (guint i = 0; i < app->playlists->len; i++) {
		Playlist *pl = g_ptr_array_index(app->playlists, i);
		if (pl->id == id)
			return pl;
	}
	return NULL;
}

static void set_play_icon(void)
{
	gboolean playing = player_state(app->player) == PLAYER_PLAYING;
	icon_set(gtk_bin_get_child(GTK_BIN(app->play_button)), playing ? ICON_PAUSE : ICON_PLAY);
	gtk_widget_set_tooltip_text(app->play_button, playing ? "Pause" : "Play");
}

/* ---- the cover and properties panels -------------------------------------- */

static guint cover_generation;

typedef struct {
	char *path;
	int size, scale;
	guint generation;
} CoverJob;

static void cover_job_free(gpointer p)
{
	CoverJob *j = p;
	g_free(j->path);
	g_free(j);
}

static void cover_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
	(void)src;
	(void)c;
	CoverJob *j = data;
	GdkPixbuf *pb = tags_cover(j->path, j->size * j->scale);
	g_task_return_pointer(task, pb, pb ? g_object_unref : NULL);
}

static void cover_done(GObject *src, GAsyncResult *res, gpointer data)
{
	(void)src;
	(void)data;
	CoverJob *j = g_task_get_task_data(G_TASK(res));
	GdkPixbuf *pb = g_task_propagate_pointer(G_TASK(res), NULL);
	if (j->generation == cover_generation) {
		if (pb) {
			/* A surface at the window's scale stays sharp on the 2x screen. */
			cairo_surface_t *s = gdk_cairo_surface_create_from_pixbuf(pb, j->scale, NULL);
			gtk_image_set_from_surface(GTK_IMAGE(app->cover), s);
			cairo_surface_destroy(s);
		} else {
			gtk_image_clear(GTK_IMAGE(app->cover));
		}
	}
	if (pb)
		g_object_unref(pb);
}

static char *cover_path;

static void show_cover(Track *t)
{
	if (!t) {
		g_clear_pointer(&cover_path, g_free);
		cover_generation++;
		gtk_image_clear(GTK_IMAGE(app->cover));
		return;
	}
	if (!g_strcmp0(cover_path, t->path))
		return;
	g_free(cover_path);
	cover_path = g_strdup(t->path);
	CoverJob *j = g_new0(CoverJob, 1);
	j->path = g_strdup(t->path);
	int w = gtk_widget_get_allocated_width(app->cover), h = gtk_widget_get_allocated_height(app->cover);
	j->size = MAX(64, MIN(w, h) - 8);
	j->scale = gtk_widget_get_scale_factor(app->cover);
	j->generation = ++cover_generation;
	GTask *task = g_task_new(NULL, NULL, cover_done, NULL);
	g_task_set_task_data(task, j, cover_job_free);
	g_task_run_in_thread(task, cover_thread);
	g_object_unref(task);
}

static void on_cover_resized(GtkWidget *w, GdkRectangle *a, gpointer data)
{
	(void)w;
	(void)data;
	/* Decode again only when the panel's size really changed: setting the
	 * image allocates again, and redoing it every time looped forever. */
	static int last_w, last_h;
	if (abs(a->width - last_w) < 8 && abs(a->height - last_h) < 8)
		return;
	last_w = a->width;
	last_h = a->height;
	if (cover_path) {
		char *p = cover_path;
		cover_path = NULL;
		Track t = {.path = p};
		show_cover(&t);
		g_free(p);
	}
}

static void props_row(GtkListStore *s, const char *name, const char *value)
{
	if (value && *value)
		gtk_list_store_insert_with_values(s, NULL, -1, 0, name, 1, value, -1);
}

void app_show_track_info(Track *t)
{
	GtkListStore *s = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(app->props)));
	gtk_list_store_clear(s);
	if (player_state(app->player) == PLAYER_STOPPED)
		show_cover(t);
	if (!t)
		return;
	char buf[256];
	props_row(s, "Title", track_display_title(t));
	props_row(s, "Artist", t->artist);
	props_row(s, "Album artist", t->albumartist);
	props_row(s, "Album", t->album);
	props_row(s, "Date", t->date);
	props_row(s, "Genre", t->genre);
	if (t->tracknumber) {
		if (t->totaltracks)
			g_snprintf(buf, sizeof buf, "%d / %d", t->tracknumber, t->totaltracks);
		else
			g_snprintf(buf, sizeof buf, "%d", t->tracknumber);
		props_row(s, "Track", buf);
	}
	if (t->discnumber) {
		g_snprintf(buf, sizeof buf, "%d", t->discnumber);
		props_row(s, "Disc", buf);
	}
	props_row(s, "Comment", t->comment);
	format_duration(t->duration, buf, sizeof buf);
	props_row(s, "Duration", buf);
	props_row(s, "Codec", t->codec);
	if (t->bitrate) {
		g_snprintf(buf, sizeof buf, "%d kbps", t->bitrate);
		props_row(s, "Bitrate", buf);
	}
	if (t->samplerate) {
		g_snprintf(buf, sizeof buf, "%d Hz", t->samplerate);
		props_row(s, "Sample rate", buf);
	}
	if (t->bits) {
		g_snprintf(buf, sizeof buf, "%d", t->bits);
		props_row(s, "Bits per sample", buf);
	}
	if (t->channels) {
		g_snprintf(buf, sizeof buf, "%d", t->channels);
		props_row(s, "Channels", buf);
	}
	static const char *const rg_names[] = {"Track gain", "Album gain", "Track peak", "Album peak"};
	double rg[] = {t->rg_track_gain, t->rg_album_gain, t->rg_track_peak, t->rg_album_peak};
	for (int i = 0; i < 4; i++)
		if (!isnan(rg[i])) {
			g_snprintf(buf, sizeof buf, i < 2 ? "%+.2f dB" : "%.6f", rg[i]);
			props_row(s, rg_names[i], buf);
		}
	if (t->size) {
		char *sz = g_format_size(t->size);
		props_row(s, "File size", sz);
		g_free(sz);
	}
	if (t->cue) {
		g_snprintf(buf, sizeof buf, "cue track %d, from %.2f s", t->cue, t->cue_start);
		props_row(s, "Range", buf);
	}
	props_row(s, "Path", t->path);
}

/* ---- status bar and position ---------------------------------------------- */

static gboolean fullscreen;

static void update_title(void)
{
	Track *t = player_current(app->player);
	if (!t) {
		gtk_window_set_title(GTK_WINDOW(app->window), "music-player");
		return;
	}
	char *title = t->artist && *t->artist
		? g_strdup_printf("%s - %s  [music-player]", t->artist, track_display_title(t))
		: g_strdup_printf("%s  [music-player]", track_display_title(t));
	gtk_window_set_title(GTK_WINDOW(app->window), title);
	g_free(title);
}

static void update_position(void)
{
	Track *t = player_current(app->player);
	char pos[32] = "", len[32] = "", label[80] = "";
	if (t) {
		double p = player_position(app->player);
		format_duration(p, pos, sizeof pos);
		format_duration(t->duration, len, sizeof len);
		g_snprintf(label, sizeof label, "%s / %s", pos, len);
		if (!app->seek_dragging && t->duration > 0) {
			gtk_range_set_range(GTK_RANGE(app->seek), 0, t->duration);
			gtk_range_set_value(GTK_RANGE(app->seek), MIN(p, t->duration));
		}
	} else if (!app->seek_dragging) {
		gtk_range_set_value(GTK_RANGE(app->seek), 0);
	}
	gtk_widget_set_sensitive(app->seek, t != NULL);
	gtk_label_set_text(GTK_LABEL(app->time_label), label);
}

void app_update_status(void)
{
	if (!app->status)
		return;
	GString *s = g_string_new(NULL);
	PlayerState st = player_state(app->player);
	if (st == PLAYER_STOPPED) {
		g_string_append(s, "Stopped");
	} else {
		char info[160];
		player_audio_info(app->player, info, sizeof info);
		g_string_append(s, info);
		if (st == PLAYER_PAUSED)
			g_string_append(s, " · paused");
	}
	if (app->stop_after_current)
		g_string_append(s, " · stop after current");
	Playlist *pl = playlist_active();
	if (app->scan_status) {
		g_string_append_printf(s, "    %s", app->scan_status);
	} else if (pl) {
		int n;
		double total = playlist_total_length(pl, &n);
		char len[32];
		format_duration(total, len, sizeof len);
		g_string_append_printf(s, "    %s: %d track%s, %s", pl->name, n, n == 1 ? "" : "s", len);
	}
	gtk_label_set_text(GTK_LABEL(app->status), s->str);
	g_string_free(s, TRUE);
	update_position();
}

/* The position updates once a second, on the second, while playing and
 * visible; nothing runs while paused, stopped or hidden. */
static void schedule_tick(void);

static gboolean tick(gpointer data)
{
	(void)data;
	app->pos_timer = 0;
	update_position();
	schedule_tick();
	return G_SOURCE_REMOVE;
}

static void schedule_tick(void)
{
	if (app->pos_timer) {
		g_source_remove(app->pos_timer);
		app->pos_timer = 0;
	}
	if (player_state(app->player) != PLAYER_PLAYING || !app->visible)
		return;
	double p = player_position(app->player);
	double wait = 1.0 - (p - floor(p)) + 0.02;
	app->pos_timer = g_timeout_add((guint)(wait * 1000), tick, NULL);
}

static void set_visible(gboolean v)
{
	if (v == app->visible)
		return;
	app->visible = v;
	if (v)
		app_update_status();
	schedule_tick();
}

static gboolean on_visibility(GtkWidget *w, GdkEventVisibility *ev, gpointer data)
{
	(void)w;
	(void)data;
	set_visible(ev->state != GDK_VISIBILITY_FULLY_OBSCURED);
	return FALSE;
}

static gboolean on_window_state(GtkWidget *w, GdkEventWindowState *ev, gpointer data)
{
	(void)w;
	(void)data;
	if (ev->changed_mask & GDK_WINDOW_STATE_FULLSCREEN)
		fullscreen = (ev->new_window_state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
	if (ev->changed_mask & GDK_WINDOW_STATE_ICONIFIED)
		set_visible(!(ev->new_window_state & GDK_WINDOW_STATE_ICONIFIED));
	return FALSE;
}

static gboolean on_unmap(GtkWidget *w, GdkEvent *ev, gpointer data)
{
	(void)w;
	(void)ev;
	(void)data;
	set_visible(FALSE);
	return FALSE;
}

/* ---- playback order ------------------------------------------------------- */

static void shuffle_build(Playlist *pl, int first)
{
	guint n = pl->tracks->len;
	if (pl->shuffle)
		g_array_unref(pl->shuffle);
	pl->shuffle = g_array_sized_new(FALSE, FALSE, sizeof(int), n);
	for (guint i = 0; i < n; i++)
		g_array_append_val(pl->shuffle, i);
	for (guint i = n - 1; n && i > 0; i--) {
		guint j = g_random_int_range(0, (gint32)i + 1);
		int tmp = g_array_index(pl->shuffle, int, i);
		g_array_index(pl->shuffle, int, i) = g_array_index(pl->shuffle, int, j);
		g_array_index(pl->shuffle, int, j) = tmp;
	}
	/* The playing track goes first, so the rest follow it. */
	for (guint i = 0; first >= 0 && i < n; i++)
		if (g_array_index(pl->shuffle, int, i) == first) {
			g_array_index(pl->shuffle, int, i) = g_array_index(pl->shuffle, int, 0);
			g_array_index(pl->shuffle, int, 0) = first;
		}
	pl->shuffle_pos = 0;
}

static char *album_key(Track *t)
{
	return g_strdup_printf("%s\x1f%s", track_album_artist(t), t->album ? t->album : t->path);
}

static int next_index(Playlist *pl, int i)
{
	int n = (int)pl->tracks->len;
	if (!n)
		return -1;
	switch (app->order) {
	case ORDER_DEFAULT:
		return i + 1 < n ? i + 1 : -1;
	case ORDER_REPEAT_PLAYLIST:
		return (i + 1) % n;
	case ORDER_REPEAT_TRACK:
		return i >= 0 && i < n ? i : 0;
	case ORDER_SHUFFLE_TRACKS: {
		if (!pl->shuffle || (int)pl->shuffle->len != n)
			shuffle_build(pl, i);
		int pos = -1;
		for (int k = 0; k < n && pos < 0; k++)
			if (g_array_index(pl->shuffle, int, k) == i)
				pos = k;
		if (pos + 1 >= n) {
			shuffle_build(pl, -1);
			/* Don't play the same track twice in a row across a reshuffle. */
			if (n > 1 && g_array_index(pl->shuffle, int, 0) == i)
				return g_array_index(pl->shuffle, int, 1);
			return g_array_index(pl->shuffle, int, 0);
		}
		return g_array_index(pl->shuffle, int, pos + 1);
	}
	case ORDER_SHUFFLE_ALBUMS: {
		char *k = i >= 0 && i < n ? album_key(g_ptr_array_index(pl->tracks, i)) : NULL;
		if (k && i + 1 < n) {
			char *k2 = album_key(g_ptr_array_index(pl->tracks, i + 1));
			gboolean same = !strcmp(k, k2);
			g_free(k2);
			if (same) {
				g_free(k);
				return i + 1;
			}
		}
		/* A random album's first track, not the one that just ended. */
		GArray *starts = g_array_new(FALSE, FALSE, sizeof(int));
		char *prev = NULL;
		for (int j = 0; j < n; j++) {
			char *kj = album_key(g_ptr_array_index(pl->tracks, j));
			if (!prev || strcmp(prev, kj)) {
				if (!k || strcmp(kj, k) || n == 1)
					g_array_append_val(starts, j);
			}
			g_free(prev);
			prev = kj;
		}
		g_free(prev);
		g_free(k);
		int r = starts->len ? g_array_index(starts, int, g_random_int_range(0, (gint32)starts->len)) : 0;
		g_array_unref(starts);
		return r;
	}
	case ORDER_RANDOM: {
		if (n == 1)
			return 0;
		int r;
		do
			r = g_random_int_range(0, n);
		while (r == i);
		return r;
	}
	default:
		return -1;
	}
}

static Track *pop_queue(void)
{
	QueueItem *q = g_queue_pop_head(app->queue);
	if (!q)
		return NULL;
	Track *t = q->track;
	app->pending_pl = q->pl;
	app->pending_index = q->pl ? playlist_index_of(q->pl, t, -1) : -1;
	g_free(q);
	playlist_redraw_all();
	return t; /* the queue's reference */
}

static Track *next_track_cb(Track *current, gpointer data)
{
	(void)data;
	if (app->queue->length)
		return pop_queue();
	if (app->stop_after_current)
		return NULL;
	Playlist *pl = app->playing_pl;
	if (!pl)
		return NULL;
	int i = playlist_index_of(pl, current, app->playing_index);
	if (i < 0)
		i = app->playing_index - 1; /* the playing track was removed: go on from its place */
	int next = next_index(pl, i);
	if (next < 0 || next >= (int)pl->tracks->len)
		return NULL;
	app->pending_pl = pl;
	app->pending_index = next;
	return track_ref(g_ptr_array_index(pl->tracks, next));
}

static void history_push(Playlist *pl, int index)
{
	if (!pl || index < 0)
		return;
	HistoryItem h = {pl->id, index};
	if (app->history->len) {
		HistoryItem *last = &g_array_index(app->history, HistoryItem, app->history->len - 1);
		if (last->playlist == h.playlist && last->index == h.index)
			return;
	}
	g_array_append_val(app->history, h);
	if (app->history->len > 500)
		g_array_remove_range(app->history, 0, 100);
}

static void track_changed_cb(Track *t, gpointer data)
{
	(void)data;
	Playlist *old_pl = app->playing_pl;
	int old_index = app->playing_index;
	if (t && app->pending_pl && app->pending_index >= 0 &&
	    app->pending_index < (int)app->pending_pl->tracks->len &&
	    g_ptr_array_index(app->pending_pl->tracks, app->pending_index) == t) {
		app->playing_pl = app->pending_pl;
		app->playing_index = app->pending_index;
	} else if (t && app->playing_pl) {
		int i = playlist_index_of(app->playing_pl, t, app->playing_index);
		if (i >= 0)
			app->playing_index = i;
	}
	app->pending_pl = NULL;
	app->pending_index = -1;
	if (!t && app->stop_after_current)
		app_set_stop_after_current(FALSE);
	playlist_row_changed(old_pl, old_index);
	if (t) {
		playlist_row_changed(app->playing_pl, app->playing_index);
		history_push(app->playing_pl, app->playing_index);
		/* Follow the playing track if the view was showing the previous one. */
		if (app->playing_pl && app->playing_pl == old_pl && playlist_cursor(old_pl) == old_index)
			playlist_scroll_to(app->playing_pl, app->playing_index, TRUE);
	}
	show_cover(t);
	if (t)
		app_show_track_info(t);
	update_title();
	app_update_status();
	schedule_tick();
}

static void state_changed_cb(PlayerState s, gpointer data)
{
	(void)s;
	(void)data;
	set_play_icon();
	playlist_row_changed(app->playing_pl, app->playing_index);
	app_update_status();
	schedule_tick();
}

static gboolean clear_scan_status(gpointer data)
{
	(void)data;
	app->scan_status_id = 0;
	g_clear_pointer(&app->scan_status, g_free);
	app_update_status();
	return G_SOURCE_REMOVE;
}

static void error_cb(Track *t, const char *message, gpointer data)
{
	(void)data;
	g_free(app->scan_status);
	app->scan_status = g_strdup_printf("Could not play %s: %s", track_display_title(t), message);
	if (app->scan_status_id)
		g_source_remove(app->scan_status_id);
	app->scan_status_id = g_timeout_add_seconds(8, clear_scan_status, NULL);
	app_update_status();
}

void app_play_track(Playlist *pl, int index)
{
	if (!pl || index < 0 || index >= (int)pl->tracks->len)
		return;
	Playlist *old_pl = app->playing_pl;
	int old_index = app->playing_index;
	app->playing_pl = pl;
	app->playing_index = index;
	app->pending_pl = NULL;
	app->pending_index = -1;
	/* Keep the shuffle order across Next and manual picks (a new one would
	 * repeat tracks); it is rebuilt when the playlist changes. */
	if (app->order == ORDER_SHUFFLE_TRACKS && (!pl->shuffle || pl->shuffle->len != pl->tracks->len))
		shuffle_build(pl, index);
	playlist_row_changed(old_pl, old_index);
	player_play(app->player, g_ptr_array_index(pl->tracks, index));
}

void app_play_tracks(GPtrArray *tracks, const char *playlist_name, gboolean play)
{
	Playlist *pl = playlist_find_or_create(playlist_name);
	playlist_clear(pl);
	playlist_add(pl, tracks, -1);
	playlist_select(pl);
	if (play)
		app_play_track(pl, 0);
}

void app_next(void)
{
	Track *cur = player_current(app->player);
	if (!cur) {
		Playlist *pl = playlist_active();
		int c = playlist_cursor(pl);
		app_play_track(pl, c >= 0 ? c : 0);
		return;
	}
	gboolean stop_after = app->stop_after_current;
	app->stop_after_current = FALSE; /* Next means next */
	Track *t = next_track_cb(cur, NULL);
	app->stop_after_current = stop_after;
	if (!t)
		return;
	if (app->pending_pl && app->pending_index >= 0) {
		app_play_track(app->pending_pl, app->pending_index);
	} else {
		player_play(app->player, t); /* queued from the library */
	}
	track_unref(t);
}

void app_previous(void)
{
	if (player_current(app->player) && player_position(app->player) > 3) {
		player_seek(app->player, 0);
		return;
	}
	/* The history's last entry is the playing track; the one before it is previous. */
	while (app->history->len >= 2) {
		HistoryItem h = g_array_index(app->history, HistoryItem, app->history->len - 2);
		g_array_set_size(app->history, app->history->len - 2);
		Playlist *pl = playlist_by_id(h.playlist);
		if (pl && h.index < (int)pl->tracks->len) {
			app_play_track(pl, h.index);
			return;
		}
	}
	if (app->playing_pl && app->playing_index > 0)
		app_play_track(app->playing_pl, app->playing_index - 1);
}

void app_requeue(void)
{
	if (player_current(app->player)) {
		app->pending_pl = NULL;
		app->pending_index = -1;
		player_requeue(app->player);
	}
}

void app_enqueue(Playlist *pl, Track *t)
{
	QueueItem *q = g_new0(QueueItem, 1);
	q->pl = pl;
	q->track = track_ref(t);
	g_queue_push_tail(app->queue, q);
	playlist_redraw_all();
	if (app->queue->length == 1)
		app_requeue();
}

int app_queue_position(Playlist *pl, Track *t)
{
	int i = 1;
	for (GList *l = app->queue->head; l; l = l->next, i++) {
		QueueItem *q = l->data;
		if (q->track == t && (q->pl == pl || !q->pl))
			return i;
	}
	return 0;
}

static const char *const order_names[ORDER_COUNT] = {
	"Default", "Repeat (playlist)", "Repeat (track)", "Shuffle (tracks)", "Shuffle (albums)", "Random",
};

void app_set_order(PlaybackOrder o)
{
	if (o >= ORDER_COUNT)
		o = ORDER_DEFAULT;
	app->order = o;
	if (gtk_combo_box_get_active(GTK_COMBO_BOX(app->order_combo)) != (int)o)
		gtk_combo_box_set_active(GTK_COMBO_BOX(app->order_combo), (int)o);
	if (app->order_items[o] && !gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(app->order_items[o])))
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->order_items[o]), TRUE);
	if (o == ORDER_SHUFFLE_TRACKS && app->playing_pl)
		shuffle_build(app->playing_pl, app->playing_index);
	app_requeue();
}

void app_set_stop_after_current(gboolean on)
{
	app->stop_after_current = on;
	if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(app->stop_after_item)) != on)
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->stop_after_item), on);
	app_requeue();
	app_update_status();
}

void app_apply_audio_settings(void)
{
	player_set_replaygain(app->player, app->rg_mode, app->rg_preamp, app->rg_clip);
	player_set_equalizer(app->player, app->eq_on ? app->eq : NULL);
}

void app_apply_theme(void)
{
	g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", app->dark, NULL);
}

void app_toggle_fullscreen(void)
{
	if (fullscreen)
		gtk_window_unfullscreen(GTK_WINDOW(app->window));
	else
		gtk_window_fullscreen(GTK_WINDOW(app->window));
}

void app_raise(void)
{
	gtk_window_present_with_time(GTK_WINDOW(app->window), GDK_CURRENT_TIME);
}

/* ---- library scanning ------------------------------------------------------ */

void app_scan_progress(const ScanProgress *p, gpointer data)
{
	(void)data;
	g_free(app->scan_status);
	if (p->done) {
		app->scan_status = g_strdup_printf("Library: %d tracks (%d added, %d updated, %d removed in %.1f s)",
						   library_count(app->lib), p->added, p->updated, p->removed, p->seconds);
		if (g_getenv("MUSIC_PLAYER_DEBUG"))
			fprintf(stderr, "music-player: scan: %d files, %d added, %d updated, %d removed, %.3f s\n",
				p->files_seen, p->added, p->updated, p->removed, p->seconds);
		if (app->scan_status_id)
			g_source_remove(app->scan_status_id);
		app->scan_status_id = g_timeout_add_seconds(6, clear_scan_status, NULL);
		libview_refresh();
		playlist_redraw_all();
	} else {
		app->scan_status = g_strdup_printf("Scanning library: %d files, %d added…", p->files_seen, p->added);
	}
	app_update_status();
}

/* ---- menus ------------------------------------------------------------------ */

static void add_files_dialog(gboolean folder)
{
	GtkFileChooserNative *fc = gtk_file_chooser_native_new(folder ? "Add folder" : "Add files",
							       GTK_WINDOW(app->window),
							       folder ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER
								      : GTK_FILE_CHOOSER_ACTION_OPEN,
							       "_Add", "_Cancel");
	gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(fc), TRUE);
	if (!folder) {
		GtkFileFilter *f = gtk_file_filter_new();
		gtk_file_filter_set_name(f, "Audio, cue sheets and playlists");
		static const char *const pats[] = {"*.flac", "*.mp3", "*.opus", "*.ogg", "*.m4a", "*.wv", "*.ape",
						   "*.wav", "*.aiff", "*.dsf", "*.dff", "*.mka", "*.cue", "*.m3u",
						   "*.m3u8", "*.alac", "*.aac", "*.wma", "*.tak", "*.tta", NULL};
		for (int i = 0; pats[i]; i++) {
			gtk_file_filter_add_pattern(f, pats[i]);
			char *up = g_ascii_strup(pats[i], -1);
			gtk_file_filter_add_pattern(f, up);
			g_free(up);
		}
		gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(fc), f);
	}
	if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
		GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(fc));
		GPtrArray *tracks = g_ptr_array_new_with_free_func((GDestroyNotify)track_unref);
		for (GSList *l = files; l; l = l->next) {
			GPtrArray *t = folder ? library_tracks_for_folder(app->lib, l->data)
					      : library_tracks_for_file(app->lib, l->data);
			for (guint i = 0; i < t->len; i++)
				g_ptr_array_add(tracks, track_ref(g_ptr_array_index(t, i)));
			g_ptr_array_unref(t);
		}
		g_slist_free_full(files, g_free);
		playlist_add(playlist_active(), tracks, -1);
		g_ptr_array_unref(tracks);
	}
	g_object_unref(fc);
}

static void m_add_files(void) { add_files_dialog(FALSE); }
static void m_add_folder(void) { add_files_dialog(TRUE); }

static void m_new_playlist(void)
{
	char name[32];
	g_snprintf(name, sizeof name, "New playlist (%u)", app->playlists->len + 1);
	playlist_new(name, TRUE);
}

static void m_close_playlist(void) { playlist_close(playlist_active()); }
static void m_rename_playlist(void) { playlist_rename_dialog(playlist_active()); }

static void m_save_playlist(void)
{
	Playlist *pl = playlist_active();
	GtkFileChooserNative *fc = gtk_file_chooser_native_new("Save playlist", GTK_WINDOW(app->window),
							       GTK_FILE_CHOOSER_ACTION_SAVE, "_Save", "_Cancel");
	gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(fc), TRUE);
	char *name = g_strconcat(pl->name, ".m3u8", NULL);
	gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(fc), name);
	g_free(name);
	if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
		char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
		playlist_export(pl, path);
		g_free(path);
	}
	g_object_unref(fc);
}

static void m_load_playlist(void)
{
	GtkFileChooserNative *fc = gtk_file_chooser_native_new("Load playlist", GTK_WINDOW(app->window),
							       GTK_FILE_CHOOSER_ACTION_OPEN, "_Open", "_Cancel");
	GtkFileFilter *f = gtk_file_filter_new();
	gtk_file_filter_set_name(f, "Playlists and cue sheets");
	gtk_file_filter_add_pattern(f, "*.m3u");
	gtk_file_filter_add_pattern(f, "*.m3u8");
	gtk_file_filter_add_pattern(f, "*.cue");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(fc), f);
	if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
		char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
		char *base = g_path_get_basename(path);
		char *dot = strrchr(base, '.');
		if (dot)
			*dot = 0;
		GPtrArray *tracks = library_tracks_for_file(app->lib, path);
		Playlist *pl = playlist_new(base, TRUE);
		playlist_add(pl, tracks, -1);
		g_ptr_array_unref(tracks);
		g_free(base);
		g_free(path);
	}
	g_object_unref(fc);
}

static void m_quit(void) { gtk_window_close(GTK_WINDOW(app->window)); }

static void m_select_all(void)
{
	gtk_tree_selection_select_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(playlist_active()->view)));
}

static void m_remove(void) { playlist_remove_selected(playlist_active()); }
static void m_clear(void) { playlist_clear(playlist_active()); }

static void sort_by(const char *fmt)
{
	playlist_sort_by_format(playlist_active(), fmt, FALSE);
}

static void m_sort_album(void)
{
	sort_by("%album artist%|%date%|%album%|$num(%discnumber%,3)|$num(%tracknumber%,4)|%path%");
}
static void m_sort_title(void) { sort_by("%title%"); }
static void m_sort_path(void) { sort_by("%path%|$num(%tracknumber%,4)"); }

static void m_randomize(void)
{
	Playlist *pl = playlist_active();
	for (guint i = pl->tracks->len; i > 1; i--) {
		guint j = g_random_int_range(0, (gint32)i);
		gpointer tmp = pl->tracks->pdata[i - 1];
		pl->tracks->pdata[i - 1] = pl->tracks->pdata[j];
		pl->tracks->pdata[j] = tmp;
	}
	playlist_changed(pl, TRUE);
}

static void m_play_pause(void)
{
	if (player_state(app->player) == PLAYER_STOPPED)
		app_next();
	else
		player_toggle(app->player);
}

static void m_stop(void) { player_stop(app->player); }

static void m_stop_after(GtkCheckMenuItem *i)
{
	gboolean on = gtk_check_menu_item_get_active(i);
	if (on != app->stop_after_current)
		app_set_stop_after_current(on);
}

static void m_order(GtkCheckMenuItem *i, gpointer data)
{
	if (gtk_check_menu_item_get_active(i) && app->order != (PlaybackOrder)GPOINTER_TO_INT(data))
		app_set_order(GPOINTER_TO_INT(data));
}

static void m_rg(GtkCheckMenuItem *i, gpointer data)
{
	if (!gtk_check_menu_item_get_active(i))
		return;
	app->rg_mode = GPOINTER_TO_INT(data);
	app_apply_audio_settings();
}

static void m_show_playing(void)
{
	if (app->playing_pl) {
		playlist_select(app->playing_pl);
		playlist_scroll_to(app->playing_pl, app->playing_index, TRUE);
	}
}

static void m_rescan(void) { library_scan(app->lib, app_scan_progress, NULL); }

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback cb, const char *accel)
{
	GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
	if (cb)
		g_signal_connect_swapped(item, "activate", cb, NULL);
	if (accel) {
		guint key;
		GdkModifierType mods;
		gtk_accelerator_parse(accel, &key, &mods);
		gtk_widget_add_accelerator(item, "activate", app->accels, key, mods, GTK_ACCEL_VISIBLE);
	}
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	return item;
}

static GtkWidget *submenu(GtkWidget *bar, const char *label)
{
	GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
	GtkWidget *menu = gtk_menu_new();
	gtk_menu_set_accel_group(GTK_MENU(menu), app->accels);
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu);
	gtk_menu_shell_append(GTK_MENU_SHELL(bar), item);
	return menu;
}

static void separator(GtkWidget *menu)
{
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

static GtkWidget *build_menubar(void)
{
	GtkWidget *bar = gtk_menu_bar_new();
	GtkWidget *m = submenu(bar, "_File");
	menu_item(m, "_Add files…", G_CALLBACK(m_add_files), "<Control>o");
	menu_item(m, "Add _folder…", G_CALLBACK(m_add_folder), "<Control><Shift>o");
	separator(m);
	menu_item(m, "_New playlist", G_CALLBACK(m_new_playlist), "<Control>n");
	menu_item(m, "_Load playlist…", G_CALLBACK(m_load_playlist), "<Control>l");
	menu_item(m, "_Save playlist…", G_CALLBACK(m_save_playlist), "<Control>s");
	menu_item(m, "_Rename playlist…", G_CALLBACK(m_rename_playlist), "F2");
	menu_item(m, "_Close playlist", G_CALLBACK(m_close_playlist), "<Control>w");
	separator(m);
	menu_item(m, "_Quit", G_CALLBACK(m_quit), "<Control>q");

	m = submenu(bar, "_Edit");
	menu_item(m, "Select _all", G_CALLBACK(m_select_all), "<Control>a");
	menu_item(m, "_Remove", G_CALLBACK(m_remove), NULL);
	menu_item(m, "_Clear playlist", G_CALLBACK(m_clear), NULL);
	separator(m);
	menu_item(m, "Sort by _album", G_CALLBACK(m_sort_album), NULL);
	menu_item(m, "Sort by _title", G_CALLBACK(m_sort_title), NULL);
	menu_item(m, "Sort by _file path", G_CALLBACK(m_sort_path), NULL);
	menu_item(m, "Ran_domize", G_CALLBACK(m_randomize), NULL);

	m = submenu(bar, "_Playback");
	menu_item(m, "_Play / pause", G_CALLBACK(m_play_pause), NULL);
	menu_item(m, "_Stop", G_CALLBACK(m_stop), "<Control>period");
	menu_item(m, "_Next", G_CALLBACK(app_next), "<Control>Right");
	menu_item(m, "Pre_vious", G_CALLBACK(app_previous), "<Control>Left");
	app->stop_after_item = gtk_check_menu_item_new_with_mnemonic("Stop _after current");
	g_signal_connect(app->stop_after_item, "toggled", G_CALLBACK(m_stop_after), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(m), app->stop_after_item);
	separator(m);
	GtkWidget *om = gtk_menu_new();
	GtkWidget *oi = gtk_menu_item_new_with_mnemonic("_Order");
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(oi), om);
	gtk_menu_shell_append(GTK_MENU_SHELL(m), oi);
	GSList *group = NULL;
	for (int i = 0; i < ORDER_COUNT; i++) {
		GtkWidget *it = gtk_radio_menu_item_new_with_label(group, order_names[i]);
		group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(it));
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it), i == (int)app->order);
		g_signal_connect(it, "toggled", G_CALLBACK(m_order), GINT_TO_POINTER(i));
		gtk_menu_shell_append(GTK_MENU_SHELL(om), it);
		app->order_items[i] = it;
	}
	GtkWidget *rm = gtk_menu_new();
	GtkWidget *ri = gtk_menu_item_new_with_mnemonic("_ReplayGain");
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(ri), rm);
	gtk_menu_shell_append(GTK_MENU_SHELL(m), ri);
	group = NULL;
	static const char *const rg_names[] = {"Off", "Track gain", "Album gain"};
	for (int i = 0; i < 3; i++) {
		GtkWidget *it = gtk_radio_menu_item_new_with_label(group, rg_names[i]);
		group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(it));
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it), i == app->rg_mode);
		g_signal_connect(it, "toggled", G_CALLBACK(m_rg), GINT_TO_POINTER(i));
		gtk_menu_shell_append(GTK_MENU_SHELL(rm), it);
	}
	menu_item(m, "_Equalizer…", G_CALLBACK(dialog_equalizer), "<Control>e");
	separator(m);
	menu_item(m, "Show _playing track", G_CALLBACK(m_show_playing), "<Control>j");

	m = submenu(bar, "_View");
	menu_item(m, "_Fullscreen", G_CALLBACK(app_toggle_fullscreen), "F11");
	menu_item(m, "Playlist _columns…", G_CALLBACK(dialog_columns), NULL);

	m = submenu(bar, "_Library");
	menu_item(m, "_Search", G_CALLBACK(libview_focus_search), "<Control>f");
	menu_item(m, "_Rescan folders", G_CALLBACK(m_rescan), NULL);
	separator(m);
	menu_item(m, "_Preferences…", G_CALLBACK(dialog_preferences), "<Control>p");

	m = submenu(bar, "_Help");
	menu_item(m, "_About", G_CALLBACK(dialog_about), NULL);
	return bar;
}

/* ---- controls --------------------------------------------------------------- */

static GtkWidget *icon_button(IconKind icon, const char *tip, GCallback cb)
{
	GtkWidget *b = gtk_button_new();
	gtk_container_add(GTK_CONTAINER(b), icon_new(icon, 16));
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(b, tip);
	gtk_widget_set_can_focus(b, FALSE);
	g_signal_connect_swapped(b, "clicked", cb, NULL);
	return b;
}

static gboolean seek_pressed(GtkWidget *w, GdkEvent *e, gpointer data)
{
	(void)w;
	(void)e;
	(void)data;
	app->seek_dragging = TRUE;
	return FALSE;
}

static gboolean seek_released(GtkWidget *w, GdkEvent *e, gpointer data)
{
	(void)e;
	(void)data;
	app->seek_dragging = FALSE;
	player_seek(app->player, gtk_range_get_value(GTK_RANGE(w)));
	schedule_tick();
	return FALSE;
}

static gboolean seek_change(GtkRange *r, GtkScrollType scroll, double value, gpointer data)
{
	(void)r;
	(void)scroll;
	(void)data;
	if (app->seek_dragging) {
		char pos[32], len[32], label[80];
		Track *t = player_current(app->player);
		format_duration(value, pos, sizeof pos);
		format_duration(t ? t->duration : 0, len, sizeof len);
		g_snprintf(label, sizeof label, "%s / %s", pos, len);
		gtk_label_set_text(GTK_LABEL(app->time_label), label);
	} else {
		player_seek(app->player, value); /* keyboard or wheel */
	}
	return FALSE;
}

static void volume_changed(GtkRange *r, gpointer data)
{
	(void)data;
	player_set_volume(app->player, gtk_range_get_value(r));
}

static void order_changed(GtkComboBox *c, gpointer data)
{
	(void)data;
	int o = gtk_combo_box_get_active(c);
	if (o >= 0 && o != (int)app->order)
		app_set_order(o);
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
	(void)data;
	GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(w));
	gboolean typing = focus && GTK_IS_EDITABLE(focus);
	guint mods = ev->state & gtk_accelerator_get_default_mod_mask();
	switch (ev->keyval) {
	case GDK_KEY_AudioPlay:
	case GDK_KEY_AudioPause:
		m_play_pause();
		return TRUE;
	case GDK_KEY_AudioStop:
		m_stop();
		return TRUE;
	case GDK_KEY_AudioNext:
		app_next();
		return TRUE;
	case GDK_KEY_AudioPrev:
		app_previous();
		return TRUE;
	case GDK_KEY_space:
		if (typing || mods)
			return FALSE;
		m_play_pause();
		return TRUE;
	case GDK_KEY_Left:
	case GDK_KEY_Right:
		if (typing || mods || !player_current(app->player) || (focus && focus == app->lib_tree))
			return FALSE;
		player_seek(app->player, MAX(0, player_position(app->player) + (ev->keyval == GDK_KEY_Left ? -5 : 5)));
		return TRUE;
	}
	return FALSE;
}

/* ---- config ------------------------------------------------------------------- */

static void load_config(void)
{
	app->cfg = g_key_file_new();
	g_key_file_load_from_file(app->cfg, app->cfg_path, G_KEY_FILE_KEEP_COMMENTS, NULL);
	GError *e = NULL;
	app->order = g_key_file_get_integer(app->cfg, "playback", "order", NULL);
	app->rg_mode = g_key_file_get_integer(app->cfg, "playback", "replaygain", &e);
	if (e) {
		app->rg_mode = 2;
		g_clear_error(&e);
	}
	app->rg_preamp = g_key_file_get_double(app->cfg, "playback", "replaygain_preamp", NULL);
	app->rg_clip = g_key_file_get_boolean(app->cfg, "playback", "replaygain_prevent_clipping", &e);
	if (e) {
		app->rg_clip = TRUE;
		g_clear_error(&e);
	}
	gsize n = 0;
	double *eq = g_key_file_get_double_list(app->cfg, "playback", "equalizer", &n, NULL);
	for (gsize i = 0; i < n && i < 10; i++)
		app->eq[i] = eq[i];
	g_free(eq);
	app->eq_on = g_key_file_get_boolean(app->cfg, "playback", "equalizer_on", NULL);
	app->dark = g_key_file_get_boolean(app->cfg, "appearance", "dark", &e);
	if (e) {
		app->dark = TRUE;
		g_clear_error(&e);
	}
	app->bitperfect = g_key_file_get_boolean(app->cfg, "playback", "follow_samplerate", NULL);
}

void app_save_config(void)
{
	GKeyFile *k = app->cfg;
	int w, h;
	gtk_window_get_size(GTK_WINDOW(app->window), &w, &h);
	g_key_file_set_integer(k, "window", "width", w);
	g_key_file_set_integer(k, "window", "height", h);
	g_key_file_set_boolean(k, "window", "maximized", gtk_window_is_maximized(GTK_WINDOW(app->window)));
	g_key_file_set_integer(k, "window", "library_width", gtk_paned_get_position(GTK_PANED(app->hpaned)));
	g_key_file_set_integer(k, "window", "library_height", gtk_paned_get_position(GTK_PANED(app->left_paned)));
	g_key_file_set_integer(k, "window", "cover_height", gtk_paned_get_position(GTK_PANED(app->info_paned)));
	g_key_file_set_integer(k, "playback", "order", app->order);
	g_key_file_set_integer(k, "playback", "replaygain", app->rg_mode);
	g_key_file_set_double(k, "playback", "replaygain_preamp", app->rg_preamp);
	g_key_file_set_boolean(k, "playback", "replaygain_prevent_clipping", app->rg_clip);
	g_key_file_set_double_list(k, "playback", "equalizer", app->eq, 10);
	g_key_file_set_boolean(k, "playback", "equalizer_on", app->eq_on);
	g_key_file_set_boolean(k, "playback", "follow_samplerate", app->bitperfect);
	g_key_file_set_double(k, "playback", "volume", gtk_range_get_value(GTK_RANGE(app->volume)));
	g_key_file_set_boolean(k, "appearance", "dark", app->dark);
	char *dir = g_path_get_dirname(app->cfg_path);
	g_mkdir_with_parents(dir, 0755);
	g_free(dir);
	g_key_file_save_to_file(k, app->cfg_path, NULL);
}

static int cfg_int(const char *group, const char *key, int def)
{
	GError *e = NULL;
	int v = g_key_file_get_integer(app->cfg, group, key, &e);
	if (e) {
		g_error_free(e);
		return def;
	}
	return v;
}

static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer data)
{
	(void)w;
	(void)e;
	(void)data;
	app_save_config();
	for (guint i = 0; i < app->playlists->len; i++) {
		Playlist *pl = g_ptr_array_index(app->playlists, i);
		if (pl->save_id) {
			g_source_remove(pl->save_id);
			pl->save_id = 0;
			library_playlist_save(app->lib, pl->id, pl->tracks);
		}
	}
	gtk_main_quit();
	return FALSE;
}

/* ---- the window ---------------------------------------------------------------- */

static const char css[] =
	"treeview.view { padding-top: 0; padding-bottom: 0; }"
	"notebook tab { padding: 2px 10px; min-height: 0; }"
	"#statusbar { padding: 3px 8px; }"
	"#controls { padding: 2px 4px; }"
	"#controls button { padding: 4px; min-height: 0; min-width: 0; }"
	"scale { padding-top: 0; padding-bottom: 0; }";

static void build_window(void)
{
	app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(app->window), "music-player");
	gtk_window_set_default_size(GTK_WINDOW(app->window), cfg_int("window", "width", 1400),
				    cfg_int("window", "height", 860));
	if (g_key_file_get_boolean(app->cfg, "window", "maximized", NULL))
		gtk_window_maximize(GTK_WINDOW(app->window));
	app->accels = gtk_accel_group_new();
	gtk_window_add_accel_group(GTK_WINDOW(app->window), app->accels);
	gtk_widget_add_events(app->window, GDK_VISIBILITY_NOTIFY_MASK);
	g_signal_connect(app->window, "delete-event", G_CALLBACK(on_delete), NULL);
	g_signal_connect(app->window, "visibility-notify-event", G_CALLBACK(on_visibility), NULL);
	g_signal_connect(app->window, "window-state-event", G_CALLBACK(on_window_state), NULL);
	g_signal_connect(app->window, "unmap-event", G_CALLBACK(on_unmap), NULL);
	g_signal_connect(app->window, "key-press-event", G_CALLBACK(on_key), NULL);

	GtkCssProvider *cp = gtk_css_provider_new();
	gtk_css_provider_load_from_data(cp, css, -1, NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(cp),
						  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(app->window), vbox);
	gtk_box_pack_start(GTK_BOX(vbox), build_menubar(), FALSE, FALSE, 0);

	/* Controls: transport, seek bar, time, order, volume. */
	GtkWidget *ctl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_widget_set_name(ctl, "controls");
	gtk_box_pack_start(GTK_BOX(ctl), icon_button(ICON_PREV, "Previous",
						     G_CALLBACK(app_previous)), FALSE, FALSE, 0);
	app->play_button = icon_button(ICON_PLAY, "Play", G_CALLBACK(m_play_pause));
	gtk_box_pack_start(GTK_BOX(ctl), app->play_button, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(ctl), icon_button(ICON_STOP, "Stop", G_CALLBACK(m_stop)),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(ctl), icon_button(ICON_NEXT, "Next", G_CALLBACK(app_next)),
			   FALSE, FALSE, 0);
	app->seek = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 1);
	gtk_scale_set_draw_value(GTK_SCALE(app->seek), FALSE);
	gtk_widget_set_can_focus(app->seek, FALSE);
	gtk_widget_set_sensitive(app->seek, FALSE);
	g_signal_connect(app->seek, "button-press-event", G_CALLBACK(seek_pressed), NULL);
	g_signal_connect(app->seek, "button-release-event", G_CALLBACK(seek_released), NULL);
	g_signal_connect(app->seek, "change-value", G_CALLBACK(seek_change), NULL);
	gtk_box_pack_start(GTK_BOX(ctl), app->seek, TRUE, TRUE, 8);
	app->time_label = gtk_label_new("");
	gtk_label_set_width_chars(GTK_LABEL(app->time_label), 15);
	gtk_style_context_add_class(gtk_widget_get_style_context(app->time_label), "numeric");
	gtk_box_pack_start(GTK_BOX(ctl), app->time_label, FALSE, FALSE, 4);
	app->order_combo = gtk_combo_box_text_new();
	for (int i = 0; i < ORDER_COUNT; i++)
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->order_combo), order_names[i]);
	gtk_combo_box_set_active(GTK_COMBO_BOX(app->order_combo), (int)app->order);
	gtk_widget_set_can_focus(app->order_combo, FALSE);
	g_signal_connect(app->order_combo, "changed", G_CALLBACK(order_changed), NULL);
	gtk_box_pack_start(GTK_BOX(ctl), app->order_combo, FALSE, FALSE, 4);
	gtk_box_pack_start(GTK_BOX(ctl), icon_new(ICON_VOLUME, 16), FALSE, FALSE, 4);
	app->volume = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
	gtk_scale_set_draw_value(GTK_SCALE(app->volume), FALSE);
	gtk_widget_set_size_request(app->volume, 120, -1);
	gtk_widget_set_can_focus(app->volume, FALSE);
	GError *e = NULL;
	double vol = g_key_file_get_double(app->cfg, "playback", "volume", &e);
	if (e) {
		vol = 100;
		g_error_free(e);
	}
	gtk_range_set_value(GTK_RANGE(app->volume), vol);
	g_signal_connect(app->volume, "value-changed", G_CALLBACK(volume_changed), NULL);
	gtk_box_pack_start(GTK_BOX(ctl), app->volume, FALSE, FALSE, 4);
	gtk_box_pack_start(GTK_BOX(vbox), ctl, FALSE, FALSE, 0);

	/* Library | (cover / properties) on the left, playlists on the right. */
	app->hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	app->left_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
	app->info_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
	gtk_paned_pack1(GTK_PANED(app->left_paned), libview_new(), TRUE, FALSE);
	app->cover = gtk_image_new();
	gtk_widget_set_size_request(app->cover, 120, 120);
	g_signal_connect(app->cover, "size-allocate", G_CALLBACK(on_cover_resized), NULL);
	GtkWidget *frame = gtk_frame_new(NULL);
	gtk_container_add(GTK_CONTAINER(frame), app->cover);
	gtk_paned_pack1(GTK_PANED(app->info_paned), frame, TRUE, FALSE);
	GtkListStore *ps = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
	app->props = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ps));
	g_object_unref(ps);
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(app->props), FALSE);
	GtkCellRenderer *r = gtk_cell_renderer_text_new();
	g_object_set(r, "foreground-rgba", &(GdkRGBA){0.6, 0.6, 0.6, 1}, "ypad", 0, NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(app->props),
				    gtk_tree_view_column_new_with_attributes("", r, "text", 0, NULL));
	r = gtk_cell_renderer_text_new();
	g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, "ypad", 0, NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(app->props),
				    gtk_tree_view_column_new_with_attributes("", r, "text", 1, NULL));
	GtkWidget *psw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(psw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(psw), app->props);
	gtk_paned_pack2(GTK_PANED(app->info_paned), psw, TRUE, FALSE);
	gtk_paned_pack2(GTK_PANED(app->left_paned), app->info_paned, TRUE, FALSE);
	gtk_paned_pack1(GTK_PANED(app->hpaned), app->left_paned, FALSE, FALSE);

	app->notebook = gtk_notebook_new();
	gtk_notebook_set_scrollable(GTK_NOTEBOOK(app->notebook), TRUE);
	gtk_notebook_set_show_border(GTK_NOTEBOOK(app->notebook), FALSE);
	gtk_paned_pack2(GTK_PANED(app->hpaned), app->notebook, TRUE, FALSE);
	gtk_box_pack_start(GTK_BOX(vbox), app->hpaned, TRUE, TRUE, 0);
	gtk_paned_set_position(GTK_PANED(app->hpaned), cfg_int("window", "library_width", 380));
	gtk_paned_set_position(GTK_PANED(app->left_paned), cfg_int("window", "library_height", 420));
	gtk_paned_set_position(GTK_PANED(app->info_paned), cfg_int("window", "cover_height", 340));

	app->status = gtk_label_new("");
	gtk_widget_set_name(app->status, "statusbar");
	gtk_label_set_xalign(GTK_LABEL(app->status), 0);
	gtk_label_set_ellipsize(GTK_LABEL(app->status), PANGO_ELLIPSIZE_END);
	gtk_box_pack_start(GTK_BOX(vbox), app->status, FALSE, FALSE, 0);
}

/* GTK scales only fonts by Xft.dpi; at 192 dpi widgets need GDK_SCALE=2 with
 * the font scale halved back (the same fix as for Thorium here). */
static void hidpi_env(void)
{
	if (g_getenv("GDK_SCALE"))
		return;
	Display *d = XOpenDisplay(NULL);
	if (!d)
		return;
	const char *dpi = XGetDefault(d, "Xft", "dpi");
	int v = dpi ? atoi(dpi) : 96;
	if (v >= 168) {
		char scale[8], dpi_scale[16];
		int s = (v + 48) / 96;
		g_snprintf(scale, sizeof scale, "%d", s);
		g_ascii_formatd(dpi_scale, sizeof dpi_scale, "%.4f", 1.0 / s);
		g_setenv("GDK_SCALE", scale, TRUE);
		g_setenv("GDK_DPI_SCALE", dpi_scale, TRUE);
	}
	XCloseDisplay(d);
}

int main(int argc, char **argv)
{
	setlocale(LC_ALL, "");
	if (argc >= 3 && !strcmp(argv[1], "--add-folder")) {
		/* Add library folders from the command line; a running instance rescans. */
		char *dbp = g_build_filename(g_get_user_data_dir(), "music-player", "library.db", NULL);
		Library *lib = library_open(dbp);
		for (int i = 2; i < argc; i++) {
			char *abs = g_canonicalize_filename(argv[i], NULL);
			library_add_folder(lib, abs);
			printf("library folder: %s\n", abs);
			g_free(abs);
		}
		library_close(lib);
		g_free(dbp);
		ctl_send("rescan", NULL);
		return 0;
	}
	/* One instance: hand the files to the running one. */
	char *reply = NULL;
	if (ctl_send("show", &reply)) {
		for (int i = 1; i < argc; i++) {
			char *abs = g_canonicalize_filename(argv[i], NULL);
			char *cmd = g_strdup_printf("%s %s", i == 1 ? "open" : "add", abs);
			ctl_send(cmd, NULL);
			g_free(cmd);
			g_free(abs);
		}
		g_free(reply);
		return 0;
	}
	hidpi_env();
	g_set_prgname("music-player");
	gtk_init(&argc, &argv);
	gdk_set_program_class("music-player"); /* WM_CLASS (gtk_init capitalizes it), for dwm rules */
	setlocale(LC_NUMERIC, "C"); /* libmpv requires it; gtk_init set the user's locale */

	app = g_new0(App, 1);
	app->cfg_path = g_build_filename(g_get_user_config_dir(), "music-player", "config.ini", NULL);
	app->data_dir = g_build_filename(g_get_user_data_dir(), "music-player", NULL);
	app->playing_index = app->pending_index = -1;
	app->queue = g_queue_new();
	app->history = g_array_new(FALSE, FALSE, sizeof(HistoryItem));
	load_config();
	app_apply_theme();

	char *db = g_build_filename(app->data_dir, "library.db", NULL);
	app->lib = library_open(db);
	g_free(db);
	PlayerCallbacks cb = {
		.track_changed = track_changed_cb,
		.state_changed = state_changed_cb,
		.next_track = next_track_cb,
		.error = error_cb,
	};
	app->player = player_new(&cb, NULL);
	app_apply_audio_settings();

	columns_load();
	build_window();
	player_set_volume(app->player, gtk_range_get_value(GTK_RANGE(app->volume)));
	playlists_init();
	libview_refresh();
	set_play_icon();
	app_update_status();
	gtk_widget_show_all(app->window);
	app->visible = TRUE;
	ctl_listen();

	for (int i = 1; i < argc; i++) {
		char *abs = g_canonicalize_filename(argv[i], NULL);
		GPtrArray *t = g_file_test(abs, G_FILE_TEST_IS_DIR) ? library_tracks_for_folder(app->lib, abs)
								      : library_tracks_for_file(app->lib, abs);
		if (i == 1)
			playlist_clear(playlist_active());
		playlist_add(playlist_active(), t, -1);
		g_ptr_array_unref(t);
		g_free(abs);
	}
	if (argc > 1)
		app_play_track(playlist_active(), 0);

	/* A quick check of the library folders for new or changed files. */
	char **folders = library_folders(app->lib);
	if (folders[0])
		library_scan(app->lib, app_scan_progress, NULL);
	g_strfreev(folders);

	gtk_main();

	ctl_stop();
	player_free(app->player);
	library_close(app->lib);
	return 0;
}

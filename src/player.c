#include "player.h"

#include <glib-unix.h>
#include <math.h>
#include <mpv/client.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>

const int player_eq_bands[10] = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};

enum { OBS_PAUSE = 1, OBS_IDLE, OBS_AO, OBS_AID };

struct Player {
	mpv_handle *mpv;
	int efd;
	guint watch;
	PlayerCallbacks cb;
	gpointer data;

	Track *current;
	GHashTable *entries; /* mpv playlist entry id -> Track* (referenced) */
	PlayerState state;
	gboolean paused, idle;
	gboolean loading; /* a file was asked for and hasn't started: idle reports are stale */

	GHashTable *pending; /* options set before mpv started: name -> value */

	gboolean audio_lost;
	int audio_tries;
	guint audio_timer;
	char aid[32];
};

/* ---- audio on demand ---------------------------------------------------
 * PipeWire on this laptop only runs after F2 or `audio ensure`; mpv here has
 * no other output. Start it before playing, like the other apps. */

static gboolean pipewire_running(void)
{
	const char *rt = g_getenv("XDG_RUNTIME_DIR");
	char *sock = g_build_filename(rt ? rt : "/tmp", "pipewire-0", NULL);
	gboolean ok = g_file_test(sock, G_FILE_TEST_EXISTS);
	g_free(sock);
	return ok;
}

static void ensure_audio(void)
{
	if (pipewire_running() || access("/usr/local/bin/audio", X_OK) != 0 || g_getenv("MUSIC_PLAYER_TEST"))
		return;
	char *argv[] = {"/usr/local/bin/audio", "ensure", NULL};
	g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
		     NULL, NULL, NULL, NULL, NULL, NULL);
	/* The status bar shows the audio state. */
	char *bar[] = {"pkill", "-USR1", "-x", "dwmstat", NULL};
	g_spawn_async(NULL, bar, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
		      G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
}

/* ---- the mpv playlist ---------------------------------------------------- */

static gboolean loadfile(Player *p, Track *t, const char *mode)
{
	char opts[128] = "";
	if (t->cue_start > 0 || t->cue_end > 0) {
		if (t->cue_end > 0)
			g_snprintf(opts, sizeof opts, "start=%.6f,end=%.6f", t->cue_start, t->cue_end);
		else
			g_snprintf(opts, sizeof opts, "start=%.6f", t->cue_start);
	}
	const char *cmd[] = {"loadfile", t->path, mode, "-1", opts, NULL};
	mpv_node res;
	if (mpv_command_ret(p->mpv, cmd, &res) < 0)
		return FALSE;
	if (res.format == MPV_FORMAT_NODE_MAP) {
		for (int i = 0; i < res.u.list->num; i++)
			if (!strcmp(res.u.list->keys[i], "playlist_entry_id") &&
			    res.u.list->values[i].format == MPV_FORMAT_INT64) {
				gint64 *id = g_new(gint64, 1);
				*id = res.u.list->values[i].u.int64;
				g_hash_table_insert(p->entries, id, track_ref(t));
			}
	}
	mpv_free_node_contents(&res);
	return TRUE;
}

static void forget_entries_except(Player *p, Track *keep)
{
	GHashTableIter it;
	gpointer k, v;
	g_hash_table_iter_init(&it, p->entries);
	while (g_hash_table_iter_next(&it, &k, &v))
		if (v != keep)
			g_hash_table_iter_remove(&it);
}

/* Drops everything after the current track and appends the next one. */
static void queue_next(Player *p)
{
	const char *clear[] = {"playlist-clear", NULL}; /* keeps the playing entry */
	mpv_command(p->mpv, clear);
	forget_entries_except(p, p->current);
	if (!p->current || !p->cb.next_track)
		return;
	Track *next = p->cb.next_track(p->current, p->data);
	if (next) {
		loadfile(p, next, "append");
		track_unref(next);
	}
}

static void set_state(Player *p)
{
	PlayerState s = p->idle || !p->current ? PLAYER_STOPPED : p->paused ? PLAYER_PAUSED : PLAYER_PLAYING;
	if (s == p->state)
		return;
	p->state = s;
	if (p->cb.state_changed)
		p->cb.state_changed(s, p->data);
}

static gboolean audio_retry(gpointer data)
{
	Player *p = data;
	if (!p->audio_lost || p->audio_tries >= 10) {
		p->audio_timer = 0;
		return G_SOURCE_REMOVE;
	}
	/* Wait for PipeWire to come back (F2 turned it off on purpose), then
	 * select the track again: after a failed open mpv drops the audio chain. */
	if (pipewire_running() && p->state == PLAYER_PLAYING) {
		p->audio_tries++;
		mpv_set_property_string(p->mpv, "aid", "no");
		mpv_set_property_string(p->mpv, "aid", p->aid[0] ? p->aid : "auto");
	}
	return G_SOURCE_CONTINUE;
}

static void handle_event(Player *p, mpv_event *e)
{
	switch (e->event_id) {
	case MPV_EVENT_START_FILE: {
		mpv_event_start_file *sf = e->data;
		p->loading = FALSE;
		p->idle = FALSE;
		gint64 id = sf->playlist_entry_id;
		Track *t = g_hash_table_lookup(p->entries, &id);
		if (t && t != p->current) {
			track_unref(p->current);
			p->current = track_ref(t);
			if (p->cb.track_changed)
				p->cb.track_changed(t, p->data);
		}
		queue_next(p);
		set_state(p);
		break;
	}
	case MPV_EVENT_END_FILE: {
		mpv_event_end_file *ef = e->data;
		if (ef->reason == MPV_END_FILE_REASON_ERROR && p->cb.error && p->current)
			p->cb.error(p->current, mpv_error_string(ef->error), p->data);
		break;
	}
	case MPV_EVENT_PROPERTY_CHANGE: {
		mpv_event_property *pr = e->data;
		if (e->reply_userdata == OBS_PAUSE && pr->format == MPV_FORMAT_FLAG)
			p->paused = *(int *)pr->data;
		else if (e->reply_userdata == OBS_IDLE && pr->format == MPV_FORMAT_FLAG) {
			gboolean idle = *(int *)pr->data;
			if (idle && p->loading)
				break; /* from before the file we just asked for */
			p->idle = idle;
			if (idle && p->current) {
				/* The playlist ran out (or stop): playback ended. */
				track_unref(p->current);
				p->current = NULL;
				g_hash_table_remove_all(p->entries);
				if (p->cb.track_changed)
					p->cb.track_changed(NULL, p->data);
			}
		}
		else if (e->reply_userdata == OBS_AID && pr->format == MPV_FORMAT_INT64)
			g_snprintf(p->aid, sizeof p->aid, "%" G_GINT64_FORMAT, *(gint64 *)pr->data);
		else if (e->reply_userdata == OBS_AO) {
			gboolean none = pr->format != MPV_FORMAT_STRING;
			if (none && !p->audio_lost && p->current) {
				p->audio_lost = TRUE;
				p->audio_tries = 0;
				if (!p->audio_timer)
					p->audio_timer = g_timeout_add_seconds(2, audio_retry, p);
			} else if (!none) {
				p->audio_lost = FALSE;
			}
		}
		set_state(p);
		break;
	}
	default:
		break;
	}
}

static gboolean on_mpv(gint fd, GIOCondition cond, gpointer data)
{
	(void)cond;
	Player *p = data;
	uint64_t n;
	if (read(fd, &n, sizeof n) < 0) {
		/* nothing: the counter was already drained */
	}
	for (;;) {
		mpv_event *e = mpv_wait_event(p->mpv, 0);
		if (e->event_id == MPV_EVENT_NONE)
			break;
		handle_event(p, e);
	}
	return G_SOURCE_CONTINUE;
}

static void wakeup(void *data)
{
	Player *p = data;
	uint64_t one = 1;
	if (write(p->efd, &one, sizeof one) < 0) {
		/* the main loop is already being woken */
	}
}

/* mpv starts on the first play: an open window that isn't playing runs no
 * mpv threads and holds none of its memory. */
static gboolean ensure_mpv(Player *p)
{
	if (p->mpv)
		return TRUE;
	p->mpv = mpv_create();
	if (!p->mpv)
		return FALSE;
	mpv_handle *m = p->mpv;
	static const char *const opts[][2] = {
		{"config", "no"},
		{"terminal", "no"},
		{"input-default-bindings", "no"},
		{"vid", "no"},
		{"audio-display", "no"},
		{"idle", "yes"},
		{"gapless-audio", "yes"},
		{"prefetch-playlist", "yes"},
		/* A second of audio buffered: the processor and PipeWire wake up less often. */
		{"audio-buffer", "1.0"},
		/* ...and a 200 ms PipeWire buffer: twice the period, half the wakeups
		 * (PipeWire's max-quantum of 2048 caps it there). */
		{"pipewire-buffer", "200"},
		{"volume-max", "100"},
		{"replaygain", "album"},
		{"replaygain-clip", "yes"},
		{"audio-client-name", "music-player"},
		{"ytdl", "no"},
		{"load-scripts", "no"},
		{"resume-playback", "no"},
		{"save-position-on-quit", "no"},
	};
	for (size_t i = 0; i < G_N_ELEMENTS(opts); i++)
		mpv_set_option_string(m, opts[i][0], opts[i][1]);
	/* Tests and screenshots: MUSIC_PLAYER_AO=null plays without sound. */
	if (g_getenv("MUSIC_PLAYER_AO"))
		mpv_set_option_string(m, "ao", g_getenv("MUSIC_PLAYER_AO"));
	GHashTableIter it;
	gpointer k, v;
	g_hash_table_iter_init(&it, p->pending);
	while (g_hash_table_iter_next(&it, &k, &v))
		mpv_set_option_string(m, k, v);
	g_hash_table_remove_all(p->pending);
	mpv_initialize(m);
	mpv_observe_property(m, OBS_PAUSE, "pause", MPV_FORMAT_FLAG);
	mpv_observe_property(m, OBS_IDLE, "idle-active", MPV_FORMAT_FLAG);
	mpv_observe_property(m, OBS_AO, "current-ao", MPV_FORMAT_STRING);
	mpv_observe_property(m, OBS_AID, "aid", MPV_FORMAT_INT64);
	p->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	p->watch = g_unix_fd_add(p->efd, G_IO_IN, on_mpv, p);
	mpv_set_wakeup_callback(m, wakeup, p);
	return TRUE;
}

/* A property now, or an option for when mpv starts. */
static void prop(Player *p, const char *name, const char *value)
{
	if (p->mpv)
		mpv_set_property_string(p->mpv, name, value);
	else
		g_hash_table_insert(p->pending, g_strdup(name), g_strdup(value));
}

Player *player_new(const PlayerCallbacks *cb, gpointer data)
{
	Player *p = g_new0(Player, 1);
	p->cb = *cb;
	p->data = data;
	p->idle = TRUE;
	p->efd = -1;
	p->entries = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)track_unref);
	p->pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	return p;
}

void player_free(Player *p)
{
	if (p->audio_timer)
		g_source_remove(p->audio_timer);
	if (p->mpv) {
		g_source_remove(p->watch);
		mpv_terminate_destroy(p->mpv);
		close(p->efd);
	}
	g_hash_table_unref(p->entries);
	g_hash_table_unref(p->pending);
	track_unref(p->current);
	g_free(p);
}

void player_play(Player *p, Track *t)
{
	if (!t || !ensure_mpv(p))
		return;
	ensure_audio();
	g_hash_table_remove_all(p->entries);
	track_unref(p->current);
	p->current = track_ref(t);
	p->loading = loadfile(p, t, "replace");
	mpv_set_property_string(p->mpv, "pause", "no");
	p->idle = FALSE;
	if (p->cb.track_changed)
		p->cb.track_changed(t, p->data);
	set_state(p);
}

void player_pause(Player *p, gboolean pause)
{
	if (!p->current || !p->mpv)
		return;
	if (!pause)
		ensure_audio();
	mpv_set_property_string(p->mpv, "pause", pause ? "yes" : "no");
}

void player_toggle(Player *p)
{
	player_pause(p, p->state == PLAYER_PLAYING);
}

void player_stop(Player *p)
{
	if (!p->mpv)
		return;
	const char *cmd[] = {"stop", NULL};
	mpv_command(p->mpv, cmd);
}

void player_seek(Player *p, double seconds)
{
	if (!p->current)
		return;
	char pos[32];
	g_ascii_formatd(pos, sizeof pos, "%.3f", seconds + p->current->cue_start);
	const char *cmd[] = {"seek", pos, "absolute", NULL};
	mpv_command(p->mpv, cmd);
}

double player_position(Player *p)
{
	double t = 0;
	if (!p->current || mpv_get_property(p->mpv, "time-pos", MPV_FORMAT_DOUBLE, &t) < 0)
		return 0;
	return MAX(0, t - p->current->cue_start);
}

PlayerState player_state(Player *p)
{
	return p->state;
}

Track *player_current(Player *p)
{
	return p->current;
}

void player_requeue(Player *p)
{
	if (p->current)
		queue_next(p);
}

void player_set_option(Player *p, const char *name, const char *value)
{
	prop(p, name, value);
}

void player_set_volume(Player *p, double percent)
{
	char v[32];
	g_ascii_formatd(v, sizeof v, "%.1f", percent);
	prop(p, "volume", v);
}

double player_volume(Player *p)
{
	double v = 100;
	if (p->mpv)
		mpv_get_property(p->mpv, "volume", MPV_FORMAT_DOUBLE, &v);
	else if (g_hash_table_contains(p->pending, "volume"))
		v = g_ascii_strtod(g_hash_table_lookup(p->pending, "volume"), NULL);
	return v;
}

void player_set_replaygain(Player *p, int mode, double preamp_db, gboolean prevent_clipping)
{
	char pre[32];
	g_ascii_formatd(pre, sizeof pre, "%.2f", preamp_db);
	prop(p, "replaygain", mode == 1 ? "track" : mode == 2 ? "album" : "no");
	prop(p, "replaygain-preamp", pre);
	prop(p, "replaygain-clip", prevent_clipping ? "yes" : "no");
}

void player_set_equalizer(Player *p, const double gains[10])
{
	GString *af = g_string_new(NULL);
	for (int i = 0; gains && i < 10; i++) {
		if (fabs(gains[i]) < 0.05)
			continue;
		char g[16];
		g_ascii_formatd(g, sizeof g, "%.1f", gains[i]);
		g_string_append_printf(af, "%sequalizer=f=%d:t=o:w=1:g=%s", af->len ? "," : "",
				       player_eq_bands[i], g);
	}
	if (af->len) {
		/* Leave headroom so boosted bands don't clip. */
		double max = 0;
		for (int i = 0; i < 10; i++)
			max = MAX(max, gains[i]);
		if (max > 0) {
			char v[16];
			g_ascii_formatd(v, sizeof v, "%.1f", -max);
			g_string_append_printf(af, ",volume=%sdB", v);
		}
		g_string_prepend(af, "lavfi=[");
		g_string_append_c(af, ']');
	}
	prop(p, "af", af->str);
	g_string_free(af, TRUE);
}

void player_audio_info(Player *p, char *buf, size_t size)
{
	buf[0] = 0;
	if (!p->current)
		return;
	const Track *t = p->current;
	GString *s = g_string_new(t->codec ? t->codec : "");
	gint64 rate = 0, bitrate = 0;
	if (mpv_get_property(p->mpv, "audio-params/samplerate", MPV_FORMAT_INT64, &rate) < 0)
		rate = t->samplerate;
	if (rate)
		g_string_append_printf(s, " · %g kHz", rate / 1000.0);
	if (t->bits)
		g_string_append_printf(s, " · %d bit", t->bits);
	g_string_append(s, t->channels == 1 ? " · mono" : t->channels == 2 ? " · stereo" : "");
	if (t->channels > 2)
		g_string_append_printf(s, " · %d ch", t->channels);
	if (mpv_get_property(p->mpv, "audio-bitrate", MPV_FORMAT_INT64, &bitrate) < 0 || !bitrate)
		bitrate = (gint64)t->bitrate * 1000;
	if (bitrate)
		g_string_append_printf(s, " · %" G_GINT64_FORMAT " kbps", (bitrate + 500) / 1000);
	g_strlcpy(buf, s->str, size);
	g_string_free(s, TRUE);
}

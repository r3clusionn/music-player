/* The control socket: one line in, one line out.
 *   play pause toggle stop next prev   playback
 *   vol+ vol- vol N                    volume (N in percent)
 *   seek+ seek- seek N                 5 s back/forward, or to N seconds
 *   order NAME                         default|repeat|repeat-track|shuffle|shuffle-albums|random
 *   stop-after                         toggle stop after the current track
 *   status                             state<TAB>artist<TAB>title<TAB>album<TAB>position<TAB>length
 *   show                               raise the window
 *   fullscreen                         toggle fullscreen
 *   search TEXT                        search the library (no TEXT: clear)
 *   rescan                             look for new or changed files in the library folders
 *   add PATH / open PATH               add to / replace the current playlist and play
 *   quit
 * Also how a second `music-player` hands its files to the running one. */
#define _GNU_SOURCE
#include "app.h"

#include <errno.h>
#include <malloc.h>
#include <glib-unix.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int listen_fd = -1;
static guint listen_watch;

char *ctl_socket_path(void)
{
	const char *rt = g_getenv("XDG_RUNTIME_DIR");
	return g_build_filename(rt && *rt ? rt : g_get_tmp_dir(), "music-player.sock", NULL);
}

static int connect_socket(void)
{
	char *path = ctl_socket_path();
	struct sockaddr_un sa = {.sun_family = AF_UNIX};
	g_strlcpy(sa.sun_path, path, sizeof sa.sun_path);
	g_free(path);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

gboolean ctl_send(const char *cmd, char **reply)
{
	int fd = connect_socket();
	if (fd < 0)
		return FALSE;
	char *line = g_strconcat(cmd, "\n", NULL);
	gboolean ok = write(fd, line, strlen(line)) == (ssize_t)strlen(line);
	g_free(line);
	GString *r = g_string_new(NULL);
	char buf[512];
	ssize_t n;
	while (ok && (n = read(fd, buf, sizeof buf)) > 0)
		g_string_append_len(r, buf, n);
	close(fd);
	if (reply)
		*reply = g_strchomp(g_string_free(r, FALSE));
	else
		g_string_free(r, TRUE);
	return ok;
}

static void open_paths(const char *path, gboolean replace)
{
	GPtrArray *tracks = g_file_test(path, G_FILE_TEST_IS_DIR) ? library_tracks_for_folder(app->lib, path)
								     : library_tracks_for_file(app->lib, path);
	if (tracks->len) {
		Playlist *pl = playlist_active();
		int start = replace ? 0 : (int)pl->tracks->len;
		if (replace)
			playlist_clear(pl);
		playlist_add(pl, tracks, -1);
		if (replace || player_state(app->player) == PLAYER_STOPPED)
			app_play_track(pl, start);
	}
	g_ptr_array_unref(tracks);
}

static char *dispatch(char *cmd)
{
	Player *p = app->player;
	g_strstrip(cmd);
	char *arg = strchr(cmd, ' ');
	if (arg)
		*arg++ = 0;
	if (!strcmp(cmd, "play")) {
		if (player_state(p) == PLAYER_STOPPED) {
			Playlist *pl = playlist_active();
			int c = playlist_cursor(pl);
			app_play_track(pl, c >= 0 ? c : 0);
		} else {
			player_pause(p, FALSE);
		}
	} else if (!strcmp(cmd, "pause")) {
		player_pause(p, TRUE);
	} else if (!strcmp(cmd, "toggle")) {
		if (player_state(p) == PLAYER_STOPPED) {
			char play[] = "play";
			return dispatch(play);
		}
		player_toggle(p);
	} else if (!strcmp(cmd, "stop")) {
		player_stop(p);
	} else if (!strcmp(cmd, "next")) {
		app_next();
	} else if (!strcmp(cmd, "prev")) {
		app_previous();
	} else if (!strcmp(cmd, "vol+") || !strcmp(cmd, "vol-") || !strcmp(cmd, "vol")) {
		double v = player_volume(p);
		v = cmd[3] == '+' ? v + 5 : cmd[3] == '-' ? v - 5 : (arg ? g_ascii_strtod(arg, NULL) : v);
		v = CLAMP(v, 0, 100);
		gtk_range_set_value(GTK_RANGE(app->volume), v); /* also sets the player */
		return g_strdup_printf("%.0f", v);
	} else if (!strcmp(cmd, "seek+") || !strcmp(cmd, "seek-") || !strcmp(cmd, "seek")) {
		double pos = player_position(p);
		pos = cmd[4] == '+' ? pos + 5 : cmd[4] == '-' ? pos - 5 : (arg ? g_ascii_strtod(arg, NULL) : pos);
		player_seek(p, MAX(0, pos));
	} else if (!strcmp(cmd, "status")) {
		Track *t = player_current(p);
		PlayerState st = player_state(p);
		const char *state = st == PLAYER_PLAYING ? "playing" : st == PLAYER_PAUSED ? "paused" : "stopped";
		if (!t)
			return g_strdup(state);
		return g_strdup_printf("%s\t%s\t%s\t%s\t%.1f\t%.1f", state, t->artist ? t->artist : "",
				       track_display_title(t), t->album ? t->album : "", player_position(p),
				       t->duration);
	} else if (!strcmp(cmd, "order") && arg) {
		static const char *const names[] = {"default", "repeat", "repeat-track", "shuffle",
						    "shuffle-albums", "random"};
		for (int i = 0; i < (int)G_N_ELEMENTS(names); i++)
			if (!strcmp(arg, names[i])) {
				app_set_order(i);
				return g_strdup("ok");
			}
		return g_strdup("error: order is default|repeat|repeat-track|shuffle|shuffle-albums|random");
	} else if (!strcmp(cmd, "stop-after")) {
		app_set_stop_after_current(!app->stop_after_current);
		return g_strdup(app->stop_after_current ? "on" : "off");
	} else if (!strcmp(cmd, "dialog") && arg && g_getenv("MUSIC_PLAYER_DEBUG")) {
		if (!strcmp(arg, "preferences")) dialog_preferences();
		else if (!strcmp(arg, "columns")) dialog_columns();
		else if (!strcmp(arg, "equalizer")) dialog_equalizer();
		else if (!strcmp(arg, "about")) dialog_about();
	} else if (!strcmp(cmd, "mem") && g_getenv("MUSIC_PLAYER_DEBUG")) {
		struct mallinfo2 before = mallinfo2();
		malloc_trim(0);
		struct mallinfo2 after = mallinfo2();
		return g_strdup_printf("heap in use %zu kB, free %zu kB, mmapped %zu kB; after trim free %zu kB",
				       before.uordblks / 1024, before.fordblks / 1024, before.hblkhd / 1024,
				       after.fordblks / 1024);
	} else if (!strcmp(cmd, "rescan")) {
		library_scan(app->lib, app_scan_progress, NULL);
	} else if (!strcmp(cmd, "search")) {
		/* Fills the library search box (empty: clear it). */
		gtk_entry_set_text(GTK_ENTRY(app->lib_search), arg ? arg : "");
	} else if (!strcmp(cmd, "fullscreen")) {
		app_toggle_fullscreen();
	} else if (!strcmp(cmd, "show")) {
		app_raise();
	} else if ((!strcmp(cmd, "add") || !strcmp(cmd, "open")) && arg) {
		open_paths(arg, cmd[0] == 'o');
	} else if (!strcmp(cmd, "quit")) {
		gtk_window_close(GTK_WINDOW(app->window));
	} else {
		return g_strdup_printf("error: unknown command %s", cmd);
	}
	return g_strdup("ok");
}

static gboolean on_client(gint fd, GIOCondition cond, gpointer data)
{
	(void)cond;
	(void)data;
	int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
	if (c < 0)
		return G_SOURCE_CONTINUE;
	/* Clients write their line right away; don't let a stuck one block us. */
	struct timeval tv = {.tv_sec = 1};
	setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	GString *line = g_string_new(NULL);
	char buf[1024];
	ssize_t n;
	while (!strchr(line->str, '\n') && (n = read(c, buf, sizeof buf)) > 0)
		g_string_append_len(line, buf, n);
	char *nl = strchr(line->str, '\n');
	if (nl)
		*nl = 0;
	char *reply = dispatch(line->str);
	char *out = g_strconcat(reply, "\n", NULL);
	if (write(c, out, strlen(out)) < 0) {
		/* the client went away */
	}
	g_free(out);
	g_free(reply);
	g_string_free(line, TRUE);
	close(c);
	return G_SOURCE_CONTINUE;
}

void ctl_listen(void)
{
	char *path = ctl_socket_path();
	struct sockaddr_un sa = {.sun_family = AF_UNIX};
	g_strlcpy(sa.sun_path, path, sizeof sa.sun_path);
	listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (bind(listen_fd, (struct sockaddr *)&sa, sizeof sa) < 0 && errno == EADDRINUSE) {
		/* Left over from a crash (a live instance was checked for at start). */
		unlink(path);
		bind(listen_fd, (struct sockaddr *)&sa, sizeof sa);
	}
	chmod(path, 0600);
	if (listen(listen_fd, 8) == 0)
		listen_watch = g_unix_fd_add(listen_fd, G_IO_IN, on_client, NULL);
	g_free(path);
}

void ctl_stop(void)
{
	if (listen_fd < 0)
		return;
	g_source_remove(listen_watch);
	close(listen_fd);
	char *path = ctl_socket_path();
	unlink(path);
	g_free(path);
	listen_fd = -1;
}

/* State shared by the window's parts. */
#pragma once

#include "format.h"
#include "library.h"
#include "player.h"

#include <gtk/gtk.h>

typedef struct Playlist Playlist;

typedef struct {
	char *name;
	char *format_src;
	Format *format;
	int width;
	float align;   /* 0 left, 0.5 center, 1 right */
	gboolean visible;
} Column;

typedef enum {
	ORDER_DEFAULT,
	ORDER_REPEAT_PLAYLIST,
	ORDER_REPEAT_TRACK,
	ORDER_SHUFFLE_TRACKS,
	ORDER_SHUFFLE_ALBUMS,
	ORDER_RANDOM,
	ORDER_COUNT
} PlaybackOrder;

typedef struct {
	Playlist *pl;
	Track *track;
} QueueItem;

typedef struct App {
	GtkWidget *window;
	GtkWidget *notebook;
	GtkWidget *lib_tree, *lib_search, *lib_mode;
	GtkWidget *cover, *props;
	GtkWidget *status, *seek, *time_label, *volume, *order_combo;
	GtkWidget *play_button;
	GtkWidget *hpaned, *left_paned, *info_paned;
	GtkWidget *stop_after_item, *order_items[ORDER_COUNT];
	GtkAccelGroup *accels;

	Library *lib;
	Player *player;

	GPtrArray *playlists;   /* Playlist*, in tab order */
	Playlist *playing_pl;
	int playing_index;      /* in playing_pl, -1 if unknown */
	Playlist *pending_pl;   /* the queued-up next track's place */
	int pending_index;
	GQueue *queue;          /* QueueItem* */
	GArray *history;        /* played (playlist id, index) pairs for Previous */

	PlaybackOrder order;
	gboolean stop_after_current;
	int rg_mode;            /* 0 off, 1 track, 2 album */
	double rg_preamp;
	gboolean rg_clip;
	double eq[10];
	gboolean eq_on;
	gboolean dark;
	gboolean bitperfect;
	int cover_size;

	GPtrArray *columns;     /* Column* */

	guint pos_timer;
	gboolean visible;       /* the window can be seen (not obscured, iconified or off-screen) */
	gboolean seek_dragging;
	guint scan_status_id;
	char *scan_status;

	GKeyFile *cfg;
	char *cfg_path;
	char *data_dir;
} App;

extern App *app;

/* main.c */
void app_play_track(Playlist *pl, int index);
void app_play_tracks(GPtrArray *tracks, const char *playlist_name, gboolean play);
void app_update_status(void);
void app_save_config(void);
void app_next(void);
void app_previous(void);
void app_requeue(void);
void app_scan_progress(const ScanProgress *p, gpointer data);
void app_raise(void);
void app_toggle_fullscreen(void);
void app_set_order(PlaybackOrder o);
void app_set_stop_after_current(gboolean on);
void app_apply_audio_settings(void);
void app_apply_theme(void);
void app_show_track_info(Track *t);
void app_enqueue(Playlist *pl, Track *t);
int app_queue_position(Playlist *pl, Track *t);

/* plview.c: playlists and their tabs */
struct Playlist {
	gint64 id;
	char *name;
	GPtrArray *tracks;      /* Track* */
	GtkTreeModel *model;
	GtkWidget *view;
	GtkWidget *scroller;
	guint save_id;
	int sort_column, sort_desc;
	GArray *shuffle;        /* indices, for shuffle orders */
	int shuffle_pos;
};

void playlists_init(void);
Playlist *playlist_new(const char *name, gboolean select);
void playlist_close(Playlist *pl);
Playlist *playlist_active(void);
Playlist *playlist_find_or_create(const char *name);
void playlist_select(Playlist *pl);
void playlist_add(Playlist *pl, GPtrArray *tracks, int at); /* at -1: end */
void playlist_clear(Playlist *pl);
void playlist_remove_selected(Playlist *pl);
void playlist_changed(Playlist *pl, gboolean keep_selection); /* after reordering or removing */
void playlist_row_changed(Playlist *pl, int index);
void playlist_redraw_all(void);
void playlist_scroll_to(Playlist *pl, int index, gboolean select);
GPtrArray *playlist_selected(Playlist *pl); /* Track*, referenced */
int playlist_cursor(Playlist *pl);
int playlist_index_of(Playlist *pl, Track *t, int hint);
void playlist_rename_dialog(Playlist *pl);
void playlist_save_soon(Playlist *pl);
void playlist_sort_by_format(Playlist *pl, const char *format, gboolean desc);
void playlist_export(Playlist *pl, const char *path);
double playlist_total_length(Playlist *pl, int *count);
void columns_rebuild(void);
void columns_load(void);
void columns_save(void);
Column *column_new(const char *name, const char *format, int width, float align, gboolean visible);
void column_free(Column *c);

/* libview.c: the library tree */
GtkWidget *libview_new(void);
void libview_refresh(void);
void libview_focus_search(void);

/* dialogs.c */
void dialog_preferences(void);
void dialog_columns(void);
void dialog_equalizer(void);
void dialog_about(void);

/* ctl.c: music-player-ctl socket */
char *ctl_socket_path(void);
gboolean ctl_send(const char *cmd, char **reply); /* to a running instance */
void ctl_listen(void);
void ctl_stop(void);

/* The library database (SQLite, FTS5 for search), the track cache, the
 * folder scanner and playlist storage. */
#pragma once

#include "track.h"

#include <sqlite3.h>

typedef struct Library Library;

typedef struct {
	int files_seen, added, updated, removed;
	double seconds;
	gboolean done;
} ScanProgress;

typedef void (*ScanCallback)(const ScanProgress *p, gpointer data); /* called on the main thread */

Library *library_open(const char *db_path);
void library_close(Library *lib);
sqlite3 *library_db(Library *lib);

/* Tracks by database ID, cached; the reference belongs to the caller. */
Track *library_track(Library *lib, gint64 id);
/* The track for a file (cue 0) or for entry `cue` of a .cue sheet on it, from
 * the database or read from the file and stored. NULL if it can't be read. */
Track *library_track_for_path(Library *lib, const char *path, int cue);
/* All tracks of a .cue sheet or a playlist file (.m3u/.m3u8), or the file. */
GPtrArray *library_tracks_for_file(Library *lib, const char *path);
/* Every audio file (and cue sheet) under a folder, sorted by path. */
GPtrArray *library_tracks_for_folder(Library *lib, const char *dir);

/* Library folders. */
char **library_folders(Library *lib);
void library_add_folder(Library *lib, const char *path);
void library_remove_folder(Library *lib, const char *path);

/* Scans the folders on a thread; progress and the end come to cb. A scan
 * already running is not started twice. */
void library_scan(Library *lib, ScanCallback cb, gpointer data);
gboolean library_scanning(Library *lib);
int library_count(Library *lib);

/* Search: fills the temporary table "filter" with the matching track IDs and
 * returns how many matched. An empty query clears the filter (returns -1). */
int library_search(Library *lib, const char *query);
gboolean library_filtered(Library *lib);

/* Playlists. */
typedef struct {
	gint64 id;
	char *name;
} PlaylistInfo;
GPtrArray *library_playlists(Library *lib); /* of PlaylistInfo*, in tab order */
void playlist_info_free(PlaylistInfo *p);
gint64 library_playlist_create(Library *lib, const char *name, int pos);
void library_playlist_rename(Library *lib, gint64 id, const char *name);
void library_playlist_delete(Library *lib, gint64 id);
void library_playlist_set_order(Library *lib, const gint64 *ids, int n);
GPtrArray *library_playlist_tracks(Library *lib, gint64 id); /* of Track* */
void library_playlist_save(Library *lib, gint64 id, GPtrArray *tracks);

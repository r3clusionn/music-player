#include "library.h"

#include "cue.h"
#include "tags.h"

#include <dirent.h>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct Library {
	sqlite3 *db;
	char *path;
	GHashTable *cache; /* id -> Track* (one reference) */
	gboolean filtered;

	GThread *scan_thread;
	ScanCallback scan_cb;
	gpointer scan_data;
	GMutex scan_lock;
	GArray *updated_ids; /* filled by the scanner, read when it's done */
};

#define TRACK_COLUMNS                                                                      \
	"id, path, cue, cue_start, cue_end, title, artist, album, albumartist, genre, date, comment, " \
	"tracknumber, totaltracks, discnumber, duration, codec, bitrate, samplerate, channels, "  \
	"bits, rg_tg, rg_ag, rg_tp, rg_ap, mtime, size"

static const char *const schema =
	"PRAGMA journal_mode=WAL;"
	"PRAGMA synchronous=NORMAL;"
	"CREATE TABLE IF NOT EXISTS tracks("
	" id INTEGER PRIMARY KEY, path TEXT NOT NULL, cue INT NOT NULL DEFAULT 0,"
	" cue_start REAL NOT NULL DEFAULT 0,"
	" cue_end REAL NOT NULL DEFAULT 0, title TEXT, artist TEXT, album TEXT, albumartist TEXT,"
	" genre TEXT, date TEXT, comment TEXT, tracknumber INT, totaltracks INT, discnumber INT,"
	" duration REAL, codec TEXT, bitrate INT, samplerate INT, channels INT, bits INT,"
	" rg_tg REAL, rg_ag REAL, rg_tp REAL, rg_ap REAL, mtime INT, size INT,"
	" in_library INT NOT NULL DEFAULT 0, UNIQUE(path, cue));"
	"CREATE INDEX IF NOT EXISTS tracks_artist ON tracks(albumartist, artist, album);"
	"CREATE VIRTUAL TABLE IF NOT EXISTS fts USING fts5(title, artist, album, albumartist, genre,"
	" date, path, content='tracks', content_rowid='id', tokenize='unicode61 remove_diacritics 2');"
	"CREATE TRIGGER IF NOT EXISTS tracks_ai AFTER INSERT ON tracks BEGIN"
	" INSERT INTO fts(rowid, title, artist, album, albumartist, genre, date, path)"
	" VALUES (new.id, new.title, new.artist, new.album, new.albumartist, new.genre, new.date, new.path); END;"
	"CREATE TRIGGER IF NOT EXISTS tracks_ad AFTER DELETE ON tracks BEGIN"
	" INSERT INTO fts(fts, rowid, title, artist, album, albumartist, genre, date, path)"
	" VALUES ('delete', old.id, old.title, old.artist, old.album, old.albumartist, old.genre, old.date, old.path); END;"
	"CREATE TRIGGER IF NOT EXISTS tracks_au AFTER UPDATE ON tracks BEGIN"
	" INSERT INTO fts(fts, rowid, title, artist, album, albumartist, genre, date, path)"
	" VALUES ('delete', old.id, old.title, old.artist, old.album, old.albumartist, old.genre, old.date, old.path);"
	" INSERT INTO fts(rowid, title, artist, album, albumartist, genre, date, path)"
	" VALUES (new.id, new.title, new.artist, new.album, new.albumartist, new.genre, new.date, new.path); END;"
	"CREATE TABLE IF NOT EXISTS folders(path TEXT PRIMARY KEY);"
	"CREATE TABLE IF NOT EXISTS playlists(id INTEGER PRIMARY KEY, name TEXT NOT NULL, pos INT NOT NULL);"
	"CREATE TABLE IF NOT EXISTS playlist_items(playlist INT NOT NULL, pos INT NOT NULL, track INT NOT NULL,"
	" PRIMARY KEY(playlist, pos)) WITHOUT ROWID;"
	"CREATE INDEX IF NOT EXISTS playlist_items_track ON playlist_items(track);"
	"CREATE TEMP TABLE IF NOT EXISTS filter(id INTEGER PRIMARY KEY);";

static sqlite3 *open_db(const char *path)
{
	sqlite3 *db;
	if (sqlite3_open(path, &db) != SQLITE_OK) {
		g_warning("library: %s: %s", path, sqlite3_errmsg(db));
		return db;
	}
	sqlite3_busy_timeout(db, 5000);
	char *err = NULL;
	if (sqlite3_exec(db, schema, NULL, NULL, &err) != SQLITE_OK) {
		g_warning("library: schema: %s", err);
		sqlite3_free(err);
	}
	return db;
}

Library *library_open(const char *db_path)
{
	Library *lib = g_new0(Library, 1);
	lib->path = g_strdup(db_path);
	char *dir = g_path_get_dirname(db_path);
	g_mkdir_with_parents(dir, 0755);
	g_free(dir);
	lib->db = open_db(db_path);
	lib->cache = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, (GDestroyNotify)track_unref);
	g_mutex_init(&lib->scan_lock);
	return lib;
}

void library_close(Library *lib)
{
	if (lib->scan_thread)
		g_thread_join(lib->scan_thread);
	g_hash_table_unref(lib->cache);
	sqlite3_close(lib->db);
	g_free(lib->path);
	g_free(lib);
}

sqlite3 *library_db(Library *lib)
{
	return lib->db;
}

static char *col_str(sqlite3_stmt *s, int i)
{
	const unsigned char *v = sqlite3_column_text(s, i);
	return v && *v ? g_strdup((const char *)v) : NULL;
}

static double col_num(sqlite3_stmt *s, int i)
{
	return sqlite3_column_type(s, i) == SQLITE_NULL ? NAN : sqlite3_column_double(s, i);
}

/* Fills t from a row selected with TRACK_COLUMNS starting at column base. */
static void row_to_track(sqlite3_stmt *s, int base, Track *t)
{
	int i = base;
	t->id = sqlite3_column_int64(s, i++);
	g_free(t->path); t->path = col_str(s, i++);
	t->cue = sqlite3_column_int(s, i++);
	t->cue_start = sqlite3_column_double(s, i++);
	t->cue_end = sqlite3_column_double(s, i++);
	g_free(t->title); t->title = col_str(s, i++);
	t->artist = track_intern((const char *)sqlite3_column_text(s, i++));
	t->album = track_intern((const char *)sqlite3_column_text(s, i++));
	t->albumartist = track_intern((const char *)sqlite3_column_text(s, i++));
	t->genre = track_intern((const char *)sqlite3_column_text(s, i++));
	t->date = track_intern((const char *)sqlite3_column_text(s, i++));
	g_free(t->comment); t->comment = col_str(s, i++);
	t->tracknumber = sqlite3_column_int(s, i++);
	t->totaltracks = sqlite3_column_int(s, i++);
	t->discnumber = sqlite3_column_int(s, i++);
	t->duration = sqlite3_column_double(s, i++);
	t->codec = track_intern((const char *)sqlite3_column_text(s, i++));
	t->bitrate = sqlite3_column_int(s, i++);
	t->samplerate = sqlite3_column_int(s, i++);
	t->channels = sqlite3_column_int(s, i++);
	t->bits = sqlite3_column_int(s, i++);
	t->rg_track_gain = col_num(s, i++);
	t->rg_album_gain = col_num(s, i++);
	t->rg_track_peak = col_num(s, i++);
	t->rg_album_peak = col_num(s, i++);
	t->mtime = sqlite3_column_int64(s, i++);
	t->size = sqlite3_column_int64(s, i++);
}

static Track *cached_row(Library *lib, sqlite3_stmt *s, int base)
{
	gint64 id = sqlite3_column_int64(s, base);
	Track *t = g_hash_table_lookup(lib->cache, &id);
	if (t)
		return track_ref(t);
	t = track_new();
	row_to_track(s, base, t);
	g_hash_table_insert(lib->cache, &t->id, t);
	return track_ref(t);
}

Track *library_track(Library *lib, gint64 id)
{
	Track *t = g_hash_table_lookup(lib->cache, &id);
	if (t)
		return track_ref(t);
	sqlite3_stmt *s;
	sqlite3_prepare_v2(lib->db, "SELECT " TRACK_COLUMNS " FROM tracks WHERE id=?", -1, &s, NULL);
	sqlite3_bind_int64(s, 1, id);
	if (sqlite3_step(s) == SQLITE_ROW)
		t = cached_row(lib, s, 0);
	sqlite3_finalize(s);
	return t;
}

static void bind_text(sqlite3_stmt *s, int i, const char *v)
{
	if (v)
		sqlite3_bind_text(s, i, v, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(s, i);
}

static void bind_num(sqlite3_stmt *s, int i, double v)
{
	if (isnan(v))
		sqlite3_bind_null(s, i);
	else
		sqlite3_bind_double(s, i, v);
}

/* Inserts or updates the row for (path, cue_start); returns its ID. */
static gint64 store(sqlite3 *db, const Track *t, gboolean in_library)
{
	static const char *sql =
		"INSERT INTO tracks(path, cue, cue_start, cue_end, title, artist, album, albumartist, genre,"
		" date, comment, tracknumber, totaltracks, discnumber, duration, codec, bitrate,"
		" samplerate, channels, bits, rg_tg, rg_ag, rg_tp, rg_ap, mtime, size, in_library)"
		" VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
		" ON CONFLICT(path, cue) DO UPDATE SET cue_start=excluded.cue_start, cue_end=excluded.cue_end,"
		" title=excluded.title, artist=excluded.artist, album=excluded.album,"
		" albumartist=excluded.albumartist, genre=excluded.genre, date=excluded.date,"
		" comment=excluded.comment, tracknumber=excluded.tracknumber,"
		" totaltracks=excluded.totaltracks, discnumber=excluded.discnumber,"
		" duration=excluded.duration, codec=excluded.codec, bitrate=excluded.bitrate,"
		" samplerate=excluded.samplerate, channels=excluded.channels, bits=excluded.bits,"
		" rg_tg=excluded.rg_tg, rg_ag=excluded.rg_ag, rg_tp=excluded.rg_tp, rg_ap=excluded.rg_ap,"
		" mtime=excluded.mtime, size=excluded.size, in_library=max(in_library, excluded.in_library)"
		" RETURNING id";
	sqlite3_stmt *s;
	sqlite3_prepare_v2(db, sql, -1, &s, NULL);
	int i = 1;
	bind_text(s, i++, t->path);
	sqlite3_bind_int(s, i++, t->cue);
	sqlite3_bind_double(s, i++, t->cue_start);
	sqlite3_bind_double(s, i++, t->cue_end);
	bind_text(s, i++, t->title);
	bind_text(s, i++, t->artist);
	bind_text(s, i++, t->album);
	bind_text(s, i++, t->albumartist);
	bind_text(s, i++, t->genre);
	bind_text(s, i++, t->date);
	bind_text(s, i++, t->comment);
	sqlite3_bind_int(s, i++, t->tracknumber);
	sqlite3_bind_int(s, i++, t->totaltracks);
	sqlite3_bind_int(s, i++, t->discnumber);
	sqlite3_bind_double(s, i++, t->duration);
	bind_text(s, i++, t->codec);
	sqlite3_bind_int(s, i++, t->bitrate);
	sqlite3_bind_int(s, i++, t->samplerate);
	sqlite3_bind_int(s, i++, t->channels);
	sqlite3_bind_int(s, i++, t->bits);
	bind_num(s, i++, t->rg_track_gain);
	bind_num(s, i++, t->rg_album_gain);
	bind_num(s, i++, t->rg_track_peak);
	bind_num(s, i++, t->rg_album_peak);
	sqlite3_bind_int64(s, i++, t->mtime);
	sqlite3_bind_int64(s, i++, t->size);
	sqlite3_bind_int(s, i++, in_library);
	gint64 id = 0;
	if (sqlite3_step(s) == SQLITE_ROW)
		id = sqlite3_column_int64(s, 0);
	sqlite3_finalize(s);
	return id;
}

Track *library_track_for_path(Library *lib, const char *path, int cue)
{
	sqlite3_stmt *s;
	sqlite3_prepare_v2(lib->db, "SELECT " TRACK_COLUMNS " FROM tracks WHERE path=? AND cue=?",
			   -1, &s, NULL);
	sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(s, 2, cue);
	Track *t = NULL;
	if (sqlite3_step(s) == SQLITE_ROW)
		t = cached_row(lib, s, 0);
	sqlite3_finalize(s);
	if (t || cue > 0)
		return t;
	t = tags_read(path);
	if (!t)
		return NULL;
	t->id = store(lib->db, t, FALSE);
	g_hash_table_insert(lib->cache, &t->id, track_ref(t));
	return t;
}

static GPtrArray *tracks_from_cue(Library *lib, const char *cue)
{
	GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)track_unref);
	GPtrArray *parsed = cue_read(cue);
	for (guint i = 0; i < parsed->len; i++) {
		Track *p = g_ptr_array_index(parsed, i);
		Track *t = library_track_for_path(lib, p->path, p->cue);
		if (!t) {
			p->id = store(lib->db, p, FALSE);
			t = track_ref(p);
			g_hash_table_insert(lib->cache, &t->id, track_ref(t));
		}
		g_ptr_array_add(out, t);
	}
	g_ptr_array_unref(parsed);
	return out;
}

static GPtrArray *tracks_from_m3u(Library *lib, const char *m3u)
{
	GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)track_unref);
	char *text;
	if (!g_file_get_contents(m3u, &text, NULL, NULL))
		return out;
	char *dir = g_path_get_dirname(m3u);
	char **lines = g_strsplit(text, "\n", -1);
	for (char **l = lines; *l; l++) {
		char *line = g_strstrip(*l);
		if (!*line || *line == '#')
			continue;
		char *path = g_str_has_prefix(line, "file://") ? g_filename_from_uri(line, NULL, NULL)
			   : g_path_is_absolute(line) ? g_strdup(line) : g_build_filename(dir, line, NULL);
		if (!path)
			continue;
		Track *t = library_track_for_path(lib, path, 0);
		if (t)
			g_ptr_array_add(out, t);
		g_free(path);
	}
	g_strfreev(lines);
	g_free(dir);
	g_free(text);
	return out;
}

GPtrArray *library_tracks_for_file(Library *lib, const char *path)
{
	if (g_str_has_suffix(path, ".cue") || g_str_has_suffix(path, ".CUE"))
		return tracks_from_cue(lib, path);
	if (g_str_has_suffix(path, ".m3u") || g_str_has_suffix(path, ".m3u8"))
		return tracks_from_m3u(lib, path);
	GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)track_unref);
	Track *t = library_track_for_path(lib, path, 0);
	if (t)
		g_ptr_array_add(out, t);
	return out;
}

/* ---- walking folders -------------------------------------------------- */

static void walk(const char *dir, GPtrArray *audio, GPtrArray *cues)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char *full = g_build_filename(dir, e->d_name, NULL);
		int type = e->d_type;
		if (type == DT_UNKNOWN || type == DT_LNK) {
			struct stat sb;
			type = stat(full, &sb) == 0 ? (S_ISDIR(sb.st_mode) ? DT_DIR : DT_REG) : DT_UNKNOWN;
		}
		if (type == DT_DIR) {
			walk(full, audio, cues);
			g_free(full);
		} else if (type == DT_REG && tags_is_audio_path(full)) {
			g_ptr_array_add(audio, full);
		} else if (type == DT_REG && (g_str_has_suffix(full, ".cue") || g_str_has_suffix(full, ".CUE"))) {
			g_ptr_array_add(cues, full);
		} else {
			g_free(full);
		}
	}
	closedir(d);
}

static gint cmp_str(gconstpointer a, gconstpointer b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

GPtrArray *library_tracks_for_folder(Library *lib, const char *dir)
{
	GPtrArray *audio = g_ptr_array_new_with_free_func(g_free);
	GPtrArray *cues = g_ptr_array_new_with_free_func(g_free);
	walk(dir, audio, cues);
	GHashTable *covered = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GPtrArray *entries = g_ptr_array_new_with_free_func(g_free);
	for (guint i = 0; i < cues->len; i++) {
		GPtrArray *files = cue_files(g_ptr_array_index(cues, i));
		gboolean any = FALSE;
		for (guint j = 0; j < files->len; j++)
			if (g_file_test(g_ptr_array_index(files, j), G_FILE_TEST_EXISTS)) {
				g_hash_table_add(covered, g_strdup(g_ptr_array_index(files, j)));
				any = TRUE;
			}
		if (any)
			g_ptr_array_add(entries, g_strdup(g_ptr_array_index(cues, i)));
		g_ptr_array_unref(files);
	}
	for (guint i = 0; i < audio->len; i++)
		if (!g_hash_table_contains(covered, g_ptr_array_index(audio, i)))
			g_ptr_array_add(entries, g_strdup(g_ptr_array_index(audio, i)));
	g_ptr_array_sort(entries, cmp_str);
	GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)track_unref);
	for (guint i = 0; i < entries->len; i++) {
		GPtrArray *t = library_tracks_for_file(lib, g_ptr_array_index(entries, i));
		for (guint j = 0; j < t->len; j++)
			g_ptr_array_add(out, track_ref(g_ptr_array_index(t, j)));
		g_ptr_array_unref(t);
	}
	g_ptr_array_unref(entries);
	g_hash_table_unref(covered);
	g_ptr_array_unref(audio);
	g_ptr_array_unref(cues);
	return out;
}

/* ---- folders ---------------------------------------------------------- */

char **library_folders(Library *lib)
{
	GPtrArray *a = g_ptr_array_new();
	sqlite3_stmt *s;
	sqlite3_prepare_v2(lib->db, "SELECT path FROM folders ORDER BY path", -1, &s, NULL);
	while (sqlite3_step(s) == SQLITE_ROW)
		g_ptr_array_add(a, g_strdup((const char *)sqlite3_column_text(s, 0)));
	sqlite3_finalize(s);
	g_ptr_array_add(a, NULL);
	return (char **)g_ptr_array_free(a, FALSE);
}

static void exec1(sqlite3 *db, const char *sql, const char *arg)
{
	sqlite3_stmt *s;
	sqlite3_prepare_v2(db, sql, -1, &s, NULL);
	if (arg)
		sqlite3_bind_text(s, 1, arg, -1, SQLITE_TRANSIENT);
	sqlite3_step(s);
	sqlite3_finalize(s);
}

void library_add_folder(Library *lib, const char *path)
{
	exec1(lib->db, "INSERT OR IGNORE INTO folders(path) VALUES (?)", path);
}

void library_remove_folder(Library *lib, const char *path)
{
	exec1(lib->db, "DELETE FROM folders WHERE path=?", path);
}

int library_count(Library *lib)
{
	sqlite3_stmt *s;
	sqlite3_prepare_v2(lib->db, "SELECT count(*) FROM tracks WHERE in_library", -1, &s, NULL);
	int n = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int(s, 0) : 0;
	sqlite3_finalize(s);
	return n;
}

/* ---- scanning --------------------------------------------------------- */

typedef struct {
	Library *lib;
	ScanProgress p;
	gint64 started;
} ScanState;

typedef struct {
	ScanCallback cb;
	gpointer data;
	ScanProgress p;
} ScanReport;

static gboolean report_idle(gpointer d)
{
	ScanReport *r = d;
	r->cb(&r->p, r->data);
	g_free(r);
	return G_SOURCE_REMOVE;
}

static void report(ScanState *st)
{
	if (!st->lib->scan_cb)
		return;
	ScanReport *r = g_new(ScanReport, 1);
	r->cb = st->lib->scan_cb;
	r->data = st->lib->scan_data;
	r->p = st->p;
	r->p.seconds = (g_get_monotonic_time() - st->started) / 1e6;
	g_idle_add(report_idle, r);
}

typedef struct {
	char *path;
	gboolean is_cue;
	GPtrArray *tracks; /* result */
} ScanJob;

static void scan_job(gpointer data, gpointer user)
{
	ScanJob *j = data;
	GAsyncQueue *done = user;
	j->tracks = g_ptr_array_new_with_free_func((GDestroyNotify)track_unref);
	if (j->is_cue) {
		GPtrArray *t = cue_read(j->path);
		for (guint i = 0; i < t->len; i++)
			g_ptr_array_add(j->tracks, track_ref(g_ptr_array_index(t, i)));
		g_ptr_array_unref(t);
	} else {
		Track *t = tags_read(j->path);
		if (t)
			g_ptr_array_add(j->tracks, t);
	}
	g_async_queue_push(done, j);
}

typedef struct {
	gint64 mtime, size;
	gboolean seen;
} Known;

static gpointer scan_thread(gpointer data)
{
	ScanState *st = data;
	Library *lib = st->lib;
	sqlite3 *db = open_db(lib->path);
	GArray *updated = g_array_new(FALSE, FALSE, sizeof(gint64));

	/* What the database knows: path -> mtime/size (cue tracks by their file). */
	GHashTable *known = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	sqlite3_stmt *s;
	sqlite3_prepare_v2(db, "SELECT path, max(mtime), max(size) FROM tracks WHERE in_library GROUP BY path",
			   -1, &s, NULL);
	while (sqlite3_step(s) == SQLITE_ROW) {
		Known *k = g_new0(Known, 1);
		k->mtime = sqlite3_column_int64(s, 1);
		k->size = sqlite3_column_int64(s, 2);
		g_hash_table_insert(known, g_strdup((const char *)sqlite3_column_text(s, 0)), k);
	}
	sqlite3_finalize(s);

	GPtrArray *audio = g_ptr_array_new_with_free_func(g_free);
	GPtrArray *cues = g_ptr_array_new_with_free_func(g_free);
	char **folders = library_folders(lib);
	for (char **f = folders; *f; f++)
		walk(*f, audio, cues);
	g_strfreev(folders);

	/* Files a cue sheet covers are listed as the sheet's tracks instead. */
	GHashTable *covered = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GHashTable *cue_of = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	for (guint i = 0; i < cues->len; i++) {
		GPtrArray *files = cue_files(g_ptr_array_index(cues, i));
		for (guint j = 0; j < files->len; j++) {
			const char *f = g_ptr_array_index(files, j);
			if (g_file_test(f, G_FILE_TEST_EXISTS)) {
				g_hash_table_add(covered, g_strdup(f));
				g_hash_table_insert(cue_of, g_strdup(f), g_strdup(g_ptr_array_index(cues, i)));
			}
		}
		g_ptr_array_unref(files);
	}

	GAsyncQueue *done = g_async_queue_new();
	GThreadPool *pool = g_thread_pool_new(scan_job, done, MAX(2, (int)g_get_num_processors() / 2),
					      FALSE, NULL);
	int jobs = 0;
	GHashTable *queued_cues = g_hash_table_new(g_str_hash, g_str_equal);
	for (guint i = 0; i < audio->len; i++) {
		const char *path = g_ptr_array_index(audio, i);
		st->p.files_seen++;
		struct stat sb;
		if (stat(path, &sb) != 0)
			continue;
		Known *k = g_hash_table_lookup(known, path);
		if (k) {
			k->seen = TRUE;
			if (k->mtime == sb.st_mtime && k->size == sb.st_size)
				continue;
		}
		ScanJob *j = g_new0(ScanJob, 1);
		const char *cue = g_hash_table_lookup(cue_of, path);
		if (cue) {
			if (g_hash_table_contains(queued_cues, cue)) {
				g_free(j);
				continue;
			}
			g_hash_table_add(queued_cues, (gpointer)cue);
			j->path = g_strdup(cue);
			j->is_cue = TRUE;
		} else {
			j->path = g_strdup(path);
		}
		g_thread_pool_push(pool, j, NULL);
		jobs++;
	}
	g_hash_table_unref(queued_cues);

	/* Write results in batches as they come. */
	sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
	gint64 last_report = g_get_monotonic_time(), batch = 0;
	for (int n = 0; n < jobs; n++) {
		ScanJob *j = g_async_queue_pop(done);
		for (guint i = 0; i < j->tracks->len; i++) {
			Track *t = g_ptr_array_index(j->tracks, i);
			gboolean existed = g_hash_table_contains(known, t->path);
			gint64 id = store(db, t, TRUE);
			if (existed) {
				st->p.updated++;
				g_array_append_val(updated, id);
			} else {
				st->p.added++;
			}
		}
		g_ptr_array_unref(j->tracks);
		g_free(j->path);
		g_free(j);
		if (++batch >= 500) {
			sqlite3_exec(db, "COMMIT; BEGIN", NULL, NULL, NULL);
			batch = 0;
		}
		if (g_get_monotonic_time() - last_report > 250000) {
			last_report = g_get_monotonic_time();
			report(st);
		}
	}
	g_thread_pool_free(pool, FALSE, TRUE);
	g_async_queue_unref(done);

	/* Files that are gone, or now covered by a cue sheet: out of the library.
	 * Rows a playlist still uses stay, marked as not in the library. */
	GHashTableIter it;
	gpointer key, val;
	g_hash_table_iter_init(&it, known);
	sqlite3_stmt *del, *unmark;
	sqlite3_prepare_v2(db, "DELETE FROM tracks WHERE path=? AND id NOT IN"
			   " (SELECT track FROM playlist_items)", -1, &del, NULL);
	sqlite3_prepare_v2(db, "UPDATE tracks SET in_library=0 WHERE path=? AND (cue=0 OR ?)",
			   -1, &unmark, NULL);
	while (g_hash_table_iter_next(&it, &key, &val)) {
		Known *k = val;
		gboolean whole_covered = g_hash_table_contains(covered, key);
		if (k->seen && !whole_covered)
			continue;
		if (k->seen && whole_covered) {
			/* The bare file's row (cue 0) leaves the library. */
			sqlite3_bind_text(unmark, 1, key, -1, SQLITE_STATIC);
			sqlite3_bind_int(unmark, 2, 0);
			sqlite3_step(unmark);
			sqlite3_reset(unmark);
			continue;
		}
		sqlite3_bind_text(del, 1, key, -1, SQLITE_STATIC);
		sqlite3_step(del);
		st->p.removed += sqlite3_changes(db);
		sqlite3_reset(del);
		sqlite3_bind_text(unmark, 1, key, -1, SQLITE_STATIC);
		sqlite3_bind_int(unmark, 2, 1);
		sqlite3_step(unmark);
		sqlite3_reset(unmark);
	}
	sqlite3_finalize(del);
	sqlite3_finalize(unmark);
	sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
	sqlite3_close(db);

	g_mutex_lock(&lib->scan_lock);
	lib->updated_ids = updated;
	g_mutex_unlock(&lib->scan_lock);

	g_hash_table_unref(known);
	g_hash_table_unref(covered);
	g_hash_table_unref(cue_of);
	g_ptr_array_unref(audio);
	g_ptr_array_unref(cues);
	st->p.done = TRUE;
	report(st);
	g_free(st);
	return NULL;
}

static void refresh_updated(Library *lib)
{
	g_mutex_lock(&lib->scan_lock);
	GArray *ids = lib->updated_ids;
	lib->updated_ids = NULL;
	g_mutex_unlock(&lib->scan_lock);
	if (!ids)
		return;
	sqlite3_stmt *s;
	sqlite3_prepare_v2(lib->db, "SELECT " TRACK_COLUMNS " FROM tracks WHERE id=?", -1, &s, NULL);
	for (guint i = 0; i < ids->len; i++) {
		gint64 id = g_array_index(ids, gint64, i);
		Track *t = g_hash_table_lookup(lib->cache, &id);
		if (!t)
			continue;
		sqlite3_bind_int64(s, 1, id);
		if (sqlite3_step(s) == SQLITE_ROW)
			row_to_track(s, 0, t); /* in place: playlists see the new tags */
		sqlite3_reset(s);
	}
	sqlite3_finalize(s);
	g_array_unref(ids);
}

typedef struct {
	Library *lib;
	ScanCallback cb;
	gpointer data;
} ScanDone;

static void scan_progress(const ScanProgress *p, gpointer data)
{
	ScanDone *d = data;
	if (p->done) {
		g_thread_join(d->lib->scan_thread);
		d->lib->scan_thread = NULL;
		refresh_updated(d->lib);
		/* The scanner's threads freed their tag-reading memory; give it back,
		 * and don't keep idle pool threads around. */
		g_thread_pool_stop_unused_threads();
		malloc_trim(0);
	}
	if (d->cb)
		d->cb(p, d->data);
	if (p->done)
		g_free(d);
}

void library_scan(Library *lib, ScanCallback cb, gpointer data)
{
	if (lib->scan_thread)
		return;
	ScanDone *d = g_new(ScanDone, 1);
	d->lib = lib;
	d->cb = cb;
	d->data = data;
	lib->scan_cb = scan_progress;
	lib->scan_data = d;
	ScanState *st = g_new0(ScanState, 1);
	st->lib = lib;
	st->started = g_get_monotonic_time();
	lib->scan_thread = g_thread_new("scan", scan_thread, st);
}

gboolean library_scanning(Library *lib)
{
	return lib->scan_thread != NULL;
}

/* ---- search ----------------------------------------------------------- */

int library_search(Library *lib, const char *query)
{
	sqlite3_exec(lib->db, "DELETE FROM filter", NULL, NULL, NULL);
	char **words = g_strsplit_set(query ? query : "", " \t", -1);
	GString *q = g_string_new(NULL);
	for (char **w = words; *w; w++) {
		if (!**w)
			continue;
		if (q->len)
			g_string_append(q, " AND ");
		/* Each word as a quoted prefix: "word"* */
		g_string_append_c(q, '"');
		for (const char *c = *w; *c; c++) {
			if (*c == '"')
				g_string_append_c(q, '"');
			g_string_append_c(q, *c);
		}
		g_string_append(q, "\"*");
	}
	g_strfreev(words);
	if (!q->len) {
		g_string_free(q, TRUE);
		lib->filtered = FALSE;
		return -1;
	}
	sqlite3_stmt *s;
	sqlite3_prepare_v2(lib->db,
			   "INSERT INTO filter(id) SELECT rowid FROM fts WHERE fts MATCH ?"
			   " AND rowid IN (SELECT id FROM tracks WHERE in_library)", -1, &s, NULL);
	sqlite3_bind_text(s, 1, q->str, -1, SQLITE_TRANSIENT);
	sqlite3_step(s);
	sqlite3_finalize(s);
	g_string_free(q, TRUE);
	lib->filtered = TRUE;
	return sqlite3_changes(lib->db);
}

gboolean library_filtered(Library *lib)
{
	return lib->filtered;
}

/* ---- playlists -------------------------------------------------------- */

void playlist_info_free(PlaylistInfo *p)
{
	g_free(p->name);
	g_free(p);
}

GPtrArray *library_playlists(Library *lib)
{
	GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)playlist_info_free);
	sqlite3_stmt *s;
	sqlite3_prepare_v2(lib->db, "SELECT id, name FROM playlists ORDER BY pos, id", -1, &s, NULL);
	while (sqlite3_step(s) == SQLITE_ROW) {
		PlaylistInfo *p = g_new0(PlaylistInfo, 1);
		p->id = sqlite3_column_int64(s, 0);
		p->name = g_strdup((const char *)sqlite3_column_text(s, 1));
		g_ptr_array_add(out, p);
	}
	sqlite3_finalize(s);
	return out;
}

gint64 library_playlist_create(Library *lib, const char *name, int pos)
{
	sqlite3_stmt *s;
	sqlite3_prepare_v2(lib->db, "INSERT INTO playlists(name, pos) VALUES (?, ?)", -1, &s, NULL);
	sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(s, 2, pos);
	sqlite3_step(s);
	sqlite3_finalize(s);
	return sqlite3_last_insert_rowid(lib->db);
}

void library_playlist_rename(Library *lib, gint64 id, const char *name)
{
	sqlite3_stmt *s;
	sqlite3_prepare_v2(lib->db, "UPDATE playlists SET name=? WHERE id=?", -1, &s, NULL);
	sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(s, 2, id);
	sqlite3_step(s);
	sqlite3_finalize(s);
}

void library_playlist_delete(Library *lib, gint64 id)
{
	char sql[160];
	g_snprintf(sql, sizeof sql, "DELETE FROM playlist_items WHERE playlist=%" G_GINT64_FORMAT
		   "; DELETE FROM playlists WHERE id=%" G_GINT64_FORMAT, id, id);
	sqlite3_exec(lib->db, sql, NULL, NULL, NULL);
}

void library_playlist_set_order(Library *lib, const gint64 *ids, int n)
{
	sqlite3_exec(lib->db, "BEGIN", NULL, NULL, NULL);
	sqlite3_stmt *s;
	sqlite3_prepare_v2(lib->db, "UPDATE playlists SET pos=? WHERE id=?", -1, &s, NULL);
	for (int i = 0; i < n; i++) {
		sqlite3_bind_int(s, 1, i);
		sqlite3_bind_int64(s, 2, ids[i]);
		sqlite3_step(s);
		sqlite3_reset(s);
	}
	sqlite3_finalize(s);
	sqlite3_exec(lib->db, "COMMIT", NULL, NULL, NULL);
}

GPtrArray *library_playlist_tracks(Library *lib, gint64 id)
{
	GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)track_unref);
	sqlite3_stmt *s;
	sqlite3_prepare_v2(lib->db, "SELECT " TRACK_COLUMNS " FROM playlist_items i JOIN tracks t"
			   " ON t.id=i.track WHERE i.playlist=? ORDER BY i.pos", -1, &s, NULL);
	sqlite3_bind_int64(s, 1, id);
	while (sqlite3_step(s) == SQLITE_ROW)
		g_ptr_array_add(out, cached_row(lib, s, 0));
	sqlite3_finalize(s);
	return out;
}

void library_playlist_save(Library *lib, gint64 id, GPtrArray *tracks)
{
	sqlite3_exec(lib->db, "BEGIN", NULL, NULL, NULL);
	sqlite3_stmt *s;
	sqlite3_prepare_v2(lib->db, "DELETE FROM playlist_items WHERE playlist=?", -1, &s, NULL);
	sqlite3_bind_int64(s, 1, id);
	sqlite3_step(s);
	sqlite3_finalize(s);
	sqlite3_prepare_v2(lib->db, "INSERT INTO playlist_items(playlist, pos, track) VALUES (?,?,?)",
			   -1, &s, NULL);
	for (guint i = 0; i < tracks->len; i++) {
		Track *t = g_ptr_array_index(tracks, i);
		if (!t->id)
			continue;
		sqlite3_bind_int64(s, 1, id);
		sqlite3_bind_int(s, 2, (int)i);
		sqlite3_bind_int64(s, 3, t->id);
		sqlite3_step(s);
		sqlite3_reset(s);
	}
	sqlite3_finalize(s);
	sqlite3_exec(lib->db, "COMMIT", NULL, NULL, NULL);
}

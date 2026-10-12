/* The library against tests/make-testdata.sh's files: scanning, tags,
 * ReplayGain, cue sheets, search, covers, rescans and playlists. */
#include "../src/library.h"
#include "../src/tags.h"

#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <utime.h>

static int failures;
static GMainLoop *loop;
static ScanProgress last;

#define CHECK(cond, ...)                                    \
	do {                                                \
		if (!(cond)) {                              \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__);                \
			printf("\n");                       \
			failures++;                         \
		}                                           \
	} while (0)

static void on_scan(const ScanProgress *p, gpointer data)
{
	(void)data;
	if (p->done) {
		last = *p;
		g_main_loop_quit(loop);
	}
}

static void scan(Library *lib)
{
	library_scan(lib, on_scan, NULL);
	g_main_loop_run(loop);
}

static Track *by_title(Library *lib, const char *title)
{
	sqlite3_stmt *s;
	sqlite3_prepare_v2(library_db(lib), "SELECT id FROM tracks WHERE title=? AND in_library", -1, &s, NULL);
	sqlite3_bind_text(s, 1, title, -1, SQLITE_STATIC);
	Track *t = sqlite3_step(s) == SQLITE_ROW ? library_track(lib, sqlite3_column_int64(s, 0)) : NULL;
	sqlite3_finalize(s);
	return t;
}

static gboolean near(double a, double b)
{
	return fabs(a - b) < 0.02;
}

int main(void)
{
	const char *data = g_getenv("TESTDATA");
	if (!data || !g_file_test(data, G_FILE_TEST_IS_DIR)) {
		printf("skipped: run tests/make-testdata.sh first (TESTDATA=%s)\n", data ? data : "unset");
		return 77;
	}
	char *dir = g_dir_make_tmp("music-player-test-XXXXXX", NULL);
	char *db = g_build_filename(dir, "library.db", NULL);
	loop = g_main_loop_new(NULL, FALSE);
	Library *lib = library_open(db);
	library_add_folder(lib, data);

	gint64 t0 = g_get_monotonic_time();
	scan(lib);
	printf("scan: %d added in %.3f s\n", last.added, (g_get_monotonic_time() - t0) / 1e6);
	/* 5 tones + 3 gapless parts + 2 opus + 1 aac + 3 cue tracks + 1 wav */
	CHECK(library_count(lib) == 15, "library has %d tracks, want 15", library_count(lib));
	CHECK(last.added == 15, "added %d", last.added);

	Track *t = by_title(lib, "Tone 3 (329.63 Hz)");
	CHECK(t, "Tone 3 not found");
	if (t) {
		CHECK(t->tracknumber == 3 && t->totaltracks == 5, "track %d/%d", t->tracknumber, t->totaltracks);
		CHECK(!g_strcmp0(t->albumartist, "Sine Ensemble"), "album artist %s", t->albumartist);
		CHECK(!g_strcmp0(t->codec, "FLAC") && t->samplerate == 44100 && t->bits == 16 && t->channels == 1,
		      "tech %s %d %d %d", t->codec, t->samplerate, t->bits, t->channels);
		CHECK(near(t->duration, 12), "duration %.3f", t->duration);
		CHECK(near(t->rg_track_gain, -5.21) && near(t->rg_album_gain, -4.20) && near(t->rg_track_peak, 0.5),
		      "replaygain %.2f %.2f %.2f", t->rg_track_gain, t->rg_album_gain, t->rg_track_peak);
		GdkPixbuf *pb = tags_cover(t->path, 200);
		CHECK(pb && gdk_pixbuf_get_width(pb) == 200, "embedded cover %dpx",
		      pb ? gdk_pixbuf_get_width(pb) : 0);
		if (pb)
			g_object_unref(pb);
		track_unref(t);
	}

	t = by_title(lib, "Hour 1");
	CHECK(t, "Hour 1 not found");
	if (t) {
		/* R128 -1280/256 = -5 dB relative to -23 LUFS = 0 dB ReplayGain */
		CHECK(near(t->rg_track_gain, 0.0), "opus R128 track gain %.2f", t->rg_track_gain);
		CHECK(!g_strcmp0(t->codec, "Opus") && !g_strcmp0(t->date, "2019-03-01"), "opus %s %s", t->codec, t->date);
		GdkPixbuf *pb = tags_cover(t->path, 128);
		CHECK(pb != NULL, "folder cover.jpg not found");
		if (pb)
			g_object_unref(pb);
		track_unref(t);
	}

	t = by_title(lib, "Second");
	CHECK(t, "cue track 2 not found");
	if (t) {
		CHECK(t->cue == 2 && near(t->cue_start, 10) && near(t->cue_end, 20) && near(t->duration, 10),
		      "cue %d %.2f-%.2f (%.2f s)", t->cue, t->cue_start, t->cue_end, t->duration);
		CHECK(!g_strcmp0(t->artist, "Guest Singer") && !g_strcmp0(t->albumartist, "Cue Performer") &&
		      !g_strcmp0(t->album, "Cue Album") && !g_strcmp0(t->genre, "Ambient") && !g_strcmp0(t->date, "1999"),
		      "cue tags %s / %s / %s / %s / %s", t->artist, t->albumartist, t->album, t->genre, t->date);
		track_unref(t);
	}
	t = by_title(lib, "Third");
	CHECK(t && near(t->duration, 10) && t->cue_end == 0, "last cue track runs to the end");
	if (t)
		track_unref(t);

	/* The image file itself is listed as its cue tracks only. */
	sqlite3_stmt *s;
	sqlite3_prepare_v2(library_db(lib), "SELECT count(*) FROM tracks WHERE in_library AND path LIKE '%Image.flac'",
			   -1, &s, NULL);
	sqlite3_step(s);
	CHECK(sqlite3_column_int(s, 0) == 3, "Image.flac rows in library: %d", sqlite3_column_int(s, 0));
	sqlite3_finalize(s);

	char *wav = g_build_filename(data, "untagged.wav", NULL);
	t = library_track_for_path(lib, wav, 0);
	g_free(wav);
	CHECK(t && !t->title && !strcmp(track_display_title(t), "untagged"), "untagged title fallback");
	if (t) {
		GdkPixbuf *pb = tags_cover(t->path, 64);
		CHECK(pb == NULL, "untagged.wav has no cover");
		track_unref(t);
	}

	CHECK(library_search(lib, "tone 329") == 1, "search 'tone 329'");
	CHECK(library_search(lib, "sine") == 5, "search 'sine' (artist)");
	CHECK(library_search(lib, "guest") == 1, "search 'guest' (cue performer)");
	CHECK(library_search(lib, "mall hou") == 0, "prefix search must match word starts only");
	CHECK(library_search(lib, "small hou") == 2, "prefix search 'small hou'");
	CHECK(library_search(lib, "") == -1 && !library_filtered(lib), "empty search clears the filter");

	/* Nothing changed: nothing re-read. */
	scan(lib);
	CHECK(last.added == 0 && last.updated == 0 && last.removed == 0, "rescan changed %d/%d/%d", last.added,
	      last.updated, last.removed);
	/* A touched file is read again. */
	char *touched = g_build_filename(data, "Gapless", "Continuous", "02 Part 2.flac", NULL);
	GStatBuf sb;
	g_stat(touched, &sb);
	struct utimbuf ut = {.actime = sb.st_atime, .modtime = sb.st_mtime + 1};
	utime(touched, &ut);
	scan(lib);
	CHECK(last.updated == 1 && last.added == 0, "after touch: %d updated, %d added", last.updated, last.added);
	g_free(touched);

	/* Playlists keep their order and survive a reopen. */
	gint64 pid = library_playlist_create(lib, "Test", 0);
	GPtrArray *tracks = g_ptr_array_new_with_free_func((GDestroyNotify)track_unref);
	const char *titles[] = {"Part 3", "Part 1", "Second"};
	for (int i = 0; i < 3; i++)
		g_ptr_array_add(tracks, by_title(lib, titles[i]));
	library_playlist_save(lib, pid, tracks);
	g_ptr_array_unref(tracks);
	library_close(lib);
	lib = library_open(db);
	tracks = library_playlist_tracks(lib, pid);
	CHECK(tracks->len == 3, "playlist has %u tracks", tracks->len);
	for (guint i = 0; i < tracks->len && i < 3; i++)
		CHECK(!strcmp(track_display_title(g_ptr_array_index(tracks, i)), titles[i]), "playlist row %u: %s", i,
		      track_display_title(g_ptr_array_index(tracks, i)));
	g_ptr_array_unref(tracks);
	library_close(lib);

	g_unlink(db);
	char *walf = g_strconcat(db, "-wal", NULL), *shm = g_strconcat(db, "-shm", NULL);
	g_unlink(walf);
	g_unlink(shm);
	g_rmdir(dir);
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}

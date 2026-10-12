#include "cue.h"

#include "tags.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
	char *file;
	int number;
	char *title, *performer;
	double start; /* INDEX 01 */
} CueTrack;

static char *read_text(const char *path)
{
	char *data;
	gsize len;
	if (!g_file_get_contents(path, &data, &len, NULL))
		return NULL;
	char *text = data;
	if (len >= 3 && (guchar)data[0] == 0xEF && (guchar)data[1] == 0xBB && (guchar)data[2] == 0xBF)
		text += 3;
	if (g_utf8_validate(text, -1, NULL)) {
		char *s = g_strdup(text);
		g_free(data);
		return s;
	}
	char *s = g_convert(text, -1, "UTF-8", "WINDOWS-1252", NULL, NULL, NULL);
	g_free(data);
	return s;
}

/* The rest of the line after the keyword, without quotes. */
static char *arg(const char *rest)
{
	while (*rest == ' ' || *rest == '\t')
		rest++;
	if (*rest == '"') {
		const char *e = strrchr(rest + 1, '"');
		return e ? g_strndup(rest + 1, e - rest - 1) : g_strdup(rest + 1);
	}
	return g_strstrip(g_strdup(rest));
}

/* The file name without the trailing file type (WAVE, MP3, ...). */
static char *file_arg(const char *rest)
{
	while (*rest == ' ' || *rest == '\t')
		rest++;
	if (*rest == '"')
		return arg(rest);
	char *s = g_strstrip(g_strdup(rest));
	char *sp = strrchr(s, ' ');
	if (sp)
		*sp = 0;
	return s;
}

static gboolean kw(const char *line, const char *word, const char **rest)
{
	size_t n = strlen(word);
	if (g_ascii_strncasecmp(line, word, n) || (line[n] != ' ' && line[n] != '\t'))
		return FALSE;
	*rest = line + n;
	return TRUE;
}

static void parse(const char *cue_path, GPtrArray *tracks, char **album, char **albumartist,
		  char **genre, char **date)
{
	char *text = read_text(cue_path);
	if (!text)
		return;
	char *dir = g_path_get_dirname(cue_path);
	char **lines = g_strsplit(text, "\n", -1);
	char *file = NULL;
	CueTrack *cur = NULL;
	for (char **l = lines; *l; l++) {
		char *line = g_strstrip(*l);
		const char *rest;
		if (kw(line, "FILE", &rest)) {
			g_free(file);
			char *name = file_arg(rest);
			file = g_build_filename(dir, name, NULL);
			g_free(name);
		} else if (kw(line, "TRACK", &rest)) {
			cur = g_new0(CueTrack, 1);
			cur->file = g_strdup(file);
			cur->number = atoi(rest);
			cur->start = -1;
			g_ptr_array_add(tracks, cur);
		} else if (kw(line, "TITLE", &rest)) {
			if (cur) cur->title = arg(rest);
			else if (!*album) *album = arg(rest);
		} else if (kw(line, "PERFORMER", &rest)) {
			if (cur) cur->performer = arg(rest);
			else if (!*albumartist) *albumartist = arg(rest);
		} else if (kw(line, "INDEX", &rest) && cur) {
			int idx, m, s, f;
			if (sscanf(rest, "%d %d:%d:%d", &idx, &m, &s, &f) == 4 && idx == 1)
				cur->start = m * 60 + s + f / 75.0;
		} else if (kw(line, "REM", &rest)) {
			const char *r2;
			while (*rest == ' ')
				rest++;
			if (kw(rest, "GENRE", &r2) && !*genre) *genre = arg(r2);
			else if (kw(rest, "DATE", &r2) && !*date) *date = arg(r2);
		}
	}
	g_free(file);
	g_strfreev(lines);
	g_free(dir);
	g_free(text);
}

static void cue_track_free(gpointer p)
{
	CueTrack *c = p;
	g_free(c->file);
	g_free(c->title);
	g_free(c->performer);
	g_free(c);
}

GPtrArray *cue_read(const char *cue_path)
{
	GPtrArray *raw = g_ptr_array_new_with_free_func(cue_track_free);
	char *album = NULL, *albumartist = NULL, *genre = NULL, *date = NULL;
	parse(cue_path, raw, &album, &albumartist, &genre, &date);

	GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)track_unref);
	GHashTable *info = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)track_unref);
	for (guint i = 0; i < raw->len; i++) {
		CueTrack *c = g_ptr_array_index(raw, i);
		if (!c->file || c->start < 0)
			continue;
		Track *file_info = g_hash_table_lookup(info, c->file);
		if (!file_info && !g_hash_table_contains(info, c->file)) {
			file_info = tags_read(c->file);
			g_hash_table_insert(info, g_strdup(c->file), file_info);
		}
		if (!file_info)
			continue;
		Track *t = track_new();
		t->path = g_strdup(c->file);
		t->cue = (int)out->len + 1;
		t->cue_start = c->start;
		/* Ends where the next track of the same file starts. */
		CueTrack *next = i + 1 < raw->len ? g_ptr_array_index(raw, i + 1) : NULL;
		if (next && !g_strcmp0(next->file, c->file) && next->start > c->start)
			t->cue_end = next->start;
		double end = t->cue_end > 0 ? t->cue_end : file_info->duration;
		t->duration = MAX(0, end - t->cue_start);
		t->title = g_strdup(c->title);
		t->artist = track_intern(c->performer ? c->performer : albumartist);
		t->albumartist = track_intern(albumartist);
		t->album = track_intern(album ? album : file_info->album);
		t->genre = track_intern(genre ? genre : file_info->genre);
		t->date = track_intern(date ? date : file_info->date);
		t->tracknumber = c->number;
		t->codec = file_info->codec;
		t->bitrate = file_info->bitrate;
		t->samplerate = file_info->samplerate;
		t->channels = file_info->channels;
		t->bits = file_info->bits;
		t->rg_album_gain = file_info->rg_album_gain;
		t->rg_album_peak = file_info->rg_album_peak;
		t->mtime = file_info->mtime;
		t->size = file_info->size;
		g_ptr_array_add(out, t);
	}
	for (guint i = 0; i < out->len; i++)
		((Track *)g_ptr_array_index(out, i))->totaltracks = (int)out->len;
	g_hash_table_unref(info);
	g_ptr_array_unref(raw);
	g_free(album);
	g_free(albumartist);
	g_free(genre);
	g_free(date);
	return out;
}

GPtrArray *cue_files(const char *cue_path)
{
	GPtrArray *raw = g_ptr_array_new_with_free_func(cue_track_free);
	char *a = NULL, *b = NULL, *c = NULL, *d = NULL;
	parse(cue_path, raw, &a, &b, &c, &d);
	GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
	for (guint i = 0; i < raw->len; i++) {
		CueTrack *t = g_ptr_array_index(raw, i);
		if (!t->file)
			continue;
		gboolean seen = FALSE;
		for (guint j = 0; j < files->len && !seen; j++)
			seen = !strcmp(g_ptr_array_index(files, j), t->file);
		if (!seen)
			g_ptr_array_add(files, g_strdup(t->file));
	}
	g_ptr_array_unref(raw);
	g_free(a);
	g_free(b);
	g_free(c);
	g_free(d);
	return files;
}

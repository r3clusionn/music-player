#include "track.h"

#include <math.h>
#include <string.h>

Track *track_new(void)
{
	Track *t = g_new0(Track, 1);
	t->rg_track_gain = t->rg_album_gain = t->rg_track_peak = t->rg_album_peak = NAN;
	t->refs = 1;
	return t;
}

Track *track_ref(Track *t)
{
	if (t)
		t->refs++;
	return t;
}

void track_unref(Track *t)
{
	if (!t || --t->refs > 0)
		return;
	g_free(t->path);
	g_free(t->title);
	g_free(t->comment);
	g_free(t);
}

const char *track_intern(const char *s)
{
	return s && *s ? g_intern_string(s) : NULL;
}

const char *track_display_title(const Track *t)
{
	static char buf[512];
	if (t->title && *t->title)
		return t->title;
	const char *base = strrchr(t->path, '/');
	base = base ? base + 1 : t->path;
	g_strlcpy(buf, base, sizeof buf);
	char *dot = strrchr(buf, '.');
	if (dot && dot != buf)
		*dot = 0;
	return buf;
}

const char *track_album_artist(const Track *t)
{
	if (t->albumartist && *t->albumartist)
		return t->albumartist;
	return t->artist ? t->artist : "";
}

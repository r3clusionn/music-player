/* A track: a file, or a range of one (a .cue sheet entry). Tracks are shared
 * between the library and the playlists and live as long as anything refers
 * to them. */
#pragma once

#include <glib.h>

typedef struct Track {
	gint64 id;          /* row in the library database, 0 if not stored */
	char *path;
	int cue;            /* position in its .cue sheet (1, 2, ...); 0 for a whole file */
	double cue_start;   /* seconds into the file */
	double cue_end;     /* 0 = to the end */

	char *title, *comment;
	/* Repeated across many tracks: interned (one shared copy, never freed). */
	const char *artist, *album, *albumartist, *genre, *date, *codec;
	int tracknumber, totaltracks, discnumber;

	double duration;    /* seconds */
	int bitrate;        /* kbit/s */
	int samplerate, channels, bits;

	double rg_track_gain, rg_album_gain, rg_track_peak, rg_album_peak; /* NAN if absent */

	gint64 mtime, size;
	int refs;
} Track;

Track *track_new(void);
Track *track_ref(Track *t);
void track_unref(Track *t);

/* The shared copy of s (NULL for NULL or ""), for the interned fields. */
const char *track_intern(const char *s);

/* Text for display: the title, or the file name without extension. */
const char *track_display_title(const Track *t);
/* The album artist, or the artist. */
const char *track_album_artist(const Track *t);

/* Tags, technical information and cover art, read with ffmpeg. */
#pragma once

#include "track.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

/* File extensions the scanner and the file dialogs treat as audio. */
gboolean tags_is_audio_path(const char *path);

/* Fills a new track from the file's tags. NULL if ffmpeg can't open it. */
Track *tags_read(const char *path);

/* Every tag in the file as name/value pairs (for the properties panel). */
GPtrArray *tags_read_all(const char *path); /* of char* pairs: name, value, name, value ... */

/* The track's cover: an embedded picture, else cover/folder/front.jpg|png
 * (or the only image) in its folder, scaled to fit size x size. NULL if none. */
GdkPixbuf *tags_cover(const char *path, int size);

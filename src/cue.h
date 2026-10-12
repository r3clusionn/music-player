/* .cue sheets: one audio file split into tracks. */
#pragma once

#include "track.h"

/* The tracks a cue sheet describes, with technical information read from the
 * audio files it names. Files that don't exist are skipped. */
GPtrArray *cue_read(const char *cue_path); /* of Track* */

/* The audio files a cue sheet refers to (to hide them from the library when
 * the sheet is there). */
GPtrArray *cue_files(const char *cue_path); /* of char* */

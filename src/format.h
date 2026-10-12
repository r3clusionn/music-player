/* foobar2000-style title formatting, the common subset:
 *   %field%            a tag or technical value (%artist%, %album artist%,
 *                      %tracknumber%, %length%, %codec%, %bitrate%, ...)
 *   $func(a,b,...)     $if(c,then[,else]) $if2(a,b) $if3(a,b,...) $num(n,len)
 *                      $upper(s) $lower(s) $left(s,n) $right(s,n) $len(s) $trim(s)
 *   [ ... ]            shown only if a field inside it has a value
 *   'text'             literal text (for the characters % $ [ ] ' , ( ))
 * A condition is true when a field in it has a value, as in foobar2000. */
#pragma once

#include "track.h"

typedef struct Format Format;

typedef struct {
	int list_index;   /* 1-based row number, 0 if unknown */
	int playing;      /* 1 playing, 2 paused, 0 not this track */
	int queued;       /* position in the playback queue, 0 if not queued */
} FormatContext;

Format *format_compile(const char *src);
void format_free(Format *f);
/* Appends the result to out. */
void format_eval(const Format *f, const Track *t, const FormatContext *ctx, GString *out);
/* Convenience: result in a static buffer, valid until the next call. */
const char *format_eval_static(const Format *f, const Track *t, const FormatContext *ctx);

/* Field value, as %name% would give it. Returns FALSE (and appends nothing)
 * when the track has no such value. */
gboolean format_field(const Track *t, const FormatContext *ctx, const char *name, GString *out);

/* "m:ss" or "h:mm:ss" */
void format_duration(double seconds, char *buf, size_t size);

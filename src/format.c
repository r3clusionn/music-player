#include "format.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { N_TEXT, N_FIELD, N_FUNC, N_COND } NodeKind;

typedef struct Node {
	NodeKind kind;
	char *str;          /* text, field name or function name */
	GPtrArray *kids;    /* N_COND: the block's nodes; N_FUNC: arguments (each a GPtrArray of nodes) */
} Node;

struct Format {
	GPtrArray *nodes;
};

static void node_free(gpointer p);

static void seq_free(gpointer p)
{
	g_ptr_array_unref(p);
}

static void node_free(gpointer p)
{
	Node *n = p;
	g_free(n->str);
	if (n->kids)
		g_ptr_array_unref(n->kids);
	g_free(n);
}

static GPtrArray *new_seq(void)
{
	return g_ptr_array_new_with_free_func(node_free);
}

static void add_text(GPtrArray *seq, GString *text)
{
	if (!text->len)
		return;
	Node *n = g_new0(Node, 1);
	n->kind = N_TEXT;
	n->str = g_strndup(text->str, text->len);
	g_ptr_array_add(seq, n);
	g_string_truncate(text, 0);
}

/* Parses until one of the stop characters at this level (or the end).
 * Returns the sequence; *pp points at the stop character. */
static GPtrArray *parse_seq(const char **pp, const char *stops)
{
	GPtrArray *seq = new_seq();
	GString *text = g_string_new(NULL);
	const char *p = *pp;
	while (*p && !strchr(stops, *p)) {
		if (*p == '\'') {
			const char *e = strchr(p + 1, '\'');
			if (!e) {
				g_string_append(text, p + 1);
				p += strlen(p);
				break;
			}
			if (e == p + 1)
				g_string_append_c(text, '\''); /* '' is a quote */
			else
				g_string_append_len(text, p + 1, e - p - 1);
			p = e + 1;
		} else if (*p == '%') {
			const char *e = strchr(p + 1, '%');
			if (!e) {
				g_string_append(text, p);
				p += strlen(p);
				break;
			}
			add_text(seq, text);
			Node *n = g_new0(Node, 1);
			n->kind = N_FIELD;
			n->str = g_ascii_strdown(p + 1, e - p - 1);
			g_ptr_array_add(seq, n);
			p = e + 1;
		} else if (*p == '[') {
			add_text(seq, text);
			p++;
			Node *n = g_new0(Node, 1);
			n->kind = N_COND;
			n->kids = parse_seq(&p, "]");
			if (*p == ']')
				p++;
			g_ptr_array_add(seq, n);
		} else if (*p == '$' && (isalnum((unsigned char)p[1]) || p[1] == '_')) {
			const char *name = p + 1;
			const char *q = name;
			while (isalnum((unsigned char)*q) || *q == '_')
				q++;
			if (*q != '(') {
				g_string_append_c(text, *p++);
				continue;
			}
			add_text(seq, text);
			Node *n = g_new0(Node, 1);
			n->kind = N_FUNC;
			n->str = g_ascii_strdown(name, q - name);
			n->kids = g_ptr_array_new_with_free_func(seq_free);
			p = q + 1;
			for (;;) {
				g_ptr_array_add(n->kids, parse_seq(&p, ",)"));
				if (*p == ',') {
					p++;
					continue;
				}
				if (*p == ')')
					p++;
				break;
			}
			g_ptr_array_add(seq, n);
		} else {
			g_string_append_c(text, *p++);
		}
	}
	add_text(seq, text);
	g_string_free(text, TRUE);
	*pp = p;
	return seq;
}

Format *format_compile(const char *src)
{
	Format *f = g_new0(Format, 1);
	const char *p = src ? src : "";
	f->nodes = parse_seq(&p, "");
	return f;
}

void format_free(Format *f)
{
	if (!f)
		return;
	g_ptr_array_unref(f->nodes);
	g_free(f);
}

void format_duration(double s, char *buf, size_t size)
{
	if (!(s >= 0) || isinf(s)) {
		g_strlcpy(buf, "", size);
		return;
	}
	long t = lround(s);
	if (t >= 3600)
		snprintf(buf, size, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
	else
		snprintf(buf, size, "%ld:%02ld", t / 60, t % 60);
}

static gboolean put(GString *out, const char *s)
{
	if (!s || !*s)
		return FALSE;
	g_string_append(out, s);
	return TRUE;
}

static gboolean put_int(GString *out, int v, int pad)
{
	if (v <= 0)
		return FALSE;
	g_string_append_printf(out, "%0*d", pad, v);
	return TRUE;
}

static gboolean put_gain(GString *out, double g)
{
	if (isnan(g))
		return FALSE;
	g_string_append_printf(out, "%+.2f dB", g);
	return TRUE;
}

gboolean format_field(const Track *t, const FormatContext *ctx, const char *name, GString *out)
{
	char buf[64];
	if (!strcmp(name, "title"))
		return put(out, track_display_title(t));
	if (!strcmp(name, "artist"))
		return put(out, t->artist ? t->artist : t->albumartist);
	if (!strcmp(name, "album artist") || !strcmp(name, "albumartist"))
		return put(out, track_album_artist(t));
	if (!strcmp(name, "track artist")) {
		/* The track's artist when it differs from the album artist. */
		if (!t->artist || !*t->artist || !t->albumartist || !*t->albumartist ||
		    !g_strcmp0(t->artist, t->albumartist))
			return FALSE;
		return put(out, t->artist);
	}
	if (!strcmp(name, "album"))
		return put(out, t->album);
	if (!strcmp(name, "genre"))
		return put(out, t->genre);
	if (!strcmp(name, "date"))
		return put(out, t->date);
	if (!strcmp(name, "year")) {
		if (!t->date || strlen(t->date) < 4)
			return FALSE;
		g_string_append_len(out, t->date, 4);
		return TRUE;
	}
	if (!strcmp(name, "comment"))
		return put(out, t->comment);
	if (!strcmp(name, "tracknumber"))
		return put_int(out, t->tracknumber, 2);
	if (!strcmp(name, "totaltracks"))
		return put_int(out, t->totaltracks, 2);
	if (!strcmp(name, "discnumber"))
		return put_int(out, t->discnumber, 1);
	if (!strcmp(name, "length")) {
		if (t->duration <= 0)
			return FALSE;
		format_duration(t->duration, buf, sizeof buf);
		return put(out, buf);
	}
	if (!strcmp(name, "length_seconds"))
		return put_int(out, (int)lround(t->duration), 1);
	if (!strcmp(name, "codec"))
		return put(out, t->codec);
	if (!strcmp(name, "bitrate"))
		return put_int(out, t->bitrate, 1);
	if (!strcmp(name, "samplerate"))
		return put_int(out, t->samplerate, 1);
	if (!strcmp(name, "channels")) {
		if (t->channels <= 0)
			return FALSE;
		return put(out, t->channels == 1 ? "mono" : t->channels == 2 ? "stereo"
			   : (snprintf(buf, sizeof buf, "%d ch", t->channels), buf));
	}
	if (!strcmp(name, "bitspersample"))
		return put_int(out, t->bits, 1);
	if (!strcmp(name, "path"))
		return put(out, t->path);
	if (!strcmp(name, "filename") || !strcmp(name, "filename_ext")) {
		const char *b = strrchr(t->path, '/');
		b = b ? b + 1 : t->path;
		if (name[8] == '_')
			return put(out, b);
		const char *dot = strrchr(b, '.');
		g_string_append_len(out, b, dot && dot != b ? dot - b : (gssize)strlen(b));
		return TRUE;
	}
	if (!strcmp(name, "directoryname")) {
		const char *e = strrchr(t->path, '/');
		if (!e)
			return FALSE;
		const char *s = e - 1;
		while (s > t->path && *s != '/')
			s--;
		if (*s == '/')
			s++;
		g_string_append_len(out, s, e - s);
		return TRUE;
	}
	if (!strcmp(name, "replaygain_track_gain"))
		return put_gain(out, t->rg_track_gain);
	if (!strcmp(name, "replaygain_album_gain"))
		return put_gain(out, t->rg_album_gain);
	if (!strcmp(name, "list_index"))
		return ctx ? put_int(out, ctx->list_index, 1) : FALSE;
	if (!strcmp(name, "isplaying"))
		return ctx && ctx->playing ? put(out, ctx->playing == 2 ? "⏸" : "▶") : FALSE;
	if (!strcmp(name, "queue_index"))
		return ctx ? put_int(out, ctx->queued, 1) : FALSE;
	return FALSE;
}

static gboolean eval_seq(const GPtrArray *seq, const Track *t, const FormatContext *ctx, GString *out);

/* Evaluates one function argument into a fresh string. */
static gboolean eval_arg(const Node *n, guint i, const Track *t, const FormatContext *ctx, GString *out)
{
	if (i >= n->kids->len)
		return FALSE;
	return eval_seq(g_ptr_array_index(n->kids, i), t, ctx, out);
}

static gboolean eval_func(const Node *n, const Track *t, const FormatContext *ctx, GString *out)
{
	const char *f = n->str;
	guint argc = n->kids->len;
	GString *a = g_string_new(NULL), *b = g_string_new(NULL);
	gboolean truth = FALSE;

	if (!strcmp(f, "if")) {
		if (eval_arg(n, 0, t, ctx, a))
			truth = eval_arg(n, 1, t, ctx, out);
		else if (argc > 2)
			truth = eval_arg(n, 2, t, ctx, out);
	} else if (!strcmp(f, "if2") || !strcmp(f, "if3")) {
		for (guint i = 0; i < argc; i++) {
			g_string_truncate(a, 0);
			if (eval_arg(n, i, t, ctx, a) || i == argc - 1) {
				g_string_append_len(out, a->str, a->len);
				truth = a->len > 0;
				break;
			}
		}
	} else if (!strcmp(f, "num")) {
		truth = eval_arg(n, 0, t, ctx, a);
		eval_arg(n, 1, t, ctx, b);
		g_string_append_printf(out, "%0*ld", (int)atoi(b->str), atol(a->str));
	} else if (!strcmp(f, "upper") || !strcmp(f, "lower")) {
		truth = eval_arg(n, 0, t, ctx, a);
		char *s = f[0] == 'u' ? g_utf8_strup(a->str, -1) : g_utf8_strdown(a->str, -1);
		g_string_append(out, s);
		g_free(s);
	} else if (!strcmp(f, "left") || !strcmp(f, "right")) {
		truth = eval_arg(n, 0, t, ctx, a);
		eval_arg(n, 1, t, ctx, b);
		glong len = g_utf8_strlen(a->str, -1), k = MAX(0, MIN(len, atol(b->str)));
		const char *start = f[0] == 'l' ? a->str : g_utf8_offset_to_pointer(a->str, len - k);
		const char *end = g_utf8_offset_to_pointer(start, k);
		g_string_append_len(out, start, end - start);
	} else if (!strcmp(f, "len")) {
		truth = eval_arg(n, 0, t, ctx, a);
		g_string_append_printf(out, "%ld", g_utf8_strlen(a->str, -1));
	} else if (!strcmp(f, "trim")) {
		truth = eval_arg(n, 0, t, ctx, a);
		g_string_append(out, g_strstrip(a->str));
	} else {
		g_string_append_printf(out, "[unknown function %s]", f);
	}
	g_string_free(a, TRUE);
	g_string_free(b, TRUE);
	return truth;
}

static gboolean eval_seq(const GPtrArray *seq, const Track *t, const FormatContext *ctx, GString *out)
{
	gboolean truth = FALSE;
	for (guint i = 0; i < seq->len; i++) {
		const Node *n = g_ptr_array_index(seq, i);
		switch (n->kind) {
		case N_TEXT:
			g_string_append(out, n->str);
			break;
		case N_FIELD:
			if (format_field(t, ctx, n->str, out))
				truth = TRUE;
			break;
		case N_FUNC:
			if (eval_func(n, t, ctx, out))
				truth = TRUE;
			break;
		case N_COND: {
			gsize mark = out->len;
			if (eval_seq(n->kids, t, ctx, out))
				truth = TRUE;
			else
				g_string_truncate(out, mark);
			break;
		}
		}
	}
	return truth;
}

void format_eval(const Format *f, const Track *t, const FormatContext *ctx, GString *out)
{
	eval_seq(f->nodes, t, ctx, out);
}

const char *format_eval_static(const Format *f, const Track *t, const FormatContext *ctx)
{
	static GString *buf;
	if (!buf)
		buf = g_string_sized_new(256);
	g_string_truncate(buf, 0);
	format_eval(f, t, ctx, buf);
	return buf->str;
}

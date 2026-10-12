#include "tags.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *const audio_exts[] = {
	"flac", "mp3", "opus", "ogg", "oga", "m4a", "mp4", "aac", "alac", "wv", "ape", "wav",
	"aif", "aiff", "aifc", "dsf", "dff", "wma", "mka", "mpc", "tta", "tak", "spx", NULL,
};

/* ffmpeg's warnings (JPEG pixel formats, odd headers) aren't ours to show. */
static void quiet(void)
{
	static gsize once;
	if (g_once_init_enter(&once)) {
		av_log_set_level(AV_LOG_ERROR);
		g_once_init_leave(&once, 1);
	}
}

gboolean tags_is_audio_path(const char *path)
{
	const char *dot = strrchr(path, '.');
	if (!dot || strchr(dot, '/'))
		return FALSE;
	for (int i = 0; audio_exts[i]; i++)
		if (!g_ascii_strcasecmp(dot + 1, audio_exts[i]))
			return TRUE;
	return FALSE;
}

static const char *tag(AVFormatContext *fc, AVStream *st, const char *key)
{
	const AVDictionaryEntry *e = av_dict_get(fc->metadata, key, NULL, 0);
	if (!e && st)
		e = av_dict_get(st->metadata, key, NULL, 0);
	return e && e->value && *e->value ? e->value : NULL;
}

static char *dup_tag(AVFormatContext *fc, AVStream *st, const char *const *keys)
{
	for (; *keys; keys++) {
		const char *v = tag(fc, st, *keys);
		if (v)
			return g_strstrip(g_strdup(v));
	}
	return NULL;
}

/* "3/12" -> 3 and 12 */
static void parse_pair(const char *s, int *a, int *b)
{
	if (!s)
		return;
	*a = atoi(s);
	const char *slash = strchr(s, '/');
	if (slash && b)
		*b = atoi(slash + 1);
}

/* "-6.50 dB" -> -6.5 */
static double parse_gain(const char *s)
{
	if (!s)
		return NAN;
	char *end;
	double v = g_ascii_strtod(s, &end);
	return end == s ? NAN : v;
}

Track *tags_read(const char *path)
{
	quiet();
	AVFormatContext *fc = NULL;
	if (avformat_open_input(&fc, path, NULL, NULL) < 0)
		return NULL;
	/* Most containers give the duration and codec from the headers; probe
	 * the stream only when they don't (raw AAC, some MP3s). */
	int st_idx = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (st_idx < 0 || fc->duration <= 0 ||
	    fc->streams[st_idx]->codecpar->sample_rate <= 0) {
		fc->probesize = 512 * 1024;
		avformat_find_stream_info(fc, NULL);
		st_idx = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	}
	if (st_idx < 0) {
		avformat_close_input(&fc);
		return NULL;
	}
	AVStream *st = fc->streams[st_idx];
	AVCodecParameters *cp = st->codecpar;

	Track *t = track_new();
	t->path = g_strdup(path);
	t->title = dup_tag(fc, st, (const char *const[]){"title", NULL});
	char *v;
#define INTERN(field, ...)                                               \
	v = dup_tag(fc, st, (const char *const[]){__VA_ARGS__, NULL});   \
	t->field = track_intern(v);                                      \
	g_free(v)
	INTERN(artist, "artist", "performer");
	INTERN(album, "album");
	INTERN(albumartist, "album_artist", "albumartist", "album artist");
	INTERN(genre, "genre");
	INTERN(date, "date", "year", "originaldate");
#undef INTERN
	t->comment = dup_tag(fc, st, (const char *const[]){"comment", "description", NULL});
	parse_pair(tag(fc, st, "track"), &t->tracknumber, &t->totaltracks);
	if (!t->totaltracks)
		parse_pair(tag(fc, st, "tracktotal") ? tag(fc, st, "tracktotal") : tag(fc, st, "totaltracks"),
			   &t->totaltracks, NULL);
	parse_pair(tag(fc, st, "disc"), &t->discnumber, NULL);

	t->rg_track_gain = parse_gain(tag(fc, st, "replaygain_track_gain"));
	t->rg_album_gain = parse_gain(tag(fc, st, "replaygain_album_gain"));
	t->rg_track_peak = parse_gain(tag(fc, st, "replaygain_track_peak"));
	t->rg_album_peak = parse_gain(tag(fc, st, "replaygain_album_peak"));
	/* Opus: R128 gains in 1/256 dB relative to -23 LUFS; ReplayGain's
	 * reference is 5 dB louder. */
	const char *r128;
	if (isnan(t->rg_track_gain) && (r128 = tag(fc, st, "r128_track_gain")))
		t->rg_track_gain = atoi(r128) / 256.0 + 5.0;
	if (isnan(t->rg_album_gain) && (r128 = tag(fc, st, "r128_album_gain")))
		t->rg_album_gain = atoi(r128) / 256.0 + 5.0;

	if (fc->duration > 0)
		t->duration = fc->duration / (double)AV_TIME_BASE;
	else if (st->duration > 0)
		t->duration = st->duration * av_q2d(st->time_base);

	const AVCodecDescriptor *d = avcodec_descriptor_get(cp->codec_id);
	const char *codec = d ? d->name : "unknown";
	/* Friendlier names for the common ones. */
	static const char *const names[][2] = {
		{"flac", "FLAC"}, {"mp3", "MP3"}, {"opus", "Opus"}, {"vorbis", "Vorbis"},
		{"aac", "AAC"}, {"alac", "ALAC"}, {"wavpack", "WavPack"}, {"ape", "Monkey's Audio"},
		{"tta", "TTA"}, {"tak", "TAK"}, {"musepack7", "Musepack"}, {"musepack8", "Musepack"},
		{"wmav2", "WMA"}, {"wmalossless", "WMA Lossless"}, {"dsd_lsbf", "DSD"},
		{"dsd_msbf", "DSD"}, {"dsd_lsbf_planar", "DSD"}, {"dsd_msbf_planar", "DSD"},
	};
	for (size_t i = 0; i < G_N_ELEMENTS(names); i++)
		if (!strcmp(codec, names[i][0]))
			codec = names[i][1];
	if (!strncmp(codec, "pcm_", 4))
		codec = "PCM";
	t->codec = track_intern(codec);
	t->samplerate = cp->sample_rate;
	t->channels = cp->ch_layout.nb_channels;
	t->bits = cp->bits_per_raw_sample ? cp->bits_per_raw_sample : cp->bits_per_coded_sample;
	if (!strcmp(codec, "MP3") || !strcmp(codec, "AAC") || !strcmp(codec, "Opus") ||
	    !strcmp(codec, "Vorbis"))
		t->bits = 0; /* lossy: no meaningful bit depth */
	int64_t br = fc->bit_rate > 0 ? fc->bit_rate : cp->bit_rate;
	struct stat sb;
	if (stat(path, &sb) == 0) {
		t->mtime = sb.st_mtime;
		t->size = sb.st_size;
		if (br <= 0 && t->duration > 0)
			br = (int64_t)(sb.st_size * 8 / t->duration);
	}
	t->bitrate = (int)((br + 500) / 1000);
	avformat_close_input(&fc);
	return t;
}

GPtrArray *tags_read_all(const char *path)
{
	quiet();
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	AVFormatContext *fc = NULL;
	if (avformat_open_input(&fc, path, NULL, NULL) < 0)
		return out;
	const AVDictionaryEntry *e = NULL;
	while ((e = av_dict_iterate(fc->metadata, e))) {
		g_ptr_array_add(out, g_strdup(e->key));
		g_ptr_array_add(out, g_strdup(e->value));
	}
	int st_idx = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (st_idx >= 0) {
		e = NULL;
		while ((e = av_dict_iterate(fc->streams[st_idx]->metadata, e))) {
			if (!strcmp(e->key, "encoder") || !strcmp(e->key, "language") ||
			    !strcmp(e->key, "handler_name") || !strcmp(e->key, "vendor_id"))
				continue;
			g_ptr_array_add(out, g_strdup(e->key));
			g_ptr_array_add(out, g_strdup(e->value));
		}
	}
	avformat_close_input(&fc);
	return out;
}

/* ---- cover art --------------------------------------------------------- */

static void free_pixels(guchar *pixels, gpointer data)
{
	(void)data;
	g_free(pixels);
}

static GdkPixbuf *frame_to_pixbuf(const AVFrame *f, int size)
{
	double s = MIN((double)size / f->width, (double)size / f->height);
	int w = MAX(1, (int)lround(f->width * s)), h = MAX(1, (int)lround(f->height * s));
	struct SwsContext *sws = sws_getContext(f->width, f->height, f->format, w, h, AV_PIX_FMT_RGB24,
						SWS_LANCZOS, NULL, NULL, NULL);
	if (!sws)
		return NULL;
	int stride = (w * 3 + 3) & ~3;
	guchar *pixels = g_malloc((gsize)stride * h);
	uint8_t *dst[4] = {pixels};
	int dst_stride[4] = {stride};
	sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, dst_stride);
	sws_freeContext(sws);
	return gdk_pixbuf_new_from_data(pixels, GDK_COLORSPACE_RGB, FALSE, 8, w, h, stride, free_pixels, NULL);
}

static GdkPixbuf *decode_packet(const AVCodecParameters *par, const AVPacket *pkt, int size)
{
	const AVCodec *codec = avcodec_find_decoder(par->codec_id);
	if (!codec)
		return NULL;
	AVCodecContext *cc = avcodec_alloc_context3(codec);
	GdkPixbuf *pb = NULL;
	AVFrame *frame = av_frame_alloc();
	if (avcodec_parameters_to_context(cc, par) >= 0 && avcodec_open2(cc, codec, NULL) >= 0 &&
	    avcodec_send_packet(cc, pkt) >= 0) {
		avcodec_send_packet(cc, NULL);
		if (avcodec_receive_frame(cc, frame) >= 0)
			pb = frame_to_pixbuf(frame, size);
	}
	av_frame_free(&frame);
	avcodec_free_context(&cc);
	return pb;
}

static GdkPixbuf *embedded_cover(const char *path, int size)
{
	AVFormatContext *fc = NULL;
	if (avformat_open_input(&fc, path, NULL, NULL) < 0)
		return NULL;
	GdkPixbuf *pb = NULL;
	for (unsigned i = 0; i < fc->nb_streams && !pb; i++) {
		AVStream *st = fc->streams[i];
		if (st->disposition & AV_DISPOSITION_ATTACHED_PIC && st->attached_pic.size > 0)
			pb = decode_packet(st->codecpar, &st->attached_pic, size);
	}
	avformat_close_input(&fc);
	return pb;
}

static GdkPixbuf *image_file(const char *path, int size)
{
	AVFormatContext *fc = NULL;
	if (avformat_open_input(&fc, path, NULL, NULL) < 0)
		return NULL;
	GdkPixbuf *pb = NULL;
	AVPacket *pkt = av_packet_alloc();
	if (fc->nb_streams > 0 && av_read_frame(fc, pkt) >= 0)
		pb = decode_packet(fc->streams[pkt->stream_index]->codecpar, pkt, size);
	av_packet_free(&pkt);
	avformat_close_input(&fc);
	return pb;
}

static gboolean is_image(const char *name)
{
	const char *dot = strrchr(name, '.');
	return dot && (!g_ascii_strcasecmp(dot, ".jpg") || !g_ascii_strcasecmp(dot, ".jpeg") ||
		       !g_ascii_strcasecmp(dot, ".png") || !g_ascii_strcasecmp(dot, ".webp"));
}

static GdkPixbuf *folder_cover(const char *path, int size)
{
	char *dir = g_path_get_dirname(path);
	GDir *d = g_dir_open(dir, 0, NULL);
	GdkPixbuf *pb = NULL;
	if (d) {
		static const char *const preferred[] = {"cover", "folder", "front", "album", NULL};
		char *best = NULL, *only = NULL;
		int nimages = 0, best_rank = 99;
		const char *name;
		while ((name = g_dir_read_name(d))) {
			if (!is_image(name))
				continue;
			nimages++;
			g_free(only);
			only = g_strdup(name);
			for (int r = 0; preferred[r]; r++) {
				size_t n = strlen(preferred[r]);
				if (r < best_rank && !g_ascii_strncasecmp(name, preferred[r], n) && name[n] == '.') {
					g_free(best);
					best = g_strdup(name);
					best_rank = r;
				}
			}
		}
		g_dir_close(d);
		const char *pick = best ? best : nimages == 1 ? only : NULL;
		if (pick) {
			char *full = g_build_filename(dir, pick, NULL);
			pb = image_file(full, size);
			g_free(full);
		}
		g_free(best);
		g_free(only);
	}
	g_free(dir);
	return pb;
}

GdkPixbuf *tags_cover(const char *path, int size)
{
	quiet();
	GdkPixbuf *pb = embedded_cover(path, size);
	return pb ? pb : folder_cover(path, size);
}

/* Playback through player.c, with mpv writing raw PCM to a file instead of
 * PipeWire (ao=pcm):
 *  - gapless: the three parts of a continuous tone, queued one after another
 *    like the app does, must come out as the original, sample for sample;
 *  - ReplayGain: a track tagged -5.21 dB must come out 5.21 dB quieter. */
#include "../src/player.h"
#include "../src/tags.h"

#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static Track *parts[3];
static int changes, order_ok = 1;
static GMainLoop *loop;

static void on_track(Track *t, gpointer data)
{
	(void)data;
	if (!t)
		return;
	if (changes < 3 && t != parts[changes])
		order_ok = 0;
	changes++;
}

static void on_state(PlayerState s, gpointer data)
{
	(void)data;
	if (s == PLAYER_STOPPED && changes)
		g_main_loop_quit(loop);
}

static Track *on_next(Track *cur, gpointer data)
{
	(void)data;
	for (int i = 0; i < 2; i++)
		if (cur == parts[i])
			return track_ref(parts[i + 1]);
	return NULL;
}

static gboolean timeout(gpointer data)
{
	(void)data;
	printf("FAIL: playback did not finish\n");
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

int main(void)
{
	const char *data = g_getenv("TESTDATA");
	char *orig = data ? g_build_filename(data, "..", "testdata-extra", "gapless.wav", NULL) : NULL;
	if (!orig || !g_file_test(orig, G_FILE_TEST_EXISTS)) {
		printf("skipped: run tests/make-testdata.sh first\n");
		return 77;
	}
	g_setenv("MUSIC_PLAYER_TEST", "1", TRUE);
	for (int i = 0; i < 3; i++) {
		char name[64];
		g_snprintf(name, sizeof name, "0%d Part %d.flac", i + 1, i + 1);
		char *path = g_build_filename(data, "Gapless", "Continuous", name, NULL);
		parts[i] = tags_read(path);
		g_free(path);
	}
	char *out = g_build_filename(g_get_tmp_dir(), "music-player-gapless.raw", NULL);
	loop = g_main_loop_new(NULL, FALSE);
	PlayerCallbacks cb = {.track_changed = on_track, .state_changed = on_state, .next_track = on_next};
	Player *p = player_new(&cb, NULL);
	player_set_option(p, "ao", "pcm");
	player_set_option(p, "ao-pcm-file", out);
	player_set_option(p, "ao-pcm-waveheader", "no");
	player_set_option(p, "audio-format", "s16");
	player_set_replaygain(p, 0, 0, TRUE);
	player_play(p, parts[0]);
	g_timeout_add_seconds(120, timeout, NULL);
	g_main_loop_run(loop);
	player_free(p);

	int failures = 0;
	if (changes != 3 || !order_ok) {
		printf("FAIL: %d track changes (want 3, in order: %s)\n", changes, order_ok ? "yes" : "no");
		failures++;
	}
	char *a, *b;
	gsize alen, blen;
	g_file_get_contents(orig, &a, &alen, NULL);
	g_file_get_contents(out, &b, &blen, NULL);
	/* The WAV's data chunk: find "data", then its size. */
	char *d = memmem(a, alen, "data", 4);
	const char *pcm = d + 8;
	gsize pcmlen = alen - (gsize)(pcm - a);
	gsize frames_in = pcmlen / 4, frames_out = blen / 4;
	gsize first_diff = frames_out;
	for (gsize i = 0; i < MIN(frames_in, frames_out); i++)
		if (memcmp(pcm + i * 4, b + i * 4, 4)) {
			first_diff = i;
			break;
		}
	printf("original %zu frames, played %zu frames, identical up to frame %zu\n", frames_in, frames_out,
	       MIN(first_diff, frames_in));
	if (frames_in != frames_out || first_diff < frames_in) {
		printf("FAIL: not sample-identical (a gap or overlap at %.4f s)\n", first_diff / 44100.0);
		failures++;
	}
	g_unlink(out);
	g_free(a);
	g_free(b);

	/* ReplayGain: the same track with track gain off, then on. */
	char *tone_path = g_build_filename(data, "Sine Ensemble", "Testing Tones", "03 Tone 3.flac", NULL);
	Track *tone = tags_read(tone_path);
	double rms[2];
	for (int mode = 0; mode < 2; mode++) {
		changes = 0;
		parts[0] = tone; /* on_next: nothing after it */
		parts[1] = parts[2] = NULL;
		p = player_new(&cb, NULL);
		player_set_option(p, "ao", "pcm");
		player_set_option(p, "ao-pcm-file", out);
		player_set_option(p, "ao-pcm-waveheader", "no");
		player_set_option(p, "audio-format", "s16");
		player_set_replaygain(p, mode, 0, FALSE);
		player_play(p, tone);
		g_main_loop_run(loop);
		player_free(p);
		g_file_get_contents(out, &b, &blen, NULL);
		double sum = 0;
		const gint16 *s16 = (const gint16 *)b;
		for (gsize i = 0; i < blen / 2; i++)
			sum += (double)s16[i] * s16[i];
		rms[mode] = sqrt(sum / (blen / 2));
		g_free(b);
		g_unlink(out);
	}
	double db = 20 * log10(rms[1] / rms[0]);
	printf("ReplayGain track gain %.2f dB (tag), measured %.2f dB\n", tone->rg_track_gain, db);
	if (fabs(db - tone->rg_track_gain) > 0.05) {
		printf("FAIL: ReplayGain not applied as tagged\n");
		failures++;
	}
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}

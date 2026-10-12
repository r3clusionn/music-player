/* Title formatting against foobar2000's behavior. */
#include "../src/format.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(const char *fmt, const Track *t, const char *want)
{
	Format *f = format_compile(fmt);
	FormatContext ctx = {.list_index = 7};
	const char *got = format_eval_static(f, t, &ctx);
	if (strcmp(got, want)) {
		printf("FAIL %-45s got \"%s\", want \"%s\"\n", fmt, got, want);
		failures++;
	}
	format_free(f);
}

int main(void)
{
	Track *t = track_new();
	t->path = g_strdup("/music/Boards of Canada/Geogaddi/03 Music Is Math.flac");
	t->title = g_strdup("Music Is Math");
	t->artist = track_intern("Boards of Canada");
	t->album = track_intern("Geogaddi");
	t->date = track_intern("2002-02-18");
	t->tracknumber = 3;
	t->totaltracks = 23;
	t->duration = 321.4;
	t->codec = track_intern("FLAC");
	t->bitrate = 912;
	t->samplerate = 44100;
	t->channels = 2;
	t->bits = 16;
	t->rg_album_gain = -6.5;

	check("%title%", t, "Music Is Math");
	check("%artist% - %title%", t, "Boards of Canada - Music Is Math");
	check("%album artist%", t, "Boards of Canada");     /* falls back to the artist */
	check("%tracknumber%", t, "03");                    /* padded like foobar2000 */
	check("%length%", t, "5:21");
	check("%year%", t, "2002");
	check("[%genre% / ]%album%", t, "Geogaddi");         /* empty field hides the block */
	check("[%date% / ]%album%", t, "2002-02-18 / Geogaddi");
	check("%title%[ // %track artist%]", t, "Music Is Math"); /* same artist: hidden */
	check("$if(%genre%,has genre,no genre)", t, "no genre");
	check("$if(%album%,%album%)", t, "Geogaddi");
	check("$if2(%genre%,%codec%)", t, "FLAC");
	check("$num(%tracknumber%,4)", t, "0003");
	check("$upper(%album%)", t, "GEOGADDI");
	check("$left(%artist%,6)", t, "Boards");
	check("$right(%artist%,6)", t, "Canada");
	check("$len(%album%)", t, "8");
	check("'[literal]' %%", t, "[literal] ");            /* %% is an empty field */
	check("%bitrate% kbps", t, "912 kbps");
	check("%channels%", t, "stereo");
	check("%replaygain_album_gain%", t, "-6.50 dB");
	check("%replaygain_track_gain%", t, "");
	check("%filename%", t, "03 Music Is Math");
	check("%directoryname%", t, "Geogaddi");
	check("%list_index%", t, "7");
	check("$if(%isplaying%,>,)", t, "");
	check("%unknown_field%x", t, "x");

	t->albumartist = track_intern("Various Artists");
	check("%title%[ // %track artist%]", t, "Music Is Math // Boards of Canada");

	g_free(t->title);
	t->title = NULL;
	check("%title%", t, "03 Music Is Math"); /* no title: the file name */

	char buf[32];
	format_duration(3725, buf, sizeof buf);
	if (strcmp(buf, "1:02:05")) {
		printf("FAIL format_duration(3725) = %s\n", buf);
		failures++;
	}
	track_unref(t);
	printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
	return failures != 0;
}

/* Playback with libmpv (audio only). mpv's playlist always holds the current
 * track and the next one, so track changes are gapless; the next one comes
 * from the app (playback order, queue, stop after current). */
#pragma once

#include "track.h"

typedef enum { PLAYER_STOPPED, PLAYER_PLAYING, PLAYER_PAUSED } PlayerState;

typedef struct {
	/* A track started (also on a gapless change); NULL when playback stopped. */
	void (*track_changed)(Track *track, gpointer data);
	void (*state_changed)(PlayerState state, gpointer data);
	/* The track to play after `current`, referenced, or NULL to stop after it. */
	Track *(*next_track)(Track *current, gpointer data);
	/* A file couldn't be played. */
	void (*error)(Track *track, const char *message, gpointer data);
} PlayerCallbacks;

typedef struct Player Player;

Player *player_new(const PlayerCallbacks *cb, gpointer data);
void player_free(Player *p);

void player_play(Player *p, Track *t);
void player_pause(Player *p, gboolean pause);
void player_toggle(Player *p);
void player_stop(Player *p);
/* Seconds from the start of the track (cue ranges included). */
void player_seek(Player *p, double seconds);
double player_position(Player *p);
PlayerState player_state(Player *p);
Track *player_current(Player *p);
/* Asks for the next track again (order, queue or stop-after changed). */
void player_requeue(Player *p);

/* Any mpv option (tests: ao=pcm). */
void player_set_option(Player *p, const char *name, const char *value);
void player_set_volume(Player *p, double percent);
double player_volume(Player *p);
/* mode: 0 off, 1 track, 2 album */
void player_set_replaygain(Player *p, int mode, double preamp_db, gboolean prevent_clipping);
/* gains in dB for 31 Hz .. 16 kHz; NULL or all zero turns the equalizer off. */
void player_set_equalizer(Player *p, const double gains[10]);
extern const int player_eq_bands[10];

/* "FLAC · 44.1 kHz · 16 bit · stereo · 1011 kbps" for the status bar. */
void player_audio_info(Player *p, char *buf, size_t size);

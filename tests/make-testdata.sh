#!/bin/sh
# Generates test music with ffmpeg into $1 (default build/testdata):
#   Sine Ensemble/Testing Tones   5 FLAC tracks, tags, ReplayGain, embedded cover
#   Gapless/Continuous            one continuous tone split sample-exactly into 3 FLACs,
#                                 plus the unsplit original (gapless.wav, outside the library)
#   Opus Band/Small Hours         2 Opus tracks with R128 gains, cover.jpg in the folder
#   AAC Artist/Lossy              1 AAC track in .m4a (this ffmpeg has no MP3 encoder)
#   Cue Image/Image.flac + .cue   one file, three cue tracks
#   untagged.wav                  no tags at all
# With "20k" as $2: also 20,000 one-second Opus files in $1/20k (400 artists x 10 albums x 5).
set -eu
OUT=${1:-build/testdata}
rm -rf "$OUT"
mkdir -p "$OUT"
cd "$OUT"
ff() { ffmpeg -nostdin -loglevel error -y "$@"; }

mkdir -p "Sine Ensemble/Testing Tones"
ff -f lavfi -i "testsrc2=size=600x600:rate=1" -frames:v 1 cover-src.png
notes="261.63 293.66 329.63 349.23 392.00"
n=1
for f in $notes; do
	gain=$(printf -- "-%d.%02d dB" $((n + 2)) $((n * 7)))
	ff -f lavfi -i "sine=frequency=$f:duration=12:sample_rate=44100" -i cover-src.png \
		-map 0 -map 1 -c:a flac -sample_fmt s16 -c:v png -disposition:v attached_pic \
		-metadata title="Tone $n ($f Hz)" -metadata artist="Sine Ensemble" \
		-metadata album="Testing Tones" -metadata album_artist="Sine Ensemble" \
		-metadata date=2024 -metadata genre=Electronic -metadata track="$n/5" \
		-metadata REPLAYGAIN_TRACK_GAIN="$gain" -metadata REPLAYGAIN_ALBUM_GAIN="-4.20 dB" \
		-metadata REPLAYGAIN_TRACK_PEAK=0.500000 -metadata REPLAYGAIN_ALBUM_PEAK=0.500000 \
		"Sine Ensemble/Testing Tones/0$n Tone $n.flac"
	n=$((n + 1))
done

# Gapless: a rising tone over 30 s, cut at sample 441000 and 882000.
mkdir -p Gapless/Continuous
ff -f lavfi -i "aevalsrc=0.4*sin(2*PI*(220+10*t)*t):s=44100:d=30" -ac 2 -c:a pcm_s16le gapless.wav
i=1
for range in "0:441000" "441000:882000" "882000:1323000"; do
	s=${range%:*} e=${range#*:}
	ff -i gapless.wav -af "atrim=start_sample=$s:end_sample=$e" -c:a flac \
		-metadata title="Part $i" -metadata artist=Gapless -metadata album=Continuous \
		-metadata track="$i/3" "Gapless/Continuous/0$i Part $i.flac"
	i=$((i + 1))
done

mkdir -p "Opus Band/Small Hours"
ff -f lavfi -i "testsrc=size=500x500:rate=1" -frames:v 1 "Opus Band/Small Hours/cover.jpg"
for i in 1 2; do
	ff -f lavfi -i "sine=frequency=$((300 + i * 50)):duration=8" -ar 48000 -ac 2 -c:a opus -strict -2 -b:a 96k \
		-metadata title="Hour $i" -metadata artist="Opus Band" -metadata album="Small Hours" \
		-metadata date=2019-03-01 -metadata track=$i -metadata R128_TRACK_GAIN=-1280 \
		"Opus Band/Small Hours/0$i Hour $i.opus"
done

mkdir -p "AAC Artist/Lossy"
ff -f lavfi -i "sine=frequency=500:duration=6" -ac 2 -c:a aac -b:a 192k \
	-metadata title="Compressed" -metadata artist="AAC Artist" -metadata album=Lossy -metadata track=1 \
	"AAC Artist/Lossy/01 Compressed.m4a"

mkdir -p "Cue Image"
ff -f lavfi -i "sine=frequency=330:duration=10" -f lavfi -i "sine=frequency=440:duration=10" \
	-f lavfi -i "sine=frequency=550:duration=10" -filter_complex "[0][1][2]concat=n=3:v=0:a=1" \
	-c:a flac "Cue Image/Image.flac"
cat > "Cue Image/Image.cue" <<'EOF'
REM GENRE Ambient
REM DATE 1999
PERFORMER "Cue Performer"
TITLE "Cue Album"
FILE "Image.flac" WAVE
  TRACK 01 AUDIO
    TITLE "First"
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    TITLE "Second"
    PERFORMER "Guest Singer"
    INDEX 01 00:10:00
  TRACK 03 AUDIO
    TITLE "Third"
    INDEX 01 00:20:00
EOF

ff -f lavfi -i "sine=frequency=700:duration=3" untagged.wav
rm cover-src.png
mkdir -p ../testdata-extra
mv gapless.wav ../testdata-extra/gapless.wav

if [ "${2:-}" = 20k ]; then
	mkdir -p 20k
	ff -f lavfi -i "sine=frequency=440:duration=1" -ar 48000 -ac 1 -c:a opus -strict -2 -b:a 24k ../testdata-extra/seed.opus
	# Same audio, different tags: remux with new metadata (fast, no encoding).
	seq 0 19999 | xargs -P "$(nproc)" -I{} sh -c '
		n={}; a=$((n / 50)); b=$((n / 5 % 10)); t=$((n % 5 + 1))
		d="20k/Artist $a/Album $b"; mkdir -p "$d"
		ffmpeg -nostdin -loglevel error -y -i ../testdata-extra/seed.opus -c copy \
			-metadata title="Song $n" -metadata artist="Artist $a" -metadata album="Album $a-$b" \
			-metadata date=$((1970 + a % 50)) -metadata genre="Genre $((a % 12))" -metadata track=$t \
			"$d/0$t Song $n.opus"'
fi
echo "test data in $PWD"

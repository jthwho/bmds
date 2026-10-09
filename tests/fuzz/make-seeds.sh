#!/bin/sh
# make-seeds.sh DIR: the decoders' fuzzing seeds (devplan/audio.md §19), made by ffmpeg: a quarter
# second of each format in its various forms, small so that each run is quick.
set -eu
out=${1:?usage: make-seeds.sh DIR}
mkdir -p "$out"
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }
st="-f lavfi -i sine=frequency=440:sample_rate=44100:duration=0.25 -f lavfi -i sine=frequency=880:sample_rate=44100:duration=0.25 -filter_complex [0][1]amerge=inputs=2"
mono() { echo "-f lavfi -i sine=frequency=440:sample_rate=$1:duration=0.25"; }
meta="-metadata title=Seed -metadata artist=BMDS -metadata album=Fuzz -metadata track=1"
# MP3: CBR with a LAME tag, VBR with a Xing table, no tag, MPEG-2 and 2.5, ID3v2.3 and 2.4; MP2.
ff $st $meta -c:a libmp3lame -b:a 64k "$out/cbr.mp3"
ff $st $meta -c:a libmp3lame -q:a 6 -id3v2_version 3 "$out/vbr.mp3"
ff $st -c:a libmp3lame -b:a 64k -write_xing 0 -id3v2_version 0 "$out/bare.mp3"
ff $(mono 22050) -c:a libmp3lame -b:a 32k "$out/mpeg2.mp3"
ff $(mono 8000) -c:a libmp3lame -b:a 8k "$out/mpeg25.mp3"
ff $st -c:a mp2 -b:a 128k "$out/layer2.mp2"
# Vorbis: stereo and mono, low and high rates.
ff $st $meta -c:a libvorbis -q:a 0 "$out/stereo.ogg"
ff $(mono 8000) -c:a libvorbis -q:a 0 "$out/mono8k.ogg"
# FLAC: 16-bit stereo, 24-bit, 8-bit mono, a seek table every 0.05 s.
ff $st $meta -c:a flac -sample_fmt s16 "$out/s16.flac"
ff $st -c:a flac -sample_fmt s32 -bits_per_raw_sample 24 "$out/s24.flac"
ff $(mono 8000) -c:a flac -sample_fmt s16 -bits_per_raw_sample 8 "$out/u8.flac"
ff $st -c:a flac -sample_fmt s16 -frame_size 1152 "$out/small-frames.flac"
# WAV, and an Ogg Opus (refused).
ff $st -t 0.02 -c:a pcm_s16le "$out/s16.wav"
ff $(mono 48000) -c:a libopus -b:a 16k "$out/opus.ogg"

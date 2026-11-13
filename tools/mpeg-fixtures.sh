#!/bin/sh
# The streams of tests/ps2/mpeg.c and what ffmpeg decodes them to (4:2:0
# planes, frame after frame): two frames of Big Buck Bunny, small.
#   tools/mpeg-fixtures.sh <big_buck_bunny_480p_h264.mov>
# (from https://download.blender.org/peach/bigbuckbunny_movies/, in a zip).
# Run from maxsdk's directory; needs ffmpeg.
#   a: MPEG-2, progressive, the default tables, 64 x 48
#   b: MPEG-2, interlaced (field DCT), alternate scan, the second VLC table,
#      the non-linear quantizer scale, 10-bit DC, its own matrices, 72 x 40
#      (not whole macroblocks)
#   c: MPEG-1, 64 x 48
#   d, e, f: as a, b (field prediction and field DCT), c with P and B pictures
#   mpeg_a.mpg: a in a program stream with LPCM sound; mpeg_a.pcm: its samples
set -e
[ -f "$1" ] || { echo "usage: $0 <big buck bunny movie file>" >&2; exit 2; }
D=tests/ps2/data
# two frames half a second apart
src() { echo "-vf fps=2,scale=$1,setpts=N/25/TB -r 25 -frames:v 2 -an -pix_fmt yuv420p -g 1 -bf 0"; }
M=8,17,18,19,21,23,25,27,17,18,19,21,23,25,27,28,20,21,22,23,24,26,28,30,21,22,23,24,26,28,30,32,22,23,24,26,28,30,32,35,23,24,26,28,30,32,35,38,25,26,28,30,32,35,38,41,27,28,30,32,35,38,41,45
ffmpeg -v error -y -ss 157 -i "$1" $(src 64:48) -c:v mpeg2video -q:v 3 -f mpeg2video $D/mpeg_a.m2v
ffmpeg -v error -y -ss 157 -i "$1" $(src 72:40) -c:v mpeg2video -q:v 4 -flags +ildct+ilme -alternate_scan 1 \
  -intra_vlc 1 -non_linear_quant 1 -qmax 28 -dc 10 -intra_matrix $M -f mpeg2video $D/mpeg_b.m2v
ffmpeg -v error -y -ss 157 -i "$1" $(src 64:48) -c:v mpeg1video -q:v 3 -f mpeg1video $D/mpeg_c.m1v
for f in mpeg_a.m2v mpeg_b.m2v mpeg_c.m1v; do
  ffmpeg -v error -y -i $D/$f -f rawvideo -pix_fmt yuv420p $D/${f%.*}.yuv
done
# with P and B pictures: nine frames that follow each other (I B B P B B P B B
# in the order of display)
mov() { echo "-vf scale=$1 -frames:v 9 -an -pix_fmt yuv420p -g 9 -bf 2"; }
ffmpeg -v error -y -ss 163 -i "$1" $(mov 64:48) -c:v mpeg2video -q:v 3 -f mpeg2video $D/mpeg_d.m2v
ffmpeg -v error -y -ss 163 -i "$1" $(mov 72:40) -c:v mpeg2video -q:v 3 -flags +ildct+ilme \
  -alternate_scan 1 -intra_vlc 1 -f mpeg2video $D/mpeg_e.m2v
ffmpeg -v error -y -ss 163 -i "$1" $(mov 64:48) -c:v mpeg1video -q:v 3 -f mpeg1video $D/mpeg_f.m1v
for f in mpeg_d.m2v mpeg_e.m2v mpeg_f.m1v; do
  ffmpeg -v error -y -i $D/$f -f rawvideo -pix_fmt yuv420p $D/${f%.*}.yuv
done
# a as a program stream with sound beside it (LPCM, a tone), and that sound
ffmpeg -v fatal -y -i $D/mpeg_a.m2v -f lavfi -i sine=frequency=440:sample_rate=48000 -t 0.08 \
  -c:v copy -c:a pcm_s16be -ac 2 -f vob $D/mpeg_a.mpg
ffmpeg -v error -y -i $D/mpeg_a.mpg -vn -f s16be $D/mpeg_a.pcm
ls -l $D

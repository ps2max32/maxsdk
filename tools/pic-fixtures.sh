#!/bin/sh
# The pictures of tests/ps2/pic.c and what ffmpeg decodes them to: a frame of
# Big Buck Bunny, small.
#   tools/pic-fixtures.sh <big_buck_bunny_480p_h264.mov>
# Run from maxsdk's directory; needs ffmpeg, and python3 with Pillow (for the
# kinds of JPEG ffmpeg does not write).
#   pic_444.jpg, pic_420.jpg (50 x 38: not whole units), pic_422.jpg, pic_grey.jpg,
#   pic_rst.jpg (4:2:0 with a restart marker every two units)
#     with .yuv: ffmpeg's Y, Cb, Cr planes (full range), the chroma as coded
#   pic_rgb.png, pic_rgba.png (alpha: a ramp), pic_pal.png, pic_grey.png
#     with .rgba: ffmpeg's pixels
set -e
[ -f "$1" ] || { echo "usage: $0 <big buck bunny movie file>" >&2; exit 2; }
D=tests/ps2/data
one() { ffmpeg -v error -y -ss 157 -i "$1" -frames:v 1 -an "$@"; }
jpg() { # name, size, pixel format
  ffmpeg -v error -y -ss 157 -i "$SRC" -frames:v 1 -an -vf scale=$2 -pix_fmt $3 -q:v 3 $D/pic_$1.jpg
  ffmpeg -v error -y -i $D/pic_$1.jpg -f rawvideo -pix_fmt $3 $D/pic_$1.yuv
}
png() { # name, filter, pixel format
  ffmpeg -v error -y -ss 157 -i "$SRC" -frames:v 1 -an -vf "$2" -pix_fmt $3 $D/pic_$1.png
  ffmpeg -v error -y -i $D/pic_$1.png -f rawvideo -pix_fmt rgba $D/pic_$1.rgba
}
SRC=$1
jpg 444 48:32 yuvj444p
jpg 420 50:38 yuvj420p
jpg 422 48:32 yuvj422p
png rgb scale=48:32 rgb24
png rgba "scale=48:32,format=rgba,geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='X*255/47'" rgba
png pal scale=48:32 pal8
png grey scale=48:32 gray
python3 - <<'PY'
from PIL import Image
d = 'tests/ps2/data/'
im = Image.open(d + 'pic_rgb.png')
im.convert('L').save(d + 'pic_grey.jpg', quality=85)
im.save(d + 'pic_rst.jpg', quality=85, subsampling='4:2:0', restart_marker_blocks=2)
PY
ffmpeg -v error -y -i $D/pic_grey.jpg -f rawvideo -pix_fmt gray $D/pic_grey.yuv
ffmpeg -v error -y -i $D/pic_rst.jpg -f rawvideo -pix_fmt yuvj420p $D/pic_rst.yuv
ls -l $D/pic_*

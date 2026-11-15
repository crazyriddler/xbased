#!/bin/sh
# Real-ESRGAN for the packages (automatic texture upscaling, native/app/texauto.c): the program for Windows and
# for Linux and the realesrgan-x4plus model, from its release, into OUT/windows/realesrgan and
# OUT/linux/realesrgan, each with the licences. Unmodified; not kept in the repository (about 40 MB each).
#   fetch_realesrgan.sh OUT
# Then: the Windows package gets OUT/windows/realesrgan beside xbased.exe; package_linux.sh takes
# REALESRGAN_DIR=OUT/linux/realesrgan.
set -eu
OUT=${1:?where to put it}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
REL=https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0
mkdir -p "$OUT"
for os in windows ubuntu; do
  d=$OUT/$( [ $os = ubuntu ] && echo linux || echo windows )/realesrgan
  mkdir -p "$d/models"
  z=$OUT/realesrgan-$os.zip
  curl -fL -o "$z" "$REL/realesrgan-ncnn-vulkan-20220424-$os.zip"
  if [ $os = windows ]; then
    unzip -o -q -j "$z" realesrgan-ncnn-vulkan.exe vcomp140.dll -d "$d"
  else
    unzip -o -q -j "$z" realesrgan-ncnn-vulkan -d "$d"
    chmod 755 "$d/realesrgan-ncnn-vulkan"
  fi
  unzip -o -q -j "$z" models/realesrgan-x4plus.bin models/realesrgan-x4plus.param -d "$d/models"
  cp "$REPO/licenses/realesrgan.license.txt" "$d/LICENSE.txt"
  rm -f "$z"
done

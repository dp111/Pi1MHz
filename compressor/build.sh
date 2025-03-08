#!/bin/sh


if [ "$#" -ne 1 ]; then
  echo "Usage: $0 <path_to_tbc_file>"
  exit 1
fi

#cleanup
rm frame.yuv
rm framecor.lz4
rm framecor.lz

rm index.dat
rm frames*.dat

tbc_file=$1
length=0

for i in $(seq 1 54000)
do

 /mnt/c/Archlinux/ld-decode/tools/ld-chroma-decoder/ld-chroma-decoder --decoder transform3d -p y4m -s $i -l 1 -q "$tbc_file"  | ffmpeg -hide_banner -loglevel error -i - -flags +ilme+ildct -pix_fmt yuv422p -color_primaries bt470bg -color_trc bt709 -colorspace bt470bg -color_range tv -vf setdar=4/3,setfield=tff,scale=768x576 -c:v rawvideo frame.yuv

#truncate :
##dd if=frame.yuv iflag=skip_bytes,count_bytes,fullblock bs=4096 skip=36 count=884736 of=framecor.yuv

#compress
lz4 -12 frame.yuv framecor.lz4

#there is a 7 byte header which we don't need so remove that

dd if=framecor.lz4 iflag=skip_bytes,count_bytes,fullblock bs=4096 skip=7 of=framecor.lz status=none

divided_i=$((i / 4096))

cat framecor.lz >>"frames$divided_i.dat"

 # Append the length to index.dat as a uint32
 # printf "%08x" $length | xxd -r -p >> index.dat

  # Append the length to index.dat as a uint32 in little-endian format
 # printf "%08x" $length | xxd -r -p | xxd -e -r >> index.dat

  # Append the length to index.dat as a uint32 in little-endian format
  printf "%08x" $file_length | sed 's/../& /g' | awk '{print $4 $3 $2 $1}' | xxd -r -p >> index.dat

# Get the length of framecor.lz
file_length=$(stat -c%s "framecor.lz")

length=$((length + file_length))

echo "frame $i length: $file_length total length: $length"
# Reset length every 4096 iterations
if [ $((i % 4096)) -eq 0 ]; then
    length=0
    echo "frame $i done"
fi

#cleanup
rm frame.yuv
rm framecor.lz4
rm framecor.lz

done

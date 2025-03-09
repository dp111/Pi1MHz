#!/bin/sh


if [ "$#" -ne 2 ]; then
  echo "Usage: $0 <path_to_tbc_file> <start_frame/4096>"
  exit 1
fi


tbc_file=$1
length=0
start=$2
start=$((start*4096))
last=$((start+4095))
if [ $((last)) -gt 54000 ]; then
    last=54000
fi
divided_i=$((start / 4096))
#cleanup
rm frame$divided_i.yuv
rm framecor$divided_i.lz4
rm framecor$divided_i.lz

rm index$divided_i.dat
rm frames$divided_i.dat

for i in $(seq $start $last)
do

ld-decode/tools/ld-chroma-decoder/ld-chroma-decoder --decoder transform3d -p y4m -s $i -l 1 -q "$tbc_file"  | ffmpeg -hide_banner -loglevel error -i - -flags +ilme+ildct -pix_fmt yuv422p -color_primaries bt470bg -color_trc bt709 -colorspace bt470bg -color_range tv -vf setdar=4/3,setfield=tff,scale=768x576 -c:v rawvideo "frame$divided_i.yuv"

#truncate :
##dd if=frame.yuv iflag=skip_bytes,count_bytes,fullblock bs=4096 skip=36 count=884736 of=framecor.yuv

#compress
lz4 -12 "frame$divided_i.yuv" "framecor$divided_i.lz4"

#there is a 7 byte header which we don't need so remove that

dd if="framecor$divided_i.lz4" iflag=skip_bytes,count_bytes,fullblock bs=4096 skip=7 of="framecor$divided_i.lz" status=none

cat "framecor$divided_i.lz" >>"frames$divided_i.dat"

 # Append the length to index.dat as a uint32
# printf "%08x" $length | xxd -r -p >> indexa.dat

  # Append the length to index.dat as a uint32 in little-endian format
 # printf "%08x" $length | xxd -r -p | xxd -e -r >> index.dat

  # Append the length to index.dat as a uint32 in little-endian format
  printf "%08x" $length | sed 's/../& /g' | awk '{print $4 $3 $2 $1}' | xxd -r -p >> index$divided_i.dat

# Get the length of framecor.lz
file_length=$(stat -c%s "framecor$divided_i.lz")

length=$((length + file_length))

echo "frame $i length: $file_length total length: $length"

#cleanup
rm frame$divided_i.yuv
rm framecor$divided_i.lz4
rm framecor$divided_i.lz

done

echo "frame $i done"

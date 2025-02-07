Doomsday notes




how to create the compressed video files

/mnt/c/Archlinux/ld-decode/tools/ld-chroma-decoder/ld-chroma-decoder --decoder transform3d -p y4m -s 3000 -l 1 -q /mnt/s/domsday/south.tbc  | ffmpeg -i - -c:v v210 -f mov -top 1 -vf setfield=tff -flags +ilme+ildct -pix_fmt yuv422p -color_primaries bt470bg -color_trc bt709 -colorspace bt470bg -color_range tv -vf setdar=4/3,setfield=tff -s768x576 -c:v rawvideo image3000.yuv


truncate :
dd if=image3000d.yuv iflag=skip_bytes,count_bytes,fullblock bs=4096 skip=36 count=884736 of=image3000cor.yuv

compress
lz4 -12 image3000d.yuv image300d.lz4
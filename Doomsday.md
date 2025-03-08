Doomsday notes




TODO notes in no order

Check mouse offsets and fix VFS ROM bug
*MOUSE appears to not always work, see if original VFS has this bug and fix
See if boot time can be improved
LZ4 decompressor speedup memcpy
YUV frame display
More Fcode support (including VP4 and VP5 )
See if power consumption of the Pi can be improved
check and fix YUV scaling
Audio support HDMI and beeb ?
composite video out ?
50Hz HDMI modes
Check 1MHz timing / add DMBs ( sometimes bad FSMAP)
Check beeb screen offset vertical appears off by a pixel ?
Menu system



how to create the compressed video files

 /mnt/c/Archlinux/ld-decode/tools/ld-chroma-decoder/ld-chroma-decoder --decoder transform3d -p y4m -s 3000 -l 1 -q /mnt/s/domsday/south.tbc  | ffmpeg -i - -c:v v210 -f mov -top 1 -vf setfield=tff -flags +ilme+ildct -pix_fmt yuv422p -color_primaries bt470bg -color_trc bt709 -colorspace bt470bg -color_range tv -vf setdar=4/3,setfield=tff,scale=768x576 -c:v rawvideo image3000.yuv


truncate :
dd if=image3000d.yuv iflag=skip_bytes,count_bytes,fullblock bs=4096 skip=36 count=884736 of=image3000cor.yuv

compress
lz4 -12 image3000cor.yuv image3000cor.lz4

there is a 7 byte header which we don't need so remove that

dd if=image3000cor.lz4 iflag=skip_bytes,count_bytes,fullblock bs=4096 skip=7 of=image3000cor.lz


Fcodes that need supporting to boot

fcodeReadBuffer
 0x0d 0x00 0x00 0x00 0x00 0x00 fcodeClearBuffer
F-Code: Received bytes: 0x49 0x30 0x0d
F-Code: Received F-Code 0x49 = Local front panel buttons disabled
fcodeReadBuffer
 0x41 0x0d 0x00 0x00 0x00 0x00 fcodeClearBuffer
F-Code: Received bytes: 0x4a 0x30 0x0d
F-Code: Received F-Code 0x4a = Remote control disabled for player control
fcodeReadBuffer
 0x41 0x0d 0x00 0x00 0x00 0x00 fcodeClearBuffer
F-Code: Received bytes: 0x24 0x30 0x0d
F-Code: Received F-Code 0x24 = Replay switch disable

fcodeReadBuffer
 0x41 0x0d 0x00 0x00 0x00 0x00 fcodeClearBuffer
F-Code: Received bytes: 0x3f 0x55 0x0d
F-Code: Received F-Code 0x3f = User code request
<UCD>fcodeReadBuffer
 0x55 0x31 0x3d 0x30 0x36 0x36 fcodeClearBuffer
fcodeReadBuffer
 0x0d 0x31 0x3d 0x30 0x36 0x36 fcodeClearBuffer
F-Code: Received bytes: 0x56 0x50 0x33 0x0d
F-Code: Received F-Code 0x56 = Video overlay mode 3 (Hard-keyed)
fcodeReadBuffer
 0x56 0x50 0x33 0x0d 0x36 0x36 fcodeClearBuffer
F-Code: Received bytes: 0x56 0x50 0x33 0x0d
F-Code: Received F-Code 0x56 = Video overlay mode 3 (Hard-keyed)
plot 4 0 964
plot 99 1279 59
fcodeReadBuffer
 0x56 0x50 0x33 0x0d 0x36 0x36 fcodeClearBuffer
F-Code: Received bytes: 0x41 0x31 0x0d
F-Code: Received F-Code 0x41 = Audio-1 on                <- TODO ->
F-Code: Received bytes: 0x42 0x31 0x0d
F-Code: Received F-Code 0x42 = Audio-2 on               <- TODO ->
fcodeReadBuffer
 0x41 0x0d 0x33 0x0d 0x36 0x36 fcodeClearBuffer
F-Code: Received bytes: 0x46 0x35 0x31 0x52 0x0d
F-Code: Received F-Code 0x46 = Load/Goto picture number : 51 op: R = Still picture   <- TODO ->
fcodeReadBuffer
 0x41 0x30 0x0d 0x0d 0x36 0x36 fcodeClearBuffer
F-Code: Received bytes: 0x45 0x31 0x0d
F-Code: Received F-Code 0x45 = Video on
F-Code: Received bytes: 0x46 0x31 0x36 0x30 0x30 0x53 0x0d
F-Code: Received F-Code 0x46 = Load/Goto picture number : 1600 op: S = Stop Register  <- TODO ->
F-Code: Received bytes: 0x4e 0x0d
F-Code: Received F-Code 0x4e = Play forward
fcodeReadBuffer
 0x41 0x32 0x0d 0x0d 0x36 0x36 fcodeClearBuffer
fcodeReadBuffer
 0x0d 0x32 0x0d 0x0d 0x36 0x36 fcodeClearBuffer
F-Code: Received bytes: 0x41 0x30 0x0d
F-Code: Received F-Code 0x41 = Audio-1 off
F-Code: Received bytes: 0x42 0x30 0x0d
F-Code: Received F-Code 0x42 = Audio-2 off

fcodeReadBuffer
 0x0d 0x32 0x0d 0x0d 0x36 0x36 fcodeClearBuffer
F-Code: Received bytes: 0x46 0x31 0x38 0x37 0x39 0x32 0x52 0x0d
F-Code: Received F-Code 0x46 = Load/Goto picture number : 18792 op: R = Still picture  <- TODO ->
fcodeReadBuffer
 0x41 0x30 0x0d 0x0d 0x36 0x36 fcodeClearBuffer
fcodeReadBuffer
 0x0d 0x30 0x0d 0x0d 0x36 0x36 fcodeClearBuffer
F-Code: Received bytes: 0x45 0x31 0x0d
F-Code: Received F-Code 0x45 = Video on

fcodeReadBuffer
 0x0d 0x30 0x0d 0x0d 0x36 0x36 fcodeClearBuffer
F-Code: Received bytes: 0x46 0x31 0x38 0x37 0x39 0x31 0x52 0x0d
F-Code: Received F-Code 0x46 = Load/Goto picture number : 18791 op: R = Still picture

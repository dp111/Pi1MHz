/* Video frame compressor

Takes YUV frames and compresses them in a way that is very fast to decode

It aims to compress to 66% of the original size so that data can be read from SDCARD

Compression stream is 16bit values

Block copy ( bias to have a greater backwards search distance than copy length)

Notation

Y1 u y2 v each which are 8 bits ( 4:2:2)


Decompression stream format is always 32 bits.( 2 pixels at a time)



Compression stream format :

Bit 15 bit 14
  0      0   Blockcopy [11 bits of backwards distance] [3 bits of copy length ( 1 to 8)] 12.5% - 50% compression
  0      1   Delta [Y1 4 bits -8/+7 ] [U 3 bits -4/+3] [Y2 4 bits -8/+7] [V 3 bits -4/+3] 50%

  1      1   0 New pixel + delta pixel [Y1 3 bits -4/+3 ] [U 3 bits -4/+3] [Y2 4 bits -8/+7] [V 3 bits -4/+3] [32 bit of new pixel] 66.6%
  1      0   New pixel small [y1 ] [u] [6 bit delta of y1] [v] [32 bit of new pixel] 100%
  1      1   1  0 unused 12bits
  1      1   1  1 [12 bit ( !0xFFF) spare]
  1      1   1  1 [12bits 1] [32 bit of new pixel] 150%

  Do something better with the spare 13bits

ld-chrome-decode settings :

ld-chroma-decoder --decoder transform3d -p yuv -s 1000 -l 1 /mnt/s/domsday/ds2south.tbc  image1

Info: Input video of 1135 x 625 will be colourised and trimmed to 928 x 576 YUV444P16 frames
Info: Using 16 threads
Info: Processing from start frame # 1000 with a length of 1 frames
Info: Processing complete - 1 frames in 0.257 seconds ( 3.89105 FPS )



/mnt/c/Archlinux/ld-decode/tools/ld-chroma-decoder/ld-chroma-decoder --decoder transform3d -p y4m -s 3000 -l 1 -q /mnt/s/domsday/south.tbc  | ffmpeg -i - -c:v v210 -f mov -top 1 -vf setfield=tff -flags +ilme+ildct -pix_fmt yuv422p -color_primaries bt470bg -color_trc bt709 -colorspace bt470bg -color_range tv -vf setdar=4/3,setfield=tff -s768x576 -c:v rawvideo image3000.yuv

*/

#define XSIZE 768
#define YSIZE 576

#include <stdio.h>

#include <stdbool.h>

#include <stdlib.h>
#include <string.h>

#define BLOCKPOSBITS 13
#define BLOCKLENBITS 1

#define MAXBLOCKPOS ((1<<BLOCKPOSBITS)+1)
#define MAXBLOCKCOPY ((1<<BLOCKLENBITS)+1)

    char yuvdatainput[ 1280 * 720 * 3 *2 ]; // 720p YUV data
    char yuvdata[ 1280 * 720 * 2 ]; // 720p YUV data

    char ydata[ 1280 * 720 ]; // 720p YUV data
    char udata[ 1280 * 720 ]; // 720p YUV data
    char vdata[ 1280 * 720 ]; // 720p YUV data

int writebmp(char *fname, int sx, int sy)
{

	FILE *fptr;
	long fs,is,a,b; //filesize, image size, counters
	int yr;
	char s[255];
	char n,o,p; //color save values

	//save the current color and restore at the end of the function
	//usually this is the last call to a viewport, but sometimes not.
	//n=cur_red[vp]; o=cur_green[vp]; p=cur_blue[vp];


	//minimalist graphics support for CLI's
	char bmp_hdr[54]={	0x42,0x4D,0x46,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x36,0x00, 0x00,0x00,
				0x28,0x00,0x00,0x00, 0x02,0x00,0x00,0x00, 0x02,0x00,0x00,0x00, 0x01,0x00,
				0x18,0x00,0x00,0x00, 0x00,0x00,0x10,0x00, 0x00,0x00,0x13,0x00, 0x00,0x00,
				0x13,0x0B,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00};

	fs=54+(sx*sy*3); //filesize is the diamentions of the array multiplied by 3 bytes (R,G and B bytes)
	is=(sx*sy*3);

	//put RED registration square in top-left hand corner at (20,20), height =20, width=20
	//for (a=0;a<20;a++)
	//	for (b=0;b<20;b++)
	//		red[a+20][b+20][vp]=255;

	//setcolor(vp,255,255,255);
	//textout(vp,20,20,"Reg",1);

	//Stanford Systems copyright notice
       	yr=2016;
	//sprintf(s,"%d (c) Copyright Stanford Systems",yr);
	//textout(vp,sx/2-(8*strlen(s))/2,sy-10,s,1);

	memcpy((void *)&bmp_hdr[2],(void *)&fs,4);
	memcpy((void *)&bmp_hdr[18],(void *)&sx,4);
	memcpy((void *)&bmp_hdr[22],(void *)&sy,4);
	memcpy((void *)&bmp_hdr[34],(void *)&is,4);

	if ((fptr=fopen(fname,"wb"))!=NULL)
	{
		fwrite(bmp_hdr,54,1,fptr);

		for (b=sy;b>0;b--)
			for (a=0;a<sx;a++)
			{
                int pixel = b*sx + a;
				fwrite(&ydata[pixel],1,1,fptr); // blue
				fwrite(&ydata[pixel],1,1,fptr); // green
				fwrite(&ydata[pixel],1,1,fptr); // red
			}
	}
	else
	{
		printf("failed to open BMP file.\n");
		return -1; //error code indicating file could not be opened
	}

	fclose(fptr);

	//restore any viewport vars we changed here
	//cur_red[vp]=n; cur_green[vp]=o; cur_blue[vp]=o;

	return 1;
}

int bytediff( int a, int b, int shift )
{
    return  ( ((a >>shift) & 0xff) - ((b >>shift) & 0xff) );
}


int main( int argc , char *argv[]  )
{
    // 928x576

    int yuvsize;
    int yuvpointer = 0;
    char *compressed; // compressed data
    int compressedsize=0;

    printf("YUV Compressor\n");

    // open file for reading
    FILE *fp = fopen( argv[1] , "rb" );
    if( fp == NULL )
    {
        printf("Error opening file\n");
        return 1;
    }

    // create output file
    FILE *out = fopen( argv[2] , "wb" );
    if( out == NULL )
    {
        printf("Error opening output file\n");
        return 1;
    }

    // read in the file
    yuvsize = fread( yuvdatainput , 1 , 1280 * 720 * 2 *3 , fp );

    printf("Read %d bytes\n", yuvsize);

    fclose( fp );

    yuvpointer = 0x24 ; // skip header

    for(int i = 0; i < XSIZE * YSIZE; i++)
    {
        ydata[i] = yuvdatainput[yuvpointer++];
        yuvdata[i*2] =  ydata[i];

    }

    for(int i = 0; i < (XSIZE * YSIZE)/2; i++)
    {
        udata[i] = yuvdatainput[yuvpointer++];
        yuvdata[i*4 + 1] = udata[i];
    }

    for(int i = 0; i < (XSIZE * YSIZE)/2; i++)
    {
        vdata[i] = yuvdatainput[yuvpointer++];
        yuvdata[i*4 + 3] = vdata[i];
    }

    writebmp("y.bmp", XSIZE, YSIZE);

    // create output file
    FILE *outdata = fopen( "ydata" , "wb" );
    if( outdata == NULL )
    {
        printf("Error opening output file\n");
        return 1;
    }
    fwrite( ydata , 1 , 928*576 , outdata ); fclose(outdata);

    // create output file
    outdata = fopen( "udata" , "wb" );
    if( outdata == NULL )
    {
        printf("Error opening output file\n");
        return 1;
    }
    fwrite( udata , 1 , 928*576/2 , outdata ); fclose(outdata);


    // create output file
    outdata = fopen( "vdata" , "wb" );
    if( outdata == NULL )
    {
        printf("Error opening output file\n");
        return 1;
    }
    fwrite( vdata , 1 , 928*576/2 , outdata ); fclose(outdata);

    // create output file
    outdata = fopen( "yuvdata" , "wb" );
    if( outdata == NULL )
    {
        printf("Error opening output file\n");
        return 1;
    }
    fwrite( yuvdata , 1 , 928*576*2 , outdata ); fclose(outdata);


    compressed = malloc( 914*575*2*2);
    if (compressed == NULL)
    {
        printf("Error allocating memory\n");
        return 1;
    }


    yuvsize = XSIZE * YSIZE/2;
    yuvpointer = 0;
    int *yuvdata_32 = (int *)yuvdata;
    // compress the data
    int blocksfoundcount = 0;
    int deltashortcount = 0;
    do {
        bool compressmodefound = false;
        // lets see if we can do a block copy

        // find the longest block copy
        int blockcopylength = -1;
        int blockcopydistance = 0;
        int blockcopytestpos = yuvpointer - MAXBLOCKPOS;
        if (blockcopytestpos < 0)
        {
            blockcopytestpos = 0;
        }
        if (yuvpointer != 0)
        {
            for( int i = blockcopytestpos; i < yuvpointer; i++ )
            {
                if( yuvdata_32[i] == yuvdata_32[yuvpointer] )
                {
                    int j = 1;
                    while( yuvdata_32[ i + j] == yuvdata_32[yuvpointer + j ] )
                    {
                        j++;
                    }
                    if (j > MAXBLOCKCOPY)
                    {
                        blockcopylength = MAXBLOCKCOPY;
                    }
                    if( j >= blockcopylength )
                    {
                        // use >= in the above as the closer to the end of the buffer the better as it is more likely to in
                        // the decoder cache
                        blockcopylength = j;
                        blockcopydistance =  yuvpointer - i ;
                    }
                }
            }
        }
        if (blockcopylength > 0)
        {
            if (blockcopylength>1)
                printf("Block copy %d %d pos (%d) \n", blockcopydistance, blockcopylength, yuvpointer);
            // write out the block copy
            compressed[compressedsize++] = 0x00 | (blockcopydistance >> BLOCKLENBITS);
            compressed[compressedsize++] = ((blockcopydistance & (0xff>>BLOCKLENBITS) ) << BLOCKLENBITS) | (blockcopylength - 1);
            yuvpointer+=blockcopylength;
            blocksfoundcount++;
            compressmodefound   = true;
        }
        if ((!compressmodefound) && (yuvpointer != 0))
            {
                // lets see if we can do a delta  ******* TODO Sort out yuv delta positions *********
                int lastword = yuvdata_32[yuvpointer-1];
                int nextword = yuvdata_32[yuvpointer];
                int diff;
                int delta = 0x4000;
                diff = bytediff(lastword,nextword,24);
                if (diff < 4 && diff >= -4)
                {
                   delta |= (diff & 0x03) << 11;
                   diff = bytediff(lastword,nextword,16);
                   if (diff < 8 && diff >= -8)
                    {
                        delta |= (diff & 0x0f) << 7;
                        diff = bytediff(lastword,nextword,8);
                        if (diff < 4 && diff >= -4)
                        {
                            delta |= (diff & 0x07) << 3;
                            diff = bytediff(lastword,nextword,0);
                            if (diff < 8 && diff >= -8)
                            {
                                delta |= (diff & 0x0f);
                                compressed[compressedsize++] = delta>>8;
                                compressed[compressedsize++] = delta & 0xff;
                                yuvpointer+=1;
                                compressmodefound = true;
                                deltashortcount++;
                            }
                        }
                    }
                }
            }

        if (!compressmodefound)
            {
               // if (yuvpointer !=0)
               //     printf("No compress mode found %x %x\n", yuvdata_32[yuvpointer-1], yuvdata_32[yuvpointer]);
              //  printf("No compress mode found\n");
                yuvpointer++;
                compressed[compressedsize++] = 0x00;
                compressed[compressedsize++] = 0x00;
                compressed[compressedsize++] = 0x00;
                compressed[compressedsize++] = 0x00;
                compressed[compressedsize++] = 0x00;
                compressed[compressedsize++] = 0x00;
             }
#if 0
        if (compressedsize >= yuvsize) {
            printf("Compressed data buffer overflow\n");
            fclose(out);
            return 1;
        }
 #endif

    } while ( yuvpointer < yuvsize );
    printf("Blocks found %d\n", blocksfoundcount);
    printf("Deltas found %d\n", deltashortcount);
    printf("yuv size %d\n", yuvsize*4);
    printf("Compressed size %d\n", compressedsize);
    printf("Compression ratio %f\n", (float)compressedsize/(float)(yuvsize*4));
    // write out the compressed data
#if 0
    if (compressedsize > 0)
        for(int i = 0; i < compressedsize; i++)
        {
            printf("%02x ", compressed[i]);
        }
#endif

    fwrite( compressed , 1 , compressedsize , out );
    fclose( out );
    return 0;

}

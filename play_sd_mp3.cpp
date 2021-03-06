/*
	Helix library Arduino interface

	Copyright (c) 2014 Frank Bösing

	This library is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This library is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this library.  If not, see <http://www.gnu.org/licenses/>.

	The helix decoder itself as a different license, look at the subdirectories for more info.

	Diese Bibliothek ist freie Software: Sie können es unter den Bedingungen
	der GNU General Public License, wie von der Free Software Foundation,
	Version 3 der Lizenz oder (nach Ihrer Wahl) jeder neueren
	veröffentlichten Version, weiterverbreiten und/oder modifizieren.

	Diese Bibliothek wird in der Hoffnung, dass es nützlich sein wird, aber
	OHNE JEDE GEWÄHRLEISTUNG, bereitgestellt; sogar ohne die implizite
	Gewährleistung der MARKTFÄHIGKEIT oder EIGNUNG FÜR EINEN BESTIMMTEN ZWECK.
	Siehe die GNU General Public License für weitere Details.

	Sie sollten eine Kopie der GNU General Public License zusammen mit diesem
	Programm erhalten haben. Wenn nicht, siehe <http://www.gnu.org/licenses/>.

	Der Helixdecoder selbst hat eine eigene Lizenz, bitte für mehr Informationen
	in den Unterverzeichnissen nachsehen.

 */

 /* The Helix-Library is modified for Teensy 3.1 */



#include "play_sd_mp3.h"


#define MP3_SD_BUF_SIZE	2048 								//Enough space for a complete stereo frame
#define MP3_BUF_SIZE	(MAX_NCHAN * MAX_NGRAN * MAX_NSAMP) //MP3 output buffer

#define DECODE_NUM_STATES 2									//How many steps in decode() ?



static File				file;

static uint8_t			*sd_buf; //decode
static uint8_t			*sd_p; 	 //decode
static int				sd_left; //decode
 
static short			*buf[2]; //output buffers
static size_t			decoded_length[2];
static size_t			decoding_block;
static unsigned int		decoding_state; //state 0: read sd, state 1: decode

static size_t 			size_id3;
//static uint32_t 		firstChunk, lastChunk;	//for MP4/M4A //TODO: use for ID3 too

static uint32_t			decode_cycles_max;
static uint32_t			decode_cycles_max_sd;

static unsigned int		playing;

static HMP3Decoder		hMP3Decoder;
static MP3FrameInfo		mp3FrameInfo;


static void mp3stop(void);
static void decode(void);

void AudioPlaySdMp3::stop(void)
{
	mp3stop();
}

bool AudioPlaySdMp3::pause(bool paused)
{
	if (playing) {
		if (paused) playing = 2; 
		else playing = 1;
	}
	return (playing == 2); 
}


bool AudioPlaySdMp3::isPlaying(void)
{
	return (playing > 0);
}

uint32_t AudioPlaySdMp3::positionMillis(void)
{
	return (AUDIO_SAMPLE_RATE_EXACT / 1000) * samples_played;
}

uint32_t AudioPlaySdMp3::lengthMillis(void)
{
//This is an estimate, takes not into account VBR, but better than nothing:
	if (playing) {
		return (file.size() - size_id3) / (mp3FrameInfo.bitrate / 8 ) * 1000;
	}
	else return 0;
}

uint32_t AudioPlaySdMp3::bitrate(void)
{
	if (playing) {
		return mp3FrameInfo.bitrate;
	}
	else return 0;
}

void AudioPlaySdMp3::processorUsageMaxResetDecoder(void){
	__disable_irq();
	decode_cycles_max = 0;
	decode_cycles_max_sd = 0;
	__enable_irq();
};

float AudioPlaySdMp3::processorUsageMaxDecoder(void){
	//this is somewhat incorrect, it does not take the interruptions of update() into account - 
	//therefore the returned number is too high.
	//Todo: better solution
	return (decode_cycles_max / (0.026*F_CPU)) * 100;
};

float AudioPlaySdMp3::processorUsageMaxSD(void){
	//this is somewhat incorrect, it does not take the interruptions of update() into account - 
	//therefore the returned number is too high.
	//Todo: better solution
	return (decode_cycles_max_sd / (0.026*F_CPU)) * 100;
};


int AudioPlaySdMp3::play(const char *filename){
	stop();

	lastError = ERR_CODEC_NONE;
	
	sd_buf = (uint8_t *) malloc(MP3_SD_BUF_SIZE);
	buf[0] = (short *) malloc(MP3_BUF_SIZE * sizeof(int16_t));
	buf[1] = (short *) malloc(MP3_BUF_SIZE * sizeof(int16_t));

	hMP3Decoder = MP3InitDecoder();
	
	if (!sd_buf || !buf[0] || !buf[1] || !hMP3Decoder)
	{
		lastError = ERR_CODEC_OUT_OF_MEMORY;
		stop();
		return lastError;
	}
	
	file = SD.open(filename);
	
	if (!file) 
	{
		lastError = ERR_CODEC_FILE_NOT_FOUND;
		stop();
		return lastError;
	}

	//Read-ahead 10 Bytes to detect ID3	
	sd_left = file.read(sd_buf, 10);

	//Skip ID3, if existent
	int skip = skipID3(sd_buf);
	if (skip) {
		size_id3 = skip;
		int b = skip & 0xfffffe00;
		file.seek(b);
		sd_left = 0;
	} else size_id3 = 0;
	
	//Fill buffer from the beginning with fresh data
	sd_left = fillReadBuffer(file, sd_buf, sd_buf, sd_left, MP3_SD_BUF_SIZE);

	if (!sd_left) {
		lastError = ERR_CODEC_FILE_NOT_FOUND;
		stop();
		return lastError;
	}

	init_interrupt();
	
	_VectorsRam[IRQ_AUDIOCODEC + 16] = &decode;
	NVIC_SET_PRIORITY(IRQ_AUDIOCODEC, IRQ_AUDIOCODEC_PRIO);
	NVIC_ENABLE_IRQ(IRQ_AUDIOCODEC);
	
	decoded_length[0] = 0;
	decoded_length[1] = 0;
	decoding_block = 0;
	decoding_state = 0;
	
	play_pos = 0;
	samples_played = 0;

	decode_cycles_max_sd = 0;
	decode_cycles_max = 0;

	sd_p = sd_buf;

	for (size_t i=0; i< DECODE_NUM_STATES; i++) decode(); 
	
	if((mp3FrameInfo.samprate != AUDIOCODECS_SAMPLE_RATE ) || (mp3FrameInfo.bitsPerSample != 16) || (mp3FrameInfo.nChans > 2)) {
		//Serial.println("incompatible MP3 file.");
		lastError = ERR_CODEC_FORMAT;
		stop();
		return lastError;
	}
	decoding_block = 1;	
	
	playing = 1;
	AudioStartUsingSPI();
    return lastError;
}

//runs in ISR
void AudioPlaySdMp3::update(void) 
{
	audio_block_t	*block_left;
	audio_block_t	*block_right;

	//paused or stopped ?
	if (0==playing or 2==playing) return;

	//chain decoder-interrupt.
	//to give the user-sketch some cpu-time, only chain
	//if the swi is not active currently.
	//In addition, check before if there waits work for it.
	int db = decoding_block;
	if (!NVIC_IS_ACTIVE(IRQ_AUDIOCODEC)) 
		if (decoded_length[db]==0)
			NVIC_TRIGGER_INTERRUPT(IRQ_AUDIOCODEC);

	//determine the block we're playing from
	int playing_block = 1 - decoding_block;
	if (decoded_length[playing_block] <= 0) return;

	// allocate the audio blocks to transmit
	block_left = allocate();
	if (block_left == NULL) return;

	uintptr_t pl = play_pos;
	
	if (mp3FrameInfo.nChans == 2) {
		// if we're playing stereo, allocate another
		// block for the right channel output
		block_right = allocate();
		if (block_right == NULL) {
			release(block_left);
			return;
		}
		
		memcpy_frominterleaved(block_left->data, block_right->data, buf[playing_block] + pl);
		
		pl += AUDIO_BLOCK_SAMPLES * 2;
		transmit(block_left, 0);
		transmit(block_right, 1);
		release(block_right);
		decoded_length[playing_block] -= AUDIO_BLOCK_SAMPLES * 2;

	} else
	{
		// if we're playing mono, no right-side block
		// let's do a (hopefully good optimized) simple memcpy
		memcpy(block_left->data, buf[playing_block] + pl, AUDIO_BLOCK_SAMPLES * sizeof(short));
		
		pl += AUDIO_BLOCK_SAMPLES;
		transmit(block_left, 0);
		transmit(block_left, 1);
		decoded_length[playing_block] -= AUDIO_BLOCK_SAMPLES;

	}

	samples_played += AUDIO_BLOCK_SAMPLES;

	release(block_left);
	
	//Switch to the next block if we have no data to play anymore:
	if (decoded_length[playing_block] == 0)
	{
		decoding_block = playing_block;
		play_pos = 0;
	} else 
	play_pos = pl;

}

//decoding-interrupt
void decode(void)
{

	if (decoded_length[decoding_block]) return; //this block is playing, do NOT fill it
		
	uint32_t cycles = ARM_DWT_CYCCNT;
	int eof = false;
	
	switch (decoding_state) {
	
	case 0: 
		{
	
		//if (sd_left < 1024) { //todo: optimize 1024..
			sd_left = fillReadBuffer( file, sd_buf, sd_p, sd_left, MP3_SD_BUF_SIZE);
			if (!sd_left) { eof = true; goto mp3end; }
			sd_p = sd_buf;
		//}
			uint32_t cycles_sd = (ARM_DWT_CYCCNT - cycles);
			if (cycles_sd > decode_cycles_max_sd ) decode_cycles_max_sd = cycles_sd;
			break;
		}
	
	case 1:
		{		
			// find start of next MP3 frame - assume EOF if no sync found
			int offset = MP3FindSyncWord(sd_p, sd_left);

			if (offset < 0) {
				//Serial.println("No sync"); //no error at end of file
				eof = true;
				goto mp3end;
			}

			sd_p += offset;
			sd_left -= offset;

			int decode_res = MP3Decode(hMP3Decoder, &sd_p, (int*)&sd_left,buf[decoding_block], 0);

			switch(decode_res)
			{
				case ERR_MP3_NONE:
				{
					MP3GetLastFrameInfo(hMP3Decoder, &mp3FrameInfo);
					decoded_length[decoding_block] = mp3FrameInfo.outputSamps;					
					break;
				}

				case ERR_MP3_MAINDATA_UNDERFLOW:
				{
					break;
				}

				default :
				{
					lastError = decode_res;
					eof = true;
					break;
				}
			}

			cycles = (ARM_DWT_CYCCNT - cycles);
			if (cycles > decode_cycles_max ) decode_cycles_max = cycles;
			break;
		}
	}//switch
	
mp3end:
	
	decoding_state++;
	if (decoding_state >= DECODE_NUM_STATES) decoding_state = 0;
	
	if (eof) {
		mp3stop();
	} 

}

void mp3stop(void)
{
	AudioStopUsingSPI();
	__disable_irq();	
	playing = 0;		
	if (buf[1]) {free(buf[1]);buf[1] = NULL;}
	if (buf[0]) {free(buf[0]);buf[0] = NULL;}
	if (sd_buf) {free(sd_buf);sd_buf = NULL;}
	if (hMP3Decoder) {MP3FreeDecoder(hMP3Decoder);hMP3Decoder=NULL;};
	__enable_irq();
	file.close();
}
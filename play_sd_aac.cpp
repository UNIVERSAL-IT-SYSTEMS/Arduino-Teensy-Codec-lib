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


#include "play_sd_aac.h"
#include "common/assembly.h"

#define AAC_SD_BUF_SIZE	3072 								//Enough space for a complete stereo frame
#define AAC_BUF_SIZE	(AAC_MAX_NCHANS * AAC_MAX_NSAMPS)	//AAC output buffer

#define DECODE_NUM_STATES 2									//How many steps in decode() ?

static File				file;

static uint8_t			*sd_buf;
static uint8_t			*sd_p;
static int				sd_left;

static short			*buf[2]; //output buffers
static size_t			decoded_length[2];
static size_t			decoding_block;
static unsigned int		decoding_state; //state 0: read sd, state 1: decode


static bool				isRAW;					//true AAC(streamable)
static size_t 			size_id3;
static uint32_t 		firstChunk, lastChunk;	//for MP4/M4A //TODO: use for ID3 too

static uint32_t			decode_cycles_max;
static uint32_t			decode_cycles_max_sd;

static unsigned int		playing;


static HAACDecoder	hAACDecoder;
static AACFrameInfo	aacFrameInfo;

static void decode(void);
static void aacstop(void);


void AudioPlaySdAac::stop(void)
{
	aacstop();
}

bool AudioPlaySdAac::pause(bool paused)
{
	if (playing) {
		if (paused) playing = 2;
		else playing = 1;
	}
	return (playing == 2);
}


bool AudioPlaySdAac::isPlaying(void)
{
	return (playing > 0);
}

uint32_t AudioPlaySdAac::positionMillis(void)
{
	return (AUDIO_SAMPLE_RATE_EXACT / 1000) * samples_played;
}

uint32_t AudioPlaySdAac::lengthMillis(void)
{
	if (playing) {
		if (duration)
			return duration;
		else //This is an estimate, takes not into account VBR, but better than nothing:
			return (file.size() - size_id3) / (aacFrameInfo.bitRate / 8 ) * 1000;
	}
	else return 0;
}

uint32_t AudioPlaySdAac::bitrate(void)
{
	if (playing) {
		return aacFrameInfo.bitRate;
	}
	else return 0;
}

void AudioPlaySdAac::processorUsageMaxResetDecoder(void){
	__disable_irq();
	decode_cycles_max = 0;
	decode_cycles_max_sd = 0;
	__enable_irq();
};

float AudioPlaySdAac::processorUsageMaxDecoder(void){
	//this is somewhat incorrect, it does not take the interruptions of update() into account -
	//therefore the returned number is too high.
	//Todo: better solution
	return (decode_cycles_max / (0.026*F_CPU)) * 100;
};

float AudioPlaySdAac::processorUsageMaxSD(void){
	//this is somewhat incorrect, it does not take the interruptions of update() into account -
	//therefore the returned number is too high.
	//Todo: better solution
	return (decode_cycles_max_sd / (0.026*F_CPU)) * 100;
};


_ATOM AudioPlaySdAac::findMp4Atom(const char *atom, uint32_t posi)
{

	bool r;
	_ATOM ret;
	_ATOMINFO atomInfo;

	ret.position = posi;
	do
	{
		r = file.seek(ret.position);
		file.read((uint8_t *) &atomInfo, sizeof(atomInfo));
		ret.size = REV32(atomInfo.size);
		if (strncmp(atom, atomInfo.name, 4)==0){
			return ret;
		}
		ret.position += ret.size ;
	} while (r);

	ret.position = 0;
	ret.size = 0;
	return ret;

}

bool AudioPlaySdAac::setupMp4(void)
{

	_ATOM ftyp = findMp4Atom("ftyp",0);
	if (!ftyp.size)
		return false; //no mp4/m4a file

	//go through the boxes to find the interesting atoms:
	uint32_t moov = findMp4Atom("moov", 0).position;
	uint32_t trak = findMp4Atom("trak", moov + 8).position;
	uint32_t mdia = findMp4Atom("mdia", trak + 8).position;

	//determine duration:
	uint32_t mdhd = findMp4Atom("mdhd", mdia + 8).position;
	uint32_t timescale = fread32(file, mdhd + 8 + 0x0c);
	duration = 1000.0 * ((float)fread32(file, mdhd + 8 + 0x10) / (float)timescale);

	//MP4-data has no aac-frames, so we have to set the parameters by hand.
	uint32_t minf = findMp4Atom("minf", mdia + 8).position;
	uint32_t stbl = findMp4Atom("stbl", minf + 8).position;
	//stsd sample description box: - infos to parametrize the decoder
	_ATOM stsd = findMp4Atom("stsd", stbl + 8);
	if (!stsd.size)
		return false; //something is not ok

	uint16_t channels = fread16(file, stsd.position + 8 + 0x20);
	//uint16_t bits		= fread16(file, stsd.position + 8 + 0x22); //not used
	uint16_t samplerate = fread32(file, stsd.position + 8 + 0x26);

	setupDecoder(channels, samplerate, AAC_PROFILE_LC);

	//stco - chunk offset atom:
	uint32_t stco = findMp4Atom("stco", stbl + 8).position;

	//number of chunks:
	uint32_t nChunks = fread32(file, stco + 8 + 0x04);
	//first entry from chunk table:
	firstChunk = fread32(file, stco + 8 + 0x08);
	//last entry from chunk table:
	lastChunk = fread32(file, stco + 8 + 0x04 + nChunks * 4);

#if 0
	Serial.print("mdhd duration=");
	Serial.print(duration);
	Serial.print(" ms, stsd: chan=");
	Serial.print(channels);
	Serial.print(" samplerate=");
	Serial.print(samplerate);
	Serial.print(" nChunks=");
	Serial.print(nChunks);
	Serial.print(" firstChunk=");
	Serial.println(firstChunk, HEX);
	Serial.print(" lastChunk=");
	Serial.println(lastChunk, HEX);
#endif

	return true;
}

void AudioPlaySdAac::setupDecoder(int channels, int samplerate, int profile)
{
	memset(&aacFrameInfo, 0, sizeof(AACFrameInfo));
	aacFrameInfo.nChans = channels;
	//aacFrameInfo.bitsPerSample = bits; not used
	aacFrameInfo.sampRateCore = samplerate;
	aacFrameInfo.profile = AAC_PROFILE_LC;
	AACSetRawBlockParams(hAACDecoder, 0, &aacFrameInfo);
}

int AudioPlaySdAac::play(const char *filename){
	stop();

	lastError = ERR_CODEC_NONE;
	
	sd_buf = (uint8_t *) malloc(AAC_SD_BUF_SIZE);
	buf[0] = (short *) malloc(AAC_BUF_SIZE * sizeof(int16_t));
	buf[1] = (short *) malloc(AAC_BUF_SIZE * sizeof(int16_t));

	hAACDecoder = AACInitDecoder();
	
	if (!sd_buf || !buf[0] || !buf[1] || !hAACDecoder)
	{
		lastError = ERR_CODEC_OUT_OF_MEMORY;
		stop();
		return lastError;
	}	
	
	file = SD.open(filename);

	if (!file) 
	{
		lastError = ERR_CODEC_OUT_OF_MEMORY;
		stop();
		return lastError;
	}

	isRAW = true;
	duration = 0;
	sd_left = 0;

	if (setupMp4()) {
		file.seek(firstChunk);
		sd_left = 0;
		isRAW = false;
	}
	else { //NO MP4. Do we have an ID3TAG ?
		file.seek(0);
		//Read-ahead 10 Bytes to detect ID3
		sd_left = file.read(sd_buf, 10);
		//Skip ID3, if existent
		uint32_t skip = skipID3(sd_buf);
		if (skip) {
			size_id3 = skip;
			int b = skip & 0xfffffe00;
			file.seek(b);
			sd_left = 0;
		} else size_id3 = 0;
	}

	//Fill buffer from the beginning with fresh data
	sd_left = fillReadBuffer(file, sd_buf, sd_buf, sd_left, AAC_SD_BUF_SIZE);

	if (!sd_left) {
		lastError = ERR_CODEC_FILE_NOT_FOUND;
		stop();
		return lastError;
	}

	init_interrupt();
	
	_VectorsRam[IRQ_AUDIOCODEC + 16] = decode;
	NVIC_SET_PRIORITY(IRQ_AUDIOCODEC, IRQ_AUDIOCODEC_PRIO);
	NVIC_ENABLE_IRQ(IRQ_AUDIOCODEC);

	decoded_length[0] = 0;
	decoded_length[1] = 0;
	decoding_state = 0;
	decoding_block = 0;
	
	play_pos = 0;
	samples_played = 0;

	decode_cycles_max_sd = 0;
	decode_cycles_max = 0;

	sd_p = sd_buf;

	for (int i=0; i< DECODE_NUM_STATES; i++) decode(); 
	
	if((aacFrameInfo.sampRateOut != AUDIOCODECS_SAMPLE_RATE ) || (aacFrameInfo.nChans > 2)) {
		//Serial.println("incompatible AAC file.");
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
void AudioPlaySdAac::update(void)
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
	int playing_block = 1 - db;
	if (decoded_length[playing_block] <= 0) 
		{
		//Huston, we have a problem: The decoder was too slow!
		//best is, we stop the whole track, as we have no chance to play it.
			stop();
			return;
		}

	// allocate the audio blocks to transmit
	block_left = allocate();
	if (block_left == NULL) return;

	int pl = play_pos;
	
	if (aacFrameInfo.nChans == 2) {
		// if we're playing stereo, allocate another
		// block for the right channel output
		block_right = allocate();
		if (block_right == NULL) {
			release(block_left);
			return;
		}

		memcpy_frominterleaved(block_left->data, block_right->data, buf[playing_block] + pl);
		
		pl += AUDIO_BLOCK_SAMPLES * 2 ;
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
	if ((decoded_length[playing_block] == 0) )
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
	
			sd_left = fillReadBuffer( file, sd_buf, sd_p, sd_left, AAC_SD_BUF_SIZE);
			if (!sd_left) { eof = true; goto aacend; }
			sd_p = sd_buf;

			uint32_t cycles_sd = ARM_DWT_CYCCNT - cycles;
			if (cycles_sd > decode_cycles_max_sd ) decode_cycles_max_sd = cycles_sd;
			break;
		}

	case 1:
		{
			if (isRAW) {
			
				// find start of next AAC frame - assume EOF if no sync found
				int offset = AACFindSyncWord(sd_p, sd_left);

				if (offset < 0) {
						//Serial.println("No sync"); //no error at end of file
					eof = true;
					goto aacend;
				}
			
				sd_p += offset;
				sd_left -= offset;
				
			}
			
			int decode_res = AACDecode(hAACDecoder, &sd_p, (int*)&sd_left, buf[decoding_block]);

			if (!decode_res) {
				AACGetLastFrameInfo(hAACDecoder, &aacFrameInfo);
				decoded_length[decoding_block] = aacFrameInfo.outputSamps;
			} else {
				//Serial.print("err:");Serial.println(decode_res);
				lastError = decode_res;
				eof = true;
				//goto aacend;
			}

			if (!isRAW && file.position() > lastChunk) {
				eof = true;
			//goto aacend;
			}

			cycles = ARM_DWT_CYCCNT - cycles;
			if (cycles > decode_cycles_max ) decode_cycles_max = cycles;
		}
	} //switch
		
aacend:

	decoding_state++;
	if (decoding_state >= DECODE_NUM_STATES) decoding_state = 0;

	if (eof) {
		aacstop();
	}

}

void aacstop(void)
{
	AudioStopUsingSPI();
	__disable_irq();
	playing = 0;
	if (buf[1]) {free(buf[1]);buf[1] = NULL;}
	if (buf[0]) {free(buf[0]);buf[0] = NULL;}
	if (sd_buf) {free(sd_buf);sd_buf = NULL;}
	if (hAACDecoder) {AACFreeDecoder(hAACDecoder);hAACDecoder=NULL;};
	__enable_irq();
	file.close();
}
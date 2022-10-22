/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997-2012 Sam Lantinga

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

    Sam Lantinga
    slouken@libsdl.org

    This file written by Ryan C. Gordon (icculus@icculus.org)
*/
#include "SDL_config.h"

/* Output audio to nowhere... */

#include "SDL_rwops.h"
#include "SDL_timer.h"
#include "SDL_audio.h"
#include "../SDL_audiomem.h"
#include "../SDL_audio_c.h"
#include "../SDL_audiodev_c.h"
#include "SDL_n3dsaudio.h"

#include <3ds.h>
#include <assert.h>

#include <string.h>

//extern volatile bool app_pause;
extern volatile bool app_exiting;

size_t stream_offset = 0;

/* The tag name used by N3DS audio */
#define N3DSAUD_DRIVER_NAME         "n3ds"

/* Audio driver functions */
static int N3DSAUD_OpenAudio(_THIS, SDL_AudioSpec *spec);
static void N3DSAUD_WaitAudio(_THIS);
static void N3DSAUD_PlayAudio(_THIS);
static Uint8 *N3DSAUD_GetAudioBuf(_THIS);
static void N3DSAUD_CloseAudio(_THIS);

/* Audio driver bootstrap functions */
static int N3DSAUD_Available(void)
{
	return(1);
}

static void N3DSAUD_LockAudio(_THIS)
{
	SDL_mutexP(this->mixer_lock);
}

static void N3DSAUD_UnlockAudio(_THIS)
{
	SDL_mutexV(this->mixer_lock);
}

static void N3DSAUD_DeleteDevice(SDL_AudioDevice *device)
{
	if ( device->hidden->mixbuf != NULL ) {
		SDL_FreeAudioMem(device->hidden->mixbuf);
		device->hidden->mixbuf = NULL;
	}
	if 	( device->hidden->waveBuf!= NULL )
		linearFree(device->hidden->waveBuf);
	ndspExit();
	SDL_free(device->hidden);
	SDL_free(device);
}

static SDL_AudioDevice *N3DSAUD_CreateDevice(int devindex)
{
	SDL_AudioDevice *this;

	/* Initialize all variables that we clean on shutdown */
	this = (SDL_AudioDevice *)SDL_malloc(sizeof(SDL_AudioDevice));
	if ( this ) {
		SDL_memset(this, 0, (sizeof *this));
		this->hidden = (struct SDL_PrivateAudioData *)
				SDL_malloc((sizeof *this->hidden));
	}
	if ( (this == NULL) || (this->hidden == NULL) ) {
		SDL_OutOfMemory();
		if ( this ) {
			SDL_free(this);
		}
		return(0);
	}

	//start 3ds DSP init
	Result rc = ndspInit();
	if (R_FAILED(rc)) {
		SDL_free(this);
		if ((R_SUMMARY(rc) == RS_NOTFOUND) && (R_MODULE(rc) == RM_DSP))
			SDL_SetError("DSP init failed: dspfirm.cdc missing!");
		else
			SDL_SetError("DSP init failed. Error code: 0x%X", rc);
		return(0);
	}

	/* Initialize internal state */
	SDL_memset(this->hidden, 0, (sizeof *this->hidden));

	/* Set the function pointers */
	this->OpenAudio = N3DSAUD_OpenAudio;
	this->WaitAudio = N3DSAUD_WaitAudio;
	this->PlayAudio = N3DSAUD_PlayAudio;
	this->GetAudioBuf = N3DSAUD_GetAudioBuf;
	this->CloseAudio = N3DSAUD_CloseAudio;
//    this->ThreadInit = N3DSAUD_ThreadInit;
	this->LockAudio = N3DSAUD_LockAudio;
	this->UnlockAudio = N3DSAUD_UnlockAudio;
	this->free = N3DSAUD_DeleteDevice;

	return this;
}

AudioBootStrap N3DSAUD_bootstrap = {
	N3DSAUD_DRIVER_NAME, "SDL N3DS audio driver",
	N3DSAUD_Available, N3DSAUD_CreateDevice
};

/* This function waits until it is possible to write a full sound buffer */
static void N3DSAUD_WaitAudio(_THIS)
{
		if (app_exiting) {
			SDL_Delay(5); 
		} else
		while(this->hidden->waveBuf[this->hidden->nextbuf].status != NDSP_WBUF_DONE && !app_exiting){
			SDL_Delay(5);//Give other thread 5ms of execution time.
		}
}

static void N3DSAUD_PlayAudio(_THIS)
{
	if (app_exiting) return; 

	if (this->hidden->format==NDSP_FORMAT_STEREO_PCM8 || this->hidden->format==NDSP_FORMAT_MONO_PCM8) {
			memcpy(this->hidden->waveBuf[this->hidden->nextbuf].data_pcm8,this->hidden->mixbuf,this->hidden->mixlen);
		DSP_FlushDataCache(this->hidden->waveBuf[this->hidden->nextbuf].data_pcm8,
							this->hidden->mixlen);
	} else {
		memcpy(this->hidden->waveBuf[this->hidden->nextbuf].data_pcm16,this->hidden->mixbuf,this->hidden->mixlen);
		DSP_FlushDataCache(this->hidden->waveBuf[this->hidden->nextbuf].data_pcm16,
							this->hidden->mixlen);
	}
	this->hidden->waveBuf[this->hidden->nextbuf].offset=0;
	this->hidden->waveBuf[this->hidden->nextbuf].status=NDSP_WBUF_QUEUED;
	ndspChnWaveBufAdd(0, &this->hidden->waveBuf[this->hidden->nextbuf]);

	this->hidden->nextbuf = (this->hidden->nextbuf+1)%NUM_BUFFERS;
}

static Uint8 *N3DSAUD_GetAudioBuf(_THIS)
{
	return(this->hidden->mixbuf);
}

static void N3DSAUD_CloseAudio(_THIS)
{
	if ( this->hidden->mixbuf != NULL ) {
		SDL_FreeAudioMem(this->hidden->mixbuf);
		this->hidden->mixbuf = NULL;
	}
	if 	( this->hidden->waveBuf!= NULL )
		linearFree(this->hidden->waveBuf);
}

/*
static void N3DSAUD_ThreadInit(SDL_AudioDevice *thisdevice)
{
	;
}
*/

static int N3DSAUD_OpenAudio(_THIS, SDL_AudioSpec *spec)
{	
   //start 3ds DSP init
	if(ndspInit()) return (-1);

	int format = 0;
	if(spec->channels > 2)
		spec->channels = 2;

    Uint16 test_format = SDL_FirstAudioFormat(spec->format);
    int valid_datatype = 0;
    while ((!valid_datatype) && (test_format)) {
        spec->format = test_format;
        switch (test_format) {
 
			case AUDIO_S8:
				/* Signed 8-bit audio supported */
				this->hidden->format=(spec->channels==2)?format=NDSP_FORMAT_STEREO_PCM8:NDSP_FORMAT_MONO_PCM8;
				this->hidden->isSigned=1;
				this->hidden->bytePerSample = (spec->channels);
				   valid_datatype = 1;
				break;
			case AUDIO_U8:
				/* Unsigned 8-bit audio supported */
				this->hidden->format=(spec->channels==2)?format=NDSP_FORMAT_STEREO_PCM8:NDSP_FORMAT_MONO_PCM8;
				this->hidden->isSigned=0;
				this->hidden->bytePerSample = (spec->channels);
				   valid_datatype = 1;
				break;
			case AUDIO_S16:
				/* Signed 16-bit audio supported */
				this->hidden->format=(spec->channels==2)?format=NDSP_FORMAT_STEREO_PCM16:NDSP_FORMAT_MONO_PCM16;
				this->hidden->isSigned=1;
				this->hidden->bytePerSample = (spec->channels) * 2;
				   valid_datatype = 1;
				break;
			case AUDIO_U16:
				/* Unsigned 16-bit audio supported */
				this->hidden->format=(spec->channels==2)?format=NDSP_FORMAT_STEREO_PCM16:NDSP_FORMAT_MONO_PCM16;
				this->hidden->isSigned=0;
				this->hidden->bytePerSample = (spec->channels) * 2;
				   valid_datatype = 1;
				break;
			default:
				test_format = SDL_NextAudioFormat();
				break;
		}
	}

    if (!valid_datatype) {  /* shouldn't happen, but just in case... */
        SDL_SetError("Unsupported audio format");
		ndspExit();
        return (-1);
    }

	/* Update the fragment size as size in bytes */
	SDL_CalculateAudioSpec(spec);

	/* Allocate mixing buffer */
	if (spec->size >= UINT32_MAX/2)
		return(-1);
	this->hidden->mixlen = spec->size;
	this->hidden->mixbuf = (Uint8 *) SDL_AllocAudioMem(spec->size);
	if ( this->hidden->mixbuf == NULL ) {
		ndspExit();
		return(-1);
	}
	SDL_memset(this->hidden->mixbuf, spec->silence, spec->size);

	Uint8 * temp = (Uint8 *) linearAlloc(this->hidden->mixlen*NUM_BUFFERS);
	memset(temp,0,this->hidden->mixlen*NUM_BUFFERS);
	DSP_FlushDataCache(temp,this->hidden->mixlen*NUM_BUFFERS);

	this->hidden->nextbuf = 0;
	this->hidden->channels = spec->channels;
	this->hidden->samplerate = spec->freq;

	ndspChnWaveBufClear(0);
 	ndspChnReset(0);
	
	ndspSetOutputMode((this->hidden->channels==2)?NDSP_OUTPUT_STEREO:NDSP_OUTPUT_MONO);

	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetRate(0, spec->freq);
	ndspChnSetFormat(0, this->hidden->format);

	float mix[12];
	memset(mix, 0, sizeof(mix));
	mix[0] = 1.0;
	mix[1] = 1.0;
	ndspChnSetMix(0, mix);

	memset(this->hidden->waveBuf,0,sizeof(ndspWaveBuf)*NUM_BUFFERS);

	this->hidden->waveBuf[0].data_vaddr = temp;
	this->hidden->waveBuf[0].nsamples = this->hidden->mixlen / this->hidden->bytePerSample;
	this->hidden->waveBuf[0].status = NDSP_WBUF_DONE;

	this->hidden->waveBuf[1].data_vaddr = temp + this->hidden->mixlen;
	this->hidden->waveBuf[1].nsamples = this->hidden->mixlen / this->hidden->bytePerSample;
	this->hidden->waveBuf[1].status = NDSP_WBUF_DONE;

	DSP_FlushDataCache(this->hidden->waveBuf,sizeof(ndspWaveBuf)*2);

	stream_offset += this->hidden->mixlen;

    // end 3ds DSP init

	/* We're ready to rock and roll. :-) */
	return(0);
}


//#include <stdlib>
#include <string.h>
//#include <ctime>
//#include <cctype>
//#include <malloc.h>

#include <3ds.h>
#include "sound.h"
#include "e8910.h"


u8 *stream;

int bufferpos=0;

int soundstate=0;

void sound_callback(int len)
{
int buffertail;
    if(soundstate) {
        if (bufferpos+len<=SOUND_BUFFER_SIZE) {
			e8910_callback(NULL, stream+bufferpos, len);  
			GSPGPU_FlushDataCache(stream+bufferpos, len);
		} else {
			buffertail = SOUND_BUFFER_SIZE - bufferpos;
			e8910_callback(NULL, stream+bufferpos, buffertail);
			e8910_callback(NULL, stream, len-buffertail);
			GSPGPU_FlushDataCache(stream+bufferpos, buffertail);
			GSPGPU_FlushDataCache(stream, len-buffertail);
		}
	bufferpos= (bufferpos+len) % (unsigned int) SOUND_BUFFER_SIZE;
    }
}

void sound_start(int freq, int len)
{
	bufferpos = 0;
	soundstate=1;
	sound_callback(len);
	GSPGPU_FlushDataCache(stream, SOUND_BUFFER_SIZE);
	csndPlaySound(0x8, SOUND_REPEAT | SOUND_FORMAT_8BIT, freq, 1.0, 0.0, (u32*)stream, (u32*)stream, SOUND_BUFFER_SIZE);
}

void sound_pause(void)
{
	if (soundstate) {
		CSND_SetPlayState(0x8, 0);//Stop audio playback.
		csndExecCmds(0);
		
		soundstate=0;
	}
}

int sound_getstate(void)
{
	return soundstate;
}

int sound_init(void)
{
 	e8910_init_sound();
   
	if(csndInit()) return 0;
    
    stream = (u8*)linearAlloc(SOUND_BUFFER_SIZE);
    if (!stream) {
        printf("ERROR : Couldn't malloc stream\n");
        return 0;
    } 
	bufferpos=0;
    return 1;
}

void sound_quit(void)
{
		CSND_SetPlayState(0x8, 0);//Stop audio playback.
		csndExecCmds(0);

   if (stream) {
		linearFree(stream);
    }
	csndExit();
	e8910_done_sound();

}

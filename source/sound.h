
#ifndef __VEX3DS_SOUND_H__
#define __VEX3DS_SOUND_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
 
#include <3ds.h>
#include "main.h"



#define SOUND_FREQUENCY	22050.0
#define SOUND_SAMPLES_PER_FRAME	(SOUND_FREQUENCY/FPS_LIMIT)
#define SOUND_BUFFER_SIZE	(SOUND_SAMPLES_PER_FRAME*4)
//#define VEXSOUNDBUFF	(SOUND_BUFFER_SIZE*2)

u8 *stream;

int  sound_init(void);
void sound_quit(void);
void sound_start(int freq, int len);
void sound_callback(int len);
void sound_pause(void);
int  sound_getstate(void);

#endif

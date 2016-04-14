#include <3ds.h>
#include <sf2d.h>
#include <sftd.h>
#include <sfil.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include "main.h"
#include "vecx.h"
#include "sound.h"
#include "gui.h"
#include "Roboto_Regular_ttf.h"

#define einline __inline

/* a global string buffer user for message output */
char gbuffer[1024];

float fps_counter = FPS_LIMIT;
u64 tickcurr, syncticknext, fpsticknext;


FS_Archive sdmcArchive;
#define MAX_PATH 1024

char tempfile[MAX_PATH];

char bios_path_and_name[MAX_PATH]; // full path of rom.dat
char rom_name_with_no_ext[MAX_PATH]; // rom name with no extension, used for savestates
char savename[MAX_PATH];

char config_roms_path[MAX_PATH];
char config_base_path[MAX_PATH];
char config_bios_path[MAX_PATH];
char config_skin_path[MAX_PATH];
char config_save_path[MAX_PATH];

int Vex_cfg_Scalemode;
int Vex_cfg_Show_FPS;
int Vex_cfg_Frameskip;
int Vex_cfg_Sound;
int Vex_cfg_Color;
int Vex_cfg_Overlay;

int exitemulator;

static int screen_x;
static int screen_y;
static int scl_factor;

static unsigned int color_set[VECTREX_COLORS];

sftd_font *font;
static char buffer[64];;
sf2d_texture *overlay, *splash;
unsigned int framecount;
unsigned int fpscnt = 0;


enum {
	EMU_TIMER = 20 /* the emulators heart beats at 20 milliseconds */
};

void get_config_path()
{
    Handle dirHandle;

	if(strlen(config_base_path) == 0) {
        sdmcArchive = (FS_Archive){ARCHIVE_SDMC, (FS_Path){PATH_EMPTY, 1, (u8*)""}};
        FSUSER_OpenArchive(&sdmcArchive);
        
        sprintf(config_base_path, "/vectrex");

        FS_Path dirPath = (FS_Path){PATH_ASCII, strlen(config_base_path)+1, (u8*)config_base_path};
        FSUSER_OpenDirectory(&dirHandle, sdmcArchive, dirPath);
        
        if(!dirHandle) sprintf(config_base_path, "/"); //!!
        FSDIR_Close(dirHandle);
		
		sprintf(config_roms_path, "%s/%s", config_base_path, "Roms");
		sprintf(config_bios_path, "%s/%s", config_base_path, "Bios");
		sprintf(config_skin_path, "%s/%s", config_base_path, "Skin");
		sprintf(config_save_path, "%s/%s", config_base_path, "Save");
	}

}

void osint_updatescale (void)
{

	if(Vex_cfg_Scalemode==0) {
		screen_x = 193; //330;
		screen_y = 240; //410;
	} else if(Vex_cfg_Scalemode==1) {
		screen_x = 240;
		screen_y = 298;
	} else {
		screen_x = 320;
		screen_y = 398;
	}

	scl_factor = ALG_MAX_X / screen_x;
}

static inline void unicodeToChar(char* dst, uint16_t* src, int max) {
    if(!src || !dst) return;
    int n = 0;
    while (*src && n < max - 1) {
        *(dst++) = (*(src++)) & 0xFF;
        n++;
    }
    *dst = 0x00;
}

void osint_reset (void)
{
	vecx_reset ();
	framecount = 1;
}


void osint_loadrom (char* load_filename)
{
	FILE *rom_file;
	
   int i;

    // strip rom name from full path and cut off the extension
    for(i = strlen(load_filename) - 1; i >= 0; i--) {
        if(load_filename[i] == '/' || i == 0) { 
            memcpy((void *)rom_name_with_no_ext, (void *)(load_filename + i + (i?1:0)), strlen(load_filename) - i);
            rom_name_with_no_ext[strlen(rom_name_with_no_ext)-4] = 0; // cut off extension.
            break;
        }
    }
 	
	if(overlay) sf2d_free_texture(overlay);
	sprintf(tempfile, "%s/%s.png", config_roms_path, rom_name_with_no_ext);
	overlay = sfil_load_PNG_file(tempfile, SF2D_PLACE_RAM);
 
	rom_file = fopen (load_filename, "rb");
	if (rom_file)
		fread(cart, 1, sizeof (cart), rom_file);
	fclose(rom_file);
	osint_reset();
}

void osint_clearrom (void)
{	
	unsigned int b;
	for (b = 0; b < sizeof (cart); b++) 
		cart[b] = 0;
}

/* Parse configuration file */
int parse_file(const char *filename, int *argc, char **argv)
{
    char token[0x100];
    FILE *handle = NULL;

    *argc = 0;

    handle = fopen(filename, "r");
    if(!handle) return (0);

    while(!(feof(handle)))
    {
		fscanf(handle, "%s", &token[0]);
        int size = strlen(token) + 1;
        argv[*argc] = (char*) malloc(size);
        if(!argv[*argc]) return (0);
		sprintf(argv[*argc], "%s", token);
        *argc += 1;
    }

    if(handle) fclose(handle);
    return (1);
}


void set_option_defaults(void)
{
	screen_x = 193; 
	screen_y = 240;

	Vex_cfg_Scalemode=0;
	Vex_cfg_Frameskip = 1;
	Vex_cfg_Color = 0;
	Vex_cfg_Overlay = 1;
	Vex_cfg_Show_FPS = 1;
}

/* Parse argument list */
void parse_args(int argc, char **argv)
{
    int i;

    for(i = 0; i < argc; i++)
    {
        int left = argc - i - 1;

        if(strcmp(argv[i], "-nosound") == 0)
        {
            Vex_cfg_Sound = 0;
        }

        if(strcmp(argv[i], "-scale") == 0 && left) 
        {
            Vex_cfg_Scalemode = atoi(argv[i+1]);
		    if (Vex_cfg_Scalemode > 4) Vex_cfg_Scalemode = 0;

        }

        if(strcmp(argv[i], "-color") == 0 && left) 
        {
            Vex_cfg_Color = atoi(argv[i+1]);
		    if (Vex_cfg_Color > 4) Vex_cfg_Color = 0;

        }

        if(strcmp(argv[i], "-frameskip") == 0 && left) 
        {
            Vex_cfg_Frameskip = atoi(argv[i+1]);
		    if (Vex_cfg_Frameskip > 4) Vex_cfg_Frameskip = 0;

        }

        if(strcmp(argv[i], "-fps") == 0)
        {
            Vex_cfg_Show_FPS = 1;
        }

        if(strcmp(argv[i], "-overlay") == 0)
        {
            Vex_cfg_Overlay = 1;
        }

        if(strcmp(argv[i], "-nooverlay") == 0)
        {
            Vex_cfg_Overlay = 0;
        }

     }
}


void save_config(char *file)
{
    FILE *handle = NULL;
    handle = fopen(file, "w+");

        if(Vex_cfg_Sound == 0)
        {
            fprintf(handle, "%s ", "-nosound");
        }

        if(Vex_cfg_Scalemode) 
        {
            fprintf(handle, "%s %i ", "-scale", Vex_cfg_Scalemode);
        }

        if(Vex_cfg_Frameskip) 
        {
            fprintf(handle, "%s %i ", "-frameskip", Vex_cfg_Frameskip);
        }

        if(Vex_cfg_Show_FPS)
        {
            fprintf(handle, "%s ", "-fps");
        }

        if(Vex_cfg_Overlay)
        {
            fprintf(handle, "%s ", "-overlay");
        } else {
            fprintf(handle, "%s ", "-nooverlay");
        }

        fprintf(handle, "%s %i ", "-color", Vex_cfg_Color);

 
 		fclose(handle);
}

void load_config(char *file)
{

    /* Our token list */
    int i, argc;
    char *argv[0x100];


	set_option_defaults();
	
    for(i = 0; i < 0x100; i++) argv[i] = NULL;

    /* Check configuration file */
    if(file) 
	  if(parse_file(file, &argc, argv)) {

		/* Check extracted tokens */
		parse_args(argc, argv);

		/* Free token list */
		for(i = 0; i < argc; i++)
		{
			if(argv[argc]) free (argv[argc]);
		}
	}
}
	

static int osint_config (void)
{
	unsigned int b;
	FILE *rom_file;

	set_option_defaults();

	sprintf(tempfile, "%s/%s", config_base_path, "Vex3DS.cfg");
	load_config(tempfile);

	osint_updatescale ();

	sprintf(bios_path_and_name, "%s/%s", config_bios_path, "bios.dat");
	rom_file = fopen (bios_path_and_name, "rb");

	if (rom_file == NULL) {
		sprintf (gbuffer, "cannot open '%s'", bios_path_and_name);
		return 1;
	}

	b = fread (rom, 1, sizeof (rom), rom_file);

	if (b < sizeof (rom)) {
		sprintf (gbuffer, "read %d bytes from '%s'. need %d bytes.",
			b, bios_path_and_name, sizeof (rom));
		return 1;
	}

	fclose (rom_file);
	
	
	sprintf(tempfile, "%s/%s", config_bios_path, "bios.png");
	overlay = sfil_load_PNG_file(tempfile, SF2D_PLACE_RAM);

	/* the cart is empty by default */
	osint_clearrom();

	return 0;
}

void osint_gencolors (void)
{
	unsigned int mask;
	
	switch (Vex_cfg_Color) {
		case 1:
			mask = 0xff0000ff;
			break;
		case 2:
			mask = 0xff00ff00;
			break;
		case 3:
			mask = 0xffff0000;
			break;
		default:
			mask = 0xffffffff;
	}
	int c;
	
// increased luminance palette
	color_set[0] =	RGBA8(0,0,0,0xff); 
	for (c = 1; c < VECTREX_COLORS; c++) {
		color_set[c] =	RGBA8(c+VECTREX_COLORS,c+VECTREX_COLORS,c+VECTREX_COLORS,0xff) & mask; 
	}
}

void osint_render (void)
{
	int v;

	sf2d_start_frame(GFX_TOP, GFX_LEFT);

// warning: sf2dlib seems to do not show lines starting and ending at the same point. Vecx assumes that in this case should be drawn a point.
// Casting to float only x1 and y1 and leaving x0 and y0 int makes a little difference in computing starting and ending coordinates, making it show


	if (Vex_cfg_Scalemode==0) {
		if((Vex_cfg_Overlay!=0) & (overlay!=NULL))
			sf2d_draw_texture_scale(overlay, 103, 0, (float)screen_x/overlay->width, (float)screen_y/overlay->height);
		else sf2d_draw_rectangle(103, 0, screen_x, screen_y, RGBA8(0x0, 0x0, 0x0, 0xFF));
		for (v = 0; v < vector_draw_cnt; v++) 
			sf2d_draw_line (vectors_draw[v].x0/scl_factor+103, vectors_draw[v].y0/scl_factor,
				(float)vectors_draw[v].x1/(float)scl_factor+103, (float)vectors_draw[v].y1/(float)scl_factor, 1,
				color_set[vectors_draw[v].color]);
		for (v = 0; v < vector_erse_cnt; v++) 
			sf2d_draw_line (vectors_erse[v].x0/scl_factor+103, vectors_erse[v].y0/scl_factor,
				(float)vectors_erse[v].x1/(float)scl_factor+103, (float)vectors_erse[v].y1/(float)scl_factor, 1,
				color_set[vectors_erse[v].color]);
	} else 	if (Vex_cfg_Scalemode==1) {
		if((Vex_cfg_Overlay!=0) & (overlay!=NULL))
			sf2d_draw_texture_part_rotate_scale(overlay, 200, 120, 1.57, 0, 0, overlay->width, overlay->height, (float)screen_x/overlay->width, (float)screen_y/overlay->height);
		else sf2d_draw_rectangle(52, 0, screen_y, screen_x, RGBA8(0x0, 0x0, 0x0, 0xFF));	
		for (v = 0; v < vector_draw_cnt; v++) 
			sf2d_draw_line (400-51-vectors_draw[v].y0/scl_factor, vectors_draw[v].x0/scl_factor, 
				400-51-(float)vectors_draw[v].y1/(float)scl_factor, (float)vectors_draw[v].x1/(float)scl_factor, 1,
				color_set[vectors_draw[v].color]);
		for (v = 0; v < vector_erse_cnt; v++) 
			sf2d_draw_line (400-51-vectors_erse[v].y0/scl_factor, vectors_erse[v].x0/scl_factor, 
				400-51-(float)vectors_erse[v].y1/(float)scl_factor, (float)vectors_erse[v].x1/(float)scl_factor, 1,
				color_set[vectors_erse[v].color]);
	} else 	if (Vex_cfg_Scalemode==2) {
		if((Vex_cfg_Overlay!=0) & (overlay!=NULL))
			sf2d_draw_texture_scale(overlay, 40, 41, (float)screen_x/overlay->width, (float)screen_y/overlay->height);
		else sf2d_draw_rectangle(40, 41, screen_x, screen_y, RGBA8(0x0, 0x0, 0x0, 0xFF));
		for (v = 0; v < vector_draw_cnt; v++) 
			sf2d_draw_line (vectors_draw[v].x0/scl_factor+40, vectors_draw[v].y0/scl_factor+41,
				(float)vectors_draw[v].x1/(float)scl_factor+40, (float)vectors_draw[v].y1/(float)scl_factor+41, 1,
				color_set[vectors_draw[v].color]);
		for (v = 0; v < vector_erse_cnt; v++) 
			sf2d_draw_line (vectors_erse[v].x0/scl_factor+40, vectors_erse[v].y0/scl_factor+41,
				(float)vectors_erse[v].x1/(float)scl_factor+40, (float)vectors_erse[v].y1/(float)scl_factor+41, 1,
				color_set[vectors_erse[v].color]);
	} else {
		if((Vex_cfg_Overlay!=0) & (overlay!=NULL))
			sf2d_draw_texture_scale(overlay, 40, 0, (float)screen_x/overlay->width, (float)screen_y/overlay->height);
		else sf2d_draw_rectangle(40, 0, screen_x, screen_y, RGBA8(0x0, 0x0, 0x0, 0xFF));
		sf2d_draw_rectangle(0, 199, 400, 320, RGBA8(0x0f, 0x0, 0x0, 0x80));
		for (v = 0; v < vector_draw_cnt; v++) 
			sf2d_draw_line (vectors_draw[v].x0/scl_factor+40, vectors_draw[v].y0/scl_factor,
				(float)vectors_draw[v].x1/(float)scl_factor+40, (float)vectors_draw[v].y1/(float)scl_factor, 1,
				color_set[vectors_draw[v].color]);
		for (v = 0; v < vector_erse_cnt; v++) 
			sf2d_draw_line (vectors_erse[v].x0/scl_factor+40, vectors_erse[v].y0/scl_factor,
				(float)vectors_erse[v].x1/(float)scl_factor+40, (float)vectors_erse[v].y1/(float)scl_factor, 1,
				color_set[vectors_erse[v].color]);
	}


	sf2d_end_frame();

    sf2d_start_frame(GFX_BOTTOM, GFX_LEFT);


	if (Vex_cfg_Scalemode==2) {
		if((Vex_cfg_Overlay!=0) & (overlay!=NULL))
			sf2d_draw_texture_scale(overlay, 0, -198, (float)screen_x/overlay->width, (float)screen_y/overlay->height);
		else sf2d_draw_rectangle(0, -198, screen_x, screen_y, RGBA8(0x0, 0x0, 0x0, 0xFF));
		for (v = 0; v < vector_draw_cnt; v++) 
			sf2d_draw_line (vectors_draw[v].x0/scl_factor, vectors_draw[v].y0/scl_factor-198,
				(float)vectors_draw[v].x1/(float)scl_factor, (float)vectors_draw[v].y1/(float)scl_factor-198, 1,
				color_set[vectors_draw[v].color]);
		for (v = 0; v < vector_erse_cnt; v++) 
			sf2d_draw_line (vectors_erse[v].x0/scl_factor, vectors_erse[v].y0/scl_factor-198,
				(float)vectors_erse[v].x1/(float)scl_factor, (float)vectors_erse[v].y1/(float)scl_factor-198, 1,
				color_set[vectors_erse[v].color]);
	} 	else if (Vex_cfg_Scalemode==3) {
		if((Vex_cfg_Overlay!=0) & (overlay!=NULL))
			sf2d_draw_texture_scale(overlay, 0, -158, (float)screen_x/overlay->width, (float)screen_y/overlay->height);
		else sf2d_draw_rectangle(0, 0, screen_x, screen_y, RGBA8(0x0, 0x0, 0x0, 0xFF));
		sf2d_draw_rectangle(0, 0, 320, 41, RGBA8(0x0f, 0x0, 0x0, 0x80));
		for (v = 0; v < vector_draw_cnt; v++) 
			sf2d_draw_line (vectors_draw[v].x0/scl_factor, vectors_draw[v].y0/scl_factor-158,
				(float)vectors_draw[v].x1/(float)scl_factor, (float)vectors_draw[v].y1/(float)scl_factor-158, 1,
				color_set[vectors_draw[v].color]);
		for (v = 0; v < vector_erse_cnt; v++) 
			sf2d_draw_line (vectors_erse[v].x0/scl_factor, vectors_erse[v].y0/scl_factor-158,
				(float)vectors_erse[v].x1/(float)scl_factor, (float)vectors_erse[v].y1/(float)scl_factor-158, 1,
				color_set[vectors_erse[v].color]);
	}

	if (Vex_cfg_Show_FPS) {
//		sprintf(buffer, "FPS: %.2f", sf2d_get_fps()*(Vex_cfg_Frameskip+1));
		sprintf(buffer, "FPS: %.2f",fps_counter);
		sftd_draw_text(font, 8, 222, RGBA8(0xFF, 0xFF, 0xFF, 0xFF), 10, buffer);
	}
	sf2d_end_frame();

    sf2d_swapbuffers();

}

void doevents(void)
{

	hidScanInput();
	if(keysDown()&KEY_START) 
	{
		sound_pause();
		gui_Run();
	}
	if(keysDown()&KEY_SELECT)
	{
		if(++Vex_cfg_Scalemode>3) Vex_cfg_Scalemode=0;
		osint_updatescale ();
	}

	if(keysDown()&KEY_Y) snd_regs[14] &= ~0x01;
	if(keysDown()&KEY_X) snd_regs[14] &= ~0x02;
	if(keysDown()&KEY_A) snd_regs[14] &= ~0x04;
	if(keysDown()&KEY_B) snd_regs[14] &= ~0x08;

	if(keysUp()&KEY_Y) snd_regs[14] |= 0x01;
	if(keysUp()&KEY_X) snd_regs[14] |= 0x02;
	if(keysUp()&KEY_A) snd_regs[14] |= 0x04;
	if(keysUp()&KEY_B) snd_regs[14] |= 0x08;

	if (Vex_cfg_Scalemode==1) {
		if(keysDown()&KEY_DOWN) alg_jch0 = 0x00;
		if(keysDown()&KEY_UP) alg_jch0 = 0xff;
		if(keysDown()&KEY_RIGHT) alg_jch1 = 0x00;
		if(keysDown()&KEY_LEFT) alg_jch1 = 0xff;
		if(keysUp()&KEY_DOWN) alg_jch0 = 0x80;
		if(keysUp()&KEY_UP) alg_jch0 = 0x80;
		if(keysUp()&KEY_RIGHT) alg_jch1 = 0x80;
		if(keysUp()&KEY_LEFT) alg_jch1 = 0x80;
	} else {
		if(keysDown()&KEY_LEFT) alg_jch0 = 0x00;
		if(keysDown()&KEY_RIGHT) alg_jch0 = 0xff;
		if(keysDown()&KEY_DOWN) alg_jch1 = 0x00;
		if(keysDown()&KEY_UP) alg_jch1 = 0xff;
		if(keysUp()&KEY_LEFT) alg_jch0 = 0x80;
		if(keysUp()&KEY_RIGHT) alg_jch0 = 0x80;
		if(keysUp()&KEY_DOWN) alg_jch1 = 0x80;
		if(keysUp()&KEY_UP) alg_jch1 = 0x80;
	}
}

void osint_emuloop (void)
{
	/* reset the vectrex hardware */

	osint_reset ();

	// initialize timers
	tickcurr=svcGetSystemTick();
	fpsticknext = tickcurr + TICKS_PER_SEC;
	syncticknext = tickcurr + TICKS_PER_FRAME;

	while (aptMainLoop() && (exitemulator==0)) {

		doevents();


		vecx_emu ((VECTREX_MHZ / 1000) * EMU_TIMER);

	}

//exit_emuloop:

}

void osint_timer (void) {

// Timing

		fpscnt++;

		if (framecount==0) {
			if (tickcurr <= syncticknext)
				svcSleepThread((syncticknext - svcGetSystemTick()) / TICKS_PER_NSEC);
			else syncticknext = svcGetSystemTick();
		} 

		syncticknext = syncticknext + TICKS_PER_FRAME;
		
		tickcurr=svcGetSystemTick();
 		
		if (tickcurr >= fpsticknext) {
			fpsticknext += TICKS_PER_SEC;
			fps_counter = fpscnt;
			fpscnt = 0;
		} 

		framecount++;
		if (framecount>Vex_cfg_Frameskip) framecount=0;
}

int main(void) {

	srvInit();
	aptInit();
	hidInit();
	fsInit();
	sdmcInit();

	sdmcArchive = (FS_Archive){ARCHIVE_SDMC, (FS_Path){PATH_EMPTY, 1, (u8*)""}};
	FSUSER_OpenArchive(&sdmcArchive);

    sf2d_init();
	sf2d_set_3D(false);
	
	sftd_init();
    font = sftd_load_font_mem(Roboto_Regular_ttf, Roboto_Regular_ttf_size);
	
	exitemulator=0;

	sf2d_set_clear_color(RGBA8(0x00, 0x00, 0x2F, 0xFF));
	
	get_config_path();

	if (osint_config ()) {
		exitemulator = 1;
	}

	sprintf(tempfile, "%s/%s", config_skin_path, "vex3ds.png");
	splash = sfil_load_PNG_file(tempfile, SF2D_PLACE_RAM);

	    // Call filebrowser
	if(gui_LoadFile(tempfile)== 0)
		osint_loadrom (tempfile);
	

	osint_gencolors ();

	sound_init();
	
/* emulator code */
	osint_emuloop ();

	sound_quit();

	if(overlay) sf2d_free_texture(overlay);
	if(splash) sf2d_free_texture(splash);
    sf2d_fini();
	sf2d_fini();
    sdmcExit();
    fsExit();
    hidExit();
    aptExit();
    srvExit();
    exit(EXIT_SUCCESS);
	return 0;
}

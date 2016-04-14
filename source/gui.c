/*

	Main menu items:
	LOAD ROM
	LOAD STATE: 0-4
	SAVE STATE: 0-4
	RESET ROM
	CONFIG
	EXIT

	Config menu items;
	SCALING: Fit top / Rotated / Split / Split fit
	OVERLAY: YES / NO 
	SOUND: YES / NO 
	SHOW FPS: YES / NO
	Color: White / Red / Green / Blue 
	Frameskip: 0-4
	SAVE CONFIG

*/ 

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include <3ds.h>
#include <sf2d.h>
#include <sfil.h>
#include <sftd.h>
#include "gui.h"
#include "main.h"

/* defines and macros */
#define MAX__PATH 1024
#define FILE_LIST_ROWS 20   //24
#define FILE_LIST_POSITION 8
#define DIR_LIST_POSITION 208

#define color16(red, green, blue) ((red << 11) | (green << 5) | blue)

#define COLOR_BG            	RGBA8(0x14, 0x0c, 0x08, 0xff) 
#define COLOR_ROM_INFO      	RGBA8(0x58, 0x90, 0x68, 0xff) 
#define COLOR_ACTIVE_ITEM   	RGBA8(0xff, 0xff, 0x00, 0xff) 
#define COLOR_INACTIVE_ITEM 	RGBA8(0xaa, 0xaa, 0xaa, 0xff)   
#define COLOR_INACTIVE_ITEM_BG 	RGBA8(0x44, 0x44, 0x44, 0xff)   
#define COLOR_FRAMESKIP_BAR 	RGBA8(0x3c, 0x7c, 0x7c, 0xff)  
#define COLOR_HELP_TEXT     	RGBA8(0x00, 0xff, 0xff, 0xff)  

void strncpy_u2a(char* dst, u16* src, int n);

void gui_LoadState();
void gui_SaveState();
void gui_FileBrowserRun();
void gui_ConfigMenuRun();
void gui_SaveConfig();
void gui_Reset();
void gui_DrawTopScreen();
void gui_Quitemu();

int guitextwidth;
int done = 0; // flag to indicate exit status


/* external references */
extern char rom_name_with_no_ext[128]; // name of current rom, used for load/save state

extern FS_Archive sdmcArchive;
//extern FS_DirectoryEntry entry;

extern int emulation;
extern sftd_font *font;
extern sf2d_texture *splash; //*previewtex, 

extern int Vex_cfg_Scalemode;
extern int Vex_cfg_Show_FPS;
extern int Vex_cfg_Frameskip;
extern int Vex_cfg_Color;
extern int Vex_cfg_Sound;
extern int Vex_cfg_Overlay;

extern int exitemulator;

extern char config_roms_path[];
extern char config_base_path[];
extern char config_bios_path[];
extern char config_skin_path[];
extern char config_save_path[];

extern float stretchx, stretchy, stretchr, bgstretchx, bgstretchy;

typedef struct {
	char *itemName;
	int *itemPar;
	int itemParMaxValue;
	char **itemParName;
	void (*itemOnA)();
} MENUITEM;

typedef struct {
	int itemNum; // number of items
	int itemCur; // current item
	MENUITEM *m; // array of items
} MENU;

char const *gui_ScaleNames[] = {"Fit","Rotated", "Split", "Split fit"}; 
char const *gui_YesNo[] = {"No", "Yes"};
char const *gui_ColorNames[] = {"White", "Red" , "Green", "Blue"}; 


MENUITEM gui_MainMenuItems[] = {
	{(char *)"Load rom", NULL, 0, NULL, &gui_FileBrowserRun},
	{(char *)"Reset", NULL, 0, NULL, &gui_Reset},
	{(char *)"Config", NULL, 0, NULL, &gui_ConfigMenuRun},
	{(char *)"Exit", NULL, 0, NULL, &gui_Quitemu} 
};

MENU gui_MainMenu = { 4, 0, (MENUITEM *)&gui_MainMenuItems };

MENUITEM gui_ConfigMenuItems[] = {
	{(char *)"Scaling : ", &Vex_cfg_Scalemode, 3, (char **)&gui_ScaleNames, NULL}, 
	{(char *)"Overlay : ", &Vex_cfg_Overlay, 1, (char **)&gui_YesNo, NULL},
	{(char *)"Sound : ", &Vex_cfg_Sound, 1, (char **)&gui_YesNo, NULL},
	{(char *)"Show fps : ", &Vex_cfg_Show_FPS, 1, (char **)&gui_YesNo, NULL},
	{(char *)"Frameskip : ", &Vex_cfg_Frameskip, 4, NULL, NULL},
	{(char *)"Color: ", &Vex_cfg_Color, 3, (char **)&gui_ColorNames, NULL},
	{(char *)"Save config", NULL, 0, NULL, &gui_SaveConfig}
};


MENU gui_ConfigMenu = { 7, 0, (MENUITEM *)&gui_ConfigMenuItems };


void gui_Quitemu()
{
	done = 1;
	exitemulator=1;
}

	void gui_SaveConfig()
{
	char savename[512];
	sprintf(savename, "%s/Vex3ds.cfg", config_base_path);  // using savename char buffer to save config
	save_config(savename);
	done = 1; 
}


void ShowMenuItem(int x, int y, MENUITEM *m, int fg_color, int showparam)
{
	static char i_str[24];

	sftd_draw_text(font, x, y, fg_color, 10, m->itemName);


	// if parameter, show it on the left
	if((m->itemPar != NULL) && showparam){
		if(m->itemParName == NULL ) {
			// if parameter is a digit
			sprintf(i_str, "%i", *m->itemPar);
		} else {
			// if parameter is a name in array
			sprintf(i_str, "%s", *(m->itemParName + *m->itemPar));
		}
		sftd_draw_text(font, x+125, y, fg_color, 10, i_str);
	}
}

int sort_function(const void *dest_str_ptr, const void *src_str_ptr)
{
	char *dest_str = *((char **)dest_str_ptr);
	char *src_str = *((char **)src_str_ptr);

	if(src_str[0] == '.') return 1;

	if(dest_str[0] == '.') return -1;

	return strcasecmp(dest_str, src_str);
}

int load_file(char **wildcards, char *result, bool startup)
{

	Handle dirHandle;
    FS_DirectoryEntry entry;
	char current_dir_name[MAX__PATH];
    char prev_dir_name[MAX__PATH];
	char current_dir_short[81];
	u32 current_dir_length;
	u32 total_filenames_allocated;
	u32 total_dirnames_allocated;
	char **file_list;
	char **dir_list;
	u32 num_files;
	u32 num_dirs;
	char *file_name;
	u32 file_name_length;
	u32 ext_pos = -1;
	s32 return_value = 1;
	u32 current_file_selection;
	u32 current_file_scroll_value;
	u32 current_dir_selection;
	u32 current_dir_scroll_value;
	u32 current_file_in_scroll;
	u32 current_dir_in_scroll;
	u32 current_file_number, current_dir_number;
	u32 current_column = 0;
	u32 repeat;
	u32 i;

    strcpy(current_dir_name, config_roms_path);
    strcpy(prev_dir_name, current_dir_name);
	while(return_value == 1) {
		current_file_selection = 0;
		current_file_scroll_value = 0;
		current_dir_selection = 0;
		current_dir_scroll_value = 0;
		current_file_in_scroll = 0;
		current_dir_in_scroll = 0;

		total_filenames_allocated = 32;
		total_dirnames_allocated = 32;
		file_list = (char **)malloc(sizeof(char *) * 32);
		dir_list = (char **)malloc(sizeof(char *) * 32);
		memset(file_list, 0, sizeof(char *) * 32);
		memset(dir_list, 0, sizeof(char *) * 32);

		num_files = 0;
		num_dirs = 0;
        
        file_name= (char*) malloc(0x105);

        FS_Path dirPath = (FS_Path){PATH_ASCII, strlen(current_dir_name)+1, (u8*)current_dir_name};
        FSUSER_OpenDirectory(&dirHandle, sdmcArchive, dirPath);

		// DEBUG
		printf("Current directory: %s\n", current_dir_name);
		u32 nread = 0;
		do {
            if(dirHandle) FSDIR_Read(dirHandle, &nread, 1, &entry);

            if(nread) { //(current_file) {
               strncpy_u2a(file_name, entry.name, 0x105);  //utf-16 to ascii function yoinked from blargSNES
				file_name_length = strlen(file_name);

                if(((file_name[0] != '.') || (file_name[1] == '.'))) {
					//if(S_ISDIR(file_info.st_mode)) {    //!!!!!!!!
                    if(entry.attributes & FS_ATTRIBUTE_DIRECTORY) {
                        if((strcmp(file_name, "filer") != 0) && (strcmp(file_name, "Nintendo 3DS") != 0) && (strcmp(file_name, "private") != 0)) {
                            dir_list[num_dirs] = (char *)malloc(file_name_length + 1);
                            strcpy(dir_list[num_dirs], file_name);

                            num_dirs++;
                        }
					} else {
					// Must match one of the wildcards, also ignore the .
						if(file_name_length >= 4) {
							if(file_name[file_name_length - 4] == '.') ext_pos = file_name_length - 4;
							else if(file_name[file_name_length - 3] == '.') ext_pos = file_name_length - 3;
							else ext_pos = 0;

							for(i = 0; wildcards[i] != NULL; i++) {
								if(!strcasecmp((file_name + ext_pos), wildcards[i])) {
									file_list[num_files] = (char *)malloc(file_name_length + 1);

									strcpy(file_list[num_files], file_name);

									num_files++;
									break;
								}
							}
						}
					}
				}

				if(num_files == total_filenames_allocated) {
					file_list = (char **)realloc(file_list, sizeof(char *) * total_filenames_allocated * 2);
					memset(file_list + total_filenames_allocated, 0, sizeof(char *) * total_filenames_allocated);
					total_filenames_allocated *= 2;
				}

				if(num_dirs == total_dirnames_allocated) {
					dir_list = (char **)realloc(dir_list, sizeof(char *) * total_dirnames_allocated * 2);
					memset(dir_list + total_dirnames_allocated, 0, sizeof(char *) * total_dirnames_allocated);
					total_dirnames_allocated *= 2;
				}
			}
        } while(nread); 

		qsort((void *)file_list, num_files, sizeof(char *), sort_function);
		qsort((void *)dir_list, num_dirs, sizeof(char *), sort_function);

        FSDIR_Close(dirHandle);

		current_dir_length = strlen(current_dir_name);

		if(current_dir_length > 80) {
			memcpy(current_dir_short, "...", 3);
			memcpy(current_dir_short + 3, current_dir_name + current_dir_length - 77, 77);
			current_dir_short[80] = 0;
		} else {
			memcpy(current_dir_short, current_dir_name, current_dir_length + 1);
		}

		repeat = 1;

		if(num_files == 0) current_column = 1;
		if(num_dirs == 0) current_column = 0;

		char print_buffer[81];

		while(repeat) {
			sf2d_start_frame(GFX_BOTTOM, GFX_LEFT); //!!
			sftd_draw_text(font, 0, 4, COLOR_ACTIVE_ITEM, 10, current_dir_short);
			const char strMsg[] = "[A] Select Rom [X] Run BIOS [Y] Dir up [B] Back";
			guitextwidth = sftd_get_text_width(font, 10, strMsg);
			sftd_draw_text(font, (320 - guitextwidth) / 2, 225, COLOR_HELP_TEXT, 10, strMsg);
			
			for(i = 0, current_file_number = i + current_file_scroll_value; i < FILE_LIST_ROWS; i++, current_file_number++) {
				if(current_file_number < num_files) {
                    strncpy(print_buffer,file_list[current_file_number], 30);   //38);
                    print_buffer[30] = 0;   //38] = 0;
					if((current_file_number == current_file_selection) && (current_column == 0)) {
						sftd_draw_text(font, FILE_LIST_POSITION, ((i + 2) * 10), COLOR_ACTIVE_ITEM, 10, print_buffer);
					} else {
						sftd_draw_text(font, FILE_LIST_POSITION, ((i + 2) * 10), COLOR_INACTIVE_ITEM, 10, print_buffer);
					}
				}
			}
			for(i = 0, current_dir_number = i + current_dir_scroll_value; i < FILE_LIST_ROWS; i++, current_dir_number++) {
				if(current_dir_number < num_dirs) {
                    strncpy(print_buffer,dir_list[current_dir_number], 8);  //13);
                    print_buffer[9] = 0;    //14] = 0;
					if((current_dir_number == current_dir_selection) && (current_column == 1)) {
						sftd_draw_text(font, DIR_LIST_POSITION, ((i + 2) * 10), COLOR_ACTIVE_ITEM, 10, print_buffer);
					} else {
						sftd_draw_text(font, DIR_LIST_POSITION, ((i + 2) * 10), COLOR_INACTIVE_ITEM, 10, print_buffer);
					}
				}
			}

			// Catch input
			// change to read key state later
            if (aptGetStatus() == APP_PREPARE_SLEEPMODE) {
                aptSignalReadyForSleep();
                aptWaitStatusEvent();
            } else if (aptGetStatus() == APP_SUSPENDING) {
                aptReturnToMenu();
            }
            hidScanInput();
            u32 keydown = hidKeysDown();
                   if (keydown & KEY_A) {  
						if(current_column == 1) {
							if(num_dirs != 0) {
								repeat = 0;
								strcpy(prev_dir_name, current_dir_name);
								if (strlen(current_dir_name)>1) strcat(current_dir_name, "/");
								strcat(current_dir_name, dir_list[current_dir_selection]);
							}
						} else {
							if(num_files != 0) {
								repeat = 0;
								return_value = 0;
								//strcpy(result, file_list[current_file_selection]);
								sprintf(result, "%s/%s", current_dir_name, file_list[current_file_selection]);
								break;
							}
						}
					}
                    if (keydown & KEY_Y) {
                            repeat = 0;
                            char* findpath = strrchr(current_dir_name,'/');
                        if(findpath > current_dir_name) 
                            findpath[0] = '\0';
                        else 
                            findpath[1] = '\0';
                    }
					if (keydown & KEY_B ) { 
						return_value = -1;
						repeat = 0;
						break;
					}
					if (keydown & KEY_X ) { 
						return_value = 1;
						repeat = 0;
						break;
					}
					if (keydown & KEY_UP) {  
						if(current_column == 0) {
							if(current_file_selection) {
								current_file_selection--;
								if(current_file_in_scroll == 0) {
									//clear_screen(COLOR_BG);
									current_file_scroll_value--;
								} else {
									current_file_in_scroll--;
								}
							}
						} else {
							if(current_dir_selection) {
								current_dir_selection--;
								if(current_dir_in_scroll == 0) {
									//clear_screen(COLOR_BG);
									current_dir_scroll_value--;
								} else {
									current_dir_in_scroll--;
								}
							}
						}
					}
					if (keydown & KEY_DOWN) { 
						if(current_column == 0) {
							if(current_file_selection < (num_files - 1)) {
								current_file_selection++;
								if(current_file_in_scroll == (FILE_LIST_ROWS - 1)) {
									//clear_screen(COLOR_BG);
									current_file_scroll_value++;
								} else {
									current_file_in_scroll++;
								}
							}
						} else {
							if(current_dir_selection < (num_dirs - 1)) {
								current_dir_selection++;
								if(current_dir_in_scroll == (FILE_LIST_ROWS - 1)) {
									//clear_screen(COLOR_BG);
									current_dir_scroll_value++;
								} else {
									current_dir_in_scroll++;
								}
							}
						}
					}
					if (keydown & KEY_L) {  
						if(current_column == 0) {
							if(current_file_selection>FILE_LIST_ROWS) {
								current_file_selection-=FILE_LIST_ROWS;
								current_file_scroll_value -= FILE_LIST_ROWS;
								if (current_file_in_scroll>current_file_selection){
									//clear_screen(COLOR_BG);
									current_file_scroll_value=0;
									current_file_in_scroll=current_file_selection;
								}
							} else {
								current_file_selection=0;
								current_file_scroll_value=0;
								current_file_in_scroll=0;
							}
						} else {
							if(current_dir_selection) {
								current_dir_selection--;
								if(current_dir_in_scroll == 0) {
									//clear_screen(COLOR_BG);
									current_dir_scroll_value--;
								} else {
									current_dir_in_scroll--;
								}
							}
						}
					}
					if (keydown & KEY_R) {  
						if(current_column == 0) {
							if(current_file_selection < (num_files - 1 - FILE_LIST_ROWS)) {
								current_file_selection+=FILE_LIST_ROWS;
								current_file_scroll_value+=FILE_LIST_ROWS;
								if (current_file_scroll_value>(num_files - FILE_LIST_ROWS)){
									//clear_screen(COLOR_BG);
									current_file_scroll_value=num_files - FILE_LIST_ROWS;
									current_file_in_scroll=  FILE_LIST_ROWS - (num_files - current_file_selection);
								}
									//clear_screen(COLOR_BG);
							} else {
								current_file_selection = num_files - 1;
								current_file_in_scroll = (num_files<=FILE_LIST_ROWS - 1)?num_files:FILE_LIST_ROWS - 1;
								current_file_scroll_value = (num_files > FILE_LIST_ROWS)?num_files - FILE_LIST_ROWS:0;
							}
						} else {
							if(current_dir_selection < (num_dirs - 1)) {
								current_dir_selection++;
								if(current_dir_in_scroll == (FILE_LIST_ROWS - 1)) {
									//clear_screen(COLOR_BG);
									current_dir_scroll_value++;
								} else {
									current_dir_in_scroll++;
								}
							}
						}
					}
					if (keydown & KEY_LEFT) { 
						if(current_column == 1) {
							if(num_files != 0) current_column = 0;
						}
					}
					if (keydown & KEY_RIGHT) {  
						if(current_column == 0) {
							if(num_dirs != 0) current_column = 1;
						}
					}

            sf2d_end_frame();
            
			gui_DrawTopScreen();
            
            sf2d_swapbuffers();


		}

		// free pointers
		for(i = 0; i < num_files; i++) free(file_list[i]);
		free(file_list);

		for(i = 0; i < num_dirs; i++) free(dir_list[i]);
		free(dir_list);
        
        free(file_name);
	}
	
	
	return return_value;
}

/*
	Rom file browser which is called from menu
*/
char *file_ext[] = { (char *) ".vec", (char *) ".bin", NULL };

void gui_FileBrowserRun()
{

	static char load_filename[512];
	int res = load_file(file_ext, load_filename, false);
	if( res != -1) { // exit if file is chosen
		if (res==0)
			osint_loadrom(load_filename);
		else 
			osint_clearrom();
		done = 1;
//		loadslot = -1;
	}
}

/*
	Rom browser which is called FIRST before all other init
	Return values :		0 - file chosen, name is written at *romname
						1 - file not chosen, run Bios
					   -1 - no file
*/
int gui_LoadFile(char *romname)
{
	return load_file(file_ext, romname, true) ;
}

/*
	Shows previews of load/save and pause
*/
/*
void ShowPreview(MENU *menu)
{
	char prename[256];
	char *prebuffer = (char*)previewtex->data;

	if(menu == &gui_MainMenu && (menu->itemCur == 1 || menu->itemCur == 2)) {   //show savestate preview
		if(loadslot != gui_LoadSlot) {
			// create preview name
			sprintf(prename, "%s/%s.%i.img", config_save_path, rom_name_with_no_ext, gui_LoadSlot);
			// check if file exists
			FILE *fp = fopen(prename, "rb");
			if(fp) {
				fread((char*)prebuffer, 1, 256 * 128 * 4, fp);
				fclose(fp);
			} else 
			if(emptyslot)
				memcpy(prebuffer, (char*)emptyslot->data, 256 * 128 * 4);
			else
				memset(prebuffer, 0xff , 256 * 128 * 4);    // white rectangle if no img loaded
 			loadslot = gui_LoadSlot; // do not load img file each time
		}
	} else {    //show...
		memset(prebuffer, 0x0 , 256 * 128 * 4);    //empty for now, maybe show logo?
        loadslot = -1;  //force reload of preview next time (disable if logo in seperate texture)

	}
}
*/
/*
	Shows menu items 
*/
void ShowMenu(MENU *menu)
{
	int i;
	MENUITEM *mi = menu->m;
	
	int boxColor;
	
    boxColor = RGBA8(0x44,   0x44, 0xaa,   0xff);

	// show menu lines
	for(i = 0; i < menu->itemNum; i++, mi++) {
		int fg_color;
		sf2d_draw_rectangle(10, 35 + i*26, 125, 19, (menu->itemCur == i)?boxColor:COLOR_INACTIVE_ITEM_BG); 
		if(menu->itemCur == i) fg_color = COLOR_ACTIVE_ITEM; else fg_color = COLOR_INACTIVE_ITEM;
		ShowMenuItem(12, 38 + i*26, mi, fg_color,(menu == &gui_MainMenu)?0:1);
	}

	// show preview screen
//	ShowPreview(menu);

	// print info string

    sftd_draw_text(font, 7, 7, RGBA8(0x66,   0x66, 0x66,   0xff), 10, "Vex3ds");
    sftd_draw_text(font, 5, 5, RGBA8(0xff,   0xff, 0xff,   0xff), 10, "Vex3ds");

/*	if (menu == &gui_MainMenu) {
		guitextwidth = sftd_get_text_width(font, 10, Handy_3DS_String_list[HANDY_STR_Back_to_Game]);
		sftd_draw_text(font, (320 - guitextwidth) / 2, 225, COLOR_HELP_TEXT, 10, Handy_3DS_String_list[HANDY_STR_Back_to_Game]);
	} else {
		guitextwidth = sftd_get_text_width(font, 10, Handy_3DS_String_list[HANDY_STR_Back_to_Previous]);
		sftd_draw_text(font, (320 - guitextwidth) / 2, 225, COLOR_HELP_TEXT, 10, Handy_3DS_String_list[HANDY_STR_Back_to_Previous]);
	}
 */
 }


void gui_DrawTopScreen() {

    sf2d_set_clear_color(RGBA8(0x00, 0x00, 0x44, 0xFF));

	sf2d_start_frame(GFX_TOP, GFX_LEFT);
		
	if(splash)
		sf2d_draw_texture_part_rotate_scale(splash, 200, 120, 0, 0, 0, splash->width, splash->height, splash->width/400, splash->height/240);
/*

		sf2d_draw_texture_part_rotate_scale(temptext, 200, 120, 0, 0, 0, 160, 102, 1, 1); //1x
*/
	sf2d_end_frame();
}

/*
	Main function that runs all the stuff
*/
void gui_MainMenuRun(MENU *menu)
{
    APT_AppStatus status;
	MENUITEM *mi;

	done = 0;

	while(!done) {
		mi = menu->m + menu->itemCur; // pointer to highlite menu option

		while((status=aptGetStatus()) != APP_RUNNING) {

			if(status == APP_SUSPENDING)
			{
				aptReturnToMenu();
			}
			else if(status == APP_PREPARE_SLEEPMODE)
			{
				aptSignalReadyForSleep();
				aptWaitStatusEvent();
			}
			else if (status == APP_SLEEPMODE) {
			}
			else if (status == APP_EXITING) {
				return;
			}

		}
        
        hidScanInput();
        u32 keydown = hidKeysDown();
		if (keydown & KEY_A) 
            if(mi->itemOnA != NULL) {
//                gui_ClearScreen();
                (*mi->itemOnA)();
            }
		// B - exit or back to previous menu
		if (keydown & KEY_B) 
            return;
		// UP - arrow down
		if (keydown & KEY_UP)  
            if(--menu->itemCur < 0) menu->itemCur = menu->itemNum - 1;
		// DOWN - arrow up
		if (keydown & KEY_DOWN)  
            if(++menu->itemCur == menu->itemNum) menu->itemCur = 0;
		// LEFT - decrease parameter value
        if (keydown & KEY_LEFT) {  
			if(mi->itemPar != NULL && *mi->itemPar > 0) *mi->itemPar -= 1;
		}
		// RIGHT - increase parameter value
        if (keydown & KEY_RIGHT) {  
			if(mi->itemPar != NULL && *mi->itemPar < mi->itemParMaxValue) *mi->itemPar += 1;
		}
 
        sf2d_start_frame(GFX_BOTTOM, GFX_LEFT);
        if(!done) {
            ShowMenu(menu); // show menu items
        }
                
        sf2d_end_frame();
                
		gui_DrawTopScreen();
 
        sf2d_swapbuffers();
	}
}

void gui_Run()
{
	gui_MainMenuRun(&gui_MainMenu);
}

void gui_ConfigMenuRun()
{
	gui_MainMenuRun(&gui_ConfigMenu);
	osint_updatescale();
	osint_gencolors();
}

void gui_Reset()
{
	osint_reset ();
	done = 1; // mark to exit
}

void strncpy_u2a(char* dst, u16* src, int n)
{
    int i = 0;
    while (i < n && src[i] != '\0')
    {
        if (src[i] & 0xFF00)
            dst[i] = 0x7F;
        else
            dst[i] = (char)src[i];
        
        i++;
    }
    
    dst[i] = '\0';
}









#ifndef __OSINT_H
#define __OSINT_H

#define FPS_LIMIT (50.0)
#define TICKS_PER_SEC (268123480)
#define TICKS_PER_NSEC (0.268123480)
#define TICKS_PER_FRAME (TICKS_PER_SEC/FPS_LIMIT)

extern char gbuffer[1024];

void osint_render (void);
void osint_gencolors (void);
void osint_updatescale (void);
void osint_reset (void);
void osint_clearrom(void);
void osint_loadrom (char* load_filename);
void osint_timer (void);
void save_config(char *file);

#endif
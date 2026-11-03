/*
 * hakuX front end — the dashboard's sounds (sound.c): sci-fi blips and
 * chimes synthesized at start, no files.
 */
#ifndef NATIVE_APP_UI_SOUND_H
#define NATIVE_APP_UI_SOUND_H

#include <stdbool.h>

enum { SND_MOVE, SND_SELECT, SND_BACK, SND_TAB, SND_ERROR, SND_LAUNCH, SND_COUNT };

void snd_init(void);
void snd_enable(bool on);
void snd_play(int which);
void snd_shutdown(void);

#endif

/*
 * xbased (hakuX for PC) — scripted input and pictures (win_auto.c): a run
 * driven and looked at without the desktop (tests, profiles).
 */
#ifndef NATIVE_APP_WIN_AUTO_H
#define NATIVE_APP_WIN_AUTO_H

#include "native/dev/usb.h"

/* HAKUX_AUTO="<s>:<action>,...": at s seconds since the first process of
 * the run started (relaunches go on with its clock), the action. Actions:
 * a button pressed for 0.2 s (A B X Y BLACK WHITE LT RT START BACK UP DOWN
 * LEFT RIGHT LS RS, the sticks LUP LDOWN LLEFT LRIGHT RUP RDOWN RLEFT
 * RRIGHT), or held for d seconds (<button>*<d>); "shot" (a picture of the
 * window, HAKUX_AUTO_DIR\shot_<s>.bmp, the exe's shots folder by default);
 * "prof <secs>" (the vCPU's profile, HAKUX_PROF's, from then); "save <slot>"
 * and "load <slot>" (the front end's states, F5 and F8's); "quit".
 * HAKUX_AUTO_CMD=<file>: the same actions, one per line, done as
 * they are appended ("shot <name>" names the picture). */
void nc_auto_init(const char *exe_dir, void (*save)(int slot), void (*load)(int slot));
/* Each pass of the window's loop: the actions due. */
void nc_auto_tick(void);
/* Player 1's state with the scripted buttons held now. */
void nc_auto_pad(NcPadState *s);
/* The "menu" action happened since the last look (the in-game menu). */
bool nc_auto_take_menu(void);
/* The command file's lines so far skipped (the in-game menu read them). */
void nc_auto_resync(void);

#endif

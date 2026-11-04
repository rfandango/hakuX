/*
 * kantaiX auto-update (update.c): checks the latest release on GitHub,
 * offers to install it over the running AppImage, and relaunches the new
 * version.
 */
#ifndef NATIVE_APP_UI_UPDATE_H
#define NATIVE_APP_UI_UPDATE_H

#include <stdbool.h>

typedef enum UpdState {
    UPD_IDLE,        /* not checked, or nothing newer */
    UPD_CHECKING,
    UPD_AVAILABLE,   /* a newer release (upd_version) */
    UPD_INSTALLING,  /* its download and install (upd_install_start) */
    UPD_INSTALLED,   /* ready: upd_relaunch starts it */
    UPD_FAILED,      /* the install failed: the release's page is the way left */
} UpdState;

/* Removes a partial download left by an interrupted install. */
void upd_cleanup(void);
/* Asks GitHub for the latest release, on a thread; current: this version. */
void upd_check_start(const char *current);
UpdState upd_state(void);
const char *upd_version(void);
/* The release's page (opened when install isn't possible or fails). */
const char *upd_page(void);
/* True when running as an AppImage and an asset URL was found. */
bool upd_can_install(void);
/* Downloads the release's AppImage and installs it, on a thread. */
void upd_install_start(void);
/* Launches the installed version; the caller should quit. */
bool upd_relaunch(void);

#endif

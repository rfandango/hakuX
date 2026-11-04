/*
 * kantaiX: auto-update stub — kantaiX is distributed as CI AppImages;
 * there is no self-update mechanism. All calls are no-ops.
 */
#ifndef NATIVE_APP_UI_UPDATE_H
#define NATIVE_APP_UI_UPDATE_H

#include <stdbool.h>

typedef enum UpdState {
    UPD_IDLE,
    UPD_CHECKING,
    UPD_AVAILABLE,
    UPD_INSTALLING,
    UPD_INSTALLED,
    UPD_FAILED,
} UpdState;

static inline void upd_cleanup(void) {}
static inline void upd_check_start(const char *current) { (void)current; }
static inline UpdState upd_state(void) { return UPD_IDLE; }
static inline const char *upd_version(void) { return ""; }
static inline const char *upd_page(void) { return ""; }
static inline bool upd_can_install(void) { return false; }
static inline void upd_install_start(void) {}
static inline bool upd_relaunch(void) { return false; }

#endif

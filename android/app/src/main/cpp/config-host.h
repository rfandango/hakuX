#ifndef XEMU_ANDROID_CONFIG_HOST_H
#define XEMU_ANDROID_CONFIG_HOST_H

#define CONFIG_ANDROID 1
#define CONFIG_POSIX 1
#define CONFIG_LINUX 1

#define CONFIG_SDL 1
#define CONFIG_SDL2 1
#define CONFIG_AUDIO_SDL 1
#define CONFIG_AUDIO_DRIVERS "sdl",
#define CONFIG_DEVICES "config-devices.h"

#define CONFIG_QEMU_DATADIR "/sdcard/Android/data/com.rfandango.xemuandroid/files"
#define CONFIG_QEMU_FIRMWAREPATH "/sdcard/Android/data/com.rfandango.xemuandroid/files/firmware",
#define CONFIG_SYSCONFDIR "/sdcard/Android/data/com.rfandango.xemuandroid/files/etc"
#define CONFIG_QEMU_CONFDIR "/sdcard/Android/data/com.rfandango.xemuandroid/files/etc/xemu"
#define CONFIG_QEMU_HELPERDIR "/sdcard/Android/data/com.rfandango.xemuandroid/files/bin"
#define CONFIG_PREFIX "/sdcard/Android/data/com.rfandango.xemuandroid/files"
#define CONFIG_BINDIR "/sdcard/Android/data/com.rfandango.xemuandroid/files/bin"
#define CONFIG_QEMU_LOCALSTATEDIR "/sdcard/Android/data/com.rfandango.xemuandroid/files/state"

#define CONFIG_BDRV_RW_WHITELIST
#define CONFIG_BDRV_RO_WHITELIST

#define CONFIG_TCG 1
#define CONFIG_SOFTMMU 1
#define CONFIG_MEMBARRIER 1

#define CONFIG_IOVEC 1

#define CONFIG_FDATASYNC 1
#define CONFIG_CLOCK_GETTIME 1
#define CONFIG_GETTIMEOFDAY 1
#define CONFIG_POSIX_MEMALIGN 1
#define CONFIG_EVENTFD 1
#define CONFIG_PIPE2 1
#define CONFIG_DUP3 1
#define CONFIG_FALLOCATE 1
#define CONFIG_SYNC_FILE_RANGE 1

#define CONFIG_GCOV 0

#define HAVE_GLIB_WITH_SLICE_ALLOCATOR 1

#endif  // XEMU_ANDROID_CONFIG_HOST_H

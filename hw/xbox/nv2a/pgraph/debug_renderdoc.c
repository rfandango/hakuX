/*
 * Geforce NV2A PGRAPH Renderdoc Helpers
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"

#include <stdint.h>
#include <stdbool.h>

#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include "thirdparty/renderdoc_app.h"

#include "hw/xbox/nv2a/debug.h"

#ifdef _WIN32
#include <libloaderapi.h>
#else
#include <dlfcn.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>
#define RDOC_LOG(fmt, ...) __android_log_print(ANDROID_LOG_INFO, \
    "xemu-renderdoc", fmt, ##__VA_ARGS__)
#else
#define RDOC_LOG(fmt, ...) fprintf(stderr, "RenderDoc: " fmt "\n", \
    ##__VA_ARGS__)
#endif

static RENDERDOC_API_1_6_0 *rdoc_api = NULL;

int renderdoc_capture_frames = 0;
bool renderdoc_trace_frames = false;

void nv2a_dbg_renderdoc_init(void)
{
    if (rdoc_api) {
        return;
    }

#ifdef _WIN32
    HMODULE renderdoc = GetModuleHandleA("renderdoc.dll");
    if (!renderdoc) {
        RDOC_LOG("Failed to open renderdoc.dll: 0x%lx", GetLastError());
        return;
    }
    pRENDERDOC_GetAPI RENDERDOC_GetAPI =
        (pRENDERDOC_GetAPI)GetProcAddress(renderdoc, "RENDERDOC_GetAPI");
#else // _WIN32
#ifdef __APPLE__
    void *renderdoc = dlopen("librenderdoc.dylib", RTLD_LAZY);
#else
    static const char *lib_names[] = {
        "librenderdoc.so",
        "libVkLayer_GLES_RenderDoc.so",
        NULL,
    };
    void *renderdoc = NULL;
    for (int i = 0; lib_names[i]; i++) {
        renderdoc = dlopen(lib_names[i], RTLD_NOW | RTLD_NOLOAD);
        if (!renderdoc)
            renderdoc = dlopen(lib_names[i], RTLD_NOW);
        if (renderdoc) {
            RDOC_LOG("loaded via %s", lib_names[i]);
            break;
        }
        RDOC_LOG("dlopen(%s) failed: %s", lib_names[i], dlerror());
    }

    if (!renderdoc) {
        FILE *maps = fopen("/proc/self/maps", "r");
        if (maps) {
            char line[1024];
            while (fgets(line, sizeof(line), maps)) {
                if (!strstr(line, "renderdoc") &&
                    !strstr(line, "RenderDoc"))
                    continue;
                char *path_start = strchr(line, '/');
                if (!path_start) continue;
                char *nl = strchr(path_start, '\n');
                if (nl) *nl = '\0';
                char *sp = strchr(path_start, ' ');
                if (sp) *sp = '\0';
                if (!strstr(path_start, ".so")) continue;
                RDOC_LOG("found in /proc/self/maps: %s", path_start);
                renderdoc = dlopen(path_start, RTLD_NOW);
                if (renderdoc) {
                    RDOC_LOG("loaded via maps path");
                    break;
                }
                RDOC_LOG("dlopen(%s) failed: %s", path_start, dlerror());
            }
            fclose(maps);
        } else {
            RDOC_LOG("cannot open /proc/self/maps");
        }
    }

    pRENDERDOC_GetAPI RENDERDOC_GetAPI = NULL;
    if (renderdoc) {
        RENDERDOC_GetAPI =
            (pRENDERDOC_GetAPI)dlsym(renderdoc, "RENDERDOC_GetAPI");
        if (RENDERDOC_GetAPI) {
            RDOC_LOG("found RENDERDOC_GetAPI via dlsym on handle");
        } else {
            RDOC_LOG("dlsym(handle, RENDERDOC_GetAPI) failed: %s", dlerror());
        }
    }
    if (!RENDERDOC_GetAPI) {
        RENDERDOC_GetAPI =
            (pRENDERDOC_GetAPI)dlsym(RTLD_DEFAULT, "RENDERDOC_GetAPI");
        if (RENDERDOC_GetAPI) {
            RDOC_LOG("found RENDERDOC_GetAPI via RTLD_DEFAULT");
        }
    }
#endif
    if (!RENDERDOC_GetAPI) {
        RDOC_LOG("API not found after all attempts");
        return;
    }
#endif // _WIN32

    if (!RENDERDOC_GetAPI) {
        RDOC_LOG("Could not get RENDERDOC_GetAPI address");
        return;
    }

    int ret =
        RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_6_0, (void **)&rdoc_api);
    if (ret != 1) {
        RDOC_LOG("RENDERDOC_GetAPI returned %d (expected 1)", ret);
    } else {
        RDOC_LOG("API v1.6.0 ready, rdoc_api=%p", (void*)rdoc_api);
    }
}

void *nv2a_dbg_renderdoc_get_api(void)
{
    return (void*)rdoc_api;
}

bool nv2a_dbg_renderdoc_available(void)
{
    return rdoc_api != NULL;
}

void nv2a_dbg_renderdoc_capture_frames(int num_frames, bool trace)
{
    renderdoc_capture_frames += num_frames;
    renderdoc_trace_frames = trace;
}

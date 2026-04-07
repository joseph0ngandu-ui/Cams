// plugin-main.cpp
// Cams OBS Plugin — entry point.
//
// Registers the Cams source with OBS when the plugin is loaded.

extern "C" {
#include <obs-module.h>
}

#include "CamsSource.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-cams", "en-US")

MODULE_EXPORT const char *obs_module_description(void) {
    return "Cams — High-performance iOS camera source over custom UDP protocol";
}

bool obs_module_load(void) {
    cams_register_source();
    blog(LOG_INFO, "[Cams] Plugin loaded. Video port: %u, Control port: %u",
         cams::kVideoPort, cams::kControlPort);
    return true;
}

void obs_module_unload(void) {
    blog(LOG_INFO, "[Cams] Plugin unloaded.");
}

/*
 * plugin-main.cpp — obs-blackhole module entry point.
 *
 * Registers the "BlackHole" dock once the OBS frontend has finished loading,
 * and tears it down on unload. All real work lives in BlackHoleDock /
 * VirtualMicEngine / AudioDeviceControl.
 */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/base.h>

#include "BlackHoleDock.h"

OBS_DECLARE_MODULE()

#define DOCK_ID "obs-blackhole-dock"

static BlackHoleDock *g_dock = nullptr;

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		if (!g_dock) {
			g_dock = new BlackHoleDock();
			obs_frontend_add_dock_by_id(DOCK_ID, "BlackHole", g_dock);
			g_dock->maybeAutoStart();
			blog(LOG_INFO, "[obs-blackhole] dock registered");
		}
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		if (g_dock) {
			g_dock->saveSettings();
			obs_frontend_remove_dock(DOCK_ID);
			g_dock = nullptr; // widget destroyed by OBS
		}
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[obs-blackhole] loaded (v%s)", "1.0.0");
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	if (g_dock) {
		obs_frontend_remove_dock(DOCK_ID);
		g_dock = nullptr;
	}
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "OBS BlackHole Virtual Mic";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Routes OBS audio into the 'OBS Audio' virtual microphone, with an "
	       "in-app dock to control the BlackHole-based driver.";
}

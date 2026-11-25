/*
Copyright (C) 2023 DEV47APPS, github.com/dev47apps
Copyright (C) 2026 Leonardo Berbert

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
extern "C" {
#include <libavcodec/avcodec.h>
}

#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <mutex>
#include <string>
#include <thread>

#include "net.h"
#include "plugin.h"
#include "source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Leonardo Berbert (based on droidcam-obs-plugin by Dev47Apps)")
OBS_MODULE_USE_DEFAULT_LOCALE("niriun-obs", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Use a phone running Niriun as a camera source, with tally.";
}

MODULE_EXPORT const char *obs_module_name(void)
{
    return "Niriun";
}

static std::mutex g_bind_lock;
static std::string g_bind_ip;

std::string niriun_bind_ip(void)
{
    std::lock_guard<std::mutex> g(g_bind_lock);
    return g_bind_ip;
}

static void update_bind_ip(void)
{
    config_t *profile = obs_frontend_get_profile_config();
    if (!profile)
        return;
    const char *ip = config_get_string(profile, "Output", "BindIP");
    std::lock_guard<std::mutex> g(g_bind_lock);
    g_bind_ip = (ip && strcmp(ip, "default") != 0) ? ip : "";
    ilog("using bind IP '%s'", g_bind_ip.c_str());
}

static void frontend_event(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_PROFILE_CHANGED)
        update_bind_ip();
}

bool obs_module_load(void)
{
    if (AV_VERSION_MAJOR(avcodec_version()) != LIBAVCODEC_VERSION_MAJOR) {
        blog(LOG_ERROR, "[niriun-obs] libavcodec major version %u does not match the %d this plugin was built for.",
             AV_VERSION_MAJOR(avcodec_version()), LIBAVCODEC_VERSION_MAJOR);
        return false;
    }

    if (!net_init())
        return false;

    register_niriun_source();
    obs_frontend_add_event_callback(frontend_event, NULL);

    blog(LOG_INFO, "[niriun-obs] module loaded, version %s", NIRIUN_VERSION_STR);
    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(frontend_event, NULL);
    niriun_source_shutdown();
    net_cleanup();
}

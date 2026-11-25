// Copyright (C) 2023 DEV47APPS, github.com/dev47apps
// Copyright (C) 2026 Leonardo Berbert
#pragma once

#include <obs-module.h>

#include <string>

/** Registers the "niriun_source" OBS source. */
void register_niriun_source(void);

/** Stops the background helpers started by the first source. Call from obs_module_unload. */
void niriun_source_shutdown(void);

/** OBS "Network > Bind to IP" of the current profile, "" for the default. Defined in plugin.cc. */
std::string niriun_bind_ip(void);

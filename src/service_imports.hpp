#pragma once
#include "mods/service.hpp"
#include "mods/svc/log.h"
#include "mods/svc/hook.h"
#include "mods/svc/resource.h"
#include "mods/svc/config.h"
#include "mods/svc/ui.h"

extern const LogService* svc_log;
extern const HookService* svc_hook;
extern const ResourceService* svc_resource;
extern const ConfigService* svc_config;
extern const UiService* svc_ui;
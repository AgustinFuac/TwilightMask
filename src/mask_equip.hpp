#pragma once
#include "mods/svc/hook.h"
namespace dawnlight {
	ModResult mask_equip_init(ModError* error);
	void mask_equip_shutdown();
	bool mask_is_equipped();
	void mask_set_equipped(bool equipped);
}
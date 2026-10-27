#pragma once

#include <mods/api.h>

namespace dawnlight {

// Instala los hooks del slot de la máscara en la pantalla de colección.
ModResult mask_collect_init(ModError* error);
void mask_collect_shutdown();

}  // namespace dawnlight

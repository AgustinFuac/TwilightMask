#pragma once
#include <mtx.h>
struct dKy_tevstr_c;
namespace dawnlight {
// BMD de la máscara: overlay opcional primero, modelo embebido como fallback.
void prepare_mask_bmd();
bool draw_mask_bmd(Mtx transform, dKy_tevstr_c* lighting);
void shutdown_mask_bmd();
bool entry_mask_bmd_menu(Mtx transform, dKy_tevstr_c* lighting);
}
#include "mods/svc/hook.hpp"
#include "mods/svc/log.h"
#include "service_imports.hpp"

// Game includes
#include "d/d_item_data.h"
#include "f_op/f_op_actor_mng.h"

#include "mask_equip.hpp"
#include "mask_collect_slot.hpp"   // NUEVO

DEFINE_MOD();

DEFINE_HOOK(fopAcM_createItem, CreateItem);

static HookAction on_create_item_pre(ModContext*, void* args, void*, void*) {
    int& itemNo = mods::arg_ref<int>(args, 1);
    if (itemNo == dItemNo_HEART_e) {
        itemNo = dItemNo_GREEN_RUPEE_e;
    }
    return HOOK_CONTINUE;
}

extern "C" {
    MOD_EXPORT ModResult mod_initialize(ModError* e) {
        ModResult result = mods::hook::add_pre<CreateItem>(on_create_item_pre);
        if (result != MOD_OK) {
            svc_log->error(mod_ctx, "failed to install on_create_item_pre");
            return result;
        }

        result = dawnlight::mask_equip_init(e);
        if (result != MOD_OK) {
            svc_log->error(mod_ctx, "failed to install mask_equip");
            return result;
        }

        // NUEVO
        result = dawnlight::mask_collect_init(e);
        if (result != MOD_OK) {
            svc_log->error(mod_ctx, "failed to install mask_collect");
            return result;
        }

        svc_log->info(mod_ctx, "Mask_mod initialized");
        return MOD_OK;
    }

    MOD_EXPORT ModResult mod_update(ModError*) {
        return MOD_OK;
    }

    MOD_EXPORT ModResult mod_shutdown(ModError*) {
        dawnlight::mask_collect_shutdown();   // NUEVO
        dawnlight::mask_equip_shutdown();
        return MOD_OK;
    }
}
#include "mask_bmd.hpp"
#include "service_imports.hpp"
#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/config.h"
#include "mods/svc/ui.h"
#include "d/actor/d_a_alink.h"
#include "d/d_cc_uty.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_mtx.h"
#include "JSystem/J3DGraphBase/J3DDrawBuffer.h"
#include "JSystem/J3DGraphBase/J3DPacket.h"
#include <cmath>
#include "SSystem/SComponent/c_math.h"   // cM_rndF
#include <cstdio>
#include "d/d_menu_collect.h"

namespace dawnlight {
    extern bool g_maskMenuPass;
    namespace {

        // ============================================================
        // Ajustes internos (se cambian aquí y se recompila)
        // ============================================================

        // Colocación de la máscara respecto al joint de la cabeza.
        constexpr float kMaskScale = 0.25f;
        constexpr float kMaskOffX = 14.5f, kMaskOffY = -10.5f, kMaskOffZ = 0.0f;   // unidades del juego, espacio del joint
        constexpr float kMaskRotX = -90.0f, kMaskRotY = 0.0f, kMaskRotZ = -90.0f;   // grados

        // Multiplicador de daño que hace Link con el potenciador activo.
        constexpr int kMaskDamageMultiplier = 10;

        // Ocultar la máscara si la cámara está más cerca que esto de la cabeza
        // (vista subjetiva: zarpa, arco, etc.).
        constexpr float kHideDistance = 60.0f;

        // Frames que la máscara sigue oculta tras un warp.
        constexpr int kWarpCooldownFrames = 30;

        // Aura de partículas.
        // El aura solo se muestra con Link quieto.
        constexpr float kAuraMaxSpeed = 1.0f;    // speedF por encima de esto = en movimiento
        constexpr int kAuraStillFrames = 10;     // frames quieto antes de que aparezca
        // Cada efecto del aura tiene sus propios ajustes. Añade o quita filas de la tabla.
        struct AuraFx {
            u16 particle;                // ID de d_particle_name.h (con 0x8000 si es del pack de sala)
            float offX, offY, offZ;      // offset en el espacio del joint de la cabeza
            float rotX, rotY, rotZ;      // grados, sumados a la orientación de Link
            float scale;                 // 1.0 = tamaño original
            u8 alpha;                    // 0 a 255
            int minTicks;                // ticks mínimos entre llamadas al emisor
            int randTicks;               // + hasta este extra aleatorio (0 = fijo)
        };

        constexpr AuraFx kAuras[] = {
            // particle,  offX,       offY,       offZ,        rotX, rotY, rotZ,  scale, alpha min, rand
            { 0x109,    kMaskOffX, kMaskOffY, kMaskOffZ,  0,   0,   0,    0.2f, 30,  0,  6 },   // ligts
            { 0x108,    kMaskOffX - 10.0f, kMaskOffY, kMaskOffZ,  90,  0,   0,    0.2f, 15,  0,  8 },   // <- efecto humo
            { 0x109,    kMaskOffX - 5.0f, kMaskOffY+50.0f, kMaskOffZ,  0,   0,   0,    0.6f, 50,  6,  6 },   // efecto 3 (opcional)
        };
        constexpr int kAuraCount = sizeof(kAuras) / sizeof(kAuras[0]);
        
        // ============================================================
        // Estado
        // ============================================================

        ConfigVarHandle s_varMaskEquipped;
        ConfigVarHandle s_varPowerEnabled;

        bool g_maskModelPrepared = false;
        int s_warpCooldown = 0;

        u32 s_auraId[kAuraCount] = {};
        int s_auraTimer[kAuraCount] = {};
        int s_auraStillTicks = 0;
        bool s_maskVisible = false;   // lo escribe MaskPacket::draw()

        bool mask_equipped() {
            bool v = false;
            svc_config->get_bool(mod_ctx, s_varMaskEquipped, &v);
            return v;
        }

        bool power_enabled() {
            bool v = false;
            svc_config->get_bool(mod_ctx, s_varPowerEnabled, &v);
            return v;
        }

        // ============================================================
        // Pose interpolada
        // ============================================================

        // Resuelto vía manifest del host: las funciones de interpolación son privadas.
        using LookupPresentationMatrix = bool (*)(const void*, Mtx);
        LookupPresentationMatrix s_lookupPresentationMatrix = nullptr;

        void presented_matrix(MtxP source, Mtx out) {
            if (!s_lookupPresentationMatrix || !s_lookupPresentationMatrix(source, out))
                MTXCopy(source, out);
        }

        // ============================================================
        // Aura
        // ============================================================

        void stop_aura() {
            for (int i = 0; i < kAuraCount; ++i) {
                if (s_auraId[i] != 0) {
                    dComIfGp_particle_setStopContinue(s_auraId[i]);
                    s_auraId[i] = 0;
                }
                s_auraTimer[i] = 0;
            }
        }

        int next_aura_ticks(const AuraFx& fx) {
            int t = fx.minTicks;
            if (fx.randTicks > 0) t += static_cast<int>(cM_rndF(static_cast<float>(fx.randTicks)));
            return t < 1 ? 1 : t;
        }

        void update_aura(daAlink_c* link) {
            Mtx head;
            presented_matrix(link->getHeadMtx(), head);

            for (int i = 0; i < kAuraCount; ++i) {
                const AuraFx& fx = kAuras[i];
                if (--s_auraTimer[i] > 0) continue;
                s_auraTimer[i] = next_aura_ticks(fx);

                Vec local = { fx.offX, fx.offY, fx.offZ };
                Vec world;
                MTXMultVec(head, &local, &world);

                cXyz pos;
                pos.set(world.x, world.y, world.z);

                cXyz scale;
                scale.set(fx.scale, fx.scale, fx.scale);

                // Orientación de Link + rotación fija del efecto (grados -> unidades del juego).
                csXyz rot = link->shape_angle;
                rot.x += static_cast<s16>(fx.rotX * (65536.0f / 360.0f));
                rot.y += static_cast<s16>(fx.rotY * (65536.0f / 360.0f));
                rot.z += static_cast<s16>(fx.rotZ * (65536.0f / 360.0f));

                // Mismo patrón que el fairy: re-llamar con el mismo ID mantiene el emisor.
                s_auraId[i] = dComIfGp_particle_set(
                    s_auraId[i], fx.particle, &pos, &link->tevStr, &rot,
                    &scale, fx.alpha, NULL, -1, NULL, NULL, NULL);
            }
        }

        // ============================================================
        // Dibujo de la máscara
        // ============================================================

        // Cámara del menú, guardada cuando el menú dibuja a Link.
        Mtx s_menuView;
        Mtx s_menuHead;               // matriz de la cabeza cuando el menú dibuja a Link
        bool s_menuHeadValid = false;
        bool s_menuViewValid = false;

        // Cambia la cámara de j3dSys mientras dura el dibujo y la restaura al salir.
        struct ViewScope {
            bool on;
            Mtx saved;
            ViewScope(bool enable, const Mtx view) : on(enable) {
                if (on) {
                    MTXCopy(j3dSys.getViewMtx(), saved);
                    MTXCopy(view, j3dSys.getViewMtx());
                }
            }
            ~ViewScope() {
                if (on) MTXCopy(saved, j3dSys.getViewMtx());
            }
        };

        // Solo guarda el ID del actor entre frames; la pose viva se resuelve en draw(),
        // que el motor llama en su pasada normal de render (igual que GliderPacket).
        class MaskPacket : public J3DPacket {
        public:
            fpc_ProcID ownerId = fpcM_ERROR_PROCESS_ID_e;
            bool active = false;
            bool menu = false;   // true = se dibuja dentro del menú de colección

            void draw() override {
                if (menu) { static int n = 0; if ((n++ % 120) == 0 && svc_log) svc_log->info(mod_ctx, active ? "Mask menu: draw() called, active" : "Mask menu: draw() called, NOT active"); }
                s_maskVisible = false;
                if (!active) return;

                auto* actor = fopAcM_SearchByID(ownerId);
                if (!actor || fopAcM_GetName(actor) != fpcNm_ALINK_e) return;
                auto* link = static_cast<daAlink_c*>(actor);
                if (!link->mpLinkModel || link->checkWolf() ||
                    link->checkPlayerNoDraw() || (!menu && link->checkStatusWindowDraw())) return;
                if (menu && !s_menuHeadValid) return;
                Mtx itemView;
                if (menu) dMenu_Collect3D_c::setupItem3D(itemView);   // cámara del menú, pedida al juego
                ViewScope viewScope(menu, itemView);
                //if (!link->mpLinkModel || link->checkWolf() ||
                //    link->checkPlayerNoDraw() || link->checkStatusWindowDraw()) return;

                // Matriz del joint de la cabeza, interpolada como el renderer de Link.
                Mtx head;
                //presented_matrix(link->getHeadMtx(), head);
                if (menu) MTXCopy(s_menuHead, head);
                else presented_matrix(link->getHeadMtx(), head);

                // Cámara pegada a la cabeza (vista subjetiva): no dibujar.
                Mtx headInView;
                MTXConcat(j3dSys.getViewMtx(), head, headInView);
                const float dx = headInView[0][3], dy = headInView[1][3], dz = headInView[2][3];
                //if (std::sqrt(dx * dx + dy * dy + dz * dz) < kHideDistance) return;
                if (menu) {
                    static int n = 0;
                    if ((n++ % 120) == 0 && svc_log) {
                        char m[160];
                        std::snprintf(m, sizeof(m), "Mask menu: head z=%.1f dist=%.1f", dz, std::sqrt(dx * dx + dy * dy + dz * dz));
                        svc_log->info(mod_ctx, m);
                    }
                }
                if (!menu && std::sqrt(dx * dx + dy * dy + dz * dz) < kHideDistance) return;

                // Rotación fija X * Y * Z en el espacio del joint.
                Mtx rx, ry, rz, tmp, rot;
                MTXRotDeg(rx, 'x', kMaskRotX);
                MTXRotDeg(ry, 'y', kMaskRotY);
                MTXRotDeg(rz, 'z', kMaskRotZ);
                MTXConcat(rx, ry, tmp);
                MTXConcat(tmp, rz, rot);

                // Escala + offset sobre la rotación.
                Mtx local;
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) local[i][j] = rot[i][j] * kMaskScale;
                local[0][3] = kMaskOffX;
                local[1][3] = kMaskOffY;
                local[2][3] = kMaskOffZ;

                Mtx transform;
                MTXConcat(head, local, transform);

                //if (draw_mask_bmd(transform, &link->tevStr)) s_maskVisible = true;
                g_maskMenuPass = menu;
                if (draw_mask_bmd(transform, &link->tevStr)) s_maskVisible = true;
                g_maskMenuPass = false;
            }
        };
        MaskPacket s_maskPacket;

        void queue_mask_packet(J3DPacket* packet) {
            auto* list = dComIfGd_getOpaList();
            if (!list || !list->mpBuffer || list->getEntryTableSize() == 0) return;
            // entryImm es un prepend intrusivo, no un set: reinsertar un packet ya
            // encolado crea un ciclo. Se verifica la pertenencia real a la lista.
            for (auto* queued = list->mpBuffer[0]; queued; queued = queued->getNextPacket()) {
                if (queued == packet) return;
            }
            list->entryImm(packet, 0);
        }

        MaskPacket s_menuMaskPacket;

        // Menú de colección: Link se dibuja en una lista especial (statusWindowDraw).
        DEFINE_HOOK(&daAlink_c::statusWindowDraw, AlinkStatusDrawHook);

        void on_status_window_draw_post(ModContext*, void* args, void*, void*) {
            //{ static int n = 0; if ((n++ % 120) == 0 && svc_log) svc_log->info(mod_ctx, "Mask menu: statusWindowDraw hook running"); }
            auto* link = mods::arg<daAlink_c*>(args, 0);
            if (!link || !mask_equipped() || link->checkWolf() ||
                link->checkPlayerNoDraw() || !link->mpLinkModel) return;

            if (!g_maskModelPrepared) {
                prepare_mask_bmd();
                g_maskModelPrepared = true;
            }
            //MTXCopy(link->getHeadMtx(), s_menuHead);
            // Misma transformada que en el mundo: head * (rotación*escala + offset)
            Mtx head, rx, ry, rz, tmp, rot, local, transform;
            MTXCopy(link->getHeadMtx(), head);
            MTXRotDeg(rx, 'x', kMaskRotX);
            MTXRotDeg(ry, 'y', kMaskRotY);
            MTXRotDeg(rz, 'z', kMaskRotZ);
            MTXConcat(rx, ry, tmp);
            MTXConcat(tmp, rz, rot);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) local[i][j] = rot[i][j] * kMaskScale;
            local[0][3] = kMaskOffX;
            local[1][3] = kMaskOffY;
            local[2][3] = kMaskOffZ;
            MTXConcat(head, local, transform);

            entry_mask_bmd_menu(transform, &link->tevStr);
        }

        // ============================================================
        // Hook visual: solo actualiza datos y encola, no dibuja
        // ============================================================

        DEFINE_HOOK(&daAlink_c::draw, AlinkDrawHook);

        void on_mask_draw_post(ModContext*, void* args, void*, void*) {
            auto* link = mods::arg<daAlink_c*>(args, 0);
            if (link == nullptr) return;

            // Ocultar durante el efecto de aparición tras un warp.
            const auto proc = link->mProcID;
            if (proc == daAlink_c::PROC_WARP ||
                proc == daAlink_c::PROC_DUNGEON_WARP ||
                proc == daAlink_c::PROC_DUNGEON_WARP_SCN_START) {
                s_warpCooldown = kWarpCooldownFrames;
            }
            if (s_warpCooldown > 0) {
                --s_warpCooldown;
                s_maskPacket.active = false;
                stop_aura();
                return;
            }

            if (!mask_equipped() || link->checkWolf() || link->checkPlayerNoDraw() ||
                !link->mpLinkModel || link->checkStatusWindowDraw()) {
                s_maskPacket.active = false;
                stop_aura();
                return;
            }

            if (!g_maskModelPrepared) {
                prepare_mask_bmd();
                g_maskModelPrepared = true;
            }


            // s_maskVisible lo calcula draw() (un frame de retraso, no se nota).
            // Aura solo con la máscara visible y Link quieto.
            if (link->speedF > kAuraMaxSpeed) s_auraStillTicks = 0;
            else if (s_auraStillTicks < kAuraStillFrames) ++s_auraStillTicks;

            if (s_maskVisible && s_auraStillTicks >= kAuraStillFrames) update_aura(link);
            else stop_aura();

            s_maskPacket.ownerId = fopAcM_GetID(link);
            s_maskPacket.active = true;
            queue_mask_packet(&s_maskPacket);
        }

        // ============================================================
        // Hook de daño: multiplica el poder de ataque de Link
        // ============================================================

        // at_power_check corre justo antes de cc_at_check, así que el multiplicador
        // llega a tiempo de afectar la vida del enemigo.
        DEFINE_HOOK(&at_power_check, AtPowerCheckHook);

        void on_at_power_check_post(ModContext*, void* args, void*, void*) {
            if (!power_enabled() || !mask_equipped()) return;

            auto* info = mods::arg<dCcU_AtInfo*>(args, 0);
            if (!info || info->mAttackPower == 0) return;

            // Solo golpes de Link.
            switch (info->mHitType) {
            case HIT_TYPE_LINK_NORMAL_ATTACK:
            case HIT_TYPE_LINK_HEAVY_ATTACK:
            case HIT_TYPE_ARROW:
            case HIT_TYPE_BOMB:
            case HIT_TYPE_BOOMERANG:
                break;
            default:
                return;
            }

            const uint32_t p = static_cast<uint32_t>(info->mAttackPower) * kMaskDamageMultiplier;
            info->mAttackPower = static_cast<u16>(p > 0xFFFF ? 0xFFFF : p);
        }

        // ============================================================
        // Panel de configuración
        // ============================================================

        ModResult build_mask_panel(ModContext*, UiElementHandle pane, void*, ModError*) {
            UiControlDesc equippedCtrl = UI_CONTROL_DESC_INIT;
            equippedCtrl.kind = UI_CONTROL_TOGGLE;
            equippedCtrl.label = "Invoke Majora's Mask";
            equippedCtrl.binding = UI_BINDING_CONFIG_VAR;
            equippedCtrl.config_var = s_varMaskEquipped;
            ModResult r = svc_ui->pane_add_control(mod_ctx, pane, &equippedCtrl, nullptr);
            if (r != MOD_OK) return r;

            UiControlDesc powerCtrl = UI_CONTROL_DESC_INIT;
            powerCtrl.kind = UI_CONTROL_TOGGLE;
            powerCtrl.label = "Use the Power of Majora";
            powerCtrl.binding = UI_BINDING_CONFIG_VAR;
            powerCtrl.config_var = s_varPowerEnabled;
            return svc_ui->pane_add_control(mod_ctx, pane, &powerCtrl, nullptr);
        }

    }  // namespace

    // ============================================================
    // Ciclo de vida
    // ============================================================

    ModResult mask_equip_init(ModError*) {
        // Interpolación de pose (opcional: si falta se usa la pose de simulación).
        s_lookupPresentationMatrix = nullptr;
        void* interpAddr = nullptr;
        if (svc_hook && svc_hook->resolve &&
            svc_hook->resolve(mod_ctx, "dusk::interp::lookup_replacement", &interpAddr, nullptr) == MOD_OK)
            s_lookupPresentationMatrix = reinterpret_cast<LookupPresentationMatrix>(interpAddr);
        if (!s_lookupPresentationMatrix && svc_log)
            svc_log->warn(mod_ctx, "Mask: presentation matrix lookup unavailable; using simulation pose");

        // Variables de configuración.
        ConfigVarDesc equippedDesc = CONFIG_VAR_DESC_INIT;
        equippedDesc.name = "mask_equipped";
        equippedDesc.type = CONFIG_VAR_BOOL;
        equippedDesc.default_bool = false;
        ModResult result = svc_config->register_var(mod_ctx, &equippedDesc, &s_varMaskEquipped);
        if (result != MOD_OK) return result;

        ConfigVarDesc powerDesc = CONFIG_VAR_DESC_INIT;
        powerDesc.name = "mask_power_enabled";
        powerDesc.type = CONFIG_VAR_BOOL;
        powerDesc.default_bool = false;
        result = svc_config->register_var(mod_ctx, &powerDesc, &s_varPowerEnabled);
        if (result != MOD_OK) return result;

        // Hooks.
        result = mods::hook::add_post<AlinkDrawHook>(on_mask_draw_post);
        if (result != MOD_OK) return result;

        result = mods::hook::add_post<AtPowerCheckHook>(on_at_power_check_post);
        if (result != MOD_OK) return result;

		// Menu de coleccion
        s_menuMaskPacket.menu = true;
        result = mods::hook::add_post<AlinkStatusDrawHook>(on_status_window_draw_post);
        if (result != MOD_OK) return result;

        // Panel.
        UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
        panel.build = build_mask_panel;
        return svc_ui->register_mods_panel(mod_ctx, &panel);
    }

    //llamada a slot
    bool mask_is_equipped() { return mask_equipped(); }
    void mask_set_equipped(bool v) { svc_config->set_bool(mod_ctx, s_varMaskEquipped, v); }

    void mask_equip_shutdown() {
        stop_aura();
        s_auraStillTicks = 0;
        s_maskVisible = false;
        s_warpCooldown = 0;
        s_maskPacket.active = false;
        s_maskPacket.ownerId = fpcM_ERROR_PROCESS_ID_e;
        shutdown_mask_bmd();
        g_maskModelPrepared = false;
        s_lookupPresentationMatrix = nullptr;
        s_menuMaskPacket.active = false;
        s_menuHeadValid = false;
        s_menuViewValid = false;
    }

}  // namespace dawnlight
// Slot de la Máscara de Majora en la pantalla de colección (fase 3: slot independiente).
//
// - Paneles reales del juego (texturas del propio juego), colgados del mismo padre
//   que la visualización de Link: siguen la animación y el layout ancho.
// - Va en la esquina inferior derecha del recuadro de Link.
// - Se llega pulsando Izquierda desde la primera celda de la fila de ropa. Mientras
//   está seleccionado, usamos el cursor NATIVO del juego (le "prestamos" nuestra
//   celda), no dibujamos nada a mano. A activa/desactiva la máscara.

#include "mask_collect_slot.hpp"
#include "mask_equip.hpp"
#include "service_imports.hpp"

#include "mods/svc/hook.hpp"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"

#include "d/d_com_inf_game.h"
#include "d/d_menu_collect.h"   //  dMenu_Collect2D_c
#include "d/d_menu_window.h"    //  dMw_A_TRIGGER(), dMw_LEFT_TRIGGER()...
#include "d/d_pane_class.h"     //  CPaneMgr
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_controller_pad.h"

#include "JSystem/J2DGraph/J2DTextBox.h"
#include "JSystem/J2DGraph/J2DPicture.h"
#include "JSystem/J2DGraph/J2DScreen.h"
#include "JSystem/JKernel/JKRHeap.h"

#include "m_Do/m_Do_audio.h"
#include "Z2AudioLib/Z2SeMgr.h"

#ifdef _WIN32
#include <malloc.h>   // _aligned_malloc / _aligned_free (MSVC)
#endif

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>

namespace dawnlight {
namespace {

    // ============================================================
    // Ajustes
    // ============================================================

    constexpr const char* kIconPath = "IconMM.bti";   // res/IconMM.bti

    constexpr bool kDumpPanes = false;   // true = vuelca la lista de paneles al log

    // Posición: esquina inferior derecha del recuadro de Link, con estos márgenes
    // (píxeles del layout 608x448) y un desplazamiento extra.
    constexpr float kInsetX = 15.0f;    // margen respecto al borde derecho del recuadro de Link
    constexpr float kInsetTop = 17.5f;  // margen respecto al borde superior
    constexpr float kNudgeX = 0.0f;
    constexpr float kNudgeY = 0.0f;

    // Colores del marco (el juego usa una textura blanca y la tiñe por código).
    // TODO: sustituir por los colores reales del juego (setEquipItemFrameColor*).
    constexpr u8 kTintIdle[3] = { 107, 107, 107 };   // igual que el juego
    constexpr u8 kTintEquip[3] = { 198, 189, 0 };     // igual que el juego
    constexpr u8 kTintFocus[3] = { 255, 255, 0 };     // sin uso (el estado 2 está comentado)

    constexpr bool kUsePlate = true;   // fondo oscuro detrás del icono (textura de casilla vacía)

    // ============================================================
    // Layout interno de dMenu_Collect2D_c (offsets de Ghidra). [VERIFICAR]
    // ============================================================

    constexpr size_t kOffScreen = 0x30;     // J2DScreen*
    constexpr size_t kOffGrid = 0x108;      // CPaneMgr* [columna][fila]
    constexpr size_t kGridColStride = 0x30;
    constexpr size_t kGridRowStride = 0x08;
    constexpr size_t kOffCursorX = 0x373;   // u8
    constexpr size_t kOffCursorY = 0x374;   // u8

    constexpr u8 kRowClothes = 0;   
    constexpr u8 kColFirst = 0;

    template <typename T>
    T& member(void* base, size_t offset) {
        return *reinterpret_cast<T*>(static_cast<u8*>(base) + offset);
    }

    void*& grid_cell(dMenu_Collect2D_c* self, int col, int row) {
        return member<void*>(self, kOffGrid + col * kGridColStride + row * kGridRowStride);
    }

    // ============================================================
    // Estado
    // ============================================================

    dMenu_Collect2D_c* s_collect = nullptr;
    bool s_built = false;
    int s_buildTries = 0;
    bool s_dumped = false;
    bool s_focus = false;
    bool s_armed = false;
    bool s_nameApplied = false;
    int s_armX = 0;
    int s_armY = 0;
    int s_savedCol = 0;
    int s_savedRow = 0;
    void* s_savedCell = nullptr;

    CPaneMgr* s_mgr = nullptr;
    J2DPicture* s_plate = nullptr;   // los libera el juego al borrar la pantalla
    J2DPicture* s_icon = nullptr;
    J2DPicture* s_frame = nullptr;
    int s_lastTint = -1;

    ResourceBuffer s_iconBuf = RESOURCE_BUFFER_INIT;
    u8* s_iconCopy = nullptr;        // ResTIMG propio, vive todo el mod

    const u64 kIdPlate = MULTI_CHAR('mmslot_p');
    const u64 kIdIcon = MULTI_CHAR('mmslot_i');
    const u64 kIdFrame = MULTI_CHAR('mmslot_f');

    // ============================================================
    // Icono (ResTIMG con alfa activado)
    // ============================================================

    const ResTIMG* get_icon_image() {
        if (s_iconCopy) return reinterpret_cast<const ResTIMG*>(s_iconCopy);

        if (svc_resource->load(mod_ctx, kIconPath, &s_iconBuf) != MOD_OK || !s_iconBuf.data) {
            if (svc_log) svc_log->warn(mod_ctx, "Mask slot: icon not found");
            return nullptr;
        }
        const size_t size = static_cast<size_t>(s_iconBuf.size);
        if (size < 32) {
            svc_resource->free(mod_ctx, &s_iconBuf);
            return nullptr;
        }
#ifdef _WIN32
        s_iconCopy = static_cast<u8*>(_aligned_malloc(size, 32));
#else
        void* ptr = nullptr;
        if (posix_memalign(&ptr, 32, size) == 0) {
            s_iconCopy = static_cast<u8*>(ptr);
        }
        else {
            s_iconCopy = nullptr;
        }
#endif
        if (s_iconCopy) {
            std::memcpy(s_iconCopy, s_iconBuf.data, size);
            s_iconCopy[1] = 1;   // ResTIMG::alphaEnabled
        }
        svc_resource->free(mod_ctx, &s_iconBuf);
        return reinterpret_cast<const ResTIMG*>(s_iconCopy);
    }

    void release_icon() {
        if (s_iconCopy) {
#ifdef _WIN32
            _aligned_free(s_iconCopy);
#else
            free(s_iconCopy);
#endif
            s_iconCopy = nullptr;
        }
    }

    // ============================================================
    // Utilidades de paneles
    // ============================================================

    J2DPane* find_parent(J2DPane* root, J2DPane* target, int depth = 0) {
        if (!root || depth > 16) return nullptr;
        for (J2DPane* c = root->getFirstChildPane(); c; c = c->getNextChildPane()) {
            if (c == target) return root;
            if (J2DPane* r = find_parent(c, target, depth + 1)) return r;
        }
        return nullptr;
    }

    void dump_pane(J2DPane* p, int depth) {
        if (!p || depth > 12) return;
        char name[9] = {};
        const u64 tag = *reinterpret_cast<const u64*>(reinterpret_cast<const u8*>(p) + 0x10);
        int n = 0;
        for (int i = 7; i >= 0; --i) {
            const char c = static_cast<char>((tag >> (i * 8)) & 0xFF);
            if (c) name[n++] = c;
        }
        const JGeometry::TBox2<f32>& b = p->getGlbBounds();
        char line[192];
        std::snprintf(line, sizeof(line), "pane %*s%s vis=%d x=%.0f y=%.0f w=%.0f h=%.0f",
                      depth * 2, "", name[0] ? name : "(none)", p->isVisible() ? 1 : 0,
                      static_cast<double>(b.i.x), static_cast<double>(b.i.y),
                      static_cast<double>(b.getWidth()), static_cast<double>(b.getHeight()));
        if (svc_log) svc_log->info(mod_ctx, line);
        for (J2DPane* c = p->getFirstChildPane(); c; c = c->getNextChildPane()) {
            dump_pane(c, depth + 1);
        }
    }

    // Textura (ResTIMG) de un panel J2DPicture, o nullptr si no lo es.
    const ResTIMG* picture_image(J2DPane* pane) {
        if (!pane || pane->getTypeID() != 0x12) return nullptr;   // [VERIFICAR] 0x12 = J2DPicture
        JUTTexture* tex = static_cast<J2DPicture*>(pane)->getTexture(0);
        return tex ? tex->getTexInfo() : nullptr;
    }

    // ============================================================
    // Construcción del slot
    // ============================================================

    bool build_slot(dMenu_Collect2D_c* self) {
        J2DScreen* screen = member<J2DScreen*>(self, kOffScreen);
        if (!screen) return false;

        J2DPane* linki = screen->search(MULTI_CHAR('linki_n'));
        J2DPane* n0 = screen->search(MULTI_CHAR('fuku_n1'));
        if (!linki || !n0) return false;

        const ResTIMG* frameImg = picture_image(screen->search(MULTI_CHAR('ken_g_0'))); //MULTI_CHAR('fuku_g_2')
        const ResTIMG* iconImg = get_icon_image();
        if (!frameImg || !iconImg) return false;

        J2DPane* parent = find_parent(screen, linki);
        if (!parent) return false;

        const JGeometry::TBox2<f32>& lb = linki->getGlbBounds();
        const JGeometry::TBox2<f32>& pb = parent->getGlbBounds();
        const float w = n0->getGlbBounds().getWidth();
        const float h = n0->getGlbBounds().getHeight();
        if (w <= 0.0f || h <= 0.0f) return false;

        const float lx = lb.i.x + lb.getWidth() - w - kInsetX - pb.i.x + kNudgeX;
        const float ly = lb.i.y + kInsetTop - pb.i.y + kNudgeY;

        JGeometry::TBox2<f32> box;
        box.i.x = lx;
        box.i.y = ly;
        box.f.x = lx + w;
        box.f.y = ly + h;
        JGeometry::TBox2<f32> fbox = box;   // el marco del juego es 2 px más grande
        fbox.i.x -= 1.0f;
        fbox.i.y -= 1.0f;
        fbox.f.x += 1.0f;
        fbox.f.y += 1.0f;

        const ResTIMG* plateImg = kUsePlate ? picture_image(screen->search(MULTI_CHAR('gray_04'))) : nullptr;

        JKRHeap* heap = member<JKRHeap*>(self, 0x08);
        JKRHeap* old = heap ? mDoExt_setCurrentHeap(heap) : nullptr;

        s_plate = plateImg ? JKR_NEW J2DPicture(kIdPlate, box, plateImg, nullptr) : nullptr;   // [VERIFICAR] ctor
        s_icon = JKR_NEW J2DPicture(kIdIcon, box, iconImg, nullptr);
        s_frame = JKR_NEW J2DPicture(kIdFrame, fbox, frameImg, nullptr);

        //if (s_plate) {
        //    s_plate->setBlackWhite(JUtility::TColor(0, 0, 0, 255), JUtility::TColor(30, 30, 36, 255));
        //    parent->appendChild(s_plate);
        //}
        if (s_icon) {
            s_icon->setAlpha(255);
            parent->appendChild(s_icon);
        }
        if (s_frame) {
            s_frame->setAlpha(255);
            parent->appendChild(s_frame);   // marco encima del icono
        }
        s_lastTint = -1;

        // Objeto que el cursor nativo usa para saber dónde posarse.
        s_mgr = s_icon ? JKR_NEW CPaneMgr(screen, kIdIcon, 0, nullptr) : nullptr;   // [VERIFICAR] ctor

        if (old) mDoExt_setCurrentHeap(old);

        if (!s_icon || !s_frame || !s_mgr) return false;

        /*if (svc_log) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "Mask slot: built at x=%.0f y=%.0f (local)",
                          static_cast<double>(lx), static_cast<double>(ly));
            svc_log->info(mod_ctx, msg);
        }*/
        return true;
    }

    // Tinte del marco: gris / dorado / dorado claro (con cursor).
    void update_look() {
        if (!s_frame) return;
        int state = mask_is_equipped() ? 1 : 0;
        //if (s_focus) state = 2;
        if (state == s_lastTint) return;
        s_lastTint = state;
        const u8* t = (state == 2) ? kTintFocus : (state == 1) ? kTintEquip : kTintIdle;
        s_frame->setBlackWhite(JUtility::TColor(0, 0, 0, 0), JUtility::TColor(t[0], t[1], t[2], 255));
    }

    // ============================================================
    // Foco con el cursor nativo
    // ============================================================

    void focus_enter(dMenu_Collect2D_c* self) {
        s_savedCol = member<u8>(self, kOffCursorX);
        s_savedRow = member<u8>(self, kOffCursorY);
        void*& cell = grid_cell(self, s_savedCol, s_savedRow);
        s_savedCell = cell;
        cell = s_mgr;               // el cursor nativo se posará sobre nuestro icono
        s_focus = true;
        self->cursorPosSet();       // [VERIFICAR] accesible
        self->setItemNameStringNull();   // una sola vez: quita el id del mensaje (y con él el icono B)
        mDoAud_seStartMenu(Z2SE_SY_CURSOR_ITEM);
    }

    void focus_leave(dMenu_Collect2D_c* self) {
        if (!s_focus) return;
        grid_cell(self, s_savedCol, s_savedRow) = s_savedCell;
        s_focus = false;
        s_nameApplied = false;
        s_savedCell = nullptr;
        self->cursorPosSet();
        self->setItemNameString(static_cast<u8>(s_savedCol), static_cast<u8>(s_savedRow));
        // Devolver el texto del botón A que calcula el juego para la casilla de espada.
        self->setAButtonString(0);       // fuerza que el id cambie y se reescriba
        self->setAButtonString(0x436);   // "Equipar" del propio juego, ya traducido por él
        mDoAud_seStartMenu(Z2SE_SY_CURSOR_ITEM);
    }

    //Primera columna ocupada de una fila(la ropa empieza en la 3, no en la 0).
    int first_col(dMenu_Collect2D_c* self, int row) {
        for (int c = 0; c <= 6; ++c) {
            if (grid_cell(self, c, row)) return c;
        }
        return -1;
    }

    // ¿Es la celda (x, fila) la más a la izquierda de su fila, según su posición en pantalla?
    bool is_leftmost(dMenu_Collect2D_c* self, int x, int row) {
        auto pane_x = [&](int col, float& out) -> bool {
            void* mgr = grid_cell(self, col, row);
            if (!mgr) return false;
            J2DPane* p = *reinterpret_cast<J2DPane**>(static_cast<u8*>(mgr) + 8);   // CPaneMgr::mpPane
            if (!p || !p->isVisible()) return false;
            const float gx = p->getGlbBounds().i.x;
            if (gx < -100.0f) return false;   // celdas ocultas fuera de pantalla
            out = gx;
            return true;
        };
        float mine = 0.0f;
        if (!pane_x(x, mine)) return false;
        for (int c = 0; c <= 6; ++c) {
            if (c == x) continue;
            float other = 0.0f;
            if (pane_x(c, other) && other < mine - 1.0f) return false;
        }
        return true;
    }

    constexpr size_t kDescMaxChars = 34;   // se cortaba en ~40; baja si aún se corta, sube si queda estrecho

    // Copia 'in' a 'out' cambiando por '\n' el último espacio de cada línea que se pasaría de largo.
    void wrap_text(const char* in, char* out, size_t cap, size_t maxChars) {
        const size_t npos = static_cast<size_t>(-1);
        size_t o = 0, lineStart = 0, lastSpace = npos;
        for (size_t i = 0; in[i] && o + 1 < cap; ++i, ++o) {
            out[o] = in[i];
            if (in[i] == ' ') lastSpace = o;
            if (o - lineStart + 1 > maxChars && lastSpace != npos) {
                out[lastSpace] = '\n';
                lineStart = lastSpace + 1;
                lastSpace = npos;
            }
        }
        out[o] = 0;
    }

    // Escribe el nombre del objeto en los paneles de texto de la descripción.
    void apply_name(dMenu_Collect2D_c* self, const char* text, const char* desc) {
        J2DScreen* screen = member<J2DScreen*>(self, kOffScreen);
        if (!screen) return;
        const u64 ids[] = {
            MULTI_CHAR('item_n00'), MULTI_CHAR('item_n01'), MULTI_CHAR('item_n02'), MULTI_CHAR('item_n03'),
            MULTI_CHAR('item_n04'), MULTI_CHAR('item_n05'), MULTI_CHAR('item_n06'), MULTI_CHAR('item_n07'),
        };
        for (u64 id : ids) {
            J2DPane* p = screen->search(id);
            if (p && p->getTypeID() == 0x13) {   // [VERIFICAR] 0x13 = J2DTextBox
                static_cast<J2DTextBox*>(p)->setString(static_cast<s16>(64), text);
            }
        }
        const u64 descIds[] = {

    MULTI_CHAR('i_text0'), MULTI_CHAR('i_text1'), MULTI_CHAR('f_text0'), MULTI_CHAR('f_text1'),
        };
        char wrapped[512];
        wrap_text(desc, wrapped, sizeof(wrapped), kDescMaxChars);
        for (u64 id : descIds) {
            J2DPane* p = screen->search(id);
            if (p && p->getTypeID() == 0x13) {
                static_cast<J2DTextBox*>(p)->setString(static_cast<s16>(512), wrapped);
            }
        }
        /*
        for (u64 id : descIds) {
            J2DPane* p = screen->search(id);
            if (p && p->getTypeID() == 0x13) {   // [VERIFICAR] 0x13 = J2DTextBox
                static_cast<J2DTextBox*>(p)->setString(static_cast<s16>(512), desc);
            }
        }*/
    }

    // ============================================================
	// Idiomas y textos de la máscara
    // ============================================================
    // Orden: 0 EN, 1 JA, 2 ES, 3 FR, 4 DE, 5 IT
    struct MaskText { const char* name; const char* desc; };
    constexpr MaskText kTexts[6] = {
        { "Majora's Mask",     "You've met with a terrible fate, haven't you?" },
        { "Majora's Mask",     "You've met with a terrible fate, haven't you?" }, // TODO japonés (ver nota)
        { "Mascara de Majora", "Te has encontrado con un terrible destino, verdad?" },
        { "Masque de Majora",  "Tu as rencontre un terrible destin, n'est-ce pas ?" },
        { "Majoras Maske",     "Du hast ein schreckliches Schicksal erlitten, nicht wahr?" },
        { "Maschera di Majora","Sei andato incontro a un terribile destino, non e cosi?" },
    };

    const MaskText& mask_text() {
        switch (dComIfGs_getPalLanguage()) {
        case dSv_player_config_c::LANGUAGE_SPANISH: return kTexts[2];
        case dSv_player_config_c::LANGUAGE_FRENCH:  return kTexts[3];
        case dSv_player_config_c::LANGUAGE_GERMAN:  return kTexts[4];
        case dSv_player_config_c::LANGUAGE_ITALIAN: return kTexts[5];
        default:                                    return kTexts[0];   // inglés y cualquier otro
        }
    }

    // Orden: 0 EN, 1 JA (usa EN por ahora), 2 ES, 3 FR, 4 DE, 5 IT
    struct AText { const char* equip; const char* unequip; };
    constexpr AText kAText[6] = {
        { "Equip",      "Unequip" },
        { "Equip",      "Unequip" },
        { "Equipar",    "Desequipar" },
        { "Equiper",    "Desequiper" },
        { "Ausrusten",  "Ablegen" },
        { "Equipaggia", "Rimuovi" },
    };

    const AText& a_text() {
        switch (dComIfGs_getPalLanguage()) {
        case dSv_player_config_c::LANGUAGE_SPANISH: return kAText[2];
        case dSv_player_config_c::LANGUAGE_FRENCH:  return kAText[3];
        case dSv_player_config_c::LANGUAGE_GERMAN:  return kAText[4];
        case dSv_player_config_c::LANGUAGE_ITALIAN: return kAText[5];
        default:                                    return kAText[0];
        }
    }

    void apply_a_label(dMenu_Collect2D_c* self) {
        J2DScreen* icon = self->getIconScreen();
        if (!icon) return;
        const char* txt = mask_is_equipped() ? a_text().unequip : a_text().equip;
        const u64 ids[] = {
            MULTI_CHAR('atext1_1'), MULTI_CHAR('atext1_2'), MULTI_CHAR('atext1_3'),
            MULTI_CHAR('atext1_4'), MULTI_CHAR('atext1_5'),
            MULTI_CHAR('fatext_1'), MULTI_CHAR('fatext_2'), MULTI_CHAR('fatext_3'),
            MULTI_CHAR('fatext_4'), MULTI_CHAR('fatext_5'),
        };
        for (u64 id : ids) {
            J2DPane* p = icon->search(id);
            if (p && p->getTypeID() == 0x13) {
                static_cast<J2DTextBox*>(p)->setString(static_cast<s16>(32), txt);
            }
        }
    }

    // ============================================================
    // Hooks
    // ============================================================

    DEFINE_HOOK(&J2DScreen::draw, CollectScreenDrawHook);   // [VERIFICAR] sin sobrecargas
    DEFINE_HOOK(&dMenu_Collect2D_c::_create, CollectCreateHook);
    DEFINE_HOOK(&dMenu_Collect2D_c::_delete, CollectDeleteHook);
    DEFINE_HOOK(&dMenu_Collect2D_c::wait_proc, CollectWaitProcHook);

        void reset_state() {
            s_collect = nullptr;
            s_built = false;
            s_buildTries = 0;
            s_focus = false;
            s_nameApplied = false;
            s_savedCell = nullptr;
            s_mgr = nullptr;
            s_plate = nullptr;
            s_icon = nullptr;
            s_frame = nullptr;
            s_lastTint = -1;
            s_armed = false;
        }

        void on_collect_create_post(ModContext*, void* args, void*, void*) {
            reset_state();
            s_collect = args ? mods::arg<dMenu_Collect2D_c*>(args, 0) : nullptr;
            s_dumped = false;
        }

        HookAction on_collect_delete_pre(ModContext*, void*, void*, void*) {
            if (s_collect) focus_leave(s_collect);   // devolver la celda antes de que el juego la borre
            reset_state();
            return HOOK_CONTINUE;
        }

        void on_screen_draw_post(ModContext*, void* args, void*, void*) {
            if (!s_collect || !args) return;
            auto* screen = mods::arg<J2DScreen*>(args, 0);
            if (!screen || screen != member<J2DScreen*>(s_collect, kOffScreen)) return;

            if (kDumpPanes && !s_dumped) {
                s_dumped = true;
                dump_pane(screen, 0);
                if (J2DScreen* icon = s_collect->getIconScreen()) {
                    if (svc_log) svc_log->info(mod_ctx, "=== ICON SCREEN ===");
                    dump_pane(icon, 0);
                }
            }
            if (!s_built && s_buildTries < 5) {
                ++s_buildTries;
                s_built = build_slot(s_collect);
            }
            if (s_built) update_look();
        }

        // Justo antes de dibujar la colección: escribimos nuestro texto, que gana al del juego.
        HookAction on_screen_draw_pre(ModContext*, void* args, void*, void*) {
            if (!s_collect || !s_focus || !args) return HOOK_CONTINUE;
            auto* screen = mods::arg<J2DScreen*>(args, 0);
            if (screen && screen == s_collect->getIconScreen()) {
                apply_a_label(s_collect);
                return HOOK_CONTINUE;
            }
            if (!screen || screen != member<J2DScreen*>(s_collect, kOffScreen)) return HOOK_CONTINUE;
            //s_collect->setItemNameStringNull();   // [VERIFICAR] si es privada, ver abajo
            apply_name(s_collect, mask_text().name, mask_text().desc);
            return HOOK_CONTINUE;
        }

        // D-pad izquierda, o stick empujado a la izquierda (solo el primer instante).
        bool left_pressed() {
            static bool s_stickWasLeft = false;
            const bool stickLeft = mDoCPd_c::getStickX(0) < -0.5f;
            const bool stickEdge = stickLeft && !s_stickWasLeft;
            s_stickWasLeft = stickLeft;
            return dMw_LEFT_TRIGGER() || stickEdge;
        }

        // Stick empujado a derecha/arriba/abajo (solo el primer instante).
        bool stick_leave_edge() {
            static bool s_was = false;
            const float sx = mDoCPd_c::getStickX(0);
            const float sy = mDoCPd_c::getStickY(0);
            const bool now = sx > 0.5f || sy > 0.5f || sy < -0.5f;
            const bool edge = now && !s_was;
            s_was = now;
            return edge;
        }

        HookAction on_wait_proc_pre(ModContext*, void* args, void*, void*) {
            auto* self = args ? mods::arg<dMenu_Collect2D_c*>(args, 0) : nullptr;
            if (!self || self != s_collect || !s_built) return HOOK_CONTINUE;

            const int x = member<u8>(self, kOffCursorX);
            const int y = member<u8>(self, kOffCursorY);
            const bool stickLeave = stick_leave_edge();
            //if (!s_focus && dMw_A_TRIGGER() && svc_log) svc_log->info(mod_ctx, "Mask slot: A SIN foco");
            if (!s_focus) {
                // Dejamos que el juego intente mover el cursor; el post comprueba si se movió.
                const bool left = left_pressed();   // hay que llamarla siempre para seguir el stick
                s_armed = (y == kRowClothes) && left && is_leftmost(self, x, kRowClothes);
                s_armX = x;
                s_armY = y;
                return HOOK_CONTINUE;
            }

            // Con foco: si el juego movió el cursor (stick), soltamos el slot.
            if (x != s_savedCol || y != s_savedRow) {
                focus_leave(self);
                return HOOK_CONTINUE;
            }
            if (dMw_A_TRIGGER()) {
                //if (svc_log) svc_log->info(mod_ctx, "Mask slot: A con foco");
                const bool nowEquipped = !mask_is_equipped();
                mask_set_equipped(nowEquipped);
                mDoAud_seStartMenu(nowEquipped ? Z2SE_SY_ITEM_SET_X : Z2SE_SY_MENU_SUB_OUT); // Z2SE_SY_ITEM_SET_B
                return HOOK_SKIP_ORIGINAL;
            }
            if (dMw_RIGHT_TRIGGER() || dMw_UP_TRIGGER() || dMw_DOWN_TRIGGER() || stickLeave) {
                focus_leave(self);
                return HOOK_SKIP_ORIGINAL;
            }
            if (dMw_LEFT_TRIGGER()) return HOOK_SKIP_ORIGINAL;
            return HOOK_CONTINUE;
        }

        void on_wait_proc_post(ModContext*, void* args, void*, void*) {
            auto* self = args ? mods::arg<dMenu_Collect2D_c*>(args, 0) : nullptr;
            if (!self || self != s_collect || !s_built) return;
            if (s_armed && !s_focus) {
                s_armed = false;
                // Izquierda y el cursor no se movió: estamos en la primera celda -> al slot.
                if (member<u8>(self, kOffCursorX) == s_armX && member<u8>(self, kOffCursorY) == s_armY) {
                    focus_enter(self);
                }
            }
            //if (s_focus) {
            //    apply_name(self, "Mascara de Majora", "Una mascara antigua. Al equiparla, su poder te acompana.");
            //}
        }

}  // namespace

// ============================================================
// Ciclo de vida del mod
// ============================================================

ModResult mask_collect_init(ModError*) {
    ModResult r = mods::hook::add_post<CollectCreateHook>(on_collect_create_post);
    if (r != MOD_OK) return r;
    r = mods::hook::add_pre<CollectDeleteHook>(on_collect_delete_pre);
    if (r != MOD_OK) return r;
    r = mods::hook::add_pre<CollectWaitProcHook>(on_wait_proc_pre);
    if (r != MOD_OK) return r;
    r = mods::hook::add_post<CollectWaitProcHook>(on_wait_proc_post);
    if (r != MOD_OK) return r;
    r = mods::hook::add_pre<CollectScreenDrawHook>(on_screen_draw_pre);
    if (r != MOD_OK) return r;
    return mods::hook::add_post<CollectScreenDrawHook>(on_screen_draw_post);
}

void mask_collect_shutdown() {
    reset_state();
    release_icon();
}

}  // namespace dawnlight

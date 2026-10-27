#include "mask_bmd.hpp"
#include "mask_bmd_format.hpp"      // análogo a glider_bmd_format.hpp: valid_mask_bmd(), kMaskBmdMaxBytes
#include "service_imports.hpp"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphBase/J3DDrawBuffer.h"
#include "JSystem/J3DGraphLoader/J3DModelLoader.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "d/d_kankyo.h"
#include <dvd.h>
#include "mods/svc/resource.h"
#include <memory>
#include <new>
#include <chrono>
#include "m_Do/m_Do_ext.h"
#include "d/d_menu_collect.h"

namespace dawnlight {
    bool g_maskMenuPass = false;   // true mientras se dibuja dentro del menú de colección
namespace {
JKRExpHeap* s_heap = nullptr;
std::unique_ptr<u8[]> s_heapStorage;
J3DModel* s_model = nullptr;
J3DDrawBuffer* s_opaque = nullptr;
J3DDrawBuffer* s_translucent = nullptr;
bool s_attempted = false;
using ForgetModelMatrices = void (*)(J3DModel*);
ForgetModelMatrices s_forgetMatrices = nullptr;

struct CurrentHeap {
    JKRHeap* previous;
    explicit CurrentHeap(JKRHeap* heap) : previous(heap->becomeCurrentHeap()) {}
    ~CurrentHeap() { previous->becomeCurrentHeap(); }
};

void release_model() {
    if (s_model && s_forgetMatrices) s_forgetMatrices(s_model);
    s_model = nullptr;
    s_opaque = s_translucent = nullptr;
    if (s_heap) s_heap->destroy();
    s_heap = nullptr;
    s_heapStorage.reset();
}

bool create_model_heap(u32 size) {
    // OJO: si tu .bmd de la máscara pesa bastante menos que un planeador
    // (probable, es una pieza chica), podés bajar el heap extra de 8 MiB
    // a algo como 2-3 MiB para no reservar de más. No lo toco acá porque
    // no sé el peso real de tu .bmd todavía.
    if (size < 32 || size > kMaskBmdMaxBytes) return false;
    const u32 heapSize = 8 * 1024 * 1024 + size * 4;
    s_heapStorage.reset(new (std::nothrow) u8[heapSize + 31]);
    if (!s_heapStorage) return false;
    auto* memory = reinterpret_cast<void*>(
        (reinterpret_cast<uintptr_t>(s_heapStorage.get()) + 31) & ~uintptr_t{31});
    s_heap = JKRExpHeap::create(memory, heapSize, JKRHeap::getRootHeap(), false);
    if (!s_heap) s_heapStorage.reset();
    return s_heap != nullptr;
}

bool load_model(void* bytes, u32 size) {
    if (!valid_mask_bmd(bytes, size)) return false;
    auto* data = J3DModelLoaderDataBase::load(bytes, 0x59020010);
    if (!data || !data->getJointNum() || !data->getShapeNum() || !data->getMaterialNum())
        return false;
    auto* model = JKR_NEW J3DModel();
    if (!model || model->entryModelData(data, 0, 1) != kJ3DError_Success) return false;
    s_opaque = JKR_NEW J3DDrawBuffer();
    s_translucent = JKR_NEW J3DDrawBuffer();
    if (!s_opaque || !s_translucent ||
        s_opaque->allocBuffer(32) != kJ3DError_Success ||
        s_translucent->allocBuffer(32) != kJ3DError_Success) return false;
    s_translucent->setZSort();
    s_model = model;
    return true;
}

bool load_overlay_model(DVDFileInfo& file) {
    const u32 size = file.length;
    if (!create_model_heap(size)) return false;
    CurrentHeap scope(s_heap);
    const auto paddedSize = (size + 31) & ~31u;
    auto* bytes = s_heap->alloc(paddedSize, 32);
    return bytes && DVDReadPrio(&file, bytes, paddedSize, 0, 2) >= static_cast<s32>(size) &&
           load_model(bytes, size);
}

bool load_bundled_model() {
    ResourceBuffer buffer = RESOURCE_BUFFER_INIT;
    if (svc_resource->load(mod_ctx, "MajorasMask.bmd", &buffer) != MOD_OK) return false;  // <- tu .bmd
    bool loaded = false;
    if (valid_mask_bmd(buffer.data, buffer.size) &&
        create_model_heap(static_cast<u32>(buffer.size))) {
        CurrentHeap scope(s_heap);
        auto* bytes = s_heap->alloc(static_cast<u32>(buffer.size), 32);
        if (bytes) {
            std::memcpy(bytes, buffer.data, buffer.size);
            loaded = load_model(bytes, static_cast<u32>(buffer.size));
        }
    }
    svc_resource->free(mod_ctx, &buffer);
    return loaded;
}
}

void prepare_mask_bmd() {
    if (s_attempted) return;
    s_attempted = true;
    void* address = nullptr;
    if (svc_hook && svc_hook->resolve &&
        svc_hook->resolve(mod_ctx, "J3DModel::forgetMtx", &address, nullptr) == MOD_OK)
        s_forgetMatrices = reinterpret_cast<ForgetModelMatrices>(address);
    DVDFileInfo file{};
    if (DVDOpen(kMaskBmdPath, &file)) {   // ej. "/res/Object/MaskOfPower.arc" o similar
        const bool loaded = load_overlay_model(file);
        DVDClose(&file);
        if (loaded) {
            if (svc_log) svc_log->info(mod_ctx, "Mask BMD: loaded overlay");
            return;
        }
        release_model();
        if (svc_log) svc_log->warn(mod_ctx, "Mask BMD: overlay failed; trying bundled model");
    }
    if (load_bundled_model()) {
        if (svc_log) svc_log->info(mod_ctx, "Mask BMD: loaded bundled model");
    } else {
        release_model();
        if (svc_log) svc_log->warn(mod_ctx, "Mask BMD: bundled model failed");
    }
}

static int menu_test_mode() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    const auto s = duration_cast<seconds>(steady_clock::now() - t0).count();
    return static_cast<int>((s / 4) % 4);
}

bool draw_mask_bmd(Mtx transform, dKy_tevstr_c* lighting) {
    if (!s_model) return false;
    const int mode = g_maskMenuPass ? menu_test_mode() : 0;
    if (g_maskMenuPass) {
        static int last = -1;
        if (mode != last) {
            last = mode;
            if (svc_log) svc_log->info(mod_ctx,
                mode == 0 ? "Mask menu: MODO 0 normal" :
                mode == 1 ? "Mask menu: MODO 1 nada" :
                mode == 2 ? "Mask menu: MODO 2 solo mascara" :
                "Mask menu: MODO 3 solo cubo rojo");
        }
    }
    if (mode == 1) return false;
    static Mtx presentedView{};
    CurrentHeap scope(s_heap);
    J3DSys saved = j3dSys;
    s_opaque->frameInit();
    s_translucent->frameInit();
    j3dSys.setDrawBuffer(s_opaque, 0);
    j3dSys.setDrawBuffer(s_translucent, 1);
    MTXConcat(saved.mViewMtx, transform, j3dSys.getViewMtx());
    Mtx identity;
    MTXIdentity(identity);
    s_model->setBaseTRMtx(identity);   // el camino del menú la deja con otro valor
    s_translucent->setZMtx(s_model->getBaseTRMtx());
    j3dSys.reinitGX();
    if (g_maskMenuPass) {
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_FALSE);   // no pisar la transparencia que dejó Link
    }
    if (lighting) g_env_light.setLightTevColorType_MAJI(s_model, lighting);
    s_model->unlock();
    s_model->update();
    s_model->lock();
    s_model->viewCalc();
    MTXCopy(j3dSys.getViewMtx(), presentedView);
    for (u16 i = 0; i < s_model->getModelData()->getShapeNum(); ++i)
        s_model->getShapePacket(i)->setBaseMtxPtr(&presentedView);

    j3dSys.setDrawModeOpaTexEdge();
    GXSetCullMode(GX_CULL_FRONT);   // <-- test: ver si así se ve sólido
    J3DShape::resetVcdVatCache();
    if (mode != 3) s_opaque->draw();

    j3dSys.setDrawModeXlu();
    GXSetCullMode(GX_CULL_FRONT);   // <-- repetir, por si el draw anterior cambió el estado
    if (mode != 3) s_translucent->draw();
    
	//prueba: dibujar caja roja plana en el centro de la pantalla mientras se dibuja dentro del menú de colección
    if (g_maskMenuPass && (mode == 0 || mode == 3)) {   // PRUEBA: caja roja plana
        GXLoadPosMtxImm(j3dSys.getViewMtx(), GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GXSetNumChans(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXColor red = { 255, 0, 0, 255 };
        GXSetChanMatColor(GX_COLOR0A0, red);
        GXSetNumTexGens(0);
        GXSetNumTevStages(1);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GXSetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GXSetCullMode(GX_CULL_NONE);
        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        GXPosition3f32(-150.f, -150.f, 0.f);
        GXPosition3f32(150.f, -150.f, 0.f);
        GXPosition3f32(150.f, 150.f, 0.f);
        GXPosition3f32(-150.f, 150.f, 0.f);
        GXEnd();
    }

    J3DShape::resetVcdVatCache();
    j3dSys = saved;
    j3dSys.reinitGX();
    return true;
}

// Camino del menú: mismo patrón que dMenu_Collect3D_c::draw().
// Debe llamarse entre dComIfGd_setListItem3D() y dComIfGd_setList().
bool entry_mask_bmd_menu(Mtx transform, dKy_tevstr_c* lighting) {
    if (!s_model) return false;
    CurrentHeap scope(s_heap);

    // Si la máscara sale mal colocada, prueba kUseItemView = false.
    constexpr bool kUseItemView = true;
    Mtx savedView, itemView;
    if (kUseItemView) {
        MTXCopy(j3dSys.getViewMtx(), savedView);
        dMenu_Collect3D_c::setupItem3D(itemView);
        MTXCopy(itemView, j3dSys.getViewMtx());
    }

    s_model->setBaseTRMtx(transform);
    if (lighting) g_env_light.setLightTevColorType_MAJI(s_model, lighting);
    mDoExt_modelUpdateDL(s_model);

    if (kUseItemView) MTXCopy(savedView, j3dSys.getViewMtx());
    return true;
}

void shutdown_mask_bmd() {
    release_model();
    s_attempted = false;
}
}
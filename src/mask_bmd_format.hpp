#pragma once
#include <cstdint>
#include <cstring>

namespace dawnlight {

// Tope con margen generoso sobre tu .bmd real (1.208.320 bytes).
// Si en el futuro cambiás de modelo por uno más pesado, subí este número.
constexpr uint32_t kMaskBmdMaxBytes = 4 * 1024 * 1024; // 4 MiB

constexpr const char* kMaskBmdPath = "/res/MajorasMask.bmd"; // overlay en disco del jugador "/res/Object/MaskOfPower.bmd"

inline bool valid_mask_bmd(const void* data, uint32_t size) {
    if (data == nullptr || size < 32) return false;
    // J3D/BMD empieza siempre con la firma "J3D2" seguida del tipo de chunk
    // ("bmd3" para modelos binarios). Validar esto evita que un archivo
    // corrupto o de otro formato llegue al loader y haga crashear el juego.
    static const char kMagic[4] = {'J', '3', 'D', '2'};
    static const char kKind[4]  = {'b', 'm', 'd', '3'};
    return std::memcmp(data, kMagic, 4) == 0 &&
           std::memcmp(static_cast<const char*>(data) + 4, kKind, 4) == 0;
}

}
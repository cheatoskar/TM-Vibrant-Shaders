#include "effects_cinema.h"
#include "effects.h"
#include "settings.h"

namespace tmvs_mp {

bool CinemaStage::init(ID3D11Device* device) {
    (void)device;
    return true;
}

void CinemaStage::release() {
    releaseTargets();
}

void CinemaStage::releaseTargets() {}

bool CinemaStage::supports(const char* key) {
    (void)key;
    return false;
}

void CinemaStage::run(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s) {
    (void)ctx;
    (void)fx;
    (void)frame;
    (void)s;
}

} // namespace tmvs_mp

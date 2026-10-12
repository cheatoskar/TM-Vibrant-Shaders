#include "effects_sky.h"
#include "effects.h"
#include "settings.h"

namespace tmvs_mp {

bool SkyStage::init(ID3D11Device* device) {
    (void)device;
    return true;
}

void SkyStage::release() {
    releaseTargets();
}

void SkyStage::releaseTargets() {}

bool SkyStage::supports(const char* key) {
    (void)key;
    return false;
}

SkyStage::Output SkyStage::run(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s) {
    (void)ctx;
    (void)fx;
    (void)frame;
    (void)s;
    return {};
}

void SkyStage::post(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s) {
    (void)ctx;
    (void)fx;
    (void)frame;
    (void)s;
}

} // namespace tmvs_mp

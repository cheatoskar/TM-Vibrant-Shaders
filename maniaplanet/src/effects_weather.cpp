#include "effects_weather.h"
#include "effects.h"
#include "settings.h"

namespace tmvs_mp {

bool WeatherStage::init(ID3D11Device* device) {
    (void)device;
    return true;
}

void WeatherStage::release() {
    releaseTargets();
}

void WeatherStage::releaseTargets() {}

bool WeatherStage::supports(const char* key) {
    (void)key;
    return false;
}

ID3D11ShaderResourceView* WeatherStage::reflections(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame,
                                                    const tmshaders::Settings& s) {
    (void)ctx;
    (void)fx;
    (void)frame;
    (void)s;
    return nullptr;
}

void WeatherStage::draw(ID3D11DeviceContext* ctx, Effects& fx, const SceneFrame& frame, const tmshaders::Settings& s) {
    (void)ctx;
    (void)fx;
    (void)frame;
    (void)s;
}

void WeatherStage::tick(const tmshaders::Settings& s, double time) {
    (void)s;
    (void)time;
}

} // namespace tmvs_mp

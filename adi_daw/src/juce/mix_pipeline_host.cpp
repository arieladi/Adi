// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mix_pipeline_host.hpp"
#include "mix_function_host.hpp"
#include "adi/mix/limit_chain/Core.hpp"
#include "adi/mix_functions.hpp"
#include <utility>
namespace adi::device {
std::unique_ptr<DeviceInstance> makeFullMixFunction(const engine::DeviceRequest& request,std::string& error) {
    const auto* info=request.ref?mix::functionForReference(request.ref->format,request.ref->uid):nullptr;
    if(!info||info->kind!=mix::Function::Limiter){error="No linked full processing backend for this function";return {};}
    return makeMixFunction(request,error,mix::limit_chain::makeBackend());
}
engine::DeviceLoader withFullMixFunctions(engine::DeviceLoader fallback) {
    return [fallback=std::move(fallback)](const engine::DeviceRequest& request,std::string& error) {
        const auto* info=request.ref?mix::functionForReference(request.ref->format,request.ref->uid):nullptr;
        if(info&&info->kind==mix::Function::Limiter)return makeFullMixFunction(request,error);
        if(fallback)return fallback(request,error);
        error="No loader for this device";return std::unique_ptr<DeviceInstance>{};
    };
}
} // namespace adi::device

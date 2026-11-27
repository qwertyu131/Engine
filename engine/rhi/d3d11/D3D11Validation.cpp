// D3D11 validation hook (plan F2 Task 2, Global Constraints): ID3D11InfoQueue of the debug device.
// D3D11 has no message callback, so the queue is drained after every call that can produce messages
// (resource / pipeline creation, every draw, submit, readback). WARNING, ERROR and CORRUPTION messages
// are logged through eng::log and counted; submitAndWait() then returns exit 4.
#include "D3D11Common.h"

#include "core/Log.h"

#include <format>
#include <vector>

namespace eng::rhi::d3d11 {

namespace {
const char* severityName(D3D11_MESSAGE_SEVERITY s) {
    switch (s) {
    case D3D11_MESSAGE_SEVERITY_CORRUPTION: return "CORRUPTION";
    case D3D11_MESSAGE_SEVERITY_ERROR: return "ERROR";
    case D3D11_MESSAGE_SEVERITY_WARNING: return "WARNING";
    case D3D11_MESSAGE_SEVERITY_INFO: return "INFO";
    case D3D11_MESSAGE_SEVERITY_MESSAGE: return "MESSAGE";
    }
    return "?";
}
} // namespace

void D3D11Device::drainMessages() {
    if (!infoQueue_) {
        return;
    }
    const UINT64 n = infoQueue_->GetNumStoredMessages();
    std::vector<char> buf;
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(infoQueue_->GetMessage(i, nullptr, &len)) || len == 0) {
            continue;
        }
        buf.resize(len);
        auto* m = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
        if (FAILED(infoQueue_->GetMessage(i, m, &len))) {
            continue;
        }
        if (m->Severity > D3D11_MESSAGE_SEVERITY_WARNING) {
            continue; // INFO / MESSAGE
        }
        const std::string text = std::format("{} id={} {}", severityName(m->Severity), static_cast<int>(m->ID),
                                             std::string(m->pDescription, m->DescriptionByteLength > 0
                                                                              ? m->DescriptionByteLength - 1
                                                                              : 0));
        ++validationErrors_;
        if (firstMessage_.empty()) {
            firstMessage_ = text;
        }
        if (m->Severity == D3D11_MESSAGE_SEVERITY_WARNING) {
            ENG_LOG_WARN("d3d11 debug layer: {}", text);
        } else {
            ENG_LOG_ERROR("d3d11 debug layer: {}", text);
        }
    }
    infoQueue_->ClearStoredMessages();
}

Error D3D11Device::validationError() const {
    return Error{kExitValidation, std::format("d3d11: {} debug-layer message(s) of severity WARNING or above; first: {}",
                                              validationErrors_, firstMessage_)};
}

} // namespace eng::rhi::d3d11

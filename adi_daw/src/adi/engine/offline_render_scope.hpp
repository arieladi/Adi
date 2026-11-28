// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace adi::engine {

// An isolated render driver can wait for an out-of-process device between
// quanta. The graph still audits its work as callback code.
inline thread_local bool inOfflineRender = false;

class OfflineRenderScope {
public:
    OfflineRenderScope() noexcept : previous_(inOfflineRender) { inOfflineRender = true; }
    ~OfflineRenderScope() { inOfflineRender = previous_; }
    OfflineRenderScope(const OfflineRenderScope&) = delete;
    OfflineRenderScope& operator=(const OfflineRenderScope&) = delete;

private:
    bool previous_;
};

} // namespace adi::engine

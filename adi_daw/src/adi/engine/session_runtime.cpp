// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/session_runtime.hpp"

namespace adi::engine {
bool DeviceRuntimeToken::sameInstance(const DeviceRuntimeToken& other) const noexcept {
    if(!data_ || !other.data_ || data_->identity.expired() || other.data_->identity.expired())return false;
    return !data_->identity.owner_before(other.data_->identity) &&
           !other.data_->identity.owner_before(data_->identity);
}
std::weak_ptr<const void> DeviceRuntimeToken::claimIdentity() const noexcept {
    return data_?data_->identity:std::weak_ptr<const void>{};
}
} // namespace adi::engine

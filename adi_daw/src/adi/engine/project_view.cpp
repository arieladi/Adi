// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/project_view.hpp"
#include "adi/engine/gesture_preview.hpp"
#include "adi/gesture_capture_detail.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include <stdexcept>
namespace adi::engine {
ProjectView::ProjectView():endpoint_(std::make_shared<ProjectViewEndpoint>()) {endpoint_->owner_=this;}
ProjectView::~ProjectView() {endpoint_->owner_=nullptr;}
void ProjectView::adoptCommitted(std::shared_ptr<const Snapshot> value,const gesture::Base& base,
                                std::shared_ptr<const std::vector<std::uint8_t>> image) {
    const bool same=base_.source.sameConnection(base.source) && base_.head==base.head &&
        base_.sequence==base.sequence && image_ && image && *image_==*image;
    committed_=std::move(value);base_=base;image_=std::move(image);++committedGeneration_;
    if(!same || !preview_) {current_=committed_;preview_=false;++presentationGeneration_;}
    ++generation_;
}
void ProjectView::showPreview(std::shared_ptr<const Snapshot> value) {
    current_=std::move(value);preview_=true;++presentationGeneration_;++generation_;
}
void ProjectView::showCommitted() noexcept {
    current_=committed_;preview_=false;++presentationGeneration_;++generation_;
}
std::uint64_t ProjectView::refresh(const Store& store,const std::set<std::int64_t>& overridden) {
    const auto source=adi::appliedSource(store);
    // Inspection of a read-only project needs a snapshot, but cannot begin
    // gestures and therefore has no editable endpoint registration.
    if(!store.readOnly()) {
        const auto registered=gesture::detail::registerProjectViewEndpoint(source,endpoint_);
        if(!registered.ok())throw std::runtime_error(registered.message);
    }
    std::shared_ptr<const Snapshot> next=committed_ ? SnapshotBuilder::fromStore(store,*committed_,overridden)
                                                   : SnapshotBuilder::fromStore(store,overridden);
    adoptCommitted(std::move(next),{source,History(const_cast<Store&>(store)).head(),source.sequence()},
                   std::make_shared<const std::vector<std::uint8_t>>(capturePresentationImage(store)));
    return generation_;
}
} // namespace adi::engine

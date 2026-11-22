// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/render/after_export.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"

#include <utility>
#include <exception>

namespace adi::render {
namespace {
template <typename Delivery>
CommitResult importVerifiedDelivery(Store& store, const Delivery& delivery, clip_edit::ImportOptions destination,
                                     double referenceRate, Actor actor) {
    CommitResult refusal;
    try {
        if (delivery.state != JobState::Completed || !delivery.publication || !delivery.deliveryBlake3 ||
            delivery.output.empty()) {
            refusal.error = "After Export requires a completed, fingerprinted delivery";
            return refusal;
        }
        destination.source = std::filesystem::absolute(delivery.output);
        const auto expected = *delivery.deliveryBlake3;
        const auto verify = [source = destination.source, expected, stop = destination.stop](
                SQLite::Database&, std::int64_t, std::string& error) {
            const auto current = media::blake3File(source, stop);
            if (current && current.hex == expected) return true;
            error = stop.stop_requested() ? "After Export cancelled" :
                "The delivered audio changed before After Export could import it";
            return false;
        };
        if (std::string error; !verify(store.db(), 0, error)) {
            refusal.error = std::move(error);
            return refusal;
        }
        auto prepared = clip_edit::prepareImportAudio(store, destination, referenceRate);
        if (!prepared.edit.ok) {
            refusal.error = std::move(prepared.edit.error);
            return refusal;
        }
        // A copy-in may have captured a different image while the delivery
        // changed. The prepared media operation (including relink of an
        // already known asset) identifies the actual import before publication.
        for (const auto& request : prepared.edit.requests) {
            if (request.opType == "media.import" && request.payload.at("row").at("hash_blake3") != expected) {
                refusal.error = "The captured import differs from the delivered audio";
                return refusal;
            }
            if (request.opType == "media.relink" && request.payload.at("hash") != expected) {
                refusal.error = "The captured link differs from the delivered audio";
                return refusal;
            }
        }
        auto before = std::move(prepared.edit.options.beforeCommit);
        prepared.edit.options.beforeCommit = [before = std::move(before), verify](
                SQLite::Database& db, std::int64_t transaction, std::string& error) {
            return (!before || before(db, transaction, error)) && verify(db, transaction, error);
        };
        return clip_edit::commitMaterial(store, prepared, actor);
    } catch (const std::exception& error) {
        refusal.error = error.what();
        return refusal;
    }
}
} // namespace

CommitResult importAfterExport(Store& store, const Result& delivery, clip_edit::ImportOptions destination,
                               double referenceRate, Actor actor) {
    return importVerifiedDelivery(store, delivery, std::move(destination), referenceRate, actor);
}

CommitResult importAfterExport(Store& store, const DeliveryResult& delivery, clip_edit::ImportOptions destination,
                               double referenceRate, Actor actor) {
    return importVerifiedDelivery(store, delivery, std::move(destination), referenceRate, actor);
}
} // namespace adi::render

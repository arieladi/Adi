// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/stem_jobs.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"

#include <algorithm>
#include <exception>

namespace adi::control {
namespace {

std::optional<JobReadSet> stemReads(Store& store, const media::SeparationResult& source) {
    if (audio::onAudioThread || source.sourceMedia <= 0 || source.sourceBegin < 0 || source.frames <= 0)
        return std::nullopt;
    try {
        const auto row = media::mediaRow(store.db(), source.sourceMedia);
        if (row.at("hash_blake3") != source.sourceHash) return std::nullopt;
        const auto length = row.at("frames").get<std::int64_t>();
        if (source.sourceBegin > length || source.frames > length - source.sourceBegin) return std::nullopt;
        std::string error;
        const auto resolved = media::resolveMedia(media::projectFolder(store), row, error);
        if (resolved.empty()) return std::nullopt;
        const auto version = Payload::array({source.sourceHash, source.sourceBegin, source.frames,
                                            row.at("sample_rate"), row.at("channels"), length}).dump();
        return JobReadSet{{"project", store.clientId() + ":" + media::pathUtf8(store.path())},
                          {"media:" + std::to_string(source.sourceMedia), version}};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace

std::optional<std::stop_token> startStemJob(JobBook& jobs, std::string_view id, Store& store,
                                            const media::SeparationResult& source) {
    auto reads = stemReads(store, source);
    if (!reads) {
        (void)jobs.fail(id, "the stem source is unavailable");
        return std::nullopt;
    }
    return jobs.start(id, std::move(*reads));
}

std::optional<JobInfo> finishStemJob(JobBook& jobs, std::string_view id, Store& store,
                                     const media::SeparationResult& result,
                                     const media::SeparationImportOptions& options, Actor actor) {
    return jobs.complete(id, [&](const JobReadSet& expected) {
        const auto current = stemReads(store, result);
        return current && std::equal(current->begin(), current->end(), expected.begin(), expected.end(),
                                    [](const JobRead& a, const JobRead& b) {
            return a.key == b.key && a.version == b.version;
        });
    }, [&] {
        auto prepared = media::prepareSeparatedStems(store, result, options);
        return prepared.ready() &&
               media::commitSeparatedStems(store, prepared, options.documentGeneration, actor).ok;
    });
}

}  // namespace adi::control

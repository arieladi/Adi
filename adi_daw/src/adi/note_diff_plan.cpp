// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/note_stream_detail.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace adi::notes {
namespace {

std::int64_t rowInteger(const Payload& value) {
    std::string error;
    const auto result = detail::signedValue(value, error);
    if (!result) {
        throw std::runtime_error(error);
    }
    return *result;
}

void typedRow(const Payload& row) {
    if (!row.is_object() || row.empty()) {
        throw std::runtime_error("notes capture needs complete typed SQL rows");
    }
    for (const auto& cell : row) {
        if (cell.is_number_integer()) {
            (void)rowInteger(cell);
        } else if (cell.is_binary()) {
            if (cell.get_binary().size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                throw std::runtime_error("captured notes BLOB exceeds the SQLite binding limit");
            }
        } else if (cell.is_string()) {
            if (cell.get_ref<const std::string&>().size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                throw std::runtime_error("captured notes text exceeds the SQLite binding limit");
            }
        } else if (cell.is_number_float()) {
            if (std::isnan(cell.get<double>())) {
                throw std::runtime_error("captured notes REAL must not be NaN because SQLite stores it as NULL");
            }
        } else if (!cell.is_null()) {
            throw std::runtime_error("notes capture contains a non-SQL cell type");
        }
    }
}

std::span<const std::byte> rowBytes(const Payload& row) {
    const auto& data = row.at("data");
    if (!data.is_binary()) {
        throw std::runtime_error("captured stream data must retain its SQL BLOB storage type");
    }
    const auto& bytes = data.get_binary();
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

std::int64_t streamRowId(const Payload& row, std::int64_t clip) {
    typedRow(row);
    const auto id = rowInteger(row.at("id"));
    if (rowInteger(row.at("clip_id")) != clip || row.at("stream_kind") != "notes"
        || !row.at("channel").is_null() || !row.at("controller").is_null()) {
        throw std::runtime_error("notes capture must name the clip's canonical notes stream");
    }
    (void)rowBytes(row);
    return id;
}

const std::string& emptyText() noexcept {
    static const std::string empty;
    return empty;
}

} // namespace

std::int64_t Snapshot::clip() const noexcept { return ok && data_ ? data_->clip : 0; }
bool Snapshot::hasStream() const noexcept { return ok && data_ && data_->streamId.has_value(); }
std::optional<std::int64_t> Snapshot::streamId() const noexcept {
    return ok && data_ ? data_->streamId : std::nullopt;
}
std::optional<std::int64_t> Snapshot::creationId() const noexcept {
    return ok && data_ ? data_->creationId : std::nullopt;
}
std::span<const Record> Snapshot::records() const noexcept {
    return ok && data_ ? std::span<const Record>(data_->baseline.records) : std::span<const Record>{};
}
const std::string& Snapshot::from() const noexcept { return ok && data_ ? data_->from : emptyText(); }
const std::string& Snapshot::sourceFile() const noexcept {
    return ok && data_ ? data_->captured.sourceFile : emptyText();
}

namespace detail {

Snapshot SnapshotAccess::seal(SnapshotData data) {
    Snapshot result;
    result.data_ = std::make_shared<const SnapshotData>(std::move(data));
    result.ok = true;
    return result;
}

Snapshot makeSnapshot(CapturedNotes captured) {
    try {
        typedRow(captured.clipRow);
        SnapshotData data;
        data.clip = rowInteger(captured.clipRow.at("id"));
        if (captured.clipRow.at("kind") != "midi") {
            throw std::runtime_error("note editing requires a MIDI clip");
        }
        if (captured.streamRow && captured.creationRow) {
            throw std::runtime_error("an existing notes stream must not carry a creation row");
        }
        std::string error;
        if (captured.streamRow) {
            data.streamId = streamRowId(*captured.streamRow, data.clip);
            const auto bytes = rowBytes(*captured.streamRow);
            auto stream = decodeNotes(bytes, error);
            if (!stream) {
                throw std::runtime_error(error);
            }
            auto hash = hashNotes(bytes, error);
            if (!hash) {
                throw std::runtime_error(error);
            }
            data.baseline = std::move(*stream);
            data.from = std::move(*hash);
        } else {
            data.baseline = emptyNotes();
            data.from = "absent";
            if (captured.creationRow) {
                data.creationId = streamRowId(*captured.creationRow, data.clip);
                const auto baseline = encodeNotes(data.baseline, error);
                if (!baseline) {
                    throw std::runtime_error(error);
                }
                const auto bytes = rowBytes(*captured.creationRow);
                if (bytes.size() != baseline->size()
                    || !std::equal(bytes.begin(), bytes.end(), baseline->begin())) {
                    throw std::runtime_error("notes creation row needs the exact native empty ANOT baseline");
                }
            }
        }
        std::set<std::int64_t> rowIds;
        std::set<std::pair<std::int64_t, std::int64_t>> addresses;
        for (const auto& row : captured.expressionRows) {
            typedRow(row);
            const auto id = rowInteger(row.at("id"));
            if (rowInteger(row.at("clip_id")) != data.clip) {
                throw std::runtime_error("captured note expression belongs to another clip");
            }
            const auto note = rowInteger(row.at("note_id"));
            const auto dimension = rowInteger(row.at("dimension"));
            (void)rowBytes(row);
            if (!rowIds.insert(id).second || !addresses.emplace(note, dimension).second) {
                throw std::runtime_error("captured note expressions have duplicate row IDs or addresses");
            }
        }
        std::sort(captured.expressionRows.begin(), captured.expressionRows.end(),
                  [](const Payload& left, const Payload& right) {
                      return rowInteger(left.at("id")) < rowInteger(right.at("id"));
                  });
        data.captured = std::move(captured);
        return SnapshotAccess::seal(std::move(data));
    } catch (const std::exception& exception) {
        Snapshot result;
        result.error = exception.what();
        return result;
    }
}

} // namespace detail

PreparedDiff prepareDiff(const Snapshot& snapshot, const Diff& diff) {
    PreparedDiff result;
    try {
        const auto* captured = detail::SnapshotAccess::get(snapshot);
        if (!snapshot.ok || captured == nullptr) {
            throw std::runtime_error(snapshot.error.empty() ? "invalid notes snapshot" : snapshot.error);
        }
        const auto plan = detail::planRecords(captured->baseline, diff);
        if (!plan.ok) {
            throw std::runtime_error(plan.error);
        }
        result.sourceFile = captured->captured.sourceFile;
        result.options.expectHead.emplace(captured->captured.head);
        if (plan.changed) {
            if (!captured->streamId && !captured->creationId) {
                throw std::runtime_error("creating notes requires a caller-owned captured stream ID");
            }
            OpRequest request;
            std::string error;
            if (!detail::makeForwardRequest(*captured, plan, request, error)) {
                throw std::runtime_error(error);
            }
            result.requests.push_back(std::move(request));
            result.effects = plan.effects;
        }
        result.ok = true;
    } catch (const std::exception& exception) {
        result.ok = false;
        result.error = exception.what();
        result.requests.clear();
        result.effects = {};
    }
    return result;
}

} // namespace adi::notes

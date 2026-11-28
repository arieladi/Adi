// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/media/media_ops.hpp"
#include "adi/settings/store.hpp"
#include "adi/ui/export_choices.hpp"
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace adi::ui {

// Each dialog starts with the current range. The other choices come from the last
// dialog, with Settings supplying their initial values and later overrides.
class ExportPreferences {
  public:
    ExportChoices open(ExportChoices initial, const settings::AppSettings &settings) {
        const auto configured = fromSettings(settings);
        if (last_) {
            auto restored = *last_;
            restored.startTicks = initial.startTicks;
            restored.lengthTicks = initial.lengthTicks;
            initial = std::move(restored);
        }
        if (!configured_ || configured.fileType != configured_->fileType ||
            configured.encodeMp3 != configured_->encodeMp3) {
            initial.fileType = configured.fileType;
            initial.encodePcm = configured.encodePcm;
            initial.encodeMp3 = configured.encodeMp3;
        }
        if (!configured_ || configured.bitDepth != configured_->bitDepth)
            initial.bitDepth = configured.bitDepth;
        if (!configured_ || configured.dither != configured_->dither)
            initial.dither = configured.dither;
        if (!configured_ || configured.normalize != configured_->normalize ||
            configured.normalization.mode != configured_->normalization.mode)
            initial.normalize = configured.normalize;
        initial.normalization = configured.normalization;
        configured_ = configured;
        return initial;
    }

    void remember(const ExportChoices &choices) { last_ = choices; }

    void forgetTrack() {
        if (last_) {
            last_->track = 0;
            last_->tracks = ExportTracks::One;
            last_->stemTracks.clear();
        }
    }

    std::filesystem::path folder(const settings::AppSettings &settings,
                                 const std::filesystem::path &projectFolder) const {
        const auto configured = settings.get("files.renderFolder").get<std::string>();
        std::error_code ignored;
        if (lastFolderSetting_ == configured && std::filesystem::is_directory(lastFolder_, ignored))
            return lastFolder_;
        const auto preferred = media::pathFromUtf8(configured);
        return std::filesystem::is_directory(preferred, ignored) ? preferred : projectFolder;
    }

    void rememberFolder(const settings::AppSettings &settings, std::filesystem::path folder) {
        lastFolderSetting_ = settings.get("files.renderFolder").get<std::string>();
        lastFolder_ = std::move(folder);
    }

  private:
    static ExportChoices fromSettings(const settings::AppSettings &settings) {
        ExportChoices c;
        const auto fileType = settings.get("export.fileType");
        if (fileType == "aiff")
            c.fileType = "AIFF";
        else if (fileType == "flac")
            c.fileType = "FLAC";
        else if (fileType == "aac")
            c.fileType = "AAC";
        else if (fileType == "mp3") {
            c.encodePcm = false;
            c.encodeMp3 = true;
        }
        c.bitDepth = settings.get("export.bitDepth").get<int>();
        const auto dither = settings.get("export.dither");
        constexpr std::array<const char *, 6> ditherIds{"none", "rectangular", "triangular", "pow-r1", "pow-r2", "pow-r3"};
        for (std::size_t i = 0; i < ditherIds.size(); ++i)
            if (dither == ditherIds[i])
                c.dither = kExportDithers[i];
        const auto normalize = settings.get("export.normalize");
        c.normalize = normalize != "off";
        if (normalize == "loudness") {
            c.normalization.mode = render::NormalizeMode::Loudness;
            c.normalization.targetLufs = settings.get("export.loudnessTargetLufs").get<double>();
            c.normalization.truePeakCeilingDb = settings.get("export.truePeakCeilingDb").get<double>();
        }
        return c;
    }

    std::optional<ExportChoices> last_;
    std::optional<ExportChoices> configured_;
    std::filesystem::path lastFolder_;
    std::string lastFolderSetting_;
};

} // namespace adi::ui

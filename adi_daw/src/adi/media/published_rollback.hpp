// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "publish_file.hpp"
#include "media_ops.hpp"
#include <vector>
namespace adi::media::detail {
// Private bookkeeping for the two SQL-backed media publication callers.
// Allocate each path record BEFORE publication; filling identity cannot throw.
struct PublishedRollback {
    struct File {std::filesystem::path path;std::optional<FileIdentity> identity;};
    PublishedRollback()=default;
    PublishedRollback(const PublishedRollback&)=delete;
    PublishedRollback& operator=(const PublishedRollback&)=delete;
    std::vector<File> files;
    bool committed=false;
    ~PublishedRollback() {
        if(!committed)for(const auto& file:files)
            if(file.identity)(void)removePublishedFile(file.path,*file.identity);
    }
    std::string rollback() {
        if(committed)return {};
        std::string warning;
        for(const auto& file:files) {
            const auto error=file.identity?removePublishedFile(file.path,*file.identity):
                std::make_error_code(std::errc::state_not_recoverable);
            if(error) {
                if(!warning.empty())warning+="; ";
                warning+="rollback retained or changed public media: "+pathUtf8(file.path)+" ("+error.message()+")";
            }
        }
        committed=true;return warning;
    }
};
}

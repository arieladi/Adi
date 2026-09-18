// SPDX-License-Identifier: GPL-3.0-or-later
//
// blob.hpp is header-only by design (templates + static_asserts). This TU
// exists so the layout assertions are compiled exactly once as part of the
// library, rather than only when some consumer happens to include the header —
// a target that links adi_core has had the SPEC 6.3 layout verified.

#include "adi/blob.hpp"

namespace adi {

const char* toString(StreamError e) {
    switch (e) {
        case StreamError::Ok:          return "ok";
        case StreamError::TooShort:    return "blob shorter than its 16-byte header";
        case StreamError::BadFourCC:   return "fourcc does not match the expected stream kind";
        case StreamError::ZeroRecSize: return "header declares rec_size 0";
        case StreamError::Truncated:   return "blob shorter than header count * rec_size";
        case StreamError::RecSizeUnknown:
            return "rec_size is narrower than ours and matches no released version, "
                   "so it would land mid-field";
        case StreamError::TooLarge:
            return "count * rec_size does not fit this platform's address space";
    }
    return "unknown";
}

}  // namespace adi

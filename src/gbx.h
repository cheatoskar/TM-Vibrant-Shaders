#pragma once
#include <string>

namespace tmshaders {
namespace gbx {

// The comments of a map (.Challenge.Gbx), as the author wrote them in the editor. They sit
// in the file's header next to the thumbnail, so only the first part of the file is read.
// False when the file can't be read or has no comments.
bool readMapComments(const std::wstring& path, std::string& comments);

} // namespace gbx
} // namespace tmshaders

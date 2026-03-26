#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace artifact_system::encoding {

std::string ArtifactPath(uint64_t artifact_id);

std::string IndexPath(uint64_t index_definition_id, std::span<const uint8_t> encoded_key);

} // namespace artifact_system::encoding

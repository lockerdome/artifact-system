#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "google/protobuf/message.h"

namespace artifact_system::encoding {

std::vector<uint8_t> EncodeVarint(uint64_t value);

absl::StatusOr<uint64_t> DecodeVarint(std::span<const uint8_t> bytes, size_t* bytes_read);

// Encodes one concrete key tuple from a protobuf message.
//
// Contract:
// - `descriptor` identifies the expected message schema.
// - `descriptor` must be exactly `*message.GetDescriptor()`.
// - Each key field path must resolve to a non-repeated scalar/enum value.
//
// This function intentionally does not expand repeated fields or skip unset
// optional fields. That behavior belongs to index-derivation orchestration,
// which decides whether zero/one/many key tuples should be emitted.
absl::StatusOr<std::vector<uint8_t>> EncodeKey(const google::protobuf::Descriptor& descriptor, const google::protobuf::Message& message,
                                               std::span<const std::string> key_fields);

// Convenience: encode a single string field as an index key.
absl::StatusOr<std::vector<uint8_t>> EncodeSingleStringKey(const google::protobuf::Descriptor& descriptor, const std::string& field_name,
                                                           const std::string& value);

// Convenience: encode a single uint64 field as an index key.
absl::StatusOr<std::vector<uint8_t>> EncodeSingleUint64Key(const google::protobuf::Descriptor& descriptor, const std::string& field_name, uint64_t value);

} // namespace artifact_system::encoding

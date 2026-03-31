#pragma once

#include <string>

#include "absl/status/status.h"
#include "absl/strings/cord.h"
#include "absl/strings/str_cat.h"
#include "grpcpp/support/status.h"

#include "artifact_service.pb.h"

namespace artifact_system::service {

// Convert absl::StatusCode to grpc::StatusCode.
inline grpc::StatusCode AbslToGrpcCode(absl::StatusCode code) {
  switch (code) {
  case absl::StatusCode::kOk:
    return grpc::OK;
  case absl::StatusCode::kCancelled:
    return grpc::CANCELLED;
  case absl::StatusCode::kInvalidArgument:
    return grpc::INVALID_ARGUMENT;
  case absl::StatusCode::kDeadlineExceeded:
    return grpc::DEADLINE_EXCEEDED;
  case absl::StatusCode::kNotFound:
    return grpc::NOT_FOUND;
  case absl::StatusCode::kAlreadyExists:
    return grpc::ALREADY_EXISTS;
  case absl::StatusCode::kPermissionDenied:
    return grpc::PERMISSION_DENIED;
  case absl::StatusCode::kResourceExhausted:
    return grpc::RESOURCE_EXHAUSTED;
  case absl::StatusCode::kFailedPrecondition:
    return grpc::FAILED_PRECONDITION;
  case absl::StatusCode::kAborted:
    return grpc::ABORTED;
  case absl::StatusCode::kOutOfRange:
    return grpc::OUT_OF_RANGE;
  case absl::StatusCode::kUnimplemented:
    return grpc::UNIMPLEMENTED;
  case absl::StatusCode::kInternal:
    return grpc::INTERNAL;
  case absl::StatusCode::kUnavailable:
    return grpc::UNAVAILABLE;
  case absl::StatusCode::kDataLoss:
    return grpc::DATA_LOSS;
  case absl::StatusCode::kUnauthenticated:
    return grpc::UNAUTHENTICATED;
  default:
    return grpc::UNKNOWN;
  }
}

namespace detail {

inline void AppendVarint(uint32_t value, std::string* out) {
  while (value >= 0x80) {
    out->push_back(static_cast<char>(value | 0x80));
    value >>= 7;
  }
  out->push_back(static_cast<char>(value));
}

inline void AppendLengthDelimited(uint8_t tag, absl::string_view data, std::string* out) {
  out->push_back(static_cast<char>(tag));
  AppendVarint(static_cast<uint32_t>(data.size()), out);
  out->append(data.data(), data.size());
}

} // namespace detail

// Convert an absl::Status to a grpc::Status, preserving any proto detail
// payload as a google.rpc.Status binary error detail.
//
// The wire encoding is manual to avoid a build dependency on
// google/rpc/status.proto. Format:
//   google.rpc.Status { int32 code=1; string message=2; repeated Any details=3; }
//   google.protobuf.Any { string type_url=1; bytes value=2; }
inline grpc::Status AbslToGrpcStatus(const absl::Status& status) {
  if (status.ok()) {
    return grpc::Status::OK;
  }

  std::string error_details;
  status.ForEachPayload([&](absl::string_view type_url, const absl::Cord& payload) {
    if (!error_details.empty()) {
      return;
    }

    std::string payload_str = std::string(payload);

    std::string any_msg;
    detail::AppendLengthDelimited(0x0a, type_url, &any_msg);
    detail::AppendLengthDelimited(0x12, payload_str, &any_msg);

    std::string rpc_status;
    rpc_status.push_back(0x08);
    detail::AppendVarint(static_cast<uint32_t>(status.code()), &rpc_status);
    detail::AppendLengthDelimited(0x12, status.message(), &rpc_status);
    detail::AppendLengthDelimited(0x1a, any_msg, &rpc_status);

    error_details = std::move(rpc_status);
  });

  return {AbslToGrpcCode(status.code()), std::string(status.message()), error_details};
}

// Build an absl::Status with a serialized proto detail payload.
template <typename ProtoMessage> absl::Status MakeStatusWithDetail(absl::StatusCode code, absl::string_view message, const ProtoMessage& detail) {
  absl::Status status(code, message);
  std::string serialized;
  detail.SerializeToString(&serialized);
  status.SetPayload(absl::StrCat("type.googleapis.com/", ProtoMessage::descriptor()->full_name()), absl::Cord(std::move(serialized)));
  return status;
}

// ── Shared error-detail builders ────────────────────────────────────────────
// Used by multiple service implementations to build typed error statuses.

inline absl::Status MakeSnapshotTxnError(absl::string_view message, SnapshotTransactionError::Category category, absl::string_view id) {
  SnapshotTransactionError error;
  error.set_category(category);
  error.set_description(std::string(message));
  error.set_id(std::string(id));
  return MakeStatusWithDetail(absl::StatusCode::kNotFound, message, error);
}

inline absl::Status MakeFetchIndexError(absl::StatusCode code, absl::string_view message, FetchIndexError::Category category, absl::string_view key_type) {
  FetchIndexError error;
  error.set_category(category);
  error.set_description(std::string(message));
  error.set_key_type(std::string(key_type));
  return MakeStatusWithDetail(code, message, error);
}

} // namespace artifact_system::service

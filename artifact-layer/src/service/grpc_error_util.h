#pragma once

#include <string>

#include "absl/status/status.h"
#include "absl/strings/cord.h"
#include "absl/strings/str_cat.h"
#include "grpcpp/support/status.h"

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

// Convert an absl::Status to a grpc::Status, preserving any proto detail
// payloads via the standard google.rpc.Status encoding.
//
// If the absl::Status carries a payload with a type URL matching a known
// proto detail type, the payload is encoded in a google.rpc.Status message
// serialized into the gRPC trailing metadata (the "binary error details"
// convention). grpc::Status supports this via its error_details parameter.
inline grpc::Status AbslToGrpcStatus(const absl::Status& status) {
  if (status.ok()) {
    return grpc::Status::OK;
  }

  // Check for proto detail payload. The convention in this codebase is
  // SetPayload("type.googleapis.com/<fully-qualified-message>", serialized).
  // We extract the first payload (there's at most one per error) and encode
  // it as a google.rpc.Status in binary error details.
  std::string error_details;
  status.ForEachPayload([&](absl::string_view type_url, const absl::Cord& payload) {
    if (!error_details.empty())
      return; // take only the first

    // Build a google.rpc.Status message manually to avoid depending on
    // google/rpc/status.proto. The wire format is:
    //   field 1 (int32): code
    //   field 2 (string): message
    //   field 3 (repeated google.protobuf.Any): details
    // google.protobuf.Any is:
    //   field 1 (string): type_url
    //   field 2 (bytes): value

    std::string payload_str = std::string(payload);

    // Encode the Any message.
    std::string any_msg;
    // field 1: type_url (tag = 0x0a, wire type 2)
    any_msg.push_back(0x0a);
    std::string type_url_str(type_url);
    // varint length
    {
      uint32_t len = static_cast<uint32_t>(type_url_str.size());
      while (len >= 0x80) {
        any_msg.push_back(static_cast<char>(len | 0x80));
        len >>= 7;
      }
      any_msg.push_back(static_cast<char>(len));
    }
    any_msg.append(type_url_str);
    // field 2: value (tag = 0x12, wire type 2)
    any_msg.push_back(0x12);
    {
      uint32_t len = static_cast<uint32_t>(payload_str.size());
      while (len >= 0x80) {
        any_msg.push_back(static_cast<char>(len | 0x80));
        len >>= 7;
      }
      any_msg.push_back(static_cast<char>(len));
    }
    any_msg.append(payload_str);

    // Encode the google.rpc.Status message.
    std::string rpc_status;
    // field 1: code (tag = 0x08, varint)
    rpc_status.push_back(0x08);
    {
      uint32_t code = static_cast<uint32_t>(status.code());
      while (code >= 0x80) {
        rpc_status.push_back(static_cast<char>(code | 0x80));
        code >>= 7;
      }
      rpc_status.push_back(static_cast<char>(code));
    }
    // field 2: message (tag = 0x12)
    std::string msg(status.message());
    rpc_status.push_back(0x12);
    {
      uint32_t len = static_cast<uint32_t>(msg.size());
      while (len >= 0x80) {
        rpc_status.push_back(static_cast<char>(len | 0x80));
        len >>= 7;
      }
      rpc_status.push_back(static_cast<char>(len));
    }
    rpc_status.append(msg);
    // field 3: details (tag = 0x1a)
    rpc_status.push_back(0x1a);
    {
      uint32_t len = static_cast<uint32_t>(any_msg.size());
      while (len >= 0x80) {
        rpc_status.push_back(static_cast<char>(len | 0x80));
        len >>= 7;
      }
      rpc_status.push_back(static_cast<char>(len));
    }
    rpc_status.append(any_msg);

    error_details = std::move(rpc_status);
  });

  return {AbslToGrpcCode(status.code()), std::string(status.message()), error_details};
}

// Build an absl::Status with a serialized proto detail payload.
template <typename ProtoMessage> absl::Status MakeStatusWithDetail(absl::StatusCode code, const std::string& message, const ProtoMessage& detail) {
  absl::Status status(code, message);
  std::string serialized;
  detail.SerializeToString(&serialized);
  status.SetPayload(absl::StrCat("type.googleapis.com/", ProtoMessage::descriptor()->full_name()), absl::Cord(serialized));
  return status;
}

} // namespace artifact_system::service

#pragma once

#include <mutex>
#include <string>
#include <variant>

#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/descriptor_database.h"

namespace artifact_system::registry {

struct CompilationResult {
  // Contains ONLY the user's file descriptor. System protos
  // (artifact_options.proto, google well-known types) are excluded — consumers
  // must merge with generated_pool() or equivalent to resolve custom extensions.
  google::protobuf::FileDescriptorSet descriptor_set;
};

struct CompilationError {
  std::string description;
};

// Runtime .proto compiler that uses protobuf's Parser API to compile
// user-submitted .proto source into FileDescriptorSets. Thread-safe: all
// compilations are serialized behind a mutex with a fresh DescriptorPool
// per invocation.
class ProtoCompiler {
public:
  ProtoCompiler();

  // Compile a .proto source string. type_name is the fully-qualified message
  // name that must resolve in the compiled descriptor set.
  // Returns CompilationResult on success or CompilationError on failure.
  std::variant<CompilationResult, CompilationError> Compile(const std::string& proto_source, const std::string& type_name);

private:
  std::mutex mu_;
  // Cached system proto descriptors (artifact_options.proto + transitive deps),
  // populated in the constructor on the main thread.
  google::protobuf::SimpleDescriptorDatabase system_db_;
};

} // namespace artifact_system::registry

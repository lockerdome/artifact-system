#include "registry/proto_compiler.h"

#include <string>
#include <unordered_set>
#include <variant>

#include "google/protobuf/compiler/parser.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/descriptor_database.h"
#include "google/protobuf/io/tokenizer.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"

#include "artifact_options.pb.h"

namespace artifact_system::registry {
namespace {

constexpr size_t kMaxInputSize = 1 * 1024 * 1024; // 1 MB
constexpr int kMaxNestingDepth = 64;

class StringErrorCollector : public google::protobuf::io::ErrorCollector {
public:
  void RecordError(int line, int column, absl::string_view message) override {
    if (!errors_.empty())
      errors_ += "; ";
    errors_ += "input.proto:" + std::to_string(line + 1) + ":" + std::to_string(column + 1) + ": " + std::string(message);
  }

  void RecordWarning(int /*line*/, int /*column*/, absl::string_view /*message*/) override {}

  const std::string& errors() const { return errors_; }
  bool has_errors() const { return !errors_.empty(); }

private:
  std::string errors_;
};

int ComputeNestingDepth(const google::protobuf::Descriptor* descriptor) {
  int max_child_depth = 0;
  for (int i = 0; i < descriptor->nested_type_count(); ++i) {
    int child = ComputeNestingDepth(descriptor->nested_type(i));
    if (child > max_child_depth)
      max_child_depth = child;
  }
  return 1 + max_child_depth;
}

std::string CheckNestingDepth(const google::protobuf::FileDescriptor* file_desc) {
  for (int i = 0; i < file_desc->message_type_count(); ++i) {
    int depth = ComputeNestingDepth(file_desc->message_type(i));
    if (depth > kMaxNestingDepth) {
      return "nesting depth exceeds limit of " + std::to_string(kMaxNestingDepth) + " in message '" +
             std::string(file_desc->message_type(i)->full_name()) + "'";
    }
  }
  return "";
}

// Recursively add a FileDescriptor and all its dependencies to a database.
void AddFileAndDeps(const google::protobuf::FileDescriptor* fd, google::protobuf::SimpleDescriptorDatabase& db,
                    std::unordered_set<std::string>& added) {
  std::string name(fd->name());
  if (added.count(name))
    return;
  added.insert(name);
  for (int i = 0; i < fd->dependency_count(); ++i) {
    AddFileAndDeps(fd->dependency(i), db, added);
  }
  google::protobuf::FileDescriptorProto proto;
  fd->CopyTo(&proto);
  db.Add(proto);
}

} // namespace

ProtoCompiler::ProtoCompiler() {
  // Pre-cache system proto descriptors (artifact_options.proto + transitive
  // deps like descriptor.proto) on the main thread during construction.
  // This avoids thread-safety concerns with generated_pool() lazy init.
  const auto* ao_desc = artifact_system::IndexDefinition::descriptor();
  std::unordered_set<std::string> added;
  AddFileAndDeps(ao_desc->file(), system_db_, added);
}

std::variant<CompilationResult, CompilationError> ProtoCompiler::Compile(const std::string& proto_source, const std::string& type_name) {
  if (proto_source.size() > kMaxInputSize) {
    return CompilationError{"proto compilation failed: input file exceeds maximum size of 1 MB"};
  }

  if (type_name.empty()) {
    return CompilationError{"proto compilation failed: type_name must not be empty"};
  }

  std::lock_guard<std::mutex> lock(mu_);

  // Compilation runs synchronously under the mutex. The protobuf Parser is
  // bounded by input size (1 MB max) and its own recursion limit (32 levels),
  // so parsing completes in bounded time without needing an async timeout.

  // Parse the .proto source text into a FileDescriptorProto.
  google::protobuf::io::ArrayInputStream input(proto_source.data(), static_cast<int>(proto_source.size()));
  StringErrorCollector error_collector;
  google::protobuf::io::Tokenizer tokenizer(&input, &error_collector);

  google::protobuf::compiler::Parser parser;
  google::protobuf::FileDescriptorProto file_proto;
  file_proto.set_name("input.proto");

  if (!parser.Parse(&tokenizer, &file_proto)) {
    std::string msg = "proto compilation failed";
    if (error_collector.has_errors())
      msg += ": " + error_collector.errors();
    return CompilationError{msg};
  }

  if (error_collector.has_errors()) {
    return CompilationError{"proto compilation failed: " + error_collector.errors()};
  }

  // Enforce proto3 syntax.
  if (file_proto.syntax() != "proto3") {
    return CompilationError{"proto compilation failed: only proto3 syntax is accepted"};
  }

  // Build a database with system protos (cached) plus the user's file.
  google::protobuf::SimpleDescriptorDatabase user_db;
  user_db.Add(file_proto);
  google::protobuf::MergedDescriptorDatabase merged_db(&system_db_, &user_db);
  google::protobuf::DescriptorPool pool(&merged_db);

  const google::protobuf::FileDescriptor* file_desc = pool.FindFileByName("input.proto");
  if (file_desc == nullptr) {
    return CompilationError{"proto compilation failed: unable to resolve imports or build descriptor"};
  }

  // Nesting depth check.
  std::string depth_err = CheckNestingDepth(file_desc);
  if (!depth_err.empty()) {
    return CompilationError{"proto compilation failed: " + depth_err};
  }

  // Verify type_name resolves to a message in the user's file.
  const google::protobuf::Descriptor* msg_desc = pool.FindMessageTypeByName(type_name);
  if (msg_desc == nullptr) {
    return CompilationError{"proto compilation failed: type_name '" + type_name + "' does not resolve to a message in the compiled proto"};
  }
  if (msg_desc->file() != file_desc) {
    return CompilationError{"proto compilation failed: type_name '" + type_name +
                            "' resolves to a message in an imported file, not the submitted proto"};
  }

  // Build the FileDescriptorSet containing ONLY the user's file descriptor.
  CompilationResult result;
  file_desc->CopyTo(result.descriptor_set.add_file());
  return result;
}

} // namespace artifact_system::registry

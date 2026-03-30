#include "registry/proto_compiler.h"

#include <thread>
#include <vector>

#include "google/protobuf/descriptor.pb.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

constexpr const char* kSimpleProto = R"(
syntax = "proto3";
message SimpleMessage {
  string name = 1;
  int32 value = 2;
}
)";

constexpr const char* kProtoWithPackage = R"(
syntax = "proto3";
package myapp.models;
message Document {
  string title = 1;
  bytes content = 2;
}
)";

constexpr const char* kProtoWithArtifactOptions = R"(
syntax = "proto3";
import "artifact_options.proto";
message Task {
  option (artifact_system.indexes) = {
    key_type: "by_owner"
    key: ["owner_id"]
    order: { field: "artifact_id" direction: ASCENDING }
  };

  string owner_id = 1 [(artifact_system.references) = {
    target_type_name: "User"
    on_delete: RESTRICT
  }];
  string description = 2;
}
)";

constexpr const char* kProto2Source = R"(
syntax = "proto2";
message LegacyMessage {
  required string name = 1;
}
)";

constexpr const char* kMalformedProto = R"(
syntax = "proto3";
messag BrokenMessage {
  string name = 1;
}
)";

// ---------------------------------------------------------------------------
// Basic compilation
// ---------------------------------------------------------------------------

TEST(ProtoCompilerTest, BasicCompilationSucceeds) {
  registry::ProtoCompiler compiler;
  auto result = compiler.Compile(kSimpleProto, "SimpleMessage");

  ASSERT_TRUE(std::holds_alternative<registry::CompilationResult>(result));
  const auto& cr = std::get<registry::CompilationResult>(result);
  EXPECT_EQ(cr.descriptor_set.file_size(), 1);
  EXPECT_EQ(cr.descriptor_set.file(0).name(), "input.proto");
}

// ---------------------------------------------------------------------------
// System imports (artifact_options.proto)
// ---------------------------------------------------------------------------

TEST(ProtoCompilerTest, SystemImportsCompileSuccessfully) {
  registry::ProtoCompiler compiler;
  auto result = compiler.Compile(kProtoWithArtifactOptions, "Task");

  ASSERT_TRUE(std::holds_alternative<registry::CompilationResult>(result))
      << "Expected success but got error: "
      << (std::holds_alternative<registry::CompilationError>(result) ? std::get<registry::CompilationError>(result).description : "");
  const auto& cr = std::get<registry::CompilationResult>(result);
  EXPECT_EQ(cr.descriptor_set.file_size(), 1);
}

// ---------------------------------------------------------------------------
// Proto3 enforcement
// ---------------------------------------------------------------------------

TEST(ProtoCompilerTest, Proto2SyntaxIsRejected) {
  registry::ProtoCompiler compiler;
  auto result = compiler.Compile(kProto2Source, "LegacyMessage");

  ASSERT_TRUE(std::holds_alternative<registry::CompilationError>(result));
  const auto& err = std::get<registry::CompilationError>(result);
  EXPECT_NE(err.description.find("proto3"), std::string::npos) << "Error should mention proto3: " << err.description;
}

// ---------------------------------------------------------------------------
// Type name resolution
// ---------------------------------------------------------------------------

TEST(ProtoCompilerTest, NonexistentTypeNameFails) {
  registry::ProtoCompiler compiler;
  auto result = compiler.Compile(kSimpleProto, "DoesNotExist");

  ASSERT_TRUE(std::holds_alternative<registry::CompilationError>(result));
  const auto& err = std::get<registry::CompilationError>(result);
  EXPECT_NE(err.description.find("DoesNotExist"), std::string::npos) << "Error should mention the type name: " << err.description;
}

TEST(ProtoCompilerTest, TypeNameResolvingToImportedMessageFails) {
  // artifact_options.proto defines IndexDefinition; asking for it as
  // type_name should fail because it lives in an imported file, not the
  // user's proto.
  constexpr const char* kProtoImportingOptions = R"(
syntax = "proto3";
import "artifact_options.proto";
message MyMessage {
  string name = 1;
}
)";

  registry::ProtoCompiler compiler;
  auto result = compiler.Compile(kProtoImportingOptions, "artifact_system.IndexDefinition");

  ASSERT_TRUE(std::holds_alternative<registry::CompilationError>(result));
  const auto& err = std::get<registry::CompilationError>(result);
  EXPECT_NE(err.description.find("imported"), std::string::npos) << "Error should mention imported file: " << err.description;
}

// ---------------------------------------------------------------------------
// Empty type_name
// ---------------------------------------------------------------------------

TEST(ProtoCompilerTest, EmptyTypeNameIsRejected) {
  registry::ProtoCompiler compiler;
  auto result = compiler.Compile(kSimpleProto, "");

  ASSERT_TRUE(std::holds_alternative<registry::CompilationError>(result));
  const auto& err = std::get<registry::CompilationError>(result);
  EXPECT_NE(err.description.find("empty"), std::string::npos) << "Error should mention empty: " << err.description;
}

// ---------------------------------------------------------------------------
// Input size limit
// ---------------------------------------------------------------------------

TEST(ProtoCompilerTest, InputExceedingOneMBIsRejected) {
  registry::ProtoCompiler compiler;
  // Create a source string just over 1 MB.
  std::string huge_source(1024 * 1024 + 1, ' ');
  auto result = compiler.Compile(huge_source, "Anything");

  ASSERT_TRUE(std::holds_alternative<registry::CompilationError>(result));
  const auto& err = std::get<registry::CompilationError>(result);
  EXPECT_NE(err.description.find("1 MB"), std::string::npos) << "Error should mention size limit: " << err.description;
}

// ---------------------------------------------------------------------------
// Nesting depth limit
// ---------------------------------------------------------------------------

// Build a proto source with messages nested to the given depth.
static std::string MakeNestedProto(int depth) {
  std::string source = "syntax = \"proto3\";\n";
  for (int i = 0; i < depth; ++i) {
    source += std::string(i * 2, ' ') + "message Level" + std::to_string(i) + " {\n";
  }
  // Add a leaf field in the innermost message.
  source += std::string(depth * 2, ' ') + "string leaf = 1;\n";
  for (int i = depth - 1; i >= 0; --i) {
    source += std::string(i * 2, ' ') + "}\n";
  }
  return source;
}

// The protobuf parser enforces its own nesting limit of 32 for message
// declarations.  Our limit of 64 is above that, so the parser rejects
// deeply nested protos before our check runs.  Test within the parser's
// limit to verify compilation succeeds, and test beyond it to verify a
// compilation error is reported.
TEST(ProtoCompilerTest, NestingDepthWithinParserLimitSucceeds) {
  registry::ProtoCompiler compiler;
  std::string source = MakeNestedProto(31);
  auto result = compiler.Compile(source, "Level0");

  ASSERT_TRUE(std::holds_alternative<registry::CompilationResult>(result))
      << "Expected success but got error: "
      << (std::holds_alternative<registry::CompilationError>(result) ? std::get<registry::CompilationError>(result).description : "");
}

TEST(ProtoCompilerTest, NestingDepthExceedingParserLimitIsRejected) {
  registry::ProtoCompiler compiler;
  // 33 levels exceeds the protobuf parser's built-in limit of 32.
  std::string source = MakeNestedProto(33);
  auto result = compiler.Compile(source, "Level0");

  ASSERT_TRUE(std::holds_alternative<registry::CompilationError>(result));
  const auto& err = std::get<registry::CompilationError>(result);
  EXPECT_NE(err.description.find("compilation failed"), std::string::npos) << "Error: " << err.description;
}

// ---------------------------------------------------------------------------
// Parse error handling
// ---------------------------------------------------------------------------

TEST(ProtoCompilerTest, MalformedProtoReturnsDescriptiveError) {
  registry::ProtoCompiler compiler;
  auto result = compiler.Compile(kMalformedProto, "BrokenMessage");

  ASSERT_TRUE(std::holds_alternative<registry::CompilationError>(result));
  const auto& err = std::get<registry::CompilationError>(result);
  EXPECT_FALSE(err.description.empty());
  // The error should contain some indication of what went wrong.
  EXPECT_NE(err.description.find("compilation failed"), std::string::npos) << "Error: " << err.description;
}

// ---------------------------------------------------------------------------
// Thread safety
// ---------------------------------------------------------------------------

TEST(ProtoCompilerTest, ConcurrentCompilationsAllSucceed) {
  registry::ProtoCompiler compiler;
  constexpr int kThreadCount = 8;
  std::vector<std::thread> threads;
  std::vector<bool> success(kThreadCount, false);

  for (int i = 0; i < kThreadCount; ++i) {
    threads.emplace_back([&compiler, &success, i]() {
      // Each thread compiles a slightly different proto to avoid caching.
      std::string source = "syntax = \"proto3\";\nmessage Msg" + std::to_string(i) + " {\n  string field = 1;\n}\n";
      auto result = compiler.Compile(source, "Msg" + std::to_string(i));
      success[i] = std::holds_alternative<registry::CompilationResult>(result);
    });
  }

  for (auto& t : threads)
    t.join();

  for (int i = 0; i < kThreadCount; ++i) {
    EXPECT_TRUE(success[i]) << "Thread " << i << " failed";
  }
}

// ---------------------------------------------------------------------------
// Package support
// ---------------------------------------------------------------------------

TEST(ProtoCompilerTest, PackageSupportWithFullyQualifiedTypeName) {
  registry::ProtoCompiler compiler;
  auto result = compiler.Compile(kProtoWithPackage, "myapp.models.Document");

  ASSERT_TRUE(std::holds_alternative<registry::CompilationResult>(result))
      << "Expected success but got error: "
      << (std::holds_alternative<registry::CompilationError>(result) ? std::get<registry::CompilationError>(result).description : "");
  const auto& cr = std::get<registry::CompilationResult>(result);
  EXPECT_EQ(cr.descriptor_set.file_size(), 1);
}

TEST(ProtoCompilerTest, PackageUnqualifiedTypeNameFails) {
  registry::ProtoCompiler compiler;
  // "Document" without the package prefix should not resolve.
  auto result = compiler.Compile(kProtoWithPackage, "Document");

  ASSERT_TRUE(std::holds_alternative<registry::CompilationError>(result));
}

// ---------------------------------------------------------------------------
// Descriptor set excludes imports
// ---------------------------------------------------------------------------

TEST(ProtoCompilerTest, DescriptorSetExcludesImportedFiles) {
  registry::ProtoCompiler compiler;
  auto result = compiler.Compile(kProtoWithArtifactOptions, "Task");

  ASSERT_TRUE(std::holds_alternative<registry::CompilationResult>(result))
      << "Expected success but got error: "
      << (std::holds_alternative<registry::CompilationError>(result) ? std::get<registry::CompilationError>(result).description : "");
  const auto& cr = std::get<registry::CompilationResult>(result);

  // Only the user's file should be present -- no artifact_options.proto,
  // no google/protobuf/descriptor.proto.
  ASSERT_EQ(cr.descriptor_set.file_size(), 1);
  EXPECT_EQ(cr.descriptor_set.file(0).name(), "input.proto");

  // Double-check none of the files are system protos.
  for (int i = 0; i < cr.descriptor_set.file_size(); ++i) {
    EXPECT_NE(cr.descriptor_set.file(i).name(), "artifact_options.proto");
    EXPECT_NE(cr.descriptor_set.file(i).name(), "google/protobuf/descriptor.proto");
  }
}

} // namespace
} // namespace artifact_system::testing

#include "encoding/index_key_encoder.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/dynamic_message.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

const google::protobuf::Descriptor* BuildKeyMessageDescriptor(google::protobuf::DescriptorPool* pool) {
  google::protobuf::FileDescriptorProto file;
  file.set_name("index_key_encoder_test.proto");
  file.set_syntax("proto3");

  auto* enum_type = file.add_enum_type();
  enum_type->set_name("TestEnum");
  auto* enum_zero = enum_type->add_value();
  enum_zero->set_name("TEST_ENUM_ZERO");
  enum_zero->set_number(0);
  auto* enum_five = enum_type->add_value();
  enum_five->set_name("TEST_ENUM_FIVE");
  enum_five->set_number(5);

  auto* nested = file.add_message_type();
  nested->set_name("Nested");
  auto* nested_leaf = nested->add_field();
  nested_leaf->set_name("leaf");
  nested_leaf->set_number(1);
  nested_leaf->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  nested_leaf->set_type(google::protobuf::FieldDescriptorProto::TYPE_INT32);

  auto* message = file.add_message_type();
  message->set_name("KeyMessage");

  auto add_field = [message](const char* name, int number, google::protobuf::FieldDescriptorProto::Type type, const char* type_name = nullptr) {
    auto* field = message->add_field();
    field->set_name(name);
    field->set_number(number);
    field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
    field->set_type(type);
    if (type_name != nullptr) {
      field->set_type_name(type_name);
    }
  };

  add_field("f_int32", 1, google::protobuf::FieldDescriptorProto::TYPE_INT32);
  add_field("f_sint32", 2, google::protobuf::FieldDescriptorProto::TYPE_SINT32);
  add_field("f_sfixed32", 3, google::protobuf::FieldDescriptorProto::TYPE_SFIXED32);
  add_field("f_uint32", 4, google::protobuf::FieldDescriptorProto::TYPE_UINT32);
  add_field("f_fixed32", 5, google::protobuf::FieldDescriptorProto::TYPE_FIXED32);
  add_field("f_int64", 6, google::protobuf::FieldDescriptorProto::TYPE_INT64);
  add_field("f_sint64", 7, google::protobuf::FieldDescriptorProto::TYPE_SINT64);
  add_field("f_sfixed64", 8, google::protobuf::FieldDescriptorProto::TYPE_SFIXED64);
  add_field("f_uint64", 9, google::protobuf::FieldDescriptorProto::TYPE_UINT64);
  add_field("f_fixed64", 10, google::protobuf::FieldDescriptorProto::TYPE_FIXED64);
  add_field("f_bool", 11, google::protobuf::FieldDescriptorProto::TYPE_BOOL);
  add_field("f_enum", 12, google::protobuf::FieldDescriptorProto::TYPE_ENUM, "TestEnum");
  add_field("f_float", 13, google::protobuf::FieldDescriptorProto::TYPE_FLOAT);
  add_field("f_double", 14, google::protobuf::FieldDescriptorProto::TYPE_DOUBLE);
  add_field("f_string", 15, google::protobuf::FieldDescriptorProto::TYPE_STRING);
  add_field("f_bytes", 16, google::protobuf::FieldDescriptorProto::TYPE_BYTES);
  add_field("nested", 17, google::protobuf::FieldDescriptorProto::TYPE_MESSAGE, "Nested");

  const google::protobuf::FileDescriptor* built = pool->BuildFile(file);
  if (built == nullptr) {
    return nullptr;
  }
  return built->FindMessageTypeByName("KeyMessage");
}

std::unique_ptr<google::protobuf::Message> BuildMessageWithValues(const google::protobuf::Descriptor* descriptor,
                                                                  google::protobuf::DynamicMessageFactory* factory) {
  const google::protobuf::Message* prototype = factory->GetPrototype(descriptor);
  std::unique_ptr<google::protobuf::Message> message(prototype->New());
  const google::protobuf::Reflection* reflection = message->GetReflection();

  reflection->SetInt32(message.get(), descriptor->FindFieldByName("f_int32"), -1);
  reflection->SetInt32(message.get(), descriptor->FindFieldByName("f_sint32"), -2);
  reflection->SetInt32(message.get(), descriptor->FindFieldByName("f_sfixed32"), -3);
  reflection->SetUInt32(message.get(), descriptor->FindFieldByName("f_uint32"), 0x01020304U);
  reflection->SetUInt32(message.get(), descriptor->FindFieldByName("f_fixed32"), 0xA0B0C0D0U);
  reflection->SetInt64(message.get(), descriptor->FindFieldByName("f_int64"), -4);
  reflection->SetInt64(message.get(), descriptor->FindFieldByName("f_sint64"), -5);
  reflection->SetInt64(message.get(), descriptor->FindFieldByName("f_sfixed64"), -6);
  reflection->SetUInt64(message.get(), descriptor->FindFieldByName("f_uint64"), 0x0102030405060708ULL);
  reflection->SetUInt64(message.get(), descriptor->FindFieldByName("f_fixed64"), 0xFFEEDDCCBBAA9988ULL);
  reflection->SetBool(message.get(), descriptor->FindFieldByName("f_bool"), true);
  reflection->SetEnumValue(message.get(), descriptor->FindFieldByName("f_enum"), 5);
  reflection->SetFloat(message.get(), descriptor->FindFieldByName("f_float"), 1.5F);
  reflection->SetDouble(message.get(), descriptor->FindFieldByName("f_double"), -2.25);
  reflection->SetString(message.get(), descriptor->FindFieldByName("f_string"), "A");
  reflection->SetString(message.get(), descriptor->FindFieldByName("f_bytes"), std::string("\x00\xFF", 2));

  google::protobuf::Message* nested = reflection->MutableMessage(message.get(), descriptor->FindFieldByName("nested"));
  nested->GetReflection()->SetInt32(nested, nested->GetDescriptor()->FindFieldByName("leaf"), 7);

  return message;
}

} // namespace

TEST(IndexKeyEncoderTest, EncodesAllSupportedTypesAndNestedScalarField) {
  google::protobuf::DescriptorPool pool;
  const google::protobuf::Descriptor* descriptor = BuildKeyMessageDescriptor(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> message = BuildMessageWithValues(descriptor, &factory);

  const std::vector<std::string> fields = {
      "f_int32",   "f_sint32", "f_sfixed32", "f_uint32", "f_fixed32", "f_int64",  "f_sint64", "f_sfixed64",  "f_uint64",
      "f_fixed64", "f_bool",   "f_enum",     "f_float",  "f_double",  "f_string", "f_bytes",  "nested.leaf",
  };

  auto encoded_or = encoding::EncodeKey(*descriptor, *message, fields);
  ASSERT_TRUE(encoded_or.ok()) << encoded_or.status();

  const std::vector<uint8_t> expected = {
      // f_int32, f_sint32, f_sfixed32
      0xFF,
      0xFF,
      0xFF,
      0xFF,
      0xFE,
      0xFF,
      0xFF,
      0xFF,
      0xFD,
      0xFF,
      0xFF,
      0xFF,
      // f_uint32, f_fixed32
      0x04,
      0x03,
      0x02,
      0x01,
      0xD0,
      0xC0,
      0xB0,
      0xA0,
      // f_int64, f_sint64, f_sfixed64
      0xFC,
      0xFF,
      0xFF,
      0xFF,
      0xFF,
      0xFF,
      0xFF,
      0xFF,
      0xFB,
      0xFF,
      0xFF,
      0xFF,
      0xFF,
      0xFF,
      0xFF,
      0xFF,
      0xFA,
      0xFF,
      0xFF,
      0xFF,
      0xFF,
      0xFF,
      0xFF,
      0xFF,
      // f_uint64, f_fixed64
      0x08,
      0x07,
      0x06,
      0x05,
      0x04,
      0x03,
      0x02,
      0x01,
      0x88,
      0x99,
      0xAA,
      0xBB,
      0xCC,
      0xDD,
      0xEE,
      0xFF,
      // f_bool, f_enum
      0x01,
      0x05,
      0x00,
      0x00,
      0x00,
      // f_float (1.5), f_double (-2.25)
      0x00,
      0x00,
      0xC0,
      0x3F,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x02,
      0xC0,
      // f_string("A"), f_bytes("\x00\xFF")
      0x01,
      0x41,
      0x02,
      0x00,
      0xFF,
      // nested.leaf
      0x07,
      0x00,
      0x00,
      0x00,
  };

  EXPECT_EQ(*encoded_or, expected);
}

TEST(IndexKeyEncoderTest, RejectsNaNInFloatAndDoubleFields) {
  google::protobuf::DescriptorPool pool;
  const google::protobuf::Descriptor* descriptor = BuildKeyMessageDescriptor(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  const google::protobuf::Message* prototype = factory.GetPrototype(descriptor);
  std::unique_ptr<google::protobuf::Message> message(prototype->New());
  const google::protobuf::Reflection* reflection = message->GetReflection();

  reflection->SetFloat(message.get(), descriptor->FindFieldByName("f_float"), std::nanf(""));
  auto float_or = encoding::EncodeKey(*descriptor, *message, std::vector<std::string>{"f_float"});
  ASSERT_FALSE(float_or.ok());
  EXPECT_EQ(float_or.status().code(), absl::StatusCode::kInvalidArgument);

  reflection->SetDouble(message.get(), descriptor->FindFieldByName("f_double"), std::nan(""));
  auto double_or = encoding::EncodeKey(*descriptor, *message, std::vector<std::string>{"f_double"});
  ASSERT_FALSE(double_or.ok());
  EXPECT_EQ(double_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(IndexKeyEncoderTest, NormalizesNegativeZeroFloatAndDouble) {
  google::protobuf::DescriptorPool pool;
  const google::protobuf::Descriptor* descriptor = BuildKeyMessageDescriptor(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  const google::protobuf::Message* prototype = factory.GetPrototype(descriptor);
  std::unique_ptr<google::protobuf::Message> message(prototype->New());
  const google::protobuf::Reflection* reflection = message->GetReflection();

  reflection->SetFloat(message.get(), descriptor->FindFieldByName("f_float"), -0.0F);
  reflection->SetDouble(message.get(), descriptor->FindFieldByName("f_double"), -0.0);

  auto encoded_or = encoding::EncodeKey(*descriptor, *message, std::vector<std::string>{"f_float", "f_double"});
  ASSERT_TRUE(encoded_or.ok()) << encoded_or.status();

  const std::vector<uint8_t> expected = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  EXPECT_EQ(*encoded_or, expected);
}

TEST(IndexKeyEncoderTest, EncodesAndDecodesVarint) {
  const uint64_t input = 300;
  const std::vector<uint8_t> encoded = encoding::EncodeVarint(input);
  EXPECT_EQ(encoded, (std::vector<uint8_t>{0xAC, 0x02}));

  size_t bytes_read = 0;
  auto decoded_or = encoding::DecodeVarint(encoded, &bytes_read);
  ASSERT_TRUE(decoded_or.ok()) << decoded_or.status();
  EXPECT_EQ(*decoded_or, input);
  EXPECT_EQ(bytes_read, encoded.size());
}

TEST(IndexKeyEncoderTest, RejectsNonMinimalVarintEncoding) {
  const std::vector<uint8_t> non_minimal = {0x81, 0x00};
  size_t bytes_read = 0;

  auto decoded_or = encoding::DecodeVarint(non_minimal, &bytes_read);
  ASSERT_FALSE(decoded_or.ok());
  EXPECT_EQ(decoded_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(IndexKeyEncoderTest, RejectsVarintOverflowEncoding) {
  const std::vector<uint8_t> overflow = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x03};
  size_t bytes_read = 0;

  auto decoded_or = encoding::DecodeVarint(overflow, &bytes_read);
  ASSERT_FALSE(decoded_or.ok());
  EXPECT_EQ(decoded_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(IndexKeyEncoderTest, RejectsUnterminatedTenByteVarint) {
  const std::vector<uint8_t> unterminated = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
  size_t bytes_read = 0;

  auto decoded_or = encoding::DecodeVarint(unterminated, &bytes_read);
  ASSERT_FALSE(decoded_or.ok());
  EXPECT_EQ(decoded_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(IndexKeyEncoderTest, RejectsDescriptorMismatch) {
  google::protobuf::DescriptorPool pool;
  const google::protobuf::Descriptor* descriptor = BuildKeyMessageDescriptor(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::FileDescriptorProto other_file;
  other_file.set_name("other.proto");
  other_file.set_syntax("proto3");
  auto* other_msg = other_file.add_message_type();
  other_msg->set_name("OtherMessage");
  auto* other_field = other_msg->add_field();
  other_field->set_name("f_int32");
  other_field->set_number(1);
  other_field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  other_field->set_type(google::protobuf::FieldDescriptorProto::TYPE_INT32);
  const google::protobuf::FileDescriptor* other_built = pool.BuildFile(other_file);
  ASSERT_NE(other_built, nullptr);
  const google::protobuf::Descriptor* other_descriptor = other_built->FindMessageTypeByName("OtherMessage");
  ASSERT_NE(other_descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> message = BuildMessageWithValues(descriptor, &factory);

  auto encoded_or = encoding::EncodeKey(*other_descriptor, *message, std::vector<std::string>{"f_int32"});
  ASSERT_FALSE(encoded_or.ok());
  EXPECT_EQ(encoded_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(IndexKeyEncoderTest, RejectsSameNameDescriptorFromDifferentPool) {
  google::protobuf::DescriptorPool pool_a;
  const google::protobuf::Descriptor* descriptor_a = BuildKeyMessageDescriptor(&pool_a);
  ASSERT_NE(descriptor_a, nullptr);

  google::protobuf::DescriptorPool pool_b;
  const google::protobuf::Descriptor* descriptor_b = BuildKeyMessageDescriptor(&pool_b);
  ASSERT_NE(descriptor_b, nullptr);
  ASSERT_EQ(descriptor_a->full_name(), descriptor_b->full_name());

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> message = BuildMessageWithValues(descriptor_a, &factory);

  auto encoded_or = encoding::EncodeKey(*descriptor_b, *message, std::vector<std::string>{"f_int32"});
  ASSERT_FALSE(encoded_or.ok());
  EXPECT_EQ(encoded_or.status().code(), absl::StatusCode::kInvalidArgument);
}

} // namespace artifact_system::testing

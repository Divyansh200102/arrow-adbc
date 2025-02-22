// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <optional>

#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.hpp>

#include "postgres_copy_reader.h"
#include "validation/adbc_validation_util.h"

namespace adbcpq {

class PostgresCopyStreamTester {
 public:
  ArrowErrorCode Init(const PostgresType& root_type, ArrowError* error = nullptr) {
    NANOARROW_RETURN_NOT_OK(reader_.Init(root_type));
    NANOARROW_RETURN_NOT_OK(reader_.InferOutputSchema(error));
    NANOARROW_RETURN_NOT_OK(reader_.InitFieldReaders(error));
    return NANOARROW_OK;
  }

  ArrowErrorCode ReadAll(ArrowBufferView* data, ArrowError* error = nullptr) {
    NANOARROW_RETURN_NOT_OK(reader_.ReadHeader(data, error));

    int result;
    do {
      result = reader_.ReadRecord(data, error);
    } while (result == NANOARROW_OK);

    return result;
  }

  void GetSchema(ArrowSchema* out) { reader_.GetSchema(out); }

  ArrowErrorCode GetArray(ArrowArray* out, ArrowError* error = nullptr) {
    return reader_.GetArray(out, error);
  }

 private:
  PostgresCopyStreamReader reader_;
};

class PostgresCopyStreamWriteTester {
 public:
  ArrowErrorCode Init(struct ArrowSchema* schema, struct ArrowArray* array,
                      ArrowError* error = nullptr) {
    NANOARROW_RETURN_NOT_OK(writer_.Init(schema, array));
    NANOARROW_RETURN_NOT_OK(writer_.InitFieldWriters(error));
    return NANOARROW_OK;
  }

  ArrowErrorCode WriteAll(struct ArrowBuffer* buffer, ArrowError* error = nullptr) {
    NANOARROW_RETURN_NOT_OK(writer_.WriteHeader(buffer, error));

    int result;
    do {
      result = writer_.WriteRecord(buffer, error);
    } while (result == NANOARROW_OK);

    return result;
  }

 private:
  PostgresCopyStreamWriter writer_;
};

// COPY (SELECT CAST("col" AS BOOLEAN) AS "col" FROM (  VALUES (TRUE), (FALSE), (NULL)) AS
// drvd("col")) TO STDOUT;
static uint8_t kTestPgCopyBoolean[] = {
    0x50, 0x47, 0x43, 0x4f, 0x50, 0x59, 0x0a, 0xff, 0x0d, 0x0a, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

TEST(PostgresCopyUtilsTest, PostgresCopyReadBoolean) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyBoolean;
  data.size_bytes = sizeof(kTestPgCopyBoolean);

  auto col_type = PostgresType(PostgresTypeId::kBool);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);

  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyBoolean, sizeof(kTestPgCopyBoolean));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 3);
  ASSERT_EQ(array->n_children, 1);

  const uint8_t* validity =
      reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  const uint8_t* data_buffer =
      reinterpret_cast<const uint8_t*>(array->children[0]->buffers[1]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_FALSE(ArrowBitGet(validity, 2));

  ASSERT_TRUE(ArrowBitGet(data_buffer, 0));
  ASSERT_FALSE(ArrowBitGet(data_buffer, 1));
  ASSERT_FALSE(ArrowBitGet(data_buffer, 2));
}

TEST(PostgresCopyUtilsTest, PostgresCopyWriteBoolean) {
  adbc_validation::Handle<struct ArrowSchema> schema;
  adbc_validation::Handle<struct ArrowArray> array;
  struct ArrowError na_error;
  ASSERT_EQ(adbc_validation::MakeSchema(&schema.value, {{"col", NANOARROW_TYPE_BOOL}}),
            ADBC_STATUS_OK);
  ASSERT_EQ(adbc_validation::MakeBatch<bool>(&schema.value, &array.value, &na_error,
                                             {true, false, std::nullopt}),
            ADBC_STATUS_OK);

  PostgresCopyStreamWriteTester tester;
  ASSERT_EQ(tester.Init(&schema.value, &array.value), NANOARROW_OK);

  struct ArrowBuffer buffer;
  ArrowBufferInit(&buffer);
  ArrowBufferReserve(&buffer, sizeof(kTestPgCopyBoolean));
  uint8_t* cursor = buffer.data;

  ASSERT_EQ(tester.WriteAll(&buffer, nullptr), ENODATA);

  // The last 4 bytes of a message can be transmitted via PQputCopyData
  // so no need to test those bytes from the Writer
  for (size_t i = 0; i < sizeof(kTestPgCopyBoolean) - 4; i++) {
    EXPECT_EQ(cursor[i], kTestPgCopyBoolean[i]);
  }

  buffer.data = cursor;
  ArrowBufferReset(&buffer);
}

// COPY (SELECT CAST("col" AS SMALLINT) AS "col" FROM (  VALUES (-123), (-1), (1), (123),
// (NULL)) AS drvd("col")) TO STDOUT WITH (FORMAT binary);
static uint8_t kTestPgCopySmallInt[] = {
    0x50, 0x47, 0x43, 0x4f, 0x50, 0x59, 0x0a, 0xff, 0x0d, 0x0a, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x02, 0xff, 0x85, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0xff, 0xff, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x7b, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

TEST(PostgresCopyUtilsTest, PostgresCopyReadSmallInt) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopySmallInt;
  data.size_bytes = sizeof(kTestPgCopySmallInt);

  auto col_type = PostgresType(PostgresTypeId::kInt2);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopySmallInt, sizeof(kTestPgCopySmallInt));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 5);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto data_buffer = reinterpret_cast<const int16_t*>(array->children[0]->buffers[1]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_TRUE(ArrowBitGet(validity, 2));
  ASSERT_TRUE(ArrowBitGet(validity, 3));
  ASSERT_FALSE(ArrowBitGet(validity, 4));

  ASSERT_EQ(data_buffer[0], -123);
  ASSERT_EQ(data_buffer[1], -1);
  ASSERT_EQ(data_buffer[2], 1);
  ASSERT_EQ(data_buffer[3], 123);
  ASSERT_EQ(data_buffer[4], 0);
}

TEST(PostgresCopyUtilsTest, PostgresCopyWriteInt16) {
  adbc_validation::Handle<struct ArrowSchema> schema;
  adbc_validation::Handle<struct ArrowArray> array;
  struct ArrowError na_error;
  ASSERT_EQ(adbc_validation::MakeSchema(&schema.value, {{"col", NANOARROW_TYPE_INT16}}),
            ADBC_STATUS_OK);
  ASSERT_EQ(adbc_validation::MakeBatch<int16_t>(&schema.value, &array.value, &na_error,
                                                {-123, -1, 1, 123, std::nullopt}),
            ADBC_STATUS_OK);

  PostgresCopyStreamWriteTester tester;
  ASSERT_EQ(tester.Init(&schema.value, &array.value), NANOARROW_OK);

  struct ArrowBuffer buffer;
  ArrowBufferInit(&buffer);
  ArrowBufferReserve(&buffer, sizeof(kTestPgCopySmallInt));
  uint8_t* cursor = buffer.data;

  ASSERT_EQ(tester.WriteAll(&buffer, nullptr), ENODATA);

  // The last 4 bytes of a message can be transmitted via PQputCopyData
  // so no need to test those bytes from the Writer
  for (size_t i = 0; i < sizeof(kTestPgCopySmallInt) - 4; i++) {
    EXPECT_EQ(cursor[i], kTestPgCopySmallInt[i]);
  }

  buffer.data = cursor;
  ArrowBufferReset(&buffer);
}

// COPY (SELECT CAST("col" AS INTEGER) AS "col" FROM (  VALUES (-123), (-1), (1), (123),
// (NULL)) AS drvd("col")) TO STDOUT WITH (FORMAT binary);
static uint8_t kTestPgCopyInteger[] = {
    0x50, 0x47, 0x43, 0x4f, 0x50, 0x59, 0x0a, 0xff, 0x0d, 0x0a, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0xff, 0xff, 0xff,
    0x85, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0xff, 0xff, 0xff, 0xff, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x00, 0x00, 0x7b, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

TEST(PostgresCopyUtilsTest, PostgresCopyReadInteger) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyInteger;
  data.size_bytes = sizeof(kTestPgCopyInteger);

  auto col_type = PostgresType(PostgresTypeId::kInt4);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyInteger, sizeof(kTestPgCopyInteger));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 5);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto data_buffer = reinterpret_cast<const int32_t*>(array->children[0]->buffers[1]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_TRUE(ArrowBitGet(validity, 2));
  ASSERT_TRUE(ArrowBitGet(validity, 3));
  ASSERT_FALSE(ArrowBitGet(validity, 4));

  ASSERT_EQ(data_buffer[0], -123);
  ASSERT_EQ(data_buffer[1], -1);
  ASSERT_EQ(data_buffer[2], 1);
  ASSERT_EQ(data_buffer[3], 123);
  ASSERT_EQ(data_buffer[4], 0);
}

TEST(PostgresCopyUtilsTest, PostgresCopyWriteInt32) {
  adbc_validation::Handle<struct ArrowSchema> schema;
  adbc_validation::Handle<struct ArrowArray> array;
  struct ArrowError na_error;
  ASSERT_EQ(adbc_validation::MakeSchema(&schema.value, {{"col", NANOARROW_TYPE_INT32}}),
            ADBC_STATUS_OK);
  ASSERT_EQ(adbc_validation::MakeBatch<int32_t>(&schema.value, &array.value, &na_error,
                                                {-123, -1, 1, 123, std::nullopt}),
            ADBC_STATUS_OK);

  PostgresCopyStreamWriteTester tester;
  ASSERT_EQ(tester.Init(&schema.value, &array.value), NANOARROW_OK);

  struct ArrowBuffer buffer;
  ArrowBufferInit(&buffer);
  ArrowBufferReserve(&buffer, sizeof(kTestPgCopyInteger));
  uint8_t* cursor = buffer.data;

  ASSERT_EQ(tester.WriteAll(&buffer, nullptr), ENODATA);

  // The last 4 bytes of a message can be transmitted via PQputCopyData
  // so no need to test those bytes from the Writer
  for (size_t i = 0; i < sizeof(kTestPgCopyInteger) - 4; i++) {
    EXPECT_EQ(cursor[i], kTestPgCopyInteger[i]);
  }

  buffer.data = cursor;
  ArrowBufferReset(&buffer);
}

// COPY (SELECT CAST("col" AS BIGINT) AS "col" FROM (  VALUES (-123), (-1), (1), (123),
// (NULL)) AS drvd("col")) TO STDOUT WITH (FORMAT binary);
static uint8_t kTestPgCopyBigInt[] = {
    0x50, 0x47, 0x43, 0x4f, 0x50, 0x59, 0x0a, 0xff, 0x0d, 0x0a, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0x85, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x7b, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

TEST(PostgresCopyUtilsTest, PostgresCopyReadBigInt) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyBigInt;
  data.size_bytes = sizeof(kTestPgCopyBigInt);

  auto col_type = PostgresType(PostgresTypeId::kInt8);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyBigInt, sizeof(kTestPgCopyBigInt));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 5);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto data_buffer = reinterpret_cast<const int64_t*>(array->children[0]->buffers[1]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_TRUE(ArrowBitGet(validity, 2));
  ASSERT_TRUE(ArrowBitGet(validity, 3));
  ASSERT_FALSE(ArrowBitGet(validity, 4));

  ASSERT_EQ(data_buffer[0], -123);
  ASSERT_EQ(data_buffer[1], -1);
  ASSERT_EQ(data_buffer[2], 1);
  ASSERT_EQ(data_buffer[3], 123);
  ASSERT_EQ(data_buffer[4], 0);
}

TEST(PostgresCopyUtilsTest, PostgresCopyWriteInt64) {
  adbc_validation::Handle<struct ArrowSchema> schema;
  adbc_validation::Handle<struct ArrowArray> array;
  struct ArrowError na_error;
  ASSERT_EQ(adbc_validation::MakeSchema(&schema.value, {{"col", NANOARROW_TYPE_INT64}}),
            ADBC_STATUS_OK);
  ASSERT_EQ(adbc_validation::MakeBatch<int64_t>(&schema.value, &array.value, &na_error,
                                                {-123, -1, 1, 123, std::nullopt}),
            ADBC_STATUS_OK);

  PostgresCopyStreamWriteTester tester;
  ASSERT_EQ(tester.Init(&schema.value, &array.value), NANOARROW_OK);

  struct ArrowBuffer buffer;
  ArrowBufferInit(&buffer);
  ArrowBufferReserve(&buffer, sizeof(kTestPgCopyBigInt));
  uint8_t* cursor = buffer.data;

  ASSERT_EQ(tester.WriteAll(&buffer, nullptr), ENODATA);

  // The last 4 bytes of a message can be transmitted via PQputCopyData
  // so no need to test those bytes from the Writer
  for (size_t i = 0; i < sizeof(kTestPgCopyBigInt) - 4; i++) {
    EXPECT_EQ(cursor[i], kTestPgCopyBigInt[i]);
  }

  buffer.data = cursor;
  ArrowBufferReset(&buffer);
}

// COPY (SELECT CAST("col" AS REAL) AS "col" FROM (  VALUES (-123.456), (-1), (1),
// (123.456), (NULL)) AS drvd("col")) TO STDOUT WITH (FORMAT binary);
static uint8_t kTestPgCopyReal[] = {
    0x50, 0x47, 0x43, 0x4f, 0x50, 0x59, 0x0a, 0xff, 0x0d, 0x0a, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0xc2, 0xf6, 0xe9,
    0x79, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0xbf, 0x80, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x04, 0x3f, 0x80, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x42,
    0xf6, 0xe9, 0x79, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

TEST(PostgresCopyUtilsTest, PostgresCopyReadReal) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyReal;
  data.size_bytes = sizeof(kTestPgCopyReal);

  auto col_type = PostgresType(PostgresTypeId::kFloat4);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyReal, sizeof(kTestPgCopyReal));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 5);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto data_buffer = reinterpret_cast<const float*>(array->children[0]->buffers[1]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_TRUE(ArrowBitGet(validity, 2));
  ASSERT_TRUE(ArrowBitGet(validity, 3));
  ASSERT_FALSE(ArrowBitGet(validity, 4));

  ASSERT_FLOAT_EQ(data_buffer[0], -123.456);
  ASSERT_EQ(data_buffer[1], -1);
  ASSERT_EQ(data_buffer[2], 1);
  ASSERT_FLOAT_EQ(data_buffer[3], 123.456);
  ASSERT_EQ(data_buffer[4], 0);
}

// COPY (SELECT CAST("col" AS DOUBLE PRECISION) AS "col" FROM (  VALUES (-123.456), (-1),
// (1), (123.456), (NULL)) AS drvd("col")) TO STDOUT WITH (FORMAT binary);
static uint8_t kTestPgCopyDoublePrecision[] = {
    0x50, 0x47, 0x43, 0x4f, 0x50, 0x59, 0x0a, 0xff, 0x0d, 0x0a, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0xc0, 0x5e, 0xdd,
    0x2f, 0x1a, 0x9f, 0xbe, 0x77, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0xbf, 0xf0, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x3f, 0xf0, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x40, 0x5e, 0xdd,
    0x2f, 0x1a, 0x9f, 0xbe, 0x77, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

TEST(PostgresCopyUtilsTest, PostgresCopyReadDoublePrecision) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyDoublePrecision;
  data.size_bytes = sizeof(kTestPgCopyDoublePrecision);

  auto col_type = PostgresType(PostgresTypeId::kFloat8);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyDoublePrecision,
            sizeof(kTestPgCopyDoublePrecision));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 5);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto data_buffer = reinterpret_cast<const double*>(array->children[0]->buffers[1]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_TRUE(ArrowBitGet(validity, 2));
  ASSERT_TRUE(ArrowBitGet(validity, 3));
  ASSERT_FALSE(ArrowBitGet(validity, 4));

  ASSERT_DOUBLE_EQ(data_buffer[0], -123.456);
  ASSERT_EQ(data_buffer[1], -1);
  ASSERT_EQ(data_buffer[2], 1);
  ASSERT_DOUBLE_EQ(data_buffer[3], 123.456);
  ASSERT_EQ(data_buffer[4], 0);
}

// For full coverage, ensure that this contains NUMERIC examples that:
// - Have >= four zeroes to the left of the decimal point
// - Have >= four zeroes to the right of the decimal point
// - Include special values (nan, -inf, inf, NULL)
// - Have >= four trailing zeroes to the right of the decimal point
// - Have >= four leading zeroes before the first digit to the right of the decimal point
// - Is < 0 (negative)
// COPY (SELECT CAST(col AS NUMERIC) AS col FROM (  VALUES (1000000), ('0.00001234'),
// ('1.0000'), (-123.456), (123.456), ('nan'), ('-inf'), ('inf'), (NULL)) AS drvd(col)) TO
// STDOUT WITH (FORMAT binary);
static uint8_t kTestPgCopyNumeric[] = {
    0x50, 0x47, 0x43, 0x4f, 0x50, 0x59, 0x0a, 0xff, 0x0d, 0x0a, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0a, 0x00,
    0x01, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x08, 0x04, 0xd2, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x0a, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x0c, 0x00, 0x02, 0x00, 0x00, 0x40, 0x00, 0x00, 0x03, 0x00, 0x7b, 0x11,
    0xd0, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x7b, 0x11, 0xd0, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x00, 0xf0, 0x00, 0x00, 0x20, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x00, 0xd0, 0x00, 0x00, 0x20, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

TEST(PostgresCopyUtilsTest, PostgresCopyReadNumeric) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyNumeric;
  data.size_bytes = sizeof(kTestPgCopyNumeric);

  auto col_type = PostgresType(PostgresTypeId::kNumeric);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyNumeric, sizeof(kTestPgCopyNumeric));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 9);
  ASSERT_EQ(array->n_children, 1);

  nanoarrow::UniqueSchema schema;
  tester.GetSchema(schema.get());

  nanoarrow::UniqueArrayView array_view;
  ASSERT_EQ(ArrowArrayViewInitFromSchema(array_view.get(), schema.get(), nullptr),
            NANOARROW_OK);
  ASSERT_EQ(array_view->children[0]->storage_type, NANOARROW_TYPE_STRING);
  ASSERT_EQ(ArrowArrayViewSetArray(array_view.get(), array.get(), nullptr), NANOARROW_OK);

  auto validity = array_view->children[0]->buffer_views[0].data.as_uint8;
  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_TRUE(ArrowBitGet(validity, 2));
  ASSERT_TRUE(ArrowBitGet(validity, 3));
  ASSERT_TRUE(ArrowBitGet(validity, 4));
  ASSERT_TRUE(ArrowBitGet(validity, 5));
  ASSERT_TRUE(ArrowBitGet(validity, 6));
  ASSERT_TRUE(ArrowBitGet(validity, 7));
  ASSERT_FALSE(ArrowBitGet(validity, 8));

  struct ArrowStringView item;
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 0);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "1000000");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 1);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "0.00001234");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 2);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "1.0000");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 3);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "-123.456");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 4);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "123.456");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 5);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "nan");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 6);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "-inf");
  item = ArrowArrayViewGetStringUnsafe(array_view->children[0], 7);
  EXPECT_EQ(std::string(item.data, item.size_bytes), "inf");
}

// COPY (SELECT CAST("col" AS TEXT) AS "col" FROM (  VALUES ('abc'), ('1234'),
// (NULL::text)) AS drvd("col")) TO STDOUT WITH (FORMAT binary);
static uint8_t kTestPgCopyText[] = {
    0x50, 0x47, 0x43, 0x4f, 0x50, 0x59, 0x0a, 0xff, 0x0d, 0x0a, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x03, 0x61, 0x62, 0x63, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x31, 0x32,
    0x33, 0x34, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

TEST(PostgresCopyUtilsTest, PostgresCopyReadText) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyText;
  data.size_bytes = sizeof(kTestPgCopyText);

  auto col_type = PostgresType(PostgresTypeId::kText);
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyText, sizeof(kTestPgCopyText));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 3);
  ASSERT_EQ(array->n_children, 1);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto offsets = reinterpret_cast<const int32_t*>(array->children[0]->buffers[1]);
  auto data_buffer = reinterpret_cast<const char*>(array->children[0]->buffers[2]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_FALSE(ArrowBitGet(validity, 2));

  ASSERT_EQ(offsets[0], 0);
  ASSERT_EQ(offsets[1], 3);
  ASSERT_EQ(offsets[2], 7);
  ASSERT_EQ(offsets[3], 7);

  ASSERT_EQ(std::string(data_buffer + 0, 3), "abc");
  ASSERT_EQ(std::string(data_buffer + 3, 4), "1234");
}

// COPY (SELECT CAST("col" AS INTEGER ARRAY) AS "col" FROM (  VALUES ('{-123, -1}'), ('{0,
// 1, 123}'), (NULL)) AS drvd("col")) TO STDOUT WITH (FORMAT binary);
static uint8_t kTestPgCopyIntegerArray[] = {
    0x50, 0x47, 0x43, 0x4f, 0x50, 0x59, 0x0a, 0xff, 0x0d, 0x0a, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0xff, 0xff, 0xff, 0x85, 0x00, 0x00, 0x00,
    0x04, 0xff, 0xff, 0xff, 0xff, 0x00, 0x01, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x7b, 0x00,
    0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

TEST(PostgresCopyUtilsTest, PostgresCopyReadArray) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyIntegerArray;
  data.size_bytes = sizeof(kTestPgCopyIntegerArray);

  auto col_type = PostgresType(PostgresTypeId::kInt4).Array();
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyIntegerArray,
            sizeof(kTestPgCopyIntegerArray));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 3);
  ASSERT_EQ(array->n_children, 1);
  ASSERT_EQ(array->children[0]->n_children, 1);
  ASSERT_EQ(array->children[0]->children[0]->length, 5);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto offsets = reinterpret_cast<const int32_t*>(array->children[0]->buffers[1]);
  auto data_buffer =
      reinterpret_cast<const int32_t*>(array->children[0]->children[0]->buffers[1]);
  ASSERT_NE(validity, nullptr);
  ASSERT_NE(data_buffer, nullptr);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_FALSE(ArrowBitGet(validity, 2));

  ASSERT_EQ(offsets[0], 0);
  ASSERT_EQ(offsets[1], 2);
  ASSERT_EQ(offsets[2], 5);
  ASSERT_EQ(offsets[3], 5);

  ASSERT_EQ(data_buffer[0], -123);
  ASSERT_EQ(data_buffer[1], -1);
  ASSERT_EQ(data_buffer[2], 0);
  ASSERT_EQ(data_buffer[3], 1);
  ASSERT_EQ(data_buffer[4], 123);
}

// CREATE TYPE custom_record AS (nested1 integer, nested2 double precision);
// COPY (SELECT CAST("col" AS custom_record) AS "col" FROM (  VALUES ('(123, 456.789)'),
// ('(12, 345.678)'), (NULL)) AS drvd("col")) TO STDOUT WITH (FORMAT binary);
static uint8_t kTestPgCopyCustomRecord[] = {
    0x50, 0x47, 0x43, 0x4f, 0x50, 0x59, 0x0a, 0xff, 0x0d, 0x0a, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x20, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
    0x00, 0x7b, 0x00, 0x00, 0x02, 0xbd, 0x00, 0x00, 0x00, 0x08, 0x40, 0x7c, 0x8c,
    0x9f, 0xbe, 0x76, 0xc8, 0xb4, 0x00, 0x01, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x17, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x0c, 0x00, 0x00, 0x02, 0xbd, 0x00, 0x00, 0x00, 0x08, 0x40, 0x75, 0x9a, 0xd9,
    0x16, 0x87, 0x2b, 0x02, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

TEST(PostgresCopyUtilsTest, PostgresCopyReadCustomRecord) {
  ArrowBufferView data;
  data.data.as_uint8 = kTestPgCopyCustomRecord;
  data.size_bytes = sizeof(kTestPgCopyCustomRecord);

  auto col_type = PostgresType(PostgresTypeId::kRecord);
  col_type.AppendChild("nested1", PostgresType(PostgresTypeId::kInt4));
  col_type.AppendChild("nested2", PostgresType(PostgresTypeId::kFloat8));
  PostgresType input_type(PostgresTypeId::kRecord);
  input_type.AppendChild("col", col_type);

  PostgresCopyStreamTester tester;
  ASSERT_EQ(tester.Init(input_type), NANOARROW_OK);
  ASSERT_EQ(tester.ReadAll(&data), ENODATA);
  ASSERT_EQ(data.data.as_uint8 - kTestPgCopyCustomRecord,
            sizeof(kTestPgCopyCustomRecord));
  ASSERT_EQ(data.size_bytes, 0);

  nanoarrow::UniqueArray array;
  ASSERT_EQ(tester.GetArray(array.get()), NANOARROW_OK);
  ASSERT_EQ(array->length, 3);
  ASSERT_EQ(array->n_children, 1);
  ASSERT_EQ(array->children[0]->n_children, 2);
  ASSERT_EQ(array->children[0]->children[0]->length, 3);
  ASSERT_EQ(array->children[0]->children[1]->length, 3);

  auto validity = reinterpret_cast<const uint8_t*>(array->children[0]->buffers[0]);
  auto data_buffer1 =
      reinterpret_cast<const int32_t*>(array->children[0]->children[0]->buffers[1]);
  auto data_buffer2 =
      reinterpret_cast<const double*>(array->children[0]->children[1]->buffers[1]);

  ASSERT_TRUE(ArrowBitGet(validity, 0));
  ASSERT_TRUE(ArrowBitGet(validity, 1));
  ASSERT_FALSE(ArrowBitGet(validity, 2));

  ASSERT_EQ(data_buffer1[0], 123);
  ASSERT_EQ(data_buffer1[1], 12);
  ASSERT_EQ(data_buffer1[2], 0);

  ASSERT_DOUBLE_EQ(data_buffer2[0], 456.789);
  ASSERT_DOUBLE_EQ(data_buffer2[1], 345.678);
  ASSERT_DOUBLE_EQ(data_buffer2[2], 0);
}

}  // namespace adbcpq

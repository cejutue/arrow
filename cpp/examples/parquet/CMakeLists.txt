# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

add_executable(parquet-low-level-example low-level-api/reader-writer.cc)
add_executable(parquet-low-level-example2 low-level-api/reader-writer2.cc)
add_executable(parquet-arrow-example parquet-arrow/reader-writer.cc)
add_executable(parquet-stream-api-example parquet-stream-api/stream-reader-writer.cc)
target_include_directories(parquet-low-level-example PRIVATE low-level-api/)
target_include_directories(parquet-low-level-example2 PRIVATE low-level-api/)

if (PARQUET_REQUIRE_ENCRYPTION)
  add_executable(parquet-encryption-example low-level-api/encryption-reader-writer.cc)
  add_executable(parquet-encryption-example-all-crypto-options low-level-api/encryption-reader-writer-all-crypto-options.cc)
  target_include_directories(parquet-encryption-example PRIVATE low-level-api/)
  target_include_directories(parquet-encryption-example-all-crypto-options PRIVATE low-level-api/)
  target_link_libraries(parquet-encryption-example parquet_static)
  target_link_libraries(parquet-encryption-example-all-crypto-options parquet_static)
endif()

# Prefer shared linkage but use static if shared build is deactivated
if (ARROW_BUILD_SHARED)
  set(PARQUET_EXAMPLE_LINK_LIBS parquet_shared)
else()
  set(PARQUET_EXAMPLE_LINK_LIBS parquet_static)
endif()

target_link_libraries(parquet-arrow-example ${PARQUET_EXAMPLE_LINK_LIBS})
target_link_libraries(parquet-low-level-example ${PARQUET_EXAMPLE_LINK_LIBS})
target_link_libraries(parquet-low-level-example2 ${PARQUET_EXAMPLE_LINK_LIBS})
target_link_libraries(parquet-stream-api-example ${PARQUET_EXAMPLE_LINK_LIBS})

add_dependencies(parquet
  parquet-low-level-example
  parquet-low-level-example2
  parquet-arrow-example
  parquet-stream-api-example)

if (PARQUET_REQUIRE_ENCRYPTION)
  add_dependencies(parquet
    parquet-encryption-example
    parquet-encryption-example-all-crypto-options)
endif()

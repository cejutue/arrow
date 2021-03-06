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

to_arrow <- function(x) {
  UseMethod("to_arrow")
}

to_arrow.RecordBatch <- function(x) x
to_arrow.Table <- function(x) x

# splice the data frame as arguments of Table$create()
# see ?rlang::list2()
to_arrow.data.frame <- function(x) Table$create(!!!x)

#' Write Arrow formatted data
#'
#' @param x an [arrow::Table][Table], an [arrow::RecordBatch][RecordBatch] or a data.frame
#'
#' @param sink where to serialize to
#'
#' - A [arrow::RecordBatchWriter][RecordBatchWriter]: the `$write()`
#'      of `x` is used. The stream is left open. This uses the streaming format
#'      or the binary file format depending on the type of the writer.
#'
#' - A string file path: `x` is serialized with
#'      a [arrow::RecordBatchFileWriter][RecordBatchFileWriter], i.e.
#'      using the binary file format.
#'
#' - A raw vector: typically of length zero (its data is ignored, and only used for
#'      dispatch). `x` is serialized using the streaming format, i.e. using the
#'      [arrow::RecordBatchStreamWriter][RecordBatchStreamWriter]
#'
#' @param ... extra parameters, currently ignored
#'
#' `write_arrow` is a convenience function, the classes [arrow::RecordBatchFileWriter][RecordBatchFileWriter]
#' and [arrow::RecordBatchStreamWriter][RecordBatchStreamWriter] can be used for more flexibility.
#'
#' @export
write_arrow <- function(x, sink, ...) {
  UseMethod("write_arrow", sink)
}

#' @export
write_arrow.RecordBatchWriter <- function(x, sink, ...){
  sink$write(x)
}

#' @export
write_arrow.character <- function(x, sink, ...) {
  assert_that(length(sink) == 1L)
  x <- to_arrow(x)
  file_stream <- FileOutputStream$create(sink)
  on.exit(file_stream$close())
  file_writer <- RecordBatchFileWriter$create(file_stream, x$schema)
  on.exit({
    # Re-set the exit code to close both connections, LIFO
    file_writer$close()
    file_stream$close()
  })
  # Available on R >= 3.5
  # on.exit(file_writer$close(), add = TRUE, after = FALSE)
  write_arrow(x, file_writer, ...)
}

#' @export
write_arrow.raw <- function(x, sink, ...) {
  x <- to_arrow(x)
  schema <- x$schema

  # how many bytes do we need
  mock_stream <- MockOutputStream$create()
  writer <- RecordBatchStreamWriter$create(mock_stream, schema)
  writer$write(x)
  writer$close()
  n <- mock_stream$GetExtentBytesWritten()

  # now that we know the size, stream in a buffer backed by an R raw vector
  bytes <- raw(n)
  buffer_writer <- FixedSizeBufferWriter$create(buffer(bytes))
  writer <- RecordBatchStreamWriter$create(buffer_writer, schema)
  writer$write(x)
  writer$close()

  bytes
}

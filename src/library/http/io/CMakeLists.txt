add_library(cpp-http-io)

target_link_libraries(cpp-http-io PUBLIC
  yutil
  library-cpp-blockcodecs
  cpp-streams-brotli
  cpp-streams-bzip2
  cpp-streams-lzma
  cpp-string_utils-stream
  cpp-string_utils-misc
)

target_sources(cpp-http-io PRIVATE
  ${YDB_SDK_SOURCE_DIR}/src/library/http/io/chunk.cpp
  ${YDB_SDK_SOURCE_DIR}/src/library/http/io/compression.cpp
  ${YDB_SDK_SOURCE_DIR}/src/library/http/io/headers.cpp
  ${YDB_SDK_SOURCE_DIR}/src/library/http/io/stream.cpp
)



add_library(cpp-openssl-io)

target_link_libraries(cpp-openssl-io PUBLIC
  yutil
  OpenSSL::SSL
  cpp-openssl-init
  cpp-openssl-method
)

target_sources(cpp-openssl-io PRIVATE
  ${YDB_SDK_SOURCE_DIR}/src/library/openssl/io/stream.cpp
)



add_library(service-pages-tablesorter INTERFACE)

target_link_libraries(service-pages-tablesorter INTERFACE
  yutil
  library-cpp-resource
  cpp-monlib-dynamic_counters
)

add_global_library_for(service-pages-tablesorter.global service-pages-tablesorter)

target_link_libraries(service-pages-tablesorter.global PUBLIC
  yutil
  library-cpp-resource
  cpp-monlib-dynamic_counters
)

target_sources(service-pages-tablesorter.global PRIVATE
  ${YDB_SDK_BINARY_DIR}/src/library/monlib/service/pages/tablesorter/0c255888afcc3e2adc5fd360ee152bac.cpp
)

resources(service-pages-tablesorter.global
  ${YDB_SDK_BINARY_DIR}/src/library/monlib/service/pages/tablesorter/0c255888afcc3e2adc5fd360ee152bac.cpp
  INPUTS
  ${YDB_SDK_SOURCE_DIR}/src/library/monlib/service/pages/tablesorter/resources/jquery.tablesorter.css
  ${YDB_SDK_SOURCE_DIR}/src/library/monlib/service/pages/tablesorter/resources/jquery.tablesorter.js
  KEYS
  jquery.tablesorter.css
  jquery.tablesorter.js
)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(service-pages-tablesorter INTERFACE
    
  )
  target_link_libraries(service-pages-tablesorter.global PUBLIC
    
  )
endif()

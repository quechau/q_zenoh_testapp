cmake_minimum_required(VERSION 3.16)
project(q_zenoh_testapp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Dependencies live in third_party/prefix, built by scripts/bootstrap.sh. They are not
# installed system-wide: zenoh-pico must be compiled with Z_FEATURE_LINK_TLS=1, which the
# packaged builds do not do, and Mbed TLS headers are usually absent.
set(QZ_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/third_party/prefix")
if(NOT EXISTS "${QZ_PREFIX}/lib/libzenohpico.so")
  message(FATAL_ERROR "Dependencies missing. Run ./scripts/bootstrap.sh first.")
endif()

# The JSON renderer reads the contract schema from generated tables rather than inferring it.
# Generated at configure time and never committed, so the tables cannot drift from proto/.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(QZ_GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${QZ_GEN}")
file(GLOB QZ_PROTOS "${CMAKE_CURRENT_SOURCE_DIR}/proto/*.proto"
                    "${CMAKE_CURRENT_SOURCE_DIR}/proto/services/*.proto")
add_custom_command(
  OUTPUT "${QZ_GEN}/proto_tables.c" "${QZ_GEN}/proto_tables.h"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen-proto-tables.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/proto"
          "${QZ_GEN}/proto_tables.c" "${QZ_GEN}/proto_tables.h"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen-proto-tables.py" ${QZ_PROTOS}
  COMMENT "Generating protobuf tables from proto/"
  VERBATIM
)

add_executable(q_zenoh_testapp
  "${QZ_GEN}/proto_tables.c"
  src/main.c
  src/session.c
  src/proto.c
  src/util.c
  src/ca.c
  src/mdns.c
  src/logfilter.c
  src/json.c
  src/build.c
)

# zenoh-pico picks its platform layer from a compile definition; without it every header
# stops at #error "Unknown platform".
target_compile_definitions(q_zenoh_testapp PRIVATE ZENOH_LINUX)
target_include_directories(q_zenoh_testapp PRIVATE "${QZ_PREFIX}/include" "${QZ_GEN}")
target_link_directories(q_zenoh_testapp PRIVATE "${QZ_PREFIX}/lib")
target_link_libraries(q_zenoh_testapp PRIVATE
  zenohpico mbedtls mbedx509 mbedcrypto pthread
)
target_compile_options(q_zenoh_testapp PRIVATE -Wall -Wextra -Wno-unused-parameter)
# Find the private prefix at run time without LD_LIBRARY_PATH.
set_target_properties(q_zenoh_testapp PROPERTIES
  BUILD_RPATH "${QZ_PREFIX}/lib"
  INSTALL_RPATH "${QZ_PREFIX}/lib"
)

include(FetchContent)
file(READ "${CMAKE_CURRENT_LIST_DIR}/../../runtime/dependencies.lock.json" dependency_lock)
set(GITSTORED_SOURCE_CACHE "" CACHE PATH "Optional directory of pinned source archives (hash checked)")
foreach(name IN ITEMS jansson sqlite)
  foreach(field IN ITEMS url sha256 archive)
    string(JSON ${name}_${field} GET "${dependency_lock}" dependencies ${name} ${field})
  endforeach()
  if(GITSTORED_SOURCE_CACHE AND EXISTS "${GITSTORED_SOURCE_CACHE}/${${name}_archive}")
    set(${name}_url "${GITSTORED_SOURCE_CACHE}/${${name}_archive}")
  endif()
endforeach()
set(JANSSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(JANSSON_WITHOUT_TESTS ON CACHE BOOL "" FORCE)
set(JANSSON_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JANSSON_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(JANSSON_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(jansson URL "${jansson_url}" URL_HASH "SHA256=${jansson_sha256}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(jansson)
# Jansson and libcrun's json-c expose overlapping C names. Keep the private
# static Jansson symbols hidden so ELF cannot interpose them into json-c.
set_target_properties(jansson PROPERTIES C_VISIBILITY_PRESET hidden)
# Hidden visibility prevents dynamic interposition, but static json-c also
# defines these names. Rename the private Jansson API at compile time for both
# its implementation and consumers; never allow duplicate-definition linking.
target_compile_definitions(jansson PUBLIC
  json_object_get=gitstored_jansson_object_get
  json_object_iter_next=gitstored_jansson_object_iter_next)
include("${CMAKE_CURRENT_LIST_DIR}/PatchJansson.cmake")
# Shadow the upstream-generated configuration with a private copy. Editing the
# upstream output in place makes configure_file restore 2048 on every configure,
# which then forces all consumers to rebuild when we change it back to 33.
set(config "${jansson_BINARY_DIR}/include/jansson_config.h")
file(READ "${config}" original_config)
if(NOT original_config MATCHES "#define JSON_PARSER_MAX_DEPTH 2048")
  message(FATAL_ERROR "Pinned Jansson depth configuration changed; audit the private override")
endif()
string(REPLACE "#define JSON_PARSER_MAX_DEPTH 2048" "#define JSON_PARSER_MAX_DEPTH 33" config_content "${original_config}")
set(private_config "${CMAKE_CURRENT_BINARY_DIR}/jansson-private-config")
file(MAKE_DIRECTORY "${private_config}")
file(CONFIGURE OUTPUT "${private_config}/jansson_config.h" CONTENT "${config_content}" @ONLY)
# The generated public header uses a quoted include, so keep it beside the
# private config too; otherwise its adjacent upstream config wins lookup.
configure_file("${jansson_BINARY_DIR}/include/jansson.h" "${private_config}/jansson.h" COPYONLY)
target_include_directories(jansson BEFORE PUBLIC "$<BUILD_INTERFACE:${private_config}>")
FetchContent_Declare(sqlite_source URL "${sqlite_url}" URL_HASH "SHA256=${sqlite_sha256}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sqlite_source)
add_library(gitstored_sqlite STATIC "${sqlite_source_SOURCE_DIR}/sqlite3.c")
target_include_directories(gitstored_sqlite PUBLIC "${sqlite_source_SOURCE_DIR}")
target_compile_definitions(gitstored_sqlite PRIVATE SQLITE_THREADSAFE=0 SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_DQS=0 SQLITE_DEFAULT_MEMSTATUS=0)
# Instrument dependency allocations too, but don't apply project -Werror to third-party code.
if(GITSTORED_SANITIZERS)
  foreach(target IN ITEMS jansson gitstored_sqlite)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  endforeach()
endif()

option(GITSTORED_LIBCRUN "Link pinned native libcrun code (Linux development backend)" OFF)
if(GITSTORED_LIBCRUN)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Native libcrun currently requires Linux; control-only builds remain available")
  endif()
  include(ExternalProject)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GITSTORED_CRUN_SYSTEM REQUIRED IMPORTED_TARGET
    libsystemd>=255 libseccomp>=2.5.5 libcap json-c>=0.17)
  find_program(GITSTORED_MAKE NAMES gmake make REQUIRED)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/experiments/libcrun/source-lock.json" crun_lock)
  string(JSON crun_url GET "${crun_lock}" url)
  string(JSON crun_sha256 GET "${crun_lock}" sha256)
  string(JSON crun_version GET "${crun_lock}" version)
  set(crun_archive "crun-${crun_version}.tar.gz")
  set(GITSTORED_LIBCRUN_ARCHIVE "" CACHE FILEPATH "Optional pinned libcrun archive (hash checked)")
  if(GITSTORED_LIBCRUN_ARCHIVE)
    set(crun_url "${GITSTORED_LIBCRUN_ARCHIVE}")
  elseif(GITSTORED_SOURCE_CACHE AND EXISTS "${GITSTORED_SOURCE_CACHE}/${crun_archive}")
    set(crun_url "${GITSTORED_SOURCE_CACHE}/${crun_archive}")
  endif()
  set(crun_source "${CMAKE_CURRENT_BINARY_DIR}/_deps/libcrun-src")
  set(crun_build "${CMAKE_CURRENT_BINARY_DIR}/_deps/libcrun-build")
  set(crun_static "${crun_build}/.libs/libcrun.a")
  set(crun_cflags "-O2 -g -fPIC ${CMAKE_C_FLAGS}")
  set(crun_ldflags "${CMAKE_EXE_LINKER_FLAGS}")
  if(GITSTORED_SANITIZERS)
    string(APPEND crun_cflags " -O1 -fsanitize=address,undefined -fno-omit-frame-pointer")
    string(APPEND crun_ldflags " -fsanitize=address,undefined")
  endif()
  file(MAKE_DIRECTORY "${crun_source}/src" "${crun_source}/libocispec/src" "${crun_build}")
  ExternalProject_Add(gitstored_libcrun_build
    URL "${crun_url}" URL_HASH "SHA256=${crun_sha256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR "${crun_source}" BINARY_DIR "${crun_build}"
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env
      "CC=${CMAKE_C_COMPILER}" "CFLAGS=${crun_cflags}" "LDFLAGS=${crun_ldflags}"
      <SOURCE_DIR>/configure --enable-libcrun --disable-crun --disable-dynload-libcrun
      --enable-static --disable-shared --enable-caps --enable-seccomp --enable-systemd
      --disable-dl --disable-criu --disable-bpf --enable-embedded-blake3
      --without-mono --without-wasmer --without-wasmtime --without-wasmedge --without-wamr
      --without-libkrun --without-spin --without-python-bindings --without-lua-bindings
    BUILD_COMMAND "${GITSTORED_MAKE}" -j2
    BUILD_BYPRODUCTS "${crun_static}"
    INSTALL_COMMAND ""
    LOG_CONFIGURE ON LOG_BUILD ON LOG_OUTPUT_ON_FAILURE ON)
  add_library(gitstored_libcrun STATIC IMPORTED)
  set_target_properties(gitstored_libcrun PROPERTIES IMPORTED_LOCATION "${crun_static}"
    INTERFACE_INCLUDE_DIRECTORIES "${crun_source}/src;${crun_source}/libocispec/src;${crun_build}")
  add_dependencies(gitstored_libcrun gitstored_libcrun_build)
  add_dependencies(gitstored_core gitstored_libcrun_build)
  target_include_directories(gitstored_core SYSTEM PRIVATE
    "${crun_source}/src" "${crun_source}/libocispec/src" "${crun_build}")
  target_link_libraries(gitstored_libcrun INTERFACE PkgConfig::GITSTORED_CRUN_SYSTEM m)
  target_link_libraries(gitstored_core PRIVATE gitstored_libcrun)
  target_compile_definitions(gitstored_core PRIVATE GITSTORED_LIBCRUN=1)
  target_sources(gitstored_core PRIVATE src/native_abi.c)
  if(GITSTORED_TESTING)
    add_executable(scylla_native_probe "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/experiments/scylladb/native_probe.c" "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/experiments/scylladb/native_diagnostic.c")
    target_include_directories(scylla_native_probe PRIVATE src)
    target_link_libraries(scylla_native_probe PRIVATE gitstored_core gitstored_libcrun PkgConfig::GITSTORED_CRUN_SYSTEM gitstored_options)
    add_executable(native_adapter_probe tests/native_adapter_probe.c)
    target_include_directories(native_adapter_probe PRIVATE src)
    target_link_libraries(native_adapter_probe PRIVATE gitstored_core gitstored_options)
    # Explicit developer probes only, excluded from production compilation and
    # never registered as a daemon backend or default CTest host mutation.
    add_executable(native_library_probe "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/experiments/libcrun/probe.c")
    target_compile_definitions(native_library_probe PRIVATE GSTD_PROBE_CGROUPS=1)
    target_link_libraries(native_library_probe PRIVATE gitstored_libcrun
      PkgConfig::GITSTORED_CRUN_SYSTEM gitstored_options m)
    add_executable(native_library_payload "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/experiments/libcrun/payload.c")
    # The namespace payload stays a tiny plain static ELF; instrument the caller
    # and native library, since ASan cannot be linked into this static payload.
    target_compile_options(native_library_payload PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_options(native_library_payload PRIVATE -static)
    add_executable(native_registered_payload "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/experiments/libcrun/payload.c")
    target_compile_definitions(native_registered_payload PRIVATE GSTD_REGISTERED_PAYLOAD=1)
    if(GITSTORED_SANITIZERS)
      target_compile_definitions(native_registered_payload PRIVATE GITSTORED_INSTRUMENTED=1)
    endif()
    target_compile_options(native_registered_payload PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_options(native_registered_payload PRIVATE -static)
  endif()
  message(STATUS "Pinned static libcrun selected; no crun executable or public lifecycle capability")
endif()

option(GITSTORED_SLIRP "Link pinned libslirp code (Linux development network backend)" OFF)
if(GITSTORED_SLIRP)
  if(NOT GITSTORED_LIBCRUN)
    message(FATAL_ERROR "The network worker requires libcrun's maintained seccomp compiler")
  endif()
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Native libslirp currently requires Linux; control-only builds remain available")
  endif()
  include(ExternalProject)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GITSTORED_GLIB REQUIRED IMPORTED_TARGET glib-2.0)
  find_program(GITSTORED_MESON meson REQUIRED)
  find_program(GITSTORED_NINJA ninja REQUIRED)
  set(slirp_record "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/build-inputs/libslirp")
  file(READ "${slirp_record}/source-lock.json" slirp_lock)
  set(slirp_input "")
  set(GITSTORED_LIBSLIRP_ARCHIVE "" CACHE FILEPATH "Optional libslirp archive (normalized source hash checked)")
  if(GITSTORED_LIBSLIRP_ARCHIVE)
    set(slirp_input --input "${GITSTORED_LIBSLIRP_ARCHIVE}")
  elseif(GITSTORED_SOURCE_CACHE AND EXISTS "${GITSTORED_SOURCE_CACHE}/libslirp-4.9.7.tar.gz")
    set(slirp_input --input "${GITSTORED_SOURCE_CACHE}/libslirp-4.9.7.tar.gz")
  endif()
  set(slirp_source "${CMAKE_CURRENT_BINARY_DIR}/_deps/libslirp-src")
  set(slirp_build "${CMAKE_CURRENT_BINARY_DIR}/_deps/libslirp-build")
  set(slirp_static "${slirp_build}/libslirp.a")
  set(slirp_sanitize "none")
  if(GITSTORED_SANITIZERS)
    set(slirp_sanitize "address,undefined")
  endif()
  file(MAKE_DIRECTORY "${slirp_source}/src" "${slirp_build}")
  ExternalProject_Add(gitstored_slirp_build
    DOWNLOAD_COMMAND "${Python3_EXECUTABLE}" "${slirp_record}/fetch-source.py"
      ${slirp_input} --output "${CMAKE_CURRENT_BINARY_DIR}/libslirp-verified.tar"
      # Give verified files fresh worktree mtimes so an incremental build never
      # reuses older objects after a source-lock update (archive mtimes are zero).
      COMMAND "${CMAKE_COMMAND}" -E chdir <SOURCE_DIR>
        "${CMAKE_COMMAND}" -E tar xf "${CMAKE_CURRENT_BINARY_DIR}/libslirp-verified.tar" --touch
    SOURCE_DIR "${slirp_source}" BINARY_DIR "${slirp_build}"
    UPDATE_COMMAND ""
    PATCH_COMMAND "${Python3_EXECUTABLE}" "${slirp_record}/apply-source-patch.py" <SOURCE_DIR>
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env "CC=${CMAKE_C_COMPILER}" "CFLAGS=${CMAKE_C_FLAGS}"
      "${GITSTORED_MESON}" setup <BINARY_DIR> <SOURCE_DIR>
      --buildtype=release --default-library=static -Dwerror=true
      -Dversion_suffix=-gstd-queueview2 "-Db_sanitize=${slirp_sanitize}" -Db_lundef=false
    BUILD_COMMAND "${GITSTORED_NINJA}" -C <BINARY_DIR> libslirp.a
    BUILD_BYPRODUCTS "${slirp_static}" INSTALL_COMMAND ""
    LOG_CONFIGURE ON LOG_BUILD ON LOG_OUTPUT_ON_FAILURE ON)
  ExternalProject_Add_StepDependencies(gitstored_slirp_build download
    "${slirp_record}/fetch-source.py" "${slirp_record}/source-lock.json")
  add_library(gitstored_slirp STATIC IMPORTED)
  set_target_properties(gitstored_slirp PROPERTIES IMPORTED_LOCATION "${slirp_static}"
    INTERFACE_INCLUDE_DIRECTORIES "${slirp_source}/src;${slirp_build}")
  add_dependencies(gitstored_slirp gitstored_slirp_build)
  add_dependencies(gitstored_core gitstored_slirp_build)
  target_include_directories(gitstored_core SYSTEM PRIVATE "${slirp_source}/src" "${slirp_build}")
  target_compile_definitions(gitstored_core PRIVATE GITSTORED_SLIRP=1)
  target_link_libraries(gitstored_core PRIVATE gitstored_slirp PkgConfig::GITSTORED_GLIB resolv)
  if(GITSTORED_TESTING)
    add_test(NAME pinned_slirp_source_verification
      COMMAND "${Python3_EXECUTABLE}" "${slirp_record}/test_source.py")
    add_executable(network_worker_probe tests/network_worker_probe.c)
    target_include_directories(network_worker_probe PRIVATE src)
    target_link_libraries(network_worker_probe PRIVATE gitstored_core gitstored_options resolv)
    add_executable(oidc_network_worker_probe tests/oidc_network_worker_probe.c)
    target_include_directories(oidc_network_worker_probe PRIVATE src)
    target_link_libraries(oidc_network_worker_probe PRIVATE gitstored_core gitstored_options)
    add_executable(oidc_network_probe tests/oidc_network_probe.c)
    target_include_directories(oidc_network_probe PRIVATE src)
    target_link_libraries(oidc_network_probe PRIVATE gitstored_core gitstored_options)
    add_executable(network_engine_probe tests/network_engine_probe.c)
    target_include_directories(network_engine_probe PRIVATE src)
    target_link_libraries(network_engine_probe PRIVATE gitstored_core gitstored_options)
    add_executable(native_network_probe "${slirp_record}/probe.c")
    target_link_libraries(native_network_probe PRIVATE gitstored_slirp PkgConfig::GITSTORED_GLIB
      gitstored_options resolv)
  endif()
  message(STATUS "Pinned static libslirp selected; no network helper executable or public capability")
endif()

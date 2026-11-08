option(GITSTORED_IMAGE_SDK "Link selected ordinary Rust image code library (Linux worker only)" OFF)
if(GITSTORED_IMAGE_SDK)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Image worker SDK currently requires Linux; macOS control build remains available")
  endif()
  find_program(GITSTORED_CARGO cargo REQUIRED)
  find_package(OpenSSL 3 REQUIRED COMPONENTS SSL Crypto)
  set(image_manifest "${CMAKE_CURRENT_SOURCE_DIR}/image-backend/Cargo.toml")
  set(image_archive "${CMAKE_CURRENT_BINARY_DIR}/image-sdk/release/libgitstored_image.a")
  set(image_features)
  if(GITSTORED_SANITIZERS)
    list(APPEND image_features --features instrumented-host)
  endif()
  file(GLOB image_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/image-backend/src/*.rs")
  # A Rust staticlib does not propagate native dynamic link flags to its C host.
  # Always embed the locked compression crate sources, independent of whether
  # the build host happens to have liblzma/zlib development headers installed.
  add_custom_command(OUTPUT "${image_archive}"
    COMMAND "${CMAKE_COMMAND}" -E env CARGO_BUILD_JOBS=2 LZMA_API_STATIC=1 LIBZ_SYS_STATIC=1 "${GITSTORED_CARGO}" build
      --manifest-path "${image_manifest}" --locked --release ${image_features}
      --target-dir "${CMAKE_CURRENT_BINARY_DIR}/image-sdk"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS ${image_sources} "${image_manifest}" "${CMAKE_CURRENT_SOURCE_DIR}/image-backend/Cargo.lock"
      "${CMAKE_CURRENT_SOURCE_DIR}/../rust-toolchain.toml"
    COMMENT "Building pinned image code library (no runtime executables)"
    JOB_SERVER_AWARE TRUE
    VERBATIM)
  add_custom_target(gitstored_image_build DEPENDS "${image_archive}")
  add_library(gitstored_image_sdk STATIC IMPORTED)
  set_target_properties(gitstored_image_sdk PROPERTIES IMPORTED_LOCATION "${image_archive}")
  add_dependencies(gitstored_image_sdk gitstored_image_build)
  find_package(Threads REQUIRED)
  target_link_libraries(gitstored_image_sdk INTERFACE OpenSSL::SSL OpenSSL::Crypto
    Threads::Threads ${CMAKE_DL_LIBS} m)
  target_link_libraries(gitstored_core PRIVATE gitstored_image_sdk)
  target_link_options(gitstored_core INTERFACE "-Wl,--gc-sections")
  target_compile_definitions(gitstored_core PRIVATE GITSTORED_IMAGE_SDK=1)
  message(STATUS "Image SDK linked for native worker only; runtime libraries and host prerequisites are checked independently. Rust SDK is not ASan-instrumented.")
endif()

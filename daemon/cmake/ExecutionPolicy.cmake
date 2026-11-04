# Build data only. Distribution users need neither Python nor policy files.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(policy_root "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/profiles/seccomp")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${policy_root}/normalize.py" --check
  RESULT_VARIABLE policy_check ERROR_VARIABLE policy_error)
if(NOT policy_check EQUAL 0)
  message(FATAL_ERROR "Pinned/generated execution policy check failed: ${policy_error}")
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(native_policy "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  set(native_policy "amd64")
else()
  message(FATAL_ERROR "Execution policy data currently supports native amd64/arm64 only")
endif()
set(policy_path "${policy_root}/${native_policy}.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${policy_path}" "${policy_root}/normalize.py" "${policy_root}/source-lock.json"
  "${policy_root}/moby-default.json" "${policy_root}/LICENSE.moby")
file(READ "${policy_path}" policy_hex HEX)
file(SHA256 "${policy_path}" policy_digest)
string(REGEX REPLACE "(..)" "0x\\1,\n" policy_bytes "${policy_hex}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/execution_policy.inc"
  CONTENT "static const unsigned char execution_policy[] = {${policy_bytes}0};\nstatic const char execution_policy_digest[] = \"sha256:${policy_digest}\";\n")

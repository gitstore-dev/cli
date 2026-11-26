# Bounded, hash-verified download of a pinned source archive.
#
# ExternalProject and FetchContent retry a failed download only a few times in quick
# succession, so a short upstream outage (for example HTTP 504 from github.com) fails the
# whole build. This prefetches over HTTPS with exponential backoff first. The pinned
# SHA-256 is verified after every successful transfer, so a retry can never accept
# different bytes, and a hash mismatch is fatal rather than retried. On success <out_var> is set to the local
# file; otherwise it is left unchanged and the normal download still runs as before.
set(GITSTORED_PREFETCH_ATTEMPTS 5 CACHE STRING "Download attempts for pinned source archives")
# The retry loop, separate from the HTTPS-only policy check so it can be tested locally.
function(gitstored_prefetch_download url sha256 filename out_var)
  set(dest "${CMAKE_BINARY_DIR}/_prefetch/${filename}")
  if(EXISTS "${dest}")
    file(SHA256 "${dest}" have)
    if(have STREQUAL "${sha256}")
      set(${out_var} "${dest}" PARENT_SCOPE)
      return()
    endif()
    file(REMOVE "${dest}")
  endif()
  set(delay 2)
  foreach(attempt RANGE 1 ${GITSTORED_PREFETCH_ATTEMPTS})
    # No EXPECTED_HASH here: with it, a failed transfer (such as HTTP 503) is a fatal CMake
    # error instead of a status the loop can retry. The hash is verified below.
    file(DOWNLOAD "${url}" "${dest}" TLS_VERIFY ON TIMEOUT 120 INACTIVITY_TIMEOUT 30 STATUS status)
    list(GET status 0 code)
    list(GET status 1 reason)
    if(code EQUAL 0)
      file(SHA256 "${dest}" have)
      if(NOT have STREQUAL "${sha256}")
        file(REMOVE "${dest}")
        message(FATAL_ERROR "Pinned archive ${filename} does not match its SHA-256; refusing to retry")
      endif()
      set(${out_var} "${dest}" PARENT_SCOPE)
      return()
    endif()
    file(REMOVE "${dest}")
    message(STATUS "Download of ${filename} failed (${reason}), attempt ${attempt} of ${GITSTORED_PREFETCH_ATTEMPTS}")
    if(attempt LESS ${GITSTORED_PREFETCH_ATTEMPTS})
      execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep ${delay})
      math(EXPR delay "${delay} * 2")
    endif()
  endforeach()
  message(STATUS "Prefetch of ${filename} did not succeed; falling back to the standard download")
endfunction()

function(gitstored_prefetch_source url sha256 filename out_var)
  if(NOT url MATCHES "^https://")
    return() # a local archive or source-cache path needs no download
  endif()
  gitstored_prefetch_download("${url}" "${sha256}" "${filename}" result)
  if(result)
    set(${out_var} "${result}" PARENT_SCOPE)
  endif()
endfunction()

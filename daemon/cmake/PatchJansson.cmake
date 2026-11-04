# Jansson's public length-aware object APIs support embedded NUL keys, but its
# decoder rejects them. Protocol keys are arbitrary UTF-8 strings; retain those
# APIs and duplicate checks rather than weakening the existing wire contract.
set(load_source "${jansson_SOURCE_DIR}/src/load.c")
file(READ "${load_source}" original_load)
set(nul_key_guard [=[        if (memchr(key, '\0', len)) {
            jsonp_free(key);
            error_set(error, lex, json_error_null_byte_in_key,
                      "NUL byte in object key not supported");
            goto error;
        }
]=])
string(FIND "${original_load}" "${nul_key_guard}" guard_position)
if(NOT guard_position EQUAL -1)
  string(REPLACE "${nul_key_guard}" "        /* GSTD: length-aware keys may contain NUL. */\n" patched_load "${original_load}")
  file(WRITE "${load_source}" "${patched_load}")
elseif(NOT original_load MATCHES "GSTD: length-aware keys may contain NUL")
  message(FATAL_ERROR "Pinned Jansson key guard changed; audit protocol patch before building")
endif()

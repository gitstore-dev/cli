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
# Pinned decoder ignores saved-token buffer growth failures. Under a bounded
# allocator, lex_unget_unsave can then pop a different byte and assert. Preserve
# stream error state and return ERROR immediately instead of returning an unsaved
# byte. All parser entry points still use Jansson's normal cleanup/error paths.
file(READ "${load_source}" original_load)
set(lex_save_before [=[static void lex_save(lex_t *lex, int c) { strbuffer_append_byte(&lex->saved_text, c); }]=])
set(lex_save_after [=[static void lex_save(lex_t *lex, int c) {
    /* GSTD: failed token growth is a sticky lexer error. */
    if (strbuffer_append_byte(&lex->saved_text, c))
        lex->stream.state = STREAM_STATE_ERROR;
}]=])
set(lex_get_before [=[    if (c != STREAM_STATE_EOF && c != STREAM_STATE_ERROR)
        lex_save(lex, c);
    return c;
}]=])
set(lex_get_after [=[    if (c != STREAM_STATE_EOF && c != STREAM_STATE_ERROR)
        lex_save(lex, c);
    return lex->stream.state == STREAM_STATE_ERROR ? STREAM_STATE_ERROR : c;
}]=])
string(FIND "${original_load}" "${lex_save_before}" lex_position)
if(NOT lex_position EQUAL -1)
  string(FIND "${original_load}" "${lex_get_before}" get_position)
  if(get_position EQUAL -1)
    message(FATAL_ERROR "Pinned Jansson lexer changed; audit allocation-failure patch")
  endif()
  string(REPLACE "${lex_save_before}" "${lex_save_after}" patched_load "${original_load}")
  string(REPLACE "${lex_get_before}" "${lex_get_after}" patched_load "${patched_load}")
  file(WRITE "${load_source}" "${patched_load}")
elseif(NOT original_load MATCHES "GSTD: failed token growth is a sticky lexer error")
  message(FATAL_ERROR "Pinned Jansson lexer changed; audit allocation-failure patch")
endif()

#!/bin/sh -e
# Host tests for webserver.c's network-facing untrusted-input parsers.
#
# webserver.c cannot be compiled whole on the host (lwIP raw-TCP, sdio,
# FatFs and framebuffer entanglement), so extract.awk pulls the parser
# functions VERBATIM out of a copy of webserver.c in a mktemp tree into
# .inc files that the test translation units include.  The extraction
# fails loudly if any expected function / type / #define is no longer
# found, so the tests always run against the real code or not at all.
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC_DIR:-$HERE/../..}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

cp "$SRC"/wifi/webserver.c "$SRC"/wifi/md5.h "$B/"
cp "$HERE"/test_parsers.c "$HERE"/test_chunked.c "$HERE"/fuzz_parsers.c "$B/"
cp -r "$HERE"/stubs/. "$B/"

# The pure text parsers: HTTP request line / header block, multipart
# Content-Type fields, URL decoding, SD path normalisation + traversal
# checks, Digest-auth fields, WebDAV Destination and RFC1123 dates.
PARSER_FNS="ws_lc,ws_stricmp,ws_prefix,ws_prefix_ci,ws_prefix_ci_str,\
ws_strcasestr,ws_hexval,ws_url_decode,ws_memfind,ws_find_header_end,\
ws_basename,ws_parse_request_line,ws_find_header,ws_extract_boundary,\
ws_extract_filename,ws_path_is_safe,ws_normalize_path,ws_parent_path,\
ws_is_root,ws_digest_field,ws_hex_eq_ci,ws_digest_uri_matches,\
dav_url_to_sdpath,dav_destination_sdpath,dav_memfind,dav_parse_http_date,\
ws_parse_range,ws_query_param"

awk -v defs="WS_HEADER_MAX,WS_FILE_CHUNK,WS_BOUNDARY_MAX,WS_UPLOAD_HEAD_MAX,WS_PATH_MAX,WS_DRAIN_MAX_BYTES" \
    -f "$HERE/extract.awk" "$B/webserver.c" > "$B/ws_defines.inc"
# types= rides along so ws_range_result_t lands ahead of ws_parse_range
# (the awk emits in file order, and the typedef precedes the function).
awk -v fns="$PARSER_FNS" -v types="ws_range_result_t" \
    -f "$HERE/extract.awk" "$B/webserver.c" > "$B/ws_parsers.inc"
awk -v types="conn_state_t,upload_state_t,dav_chunk_state_t,ws_conn_t" \
    -f "$HERE/extract.awk" "$B/webserver.c" > "$B/ws_conn.inc"
awk -v fns="dav_put_consume_chunked" \
    -f "$HERE/extract.awk" "$B/webserver.c" > "$B/ws_chunked.inc"

echo "== unit: HTTP / path / digest / date parsers =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/tp" "$B/test_parsers.c"
"$B/tp"

echo "== unit: chunked transfer-encoding state machine =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/tc" "$B/test_chunked.c"
"$B/tc"

echo "== fuzz: hostile inputs through every parser (ASan/UBSan) =="
gcc -std=gnu2x -Wall -Wextra -Wconversion -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B" -o "$B/fp" "$B/fuzz_parsers.c"
"$B/fp"

echo "WEBSERVER PARSER TESTS PASSED"

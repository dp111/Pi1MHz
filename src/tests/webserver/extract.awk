# Pull named pieces VERBATIM out of a copy of src/wifi/webserver.c so the
# untrusted-input parsers can be compiled host-side without dragging in
# lwIP / sdio / FatFs / framebuffer.  webserver.c is NOT modified - the
# extraction runs against the copy in the mktemp build tree.
#
# Usage:
#   awk -v fns="ws_url_decode,ws_find_header" \
#       -v types="ws_conn_t" \
#       -v defs="WS_PATH_MAX" \
#       -f extract.awk webserver.c > out.inc
#
# Rules (matched to this file's style):
#   * functions: definition starts at a column-0 "static ..." line that
#     contains "<name>(" and ends at the first column-0 "}".  Forward
#     declarations (";" before the body "{") are skipped.
#   * types: "typedef struct {" / "typedef enum {" blocks ending at a
#     column-0 "} <name>;".  Nested (indented) braces are untouched.
#   * defs: "#define <name> ..." single lines plus "\" continuations.
#
# Exits nonzero listing anything requested but not found, so drift in
# webserver.c breaks the test run loudly instead of silently testing
# nothing.

BEGIN {
   nf = split(fns,   a, ","); for (i = 1; i <= nf; i++) if (a[i] != "") wantfn[a[i]] = 1
   nt = split(types, a, ","); for (i = 1; i <= nt; i++) if (a[i] != "") wanttype[a[i]] = 1
   nd = split(defs,  a, ","); for (i = 1; i <= nd; i++) if (a[i] != "") wantdef[a[i]] = 1
   mode = ""
}

# ---- #defines (with backslash continuations) ----
mode == "" && /^#define[ \t]/ {
   name = $2
   sub(/\(.*/, "", name)
   if (name in wantdef) {
      print
      while (/\\[ \t]*$/) { if (getline <= 0) break; print }
      gotdef[name] = 1
      next
   }
}

# ---- typedef blocks ----
mode == "" && /^typedef (struct|enum) \{/ {
   mode = "type"; tbuf = $0 "\n"
   next
}
mode == "type" {
   tbuf = tbuf $0 "\n"
   if ($0 ~ /^\} [A-Za-z_][A-Za-z_0-9]*;/) {
      name = $2
      sub(/;.*/, "", name)
      if (name in wanttype) { printf "%s\n", tbuf; gottype[name] = 1 }
      mode = ""
   }
   next
}

# ---- functions ----
mode == "" && /^static/ {
   fname = ""
   for (n in wantfn) {
      if (index($0, n "(") > 0) { fname = n; break }
   }
   if (fname != "") {
      if ($0 ~ /;[ \t]*$/) next            # forward declaration
      mode = "fn"; fbuf = $0 "\n"
      body = ($0 ~ /\{/) ? 1 : 0
      next
   }
}
mode == "fn" {
   fbuf = fbuf $0 "\n"
   if (!body) {
      if ($0 ~ /;[ \t]*$/) { mode = ""; next }   # multi-line declaration
      if ($0 ~ /\{/) body = 1
      next
   }
   if ($0 ~ /^\}/) {
      printf "%s\n", fbuf
      gotfn[fname] = 1
      mode = ""
   }
   next
}

END {
   err = 0
   for (n in wantfn)   if (!(n in gotfn))   { print "extract.awk: function not found: " n > "/dev/stderr"; err = 1 }
   for (n in wanttype) if (!(n in gottype)) { print "extract.awk: type not found: "     n > "/dev/stderr"; err = 1 }
   for (n in wantdef)  if (!(n in gotdef))  { print "extract.awk: define not found: "   n > "/dev/stderr"; err = 1 }
   if (err) exit 1
}

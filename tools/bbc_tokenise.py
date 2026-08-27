#!/usr/bin/env python3
"""Tokenise an ASCII BBC BASIC listing into BASIC II program format.

Input:  text lines "NNNN statement..." (LF or CR separated)
Output: tokenised program: per line 0x0D <hi> <lo> <len> <bytes...>,
        terminated 0x0D 0xFF.

The token table and behaviour follow BBC BASIC II:
- keywords tokenise longest-match, only outside strings and not mid-identifier;
- REM and DATA leave the rest of the statement untokenised;
- PTR/PAGE/TIME/LOMEM/HIMEM use the statement token at statement start
  (assignment) and the function token in expressions;
- line numbers after GOTO/GOSUB stay ASCII (BASIC evaluates them fine);
- "conditional" keywords do not tokenise when directly followed by an
  identifier character, so names like TOP or COUNTER survive.

Self-check: --verify detokenises the result and diffs it against the input.
"""
import sys

# (keyword, token, conditional). Conditional = BASIC II's niladic
# pseudo-variables: a trailing identifier character means it is a variable
# (TIMER, PAGE2), so the keyword is not tokenised. Statement keywords are
# NOT conditional - PRINTTAB( must tokenise as PRINT TAB( (the bug that
# hung the first tokenised menu in an ON ERROR loop).
KEYWORDS = [
    ("AND",0x80,0),("ABS",0x94,0),("ACS",0x95,0),("ADVAL",0x96,0),("ASC",0x97,0),
    ("ASN",0x98,0),("ATN",0x99,0),("AUTO",0xC6,0),("BGET",0x9A,0),("BPUT",0xD5,0),
    ("COLOUR",0xFB,0),("CALL",0xD6,0),("CHAIN",0xD7,0),("CHR$",0xBD,0),("CLEAR",0xD8,0),
    ("CLOSE",0xD9,0),("CLG",0xDA,0),("CLS",0xDB,0),("COS",0x9B,0),("COUNT",0x9C,1),
    ("DATA",0xDC,0),("DEG",0x9D,0),("DEF",0xDD,0),("DELETE",0xC7,0),("DIV",0x81,0),
    ("DIM",0xDE,0),("DRAW",0xDF,0),("ENDPROC",0xE1,0),("END",0xE0,0),("ENVELOPE",0xE2,0),
    ("ELSE",0x8B,0),("EVAL",0xA0,0),("ERL",0x9E,1),("ERROR",0x85,0),("EOF",0xC5,1),
    ("EOR",0x82,0),("ERR",0x9F,1),("EXP",0xA1,0),("EXT",0xA2,0),("FOR",0xE3,0),
    ("FALSE",0xA3,1),("FN",0xA4,0),("GOTO",0xE5,0),("GET$",0xBE,0),("GET",0xA5,0),
    ("GOSUB",0xE4,0),("GCOL",0xE6,0),("HIMEM",0x93,1),("INPUT",0xE8,0),("IF",0xE7,0),
    ("INKEY$",0xBF,0),("INKEY",0xA6,0),("INT",0xA8,0),("INSTR(",0xA7,0),("LIST",0xC9,0),
    ("LINE",0x86,0),("LOAD",0xC8,0),("LOMEM",0x92,1),("LOCAL",0xEA,0),("LEFT$(",0xC0,0),
    ("LEN",0xA9,0),("LET",0xE9,0),("LOG",0xAB,0),("LN",0xAA,0),("MID$(",0xC1,0),
    ("MODE",0xEB,0),("MOD",0x83,0),("MOVE",0xEC,0),("NEXT",0xED,0),("NEW",0xCA,0),
    ("NOT",0xAC,0),("OLD",0xCB,0),("ON",0xEE,0),("OFF",0x87,0),("OR",0x84,0),
    ("OPENIN",0x8E,0),("OPENOUT",0xAE,0),("OPENUP",0xAD,0),("OSCLI",0xFF,0),("PRINT",0xF1,0),
    ("PAGE",0x90,1),("PTR",0x8F,1),("PI",0xAF,1),("PLOT",0xF0,0),("POINT(",0xB0,0),
    ("PROC",0xF2,0),("POS",0xB1,1),("RETURN",0xF8,0),("REPEAT",0xF5,0),("REPORT",0xF6,0),
    ("READ",0xF3,0),("REM",0xF4,0),("RUN",0xF9,0),("RAD",0xB2,0),("RESTORE",0xF7,0),
    ("RIGHT$(",0xC2,0),("RND",0xB3,1),("RENUMBER",0xCC,0),("STEP",0x88,0),("SAVE",0xCD,0),
    ("SGN",0xB4,0),("SIN",0xB5,0),("SQR",0xB6,0),("SPC",0x89,0),("STR$",0xC3,0),
    ("STRING$(",0xC4,0),("SOUND",0xD4,0),("STOP",0xFA,0),("TAN",0xB7,0),("THEN",0x8C,0),
    ("TO",0xB8,0),("TAB(",0x8A,0),("TRACE",0xFC,0),("TIME",0x91,1),("TRUE",0xB9,1),
    ("UNTIL",0xFD,0),("USR",0xBA,0),("VDU",0xEF,0),("VAL",0xBB,0),("VPOS",0xBC,1),
    ("WIDTH",0xFE,0),
]
KEYWORDS.sort(key=lambda k: -len(k[0]))
# function->statement token pairs (assignment at statement start)
STMT_FORM = {0x8F:0xCF, 0x90:0xD0, 0x91:0xD1, 0x92:0xD2, 0x93:0xD3}
NO_TOKENISE_REST = {0xF4, 0xDC}      # REM, DATA
IDCHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_$%(")

def tokenise_line(text):
    out = bytearray()
    i = 0
    n = len(text)
    stmt_start = True                 # start of a statement (for PAGE= etc.)
    in_ident = False                  # last char consumed was identifier text
    while i < n:
        c = text[i]
        if c == '"':
            in_ident = False
            out.append(0x22); i += 1
            while i < n:
                out.append(ord(text[i])); i += 1
                if text[i-1] == '"': break
            continue
        if c == ':':
            in_ident = False
            out.append(0x3A); i += 1; stmt_start = True
            continue
        if c == '*' and stmt_start:
            # a *command: rest of statement verbatim (up to end of line)
            while i < n:
                out.append(ord(text[i])); i += 1
            break
        if c.isalpha() or c == '@':
            matched = False
            if not in_ident:
                for kw, tok, cond in KEYWORDS:
                    if text.startswith(kw, i):
                        nxt = text[i+len(kw)] if i+len(kw) < n else ''
                        if cond and (nxt.isalnum() or nxt == '_'):
                            continue
                        t = tok
                        if tok in STMT_FORM and stmt_start:
                            # statement form only when this is an assignment
                            rest = text[i+len(kw):].lstrip()
                            if rest.startswith('='):
                                t = STMT_FORM[tok]
                        out.append(t)
                        i += len(kw)
                        if tok in (0xA4, 0xF2):   # FN/PROC: name copied verbatim
                            while i < n and (text[i].isalnum() or text[i] == '_'):
                                out.append(ord(text[i])); i += 1
                        if t in NO_TOKENISE_REST:
                            while i < n:
                                out.append(ord(text[i])); i += 1
                        elif tok in (0x8C, 0x8B, 0xF5):   # THEN/ELSE/REPEAT
                            stmt_start = True
                        else:
                            stmt_start = False
                        matched = True
                        break
            if matched:
                in_ident = False      # a token never continues an identifier
                continue
            # identifier: copy whole run so keywords never match mid-name.
            # '$'/'%' are suffixes: consume one and STOP the run, so a
            # keyword directly after (IFA$=B$THEN) still tokenises.
            while i < n and text[i] in IDCHARS and text[i] != '(':
                ch = text[i]
                out.append(ord(ch)); i += 1
                if ch in '$%':
                    break
            in_ident = (text[i-1] not in '$%') if i > 0 else True
            stmt_start = False
            continue
        if c != ' ':
            stmt_start = False if c not in ':' else stmt_start
        in_ident = False
        out.append(ord(c)); i += 1
    return bytes(out)

TOKENS_REV = {}
for kw, tok, _ in KEYWORDS: TOKENS_REV.setdefault(tok, kw)
for f, st in STMT_FORM.items(): TOKENS_REV[st] = TOKENS_REV[f]

def detokenise_line(data):
    out = []
    i = 0
    while i < len(data):
        b = data[i]
        if b == 0x22:
            out.append('"'); i += 1
            while i < len(data):
                out.append(chr(data[i])); i += 1
                if data[i-1] == 0x22: break
            continue
        if b >= 0x80:
            out.append(TOKENS_REV.get(b, '?%02X?' % b)); i += 1
            continue
        out.append(chr(b)); i += 1
    return ''.join(out)

def tokenise_program(src):
    prog = bytearray()
    lines = []
    for raw in src.replace('\r\n','\n').replace('\r','\n').split('\n'):
        if not raw.strip(): continue
        j = 0
        while j < len(raw) and raw[j].isdigit(): j += 1
        if j == 0:
            # NEW / RUN wrapper lines from the *EXEC-era listing: not part of
            # the tokenised program
            assert raw.strip() in ("NEW", "RUN"), "line without number: %r" % raw[:40]
            continue
        num = int(raw[:j])
        body = raw[j:]
        if body.startswith(' '): body = body[1:]   # single separator space
        tok = tokenise_line(body)
        rec = bytes([num >> 8, num & 0xFF, len(tok) + 4]) + tok
        assert len(tok) + 4 <= 255, "line %d too long tokenised" % num
        prog += b'\x0D' + rec
        lines.append((num, body))
    prog += b'\x0D\xFF'
    return bytes(prog), lines

def main():
    src_path, out_path = sys.argv[1], sys.argv[2]
    src = open(src_path, 'r', newline='').read()
    prog, lines = tokenise_program(src)
    # verify: detokenise and diff
    i = 0; k = 0; errors = 0
    data = prog
    while i < len(data):
        assert data[i] == 0x0D
        if data[i+1] == 0xFF: break
        num = (data[i+1] << 8) | data[i+2]
        ln = data[i+3]
        body = detokenise_line(data[i+4:i+ln])
        onum, obody = lines[k]
        if num != onum or body != obody:
            print("MISMATCH line %d:\n  in : %r\n  out: %r" % (onum, obody, body))
            errors += 1
        k += 1; i += ln
    if errors:
        sys.exit("verify FAILED: %d line(s)" % errors)
    open(out_path, 'wb').write(prog)
    print("%s: %d lines, %d bytes tokenised, round-trip verified" % (out_path, k, len(prog)))

if __name__ == '__main__':
    main()

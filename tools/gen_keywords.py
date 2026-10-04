#!/usr/bin/env python3
"""Generate the keyword perfect-hash table in src/keyword.c.

take_keyword() looks a token up with one djb2 hash (computed in uint32_t) and
one probe into kSlots, so every keyword must land in its own slot. This script
picks the smallest table size with no collisions and rewrites the block
between the BEGIN/END GENERATED markers in src/keyword.c.

Usage:
  tools/gen_keywords.py          rewrite src/keyword.c
  tools/gen_keywords.py --check  exit 1 if src/keyword.c is out of date

To add a keyword: add a row below, add its KW_ id to enum KeywordId in
src/keyword.h, and rerun this script.
"""
import os
import sys

# (spelling, class, enum id). Classes match KW_CLASS_* in src/keyword.h:
# PSEUDO ops are expanded by the preprocessor, MNEMONICs are encoded by
# encode.c, and DIRECTIVEs are handled by assembler.c.
KEYWORDS = [
    ("nop", "PSEUDO", "KW_NOP"),
    ("ret", "PSEUDO", "KW_RET"),
    ("push", "PSEUDO", "KW_PUSH"),
    ("pop", "PSEUDO", "KW_POP"),
    ("pshw", "PSEUDO", "KW_PSHW"),
    ("popw", "PSEUDO", "KW_POPW"),
    ("pshd", "PSEUDO", "KW_PSHD"),
    ("popd", "PSEUDO", "KW_POPD"),
    ("pshb", "PSEUDO", "KW_PSHB"),
    ("popb", "PSEUDO", "KW_POPB"),
    ("movi", "PSEUDO", "KW_MOVI"),
    ("mov", "PSEUDO", "KW_MOV"),
    ("call", "PSEUDO", "KW_CALL"),
    ("and", "MNEMONIC", "KW_AND"),
    ("nand", "MNEMONIC", "KW_NAND"),
    ("or", "MNEMONIC", "KW_OR"),
    ("nor", "MNEMONIC", "KW_NOR"),
    ("xor", "MNEMONIC", "KW_XOR"),
    ("xnor", "MNEMONIC", "KW_XNOR"),
    ("not", "MNEMONIC", "KW_NOT"),
    ("lsl", "MNEMONIC", "KW_LSL"),
    ("lsr", "MNEMONIC", "KW_LSR"),
    ("asr", "MNEMONIC", "KW_ASR"),
    ("rotl", "MNEMONIC", "KW_ROTL"),
    ("rotr", "MNEMONIC", "KW_ROTR"),
    ("lslc", "MNEMONIC", "KW_LSLC"),
    ("lsrc", "MNEMONIC", "KW_LSRC"),
    ("add", "MNEMONIC", "KW_ADD"),
    ("addc", "MNEMONIC", "KW_ADDC"),
    ("sub", "MNEMONIC", "KW_SUB"),
    ("subb", "MNEMONIC", "KW_SUBB"),
    ("cmp", "MNEMONIC", "KW_CMP"),
    ("sxtb", "MNEMONIC", "KW_SXTB"),
    ("sxtd", "MNEMONIC", "KW_SXTD"),
    ("tncb", "MNEMONIC", "KW_TNCB"),
    ("tncd", "MNEMONIC", "KW_TNCD"),
    ("lui", "MNEMONIC", "KW_LUI"),
    ("swa", "MNEMONIC", "KW_SWA"),
    ("lwa", "MNEMONIC", "KW_LWA"),
    ("sw", "MNEMONIC", "KW_SW"),
    ("lw", "MNEMONIC", "KW_LW"),
    ("sda", "MNEMONIC", "KW_SDA"),
    ("lda", "MNEMONIC", "KW_LDA"),
    ("sd", "MNEMONIC", "KW_SD"),
    ("ld", "MNEMONIC", "KW_LD"),
    ("sba", "MNEMONIC", "KW_SBA"),
    ("lba", "MNEMONIC", "KW_LBA"),
    ("sb", "MNEMONIC", "KW_SB"),
    ("lb", "MNEMONIC", "KW_LB"),
    ("br", "MNEMONIC", "KW_BR"),
    ("bz", "MNEMONIC", "KW_BZ"),
    ("bnz", "MNEMONIC", "KW_BNZ"),
    ("bs", "MNEMONIC", "KW_BS"),
    ("bns", "MNEMONIC", "KW_BNS"),
    ("bc", "MNEMONIC", "KW_BC"),
    ("bnc", "MNEMONIC", "KW_BNC"),
    ("bo", "MNEMONIC", "KW_BO"),
    ("bno", "MNEMONIC", "KW_BNO"),
    ("bps", "MNEMONIC", "KW_BPS"),
    ("bnps", "MNEMONIC", "KW_BNPS"),
    ("bg", "MNEMONIC", "KW_BG"),
    ("bge", "MNEMONIC", "KW_BGE"),
    ("bl", "MNEMONIC", "KW_BL"),
    ("ble", "MNEMONIC", "KW_BLE"),
    ("ba", "MNEMONIC", "KW_BA"),
    ("bae", "MNEMONIC", "KW_BAE"),
    ("bb", "MNEMONIC", "KW_BB"),
    ("bbe", "MNEMONIC", "KW_BBE"),
    ("bra", "MNEMONIC", "KW_BRA"),
    ("bza", "MNEMONIC", "KW_BZA"),
    ("bnza", "MNEMONIC", "KW_BNZA"),
    ("bsa", "MNEMONIC", "KW_BSA"),
    ("bnsa", "MNEMONIC", "KW_BNSA"),
    ("bca", "MNEMONIC", "KW_BCA"),
    ("bnca", "MNEMONIC", "KW_BNCA"),
    ("boa", "MNEMONIC", "KW_BOA"),
    ("bnoa", "MNEMONIC", "KW_BNOA"),
    ("bpa", "MNEMONIC", "KW_BPA"),
    ("bnpa", "MNEMONIC", "KW_BNPA"),
    ("bga", "MNEMONIC", "KW_BGA"),
    ("bgea", "MNEMONIC", "KW_BGEA"),
    ("bla", "MNEMONIC", "KW_BLA"),
    ("blea", "MNEMONIC", "KW_BLEA"),
    ("baa", "MNEMONIC", "KW_BAA"),
    ("baea", "MNEMONIC", "KW_BAEA"),
    ("bba", "MNEMONIC", "KW_BBA"),
    ("bbea", "MNEMONIC", "KW_BBEA"),
    ("jmp", "MNEMONIC", "KW_JMP"),
    ("adpc", "MNEMONIC", "KW_ADPC"),
    ("trap", "MNEMONIC", "KW_TRAP"),
    ("fada", "MNEMONIC", "KW_FADA"),
    ("fad", "MNEMONIC", "KW_FAD"),
    ("swpa", "MNEMONIC", "KW_SWPA"),
    ("swp", "MNEMONIC", "KW_SWP"),
    ("tlbr", "MNEMONIC", "KW_TLBR"),
    ("tlbw", "MNEMONIC", "KW_TLBW"),
    ("tlbi", "MNEMONIC", "KW_TLBI"),
    ("tlbc", "MNEMONIC", "KW_TLBC"),
    ("crmv", "MNEMONIC", "KW_CRMV"),
    ("mode", "MNEMONIC", "KW_MODE"),
    ("rfe", "MNEMONIC", "KW_RFE"),
    ("ipi", "MNEMONIC", "KW_IPI"),
    ("eoi", "MNEMONIC", "KW_EOI"),
    ("movu", "MNEMONIC", "KW_MOVU"),
    ("movl", "MNEMONIC", "KW_MOVL"),
    (".global", "DIRECTIVE", "KW_DIR_GLOBAL"),
    (".origin", "DIRECTIVE", "KW_DIR_ORIGIN"),
    (".text", "DIRECTIVE", "KW_DIR_TEXT"),
    (".rodata", "DIRECTIVE", "KW_DIR_RODATA"),
    (".data", "DIRECTIVE", "KW_DIR_DATA"),
    (".bss", "DIRECTIVE", "KW_DIR_BSS"),
    (".text_load", "DIRECTIVE", "KW_DIR_TEXT_LOAD"),
    (".rodata_load", "DIRECTIVE", "KW_DIR_RODATA_LOAD"),
    (".data_load", "DIRECTIVE", "KW_DIR_DATA_LOAD"),
    (".bss_load", "DIRECTIVE", "KW_DIR_BSS_LOAD"),
    (".fill", "DIRECTIVE", "KW_DIR_FILL"),
    (".fild", "DIRECTIVE", "KW_DIR_FILD"),
    (".filb", "DIRECTIVE", "KW_DIR_FILB"),
    (".space", "DIRECTIVE", "KW_DIR_SPACE"),
    (".align", "DIRECTIVE", "KW_DIR_ALIGN"),
    (".define", "DIRECTIVE", "KW_DIR_DEFINE"),
    (".line", "DIRECTIVE", "KW_DIR_LINE"),
    (".local", "DIRECTIVE", "KW_DIR_LOCAL"),
]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.path.join(ROOT, "src", "keyword.c")
BEGIN = "// BEGIN GENERATED by tools/gen_keywords.py; do not edit by hand."
END = "// END GENERATED"


def djb2(text):
    value = 5381
    for byte in text.encode("ascii"):
        value = (value * 33 + byte) & 0xFFFFFFFF
    return value


def smallest_perfect_size(words):
    size = len(words)
    while len({djb2(w) % size for w in words}) != len(words):
        size += 1
    return size


def generate():
    words = [w for w, _, _ in KEYWORDS]
    if len(set(words)) != len(words):
        sys.exit("gen_keywords: duplicate keyword spelling in KEYWORDS")
    size = smallest_perfect_size(words)
    max_len = max(len(w) for w in words)
    pseudo_leads = sorted({w[0] for w, k, _ in KEYWORDS if k == "PSEUDO"})

    lines = [BEGIN]
    lines.append(f"// Longest recognized keyword. Longer tokens cannot match.")
    lines.append(f"enum {{ kMaxKeywordLen = {max_len} }};")
    lines.append("")
    lines.append("// Smallest table size for which every keyword hashes to its own slot.")
    lines.append(f"enum {{ kKeywordSlotCount = {size} }};")
    lines.append("")
    lines.append("static const struct KwSlot kSlots[kKeywordSlotCount] = {")
    for word, klass, kid in sorted(KEYWORDS, key=lambda r: djb2(r[0]) % size):
        lines.append(f'  [{djb2(word) % size}] = {{"{word}", {len(word)}, KW_CLASS_{klass}, {kid}}},')
    lines.append("};")
    lines.append("")
    lines.append("// Return whether c can begin a pseudo-op. The preprocessor calls take_keyword")
    lines.append("// on every source byte, so most bytes are rejected here before any scan.")
    lines.append("static bool is_pseudo_lead(unsigned char c) {")
    lines.append("  return " + " || ".join(f"c == '{c}'" for c in pseudo_leads) + ";")
    lines.append("}")
    lines.append(END)
    return "\n".join(lines)


def main():
    with open(TARGET) as f:
        source = f.read()
    start = source.index(BEGIN)
    end = source.index(END) + len(END)
    updated = source[:start] + generate() + source[end:]
    if "--check" in sys.argv[1:]:
        if updated != source:
            sys.exit("gen_keywords: src/keyword.c is out of date; run tools/gen_keywords.py")
        return
    with open(TARGET, "w") as f:
        f.write(updated)


if __name__ == "__main__":
    main()

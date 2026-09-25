#include "keyword.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "assembler.h"

// Longest recognized keyword (.rodata_load). Longer tokens cannot match.
enum { kMaxKeywordLen = 12 };

// Perfect hash of the keyword set. djb2 is computed in uint32_t so the slot
// chosen for a spelling does not depend on the width of size_t.
enum { kKeywordSlotCount = 1264 };

struct KwSlot {
  const char* text;
  uint8_t len;
  uint8_t klass;
  uint16_t id;
};

static const struct KwSlot kSlots[kKeywordSlotCount] = {
  [7] = {".text_load", 10, 4, KW_DIR_TEXT_LOAD},
  [39] = {"mov", 3, 1, KW_MOV},
  [54] = {"rotl", 4, 2, KW_ROTL},
  [60] = {"rotr", 4, 2, KW_ROTR},
  [76] = {".data_load", 10, 4, KW_DIR_DATA_LOAD},
  [86] = {"popb", 4, 1, KW_POPB},
  [88] = {"popd", 4, 1, KW_POPD},
  [96] = {"ret", 3, 1, KW_RET},
  [106] = {"sb", 2, 2, KW_SB},
  [107] = {"popw", 4, 1, KW_POPW},
  [108] = {"sd", 2, 2, KW_SD},
  [114] = {"rfe", 3, 2, KW_RFE},
  [118] = {"nand", 4, 2, KW_NAND},
  [127] = {"sw", 2, 2, KW_SW},
  [132] = {".global", 7, 4, KW_DIR_GLOBAL},
  [157] = {"adpc", 4, 2, KW_ADPC},
  [162] = {"eoi", 3, 2, KW_EOI},
  [181] = {"bnoa", 4, 2, KW_BNOA},
  [201] = {"blea", 4, 2, KW_BLEA},
  [209] = {"subb", 4, 2, KW_SUBB},
  [214] = {"bnpa", 4, 2, KW_BNPA},
  [217] = {"baa", 3, 2, KW_BAA},
  [221] = {"bae", 3, 2, KW_BAE},
  [232] = {"bnps", 4, 2, KW_BNPS},
  [250] = {"bba", 3, 2, KW_BBA},
  [254] = {"bbe", 3, 2, KW_BBE},
  [278] = {"sxtb", 4, 2, KW_SXTB},
  [280] = {"sxtd", 4, 2, KW_SXTD},
  [283] = {"bca", 3, 2, KW_BCA},
  [313] = {"bnsa", 4, 2, KW_BNSA},
  [320] = {"swpa", 4, 2, KW_SWPA},
  [336] = {"lsl", 3, 2, KW_LSL},
  [342] = {"lsr", 3, 2, KW_LSR},
  [385] = {"call", 4, 1, KW_CALL},
  [386] = {"pshb", 4, 1, KW_PSHB},
  [388] = {"pshd", 4, 1, KW_PSHD},
  [399] = {"lui", 3, 2, KW_LUI},
  [405] = {"push", 4, 1, KW_PUSH},
  [407] = {"pshw", 4, 1, KW_PSHW},
  [415] = {"bga", 3, 2, KW_BGA},
  [419] = {"bge", 3, 2, KW_BGE},
  [431] = {"sub", 3, 2, KW_SUB},
  [446] = {".define", 7, 4, KW_DIR_DEFINE},
  [453] = {"cmp", 3, 2, KW_CMP},
  [457] = {"lwa", 3, 2, KW_LWA},
  [481] = {"fada", 4, 2, KW_FADA},
  [492] = {"jmp", 3, 2, KW_JMP},
  [494] = {"add", 3, 2, KW_ADD},
  [496] = {"swa", 3, 2, KW_SWA},
  [509] = {".data", 5, 4, KW_DIR_DATA},
  [511] = {"swp", 3, 2, KW_SWP},
  [542] = {".local", 6, 4, KW_DIR_LOCAL},
  [544] = {"bnza", 4, 2, KW_BNZA},
  [580] = {"bla", 3, 2, KW_BLA},
  [584] = {"ble", 3, 2, KW_BLE},
  [586] = {"mode", 4, 2, KW_MODE},
  [635] = {".bss", 4, 4, KW_DIR_BSS},
  [638] = {"xor", 3, 2, KW_XOR},
  [648] = {"bnc", 3, 2, KW_BNC},
  [660] = {"bno", 3, 2, KW_BNO},
  [664] = {"bns", 3, 2, KW_BNS},
  [671] = {"bnz", 3, 2, KW_BNZ},
  [679] = {"boa", 3, 2, KW_BOA},
  [687] = {"bbea", 4, 2, KW_BBEA},
  [712] = {"bpa", 3, 2, KW_BPA},
  [730] = {"bps", 3, 2, KW_BPS},
  [747] = {".line", 5, 4, KW_DIR_LINE},
  [748] = {"tncb", 4, 2, KW_TNCB},
  [750] = {"tncd", 4, 2, KW_TNCD},
  [759] = {"ipi", 3, 2, KW_IPI},
  [772] = {"pop", 3, 1, KW_POP},
  [778] = {"bra", 3, 2, KW_BRA},
  [784] = {"fad", 3, 2, KW_FAD},
  [808] = {"ba", 2, 2, KW_BA},
  [809] = {"bb", 2, 2, KW_BB},
  [810] = {"bc", 2, 2, KW_BC},
  [811] = {"bsa", 3, 2, KW_BSA},
  [812] = {"xnor", 4, 2, KW_XNOR},
  [814] = {"bg", 2, 2, KW_BG},
  [819] = {"bl", 2, 2, KW_BL},
  [822] = {"bo", 2, 2, KW_BO},
  [824] = {"and", 3, 2, KW_AND},
  [825] = {"br", 2, 2, KW_BR},
  [826] = {"bs", 2, 2, KW_BS},
  [833] = {"bz", 2, 2, KW_BZ},
  [846] = {".rodata", 7, 4, KW_DIR_RODATA},
  [862] = {"baea", 4, 2, KW_BAEA},
  [867] = {"lslc", 4, 2, KW_LSLC},
  [971] = {".origin", 7, 4, KW_DIR_ORIGIN},
  [991] = {".space", 6, 4, KW_DIR_SPACE},
  [1003] = {"asr", 3, 2, KW_ASR},
  [1018] = {".bss_load", 9, 4, KW_DIR_BSS_LOAD},
  [1025] = {"addc", 4, 2, KW_ADDC},
  [1028] = {"lba", 3, 2, KW_LBA},
  [1042] = {"bza", 3, 2, KW_BZA},
  [1049] = {"bnca", 4, 2, KW_BNCA},
  [1065] = {"lsrc", 4, 2, KW_LSRC},
  [1066] = {"tlbc", 4, 2, KW_TLBC},
  [1067] = {"sba", 3, 2, KW_SBA},
  [1072] = {"tlbi", 4, 2, KW_TLBI},
  [1076] = {"bgea", 4, 2, KW_BGEA},
  [1081] = {"tlbr", 4, 2, KW_TLBR},
  [1086] = {"tlbw", 4, 2, KW_TLBW},
  [1094] = {"lda", 3, 2, KW_LDA},
  [1096] = {".text", 5, 4, KW_DIR_TEXT},
  [1122] = {"nop", 3, 1, KW_NOP},
  [1124] = {"nor", 3, 2, KW_NOR},
  [1126] = {"not", 3, 2, KW_NOT},
  [1133] = {"sda", 3, 2, KW_SDA},
  [1139] = {"lb", 2, 2, KW_LB},
  [1141] = {"ld", 2, 2, KW_LD},
  [1149] = {".rodata_load", 12, 4, KW_DIR_RODATA_LOAD},
  [1160] = {"lw", 2, 2, KW_LW},
  [1184] = {"movi", 4, 1, KW_MOVI},
  [1187] = {"movl", 4, 2, KW_MOVL},
  [1196] = {"movu", 4, 2, KW_MOVU},
  [1198] = {".align", 6, 4, KW_DIR_ALIGN},
  [1200] = {".filb", 5, 4, KW_DIR_FILB},
  [1202] = {".fild", 5, 4, KW_DIR_FILD},
  [1210] = {".fill", 5, 4, KW_DIR_FILL},
  [1245] = {"crmv", 4, 2, KW_CRMV},
  [1254] = {"or", 2, 2, KW_OR},
  [1260] = {"trap", 4, 2, KW_TRAP},
};

static bool kw_is_alpha(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool kw_is_digit(unsigned char c) {
  return c >= '0' && c <= '9';
}

static bool kw_is_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// Identifier body characters recognized by the assembler lexer.
static bool kw_is_body(unsigned char c) {
  return kw_is_alpha(c) || kw_is_digit(c) || c == '_' || c == '.';
}

static uint32_t kw_hash(const char* text, size_t len) {
  uint32_t hash = 5381;
  for (size_t i = 0; i < len; ++i) {
    hash = hash * 33u + (uint32_t)(unsigned char)text[i];
  }
  return hash;
}

enum KeywordId take_keyword(unsigned class_mask) {
  if (current != current_buffer_start && kw_is_body((unsigned char)current[-1])) {
    return KW_NONE;
  }

  unsigned char lead = (unsigned char)*current;
  if (class_mask == KW_CLASS_PSEUDO) {
    // Pseudo-ops start with a closed set of letters. Reject everything else
    // before scanning, because the preprocessor calls this on every byte.
    if (lead != 'n' && lead != 'r' && lead != 'p' && lead != 'm' && lead != 'c') {
      return KW_NONE;
    }
  } else if (class_mask == KW_CLASS_DIRECTIVE) {
    if (lead != '.') return KW_NONE;
  } else if (!(kw_is_alpha(lead) || lead == '_')) {
    return KW_NONE;
  }

  const char* start = current;
  size_t len = 1;
  while (kw_is_body((unsigned char)start[len])) len++;
  if (len > kMaxKeywordLen) return KW_NONE;

  unsigned char next = (unsigned char)start[len];
  if (!(kw_is_space(next) || next == '\0' || next == ',' || next == ';' || next == ':')) {
    return KW_NONE;
  }

  const struct KwSlot* slot = &kSlots[kw_hash(start, len) % kKeywordSlotCount];
  if (slot->text == NULL || slot->len != len || (slot->klass & class_mask) == 0) {
    return KW_NONE;
  }
  if (memcmp(slot->text, start, len) != 0) return KW_NONE;

  current += len;
  return (enum KeywordId)slot->id;
}

#pragma once
// TextTransliterate.h — what a text becomes before the drum is asked for it
// (#618). The 45 flaps carry A–Z, 0–9, ä ö ü (on the wire $ & #), space and
// : . ! ? - and nothing else: an accented letter used to show as a blank.
// Every text path (the API, MQTT, the page) runs its text through
// transliterateForFlaps() once, at the master, before it is retained or laid
// out; the row boards only ever see what came out of it.
//
// Rules: a–z become A–Z (what the flaps show, so the retained text and
// Home Assistant's text/state read as the wall does); a Latin letter with a
// diacritic becomes its base letter; ä ö ü and their capitals become the
// drum's own flaps; ß æ œ þ become SS AE OE TH. Anything else passes through
// unchanged and shows as a blank, as it did. Pure, natively tested by
// test_text_transliterate.

#include <Arduino.h>

#include "FlapLetters.h"  // flapLetterIndex

// The flap characters of ä ö ü (SplitFlapProtocol.h wire contract).
#define FLAP_CHAR_AE '$'
#define FLAP_CHAR_OE '&'
#define FLAP_CHAR_UE '#'

// The replacement of the Latin-1 code point 0xC0 + `index` (À … ÿ), or
// nullptr when it stays what it is. Two characters at most.
inline const char* transliterateLatin1(uint8_t index) {
  static const char* const upper[32] = {
      "A", "A", "A", "A", "$", "A", "AE", "C",   // À Á Â Ã Ä Å Æ Ç
      "E", "E", "E", "E", "I", "I", "I", "I",    // È É Ê Ë Ì Í Î Ï
      "D", "N", "O", "O", "O", "O", "&", nullptr,  // Ð Ñ Ò Ó Ô Õ Ö ×
      "O", "U", "U", "U", "#", "Y", "TH", "SS"}; // Ø Ù Ú Û Ü Ý Þ ß
  static const char* const lower[32] = {
      "A", "A", "A", "A", "$", "A", "AE", "C",   // à á â ã ä å æ ç
      "E", "E", "E", "E", "I", "I", "I", "I",    // è é ê ë ì í î ï
      "D", "N", "O", "O", "O", "O", "&", nullptr,  // ð ñ ò ó ô õ ö ÷
      "O", "U", "U", "U", "#", "Y", "TH", "Y"};  // ø ù ú û ü ý þ ÿ
  if (index < 32) return upper[index];
  if (index < 64) return lower[index - 32];
  return nullptr;
}

// A few letters past Latin-1 that Dutch and its neighbours write.
inline const char* transliterateLatinExtended(uint16_t codePoint) {
  switch (codePoint) {
    case 0x0152: case 0x0153: return "OE";  // Œ œ
    case 0x0160: case 0x0161: return "S";   // Š š
    case 0x0178: return "Y";                // Ÿ
    case 0x017D: case 0x017E: return "Z";   // Ž ž
    case 0x0132: case 0x0133: return "IJ";  // Ĳ ĳ
    default: return nullptr;
  }
}

inline String transliterateForFlaps(const String& text) {
  String out;
  out.reserve(text.length());
  const unsigned int n = text.length();
  for (unsigned int i = 0; i < n; i++) {
    const uint8_t c = (uint8_t)text[i];
    // A two-byte sequence: U+0080..U+07FF.
    if ((c & 0xE0) == 0xC0 && i + 1 < n && ((uint8_t)text[i + 1] & 0xC0) == 0x80) {
      const uint16_t codePoint = (uint16_t)(((c & 0x1F) << 6) | ((uint8_t)text[i + 1] & 0x3F));
      const char* replacement = codePoint >= 0xC0 && codePoint <= 0xFF
                                    ? transliterateLatin1((uint8_t)(codePoint - 0xC0))
                                    : transliterateLatinExtended(codePoint);
      if (replacement != nullptr) {
        out += replacement;
        i++;
        continue;
      }
    }
    out += (char)(c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c);
  }
  return out;
}

// Characters of `text` the drum has no flap for, after transliteration (a
// multi-byte character counts once; a line break is not a character).
inline int unsupportedFlapCount(const String& text) {
  int count = 0;
  const unsigned int n = text.length();
  for (unsigned int i = 0; i < n; i++) {
    const uint8_t c = (uint8_t)text[i];
    if ((c & 0xC0) == 0x80) continue;  // a continuation byte of the lead counted
    if (c == '\n') continue;
    if (flapLetterIndex((char)c) < 0) count++;
  }
  return count;
}

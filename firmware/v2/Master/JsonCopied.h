#pragma once
// JsonCopied.h — for putting a string from a snapshot into an ArduinoJson
// document that outlives the snapshot.
//
// Handed a `const char[N]` (a char array member reached through a const
// struct), ArduinoJson takes it for a string literal and keeps only the
// pointer. An async web response is written out after its handler returned,
// when the snapshot the handler read is gone: the document then points at
// freed memory. A plain `const char*` is copied, and this turns the array
// into one. test_json_copied pins both halves.
inline const char* jsonCopied(const char* text) { return text; }

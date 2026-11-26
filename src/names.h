// Reversible mapping between S3 key segments and names Total Commander can display and copy.
//
// Windows forbids \ / : * ? " < > | and control characters in file names, and drops trailing dots
// and spaces. S3 keys may contain all of them. We substitute look-alike Unicode characters
// (the scheme rclone uses):
//   \ / : * ? " < > |   ->  fullwidth forms (U+FF3C, U+FF0F, U+FF1A, ...)
//   0x01..0x1F          ->  control pictures U+2401..U+241F
//   trailing '.' / ' '  ->  U+FF0E / U+2420
// A segment that already contains one of the substitute characters, or the escape character
// U+201B itself, gets that character prefixed with U+201B, so decoding is unambiguous.
// An empty segment (key "a//b") is shown as a lone U+201B.
#pragma once

#include <string>

namespace s3tc {

std::wstring EncodeName(const std::wstring& segment);
std::wstring DecodeName(const std::wstring& name);

}  // namespace s3tc

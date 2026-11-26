#include "names.h"

namespace s3tc {

namespace {

constexpr wchar_t kEscape = 0x201B;
constexpr wchar_t kTrailingDot = 0xFF0E;
constexpr wchar_t kTrailingSpace = 0x2420;

struct Pair {
    wchar_t plain;
    wchar_t substitute;
};

constexpr Pair kPairs[] = {
    {L'\\', 0xFF3C}, {L'/', 0xFF0F}, {L':', 0xFF1A}, {L'*', 0xFF0A}, {L'?', 0xFF1F},
    {L'"', 0xFF02},  {L'<', 0xFF1C}, {L'>', 0xFF1E}, {L'|', 0xFF5C},
};

wchar_t SubstituteFor(wchar_t c) {
    if (c >= 0x01 && c <= 0x1F) return static_cast<wchar_t>(0x2400 + c);
    for (const auto& p : kPairs) {
        if (p.plain == c) return p.substitute;
    }
    return 0;
}

wchar_t PlainFor(wchar_t c) {
    if (c >= 0x2401 && c <= 0x241F) return static_cast<wchar_t>(c - 0x2400);
    if (c == kTrailingDot) return L'.';
    if (c == kTrailingSpace) return L' ';
    for (const auto& p : kPairs) {
        if (p.substitute == c) return p.plain;
    }
    return 0;
}

bool NeedsEscape(wchar_t c) { return c == kEscape || PlainFor(c) != 0; }

}  // namespace

std::wstring EncodeName(const std::wstring& segment) {
    if (segment.empty()) return std::wstring(1, kEscape);
    std::wstring out;
    out.reserve(segment.size() + 4);
    for (size_t i = 0; i < segment.size(); ++i) {
        wchar_t c = segment[i];
        const bool last = i + 1 == segment.size();
        if (NeedsEscape(c)) {
            out.push_back(kEscape);
            out.push_back(c);
        } else if (wchar_t s = SubstituteFor(c)) {
            out.push_back(s);
        } else if (last && c == L'.') {
            out.push_back(kTrailingDot);
        } else if (last && c == L' ') {
            out.push_back(kTrailingSpace);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::wstring DecodeName(const std::wstring& name) {
    std::wstring out;
    out.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        wchar_t c = name[i];
        if (c == kEscape) {
            if (i + 1 < name.size()) out.push_back(name[++i]);
            // A lone trailing escape encodes the empty segment: emit nothing.
        } else if (wchar_t p = PlainFor(c)) {
            out.push_back(p);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

}  // namespace s3tc

#include "progress.h"

#include <cstdlib>
#include <cstring>

namespace s3tc {

namespace {

// Parses "[~]<number> <unit>" at p; advances p. Units: Bytes, KiB, MiB, GiB, TiB, PiB.
bool ParseAmount(const char*& p, double* bytes, bool* estimate) {
    while (*p == ' ') ++p;
    if (estimate) *estimate = false;
    if (*p == '~') {
        if (estimate) *estimate = true;
        ++p;
    }
    char* end = nullptr;
    double v = strtod(p, &end);
    if (end == p) return false;
    p = end;
    while (*p == ' ') ++p;
    static const struct {
        const char* name;
        double mult;
    } kUnits[] = {{"Bytes", 1.0},
                  {"Byte", 1.0},
                  {"KiB", 1024.0},
                  {"MiB", 1024.0 * 1024},
                  {"GiB", 1024.0 * 1024 * 1024},
                  {"TiB", 1024.0 * 1024 * 1024 * 1024},
                  {"PiB", 1024.0 * 1024 * 1024 * 1024 * 1024}};
    for (const auto& u : kUnits) {
        size_t n = strlen(u.name);
        if (strncmp(p, u.name, n) == 0) {
            *bytes = v * u.mult;
            p += n;
            return true;
        }
    }
    return false;
}

}  // namespace

bool ParseCpProgress(const std::string& line, CpProgress* out) {
    const char kPrefix[] = "Completed ";
    size_t pos = line.find(kPrefix);
    if (pos == std::string::npos) return false;
    const char* p = line.c_str() + pos + sizeof(kPrefix) - 1;
    CpProgress r;
    if (!ParseAmount(p, &r.doneBytes, nullptr)) return false;
    if (*p != '/') return false;
    ++p;
    if (!ParseAmount(p, &r.totalBytes, &r.totalIsEstimate)) return false;
    *out = r;
    return true;
}

void LineSplitter::Feed(const std::string& chunk) {
    for (char c : chunk) {
        if (c == '\r' || c == '\n') {
            if (!pending_.empty()) onLine_(pending_);
            pending_.clear();
        } else {
            pending_.push_back(c);
        }
    }
}

void LineSplitter::Flush() {
    if (!pending_.empty()) onLine_(pending_);
    pending_.clear();
}

}  // namespace s3tc

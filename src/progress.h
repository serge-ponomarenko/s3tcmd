// Parser for `aws s3 cp` progress records (measured):
//   "Completed 8.1 MiB/22.2 MiB (3.0 MiB/s) with 3 file(s) remaining"
//   "Completed 256.0 KiB/~1.2 MiB (1.1 MiB/s) with ~3 file(s) remaining (calculating...)"
#pragma once

#include <functional>
#include <string>

namespace s3tc {

struct CpProgress {
    double doneBytes = 0;
    double totalBytes = 0;
    bool totalIsEstimate = false;  // "~" prefix: the CLI is still listing the source
};

bool ParseCpProgress(const std::string& line, CpProgress* out);

// Splits a stream of stdout chunks into records ('\r' or '\n' terminated) and calls onLine.
class LineSplitter {
public:
    explicit LineSplitter(std::function<void(const std::string&)> onLine) : onLine_(std::move(onLine)) {}
    void Feed(const std::string& chunk);
    void Flush();

private:
    std::function<void(const std::string&)> onLine_;
    std::string pending_;
};

}  // namespace s3tc

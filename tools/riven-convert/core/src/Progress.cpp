#include "riven/Progress.hpp"

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace riven
{

const char *toString(Severity s)
{
    switch (s)
    {
    case Severity::Warning: return "warning";
    case Severity::Error:   return "error";
    default:                return "info";
    }
}

void ProgressSink::logf(Severity severity, std::string_view stage, const char *fmt, ...)
{
    // Two passes so a long line is never truncated: most messages fit the stack
    // buffer, and the few that do not (a path plus a reason) get an exact
    // allocation rather than an ellipsis.
    char stackBuf[512];

    std::va_list args;
    va_start(args, fmt);
    const int needed = std::vsnprintf(stackBuf, sizeof(stackBuf), fmt, args);
    va_end(args);

    if (needed < 0)
        return; // encoding error: nothing sensible to report

    if (static_cast<std::size_t>(needed) < sizeof(stackBuf))
    {
        log(severity, stage, std::string_view(stackBuf, static_cast<std::size_t>(needed)));
        return;
    }

    std::vector<char> heapBuf(static_cast<std::size_t>(needed) + 1);
    va_start(args, fmt);
    std::vsnprintf(heapBuf.data(), heapBuf.size(), fmt, args);
    va_end(args);
    log(severity, stage, std::string_view(heapBuf.data(), static_cast<std::size_t>(needed)));
}

} // namespace riven

#pragma once

// Progress reporting and cancellation for a long conversion.
//
// The core is deliberately free of any UI framework: it talks to whatever is
// driving it through ProgressSink, which the CLI implements as stdout and the
// Qt GUI implements by emitting queued signals. Nothing here knows about Qt.
//
// The cancellation model is lifted from the Myst converter's cancel.py, whose
// design note is worth restating because it is the whole rule: "'Stop' cannot
// mean 'kill the thread'. Instead every stage polls a token at the points where
// it is already doing bookkeeping, and unwinds by raising. Callers that own
// partial output decide what to keep."

#include <atomic>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace riven
{

/// How much attention a log line deserves.
///
/// This exists so a UI can keep a Problems list separate from the log tail. A
/// Riven conversion emits tens of thousands of lines; a warning about a damaged
/// resource that scrolls past at line 8000 is a warning nobody reads.
enum class Severity
{
    Info,
    Warning,
    Error,
};

const char *toString(Severity s);

/// Thrown by CancelToken::throwIfCancelled to unwind a running conversion.
/// Distinct from every other exception so the pipeline's per-asset "log it and
/// carry on" handlers can rethrow it rather than swallow it.
class ConversionCancelled : public std::exception
{
public:
    const char *what() const noexcept override { return "conversion cancelled"; }
};

/// Cooperative cancellation. Safe to call from any thread.
class CancelToken
{
public:
    void cancel() noexcept { flag_.store(true, std::memory_order_relaxed); }
    bool cancelled() const noexcept { return flag_.load(std::memory_order_relaxed); }
    void reset() noexcept { flag_.store(false, std::memory_order_relaxed); }

    void throwIfCancelled() const
    {
        if (cancelled())
            throw ConversionCancelled();
    }

private:
    std::atomic<bool> flag_{false};
};

/// Where a running conversion sends its output.
///
/// Both methods are called from the worker thread, possibly very often. An
/// implementation must be cheap and must not block: the Qt one turns each call
/// into a queued signal and throttles progress before emitting.
class ProgressSink
{
public:
    virtual ~ProgressSink() = default;

    virtual void log(Severity severity, std::string_view stage,
                     std::string_view message) = 0;

    /// `done`/`total` are in abstract work units (see Converter's weighting).
    /// Counts are passed by value rather than accumulated by the sink so that a
    /// future thread pool can aggregate them -- the Myst pipeline's equivalent
    /// closure captured a `nonlocal done` and is serial-only as a result.
    virtual void progress(std::uint64_t done, std::uint64_t total,
                          std::string_view stage, std::string_view detail) = 0;

    // Convenience wrappers.
    void info(std::string_view stage, std::string_view m) { log(Severity::Info, stage, m); }
    void warn(std::string_view stage, std::string_view m) { log(Severity::Warning, stage, m); }
    void error(std::string_view stage, std::string_view m) { log(Severity::Error, stage, m); }

    /// printf-style, for the many call sites that need to interpolate ids.
    void logf(Severity severity, std::string_view stage, const char *fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 4, 5)))
#endif
        ;
};

/// A sink that discards everything. Useful in tests and as a default.
class NullProgressSink final : public ProgressSink
{
public:
    void log(Severity, std::string_view, std::string_view) override {}
    void progress(std::uint64_t, std::uint64_t, std::string_view, std::string_view) override {}
};

/// Counts what it is told, so a caller can report a summary without keeping its
/// own tallies. Composable: wraps another sink and forwards to it.
class CountingProgressSink final : public ProgressSink
{
public:
    explicit CountingProgressSink(ProgressSink &inner) : inner_(inner) {}

    void log(Severity severity, std::string_view stage, std::string_view message) override
    {
        if (severity == Severity::Warning)
            ++warnings_;
        else if (severity == Severity::Error)
            ++errors_;
        inner_.log(severity, stage, message);
    }

    void progress(std::uint64_t done, std::uint64_t total, std::string_view stage,
                  std::string_view detail) override
    {
        inner_.progress(done, total, stage, detail);
    }

    int warnings() const { return warnings_; }
    int errors() const { return errors_; }

private:
    ProgressSink &inner_;
    int warnings_ = 0;
    int errors_ = 0;
};

} // namespace riven

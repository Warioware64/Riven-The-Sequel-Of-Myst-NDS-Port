#pragma once

// The bridge between the Qt-free core and the Qt UI.
//
// The rule is absolute and is the one thing most likely to be got wrong here:
// no widget is ever touched from the worker thread. The core calls into
// GuiProgressSink on the worker; the sink emits Qt signals; those cross to the
// UI thread as queued connections, which is Qt's own version of the queue-plus-
// pump the Myst converter had to build by hand.
//
// Two rate limits are deliberate, because Qt's event queue will happily accept
// a flood and then make the window unresponsive while it drains:
//
//   * progress is throttled to ~20 emissions a second rather than one per
//     asset (a full conversion touches tens of thousands of assets);
//   * log lines are batched, and the widget on the other end caps its own
//     scrollback.

#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include "riven/Converter.hpp"
#include "riven/Preflight.hpp"
#include "riven/Options.hpp"
#include "riven/Progress.hpp"

/// A log line as it crosses to the UI thread.
struct LogLine
{
    int severity = 0; ///< riven::Severity
    QString stage;
    QString message;
};
Q_DECLARE_METATYPE(LogLine)

// Passed by value across the thread boundary so the window can recompute the
// estimate and the checks from a cached census instead of re-scanning.
Q_DECLARE_METATYPE(riven::SourceInfo)

/// Lives on the worker thread; owns the actual conversion.
class ConversionWorker : public QObject
{
    Q_OBJECT

public:
    ConversionWorker() = default;

    /// Called from the UI thread BEFORE the run starts, and (for cancel) while
    /// it is running. CancelToken is atomic, so this is safe.
    riven::CancelToken &cancelToken() { return cancel_; }

public slots:
    /// Entry point, invoked on the worker thread by a queued connection.
    void run(const riven::Options &opts);
    /// Scan a source directory and report back. Cheap, but not free -- a full
    /// install means opening a dozen multi-hundred-megabyte archives -- so it
    /// runs here rather than blocking the UI on every keystroke.
    void scan(const QString &path);

signals:
    void progress(quint64 done, quint64 total, const QString &stage, const QString &detail);
    void logged(const LogLine &line);
    void finished(int outcome, const QString &message, const QString &summary);
    void scanned(const riven::SourceInfo &info);

private:
    riven::CancelToken cancel_;
};

/// ProgressSink that forwards to a ConversionWorker's signals.
class GuiProgressSink final : public riven::ProgressSink
{
public:
    explicit GuiProgressSink(ConversionWorker &worker) : worker_(worker) { clock_.start(); }

    void log(riven::Severity severity, std::string_view stage,
             std::string_view message) override;

    void progress(std::uint64_t done, std::uint64_t total, std::string_view stage,
                  std::string_view detail) override;

private:
    ConversionWorker &worker_;
    QElapsedTimer clock_;
    qint64 lastEmitMs_ = -1;
};

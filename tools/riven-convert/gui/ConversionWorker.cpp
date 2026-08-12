#include "ConversionWorker.hpp"

#include "riven/Preflight.hpp"

namespace
{
    /// Minimum gap between progress emissions. 50 ms is well under the
    /// threshold at which a bar looks like it is stuttering, and it turns tens
    /// of thousands of per-asset callbacks into a few hundred UI events.
    constexpr qint64 kProgressIntervalMs = 50;

    QString qs(std::string_view v)
    {
        return QString::fromUtf8(v.data(), static_cast<qsizetype>(v.size()));
    }
} // namespace

void GuiProgressSink::log(riven::Severity severity, std::string_view stage,
                          std::string_view message)
{
    // Every log line is delivered -- unlike progress, these are the record of
    // what happened and dropping one would lose a warning about damaged data.
    // The widget caps its own scrollback instead.
    LogLine line;
    line.severity = static_cast<int>(severity);
    line.stage = qs(stage);
    line.message = qs(message);
    emit worker_.logged(line);
}

void GuiProgressSink::progress(std::uint64_t done, std::uint64_t total,
                               std::string_view stage, std::string_view detail)
{
    const qint64 now = clock_.elapsed();
    const bool last = done >= total;
    if (!last && lastEmitMs_ >= 0 && now - lastEmitMs_ < kProgressIntervalMs)
        return;
    lastEmitMs_ = now;

    emit worker_.progress(static_cast<quint64>(done), static_cast<quint64>(total),
                          qs(stage), qs(detail));
}

void ConversionWorker::run(const riven::Options &opts)
{
    cancel_.reset();

    GuiProgressSink sink(*this);
    riven::Converter converter;
    const riven::ConversionResult result = converter.run(opts, sink, cancel_);

    emit finished(static_cast<int>(result.outcome), QString::fromStdString(result.message),
                  QString::fromStdString(result.summary()));
}

void ConversionWorker::scan(const QString &path)
{
    // Opening every archive in a full install takes seconds, which is exactly
    // why this runs here and not on the UI thread. The window caches what comes
    // back so that toggling a checkbox costs nothing.
    emit scanned(riven::scanSource(path.toStdString()));
}

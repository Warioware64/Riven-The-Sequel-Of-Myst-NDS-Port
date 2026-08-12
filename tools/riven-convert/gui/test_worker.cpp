// Exercises the GUI's threading contract without a window.
//
// The window itself is mostly layout, but the worker is where the interesting
// failure modes live: signals arriving on the wrong thread, progress flooding
// the event queue, cancellation never being observed. Those are exactly the
// bugs that are miserable to reproduce by hand and trivial to assert here.
//
// Needs RIVEN_TEST_DATA; skips and passes without it, like the other tests.

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "ConversionWorker.hpp"

namespace
{
    int g_failures = 0;

    void check(bool cond, const char *what)
    {
        if (!cond)
        {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++g_failures;
        }
    }
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<LogLine>("LogLine");
    qRegisterMetaType<riven::SourceInfo>("riven::SourceInfo");
    qRegisterMetaType<riven::Options>("riven::Options");

    const char *dataEnv = std::getenv("RIVEN_TEST_DATA");
    if (dataEnv == nullptr || dataEnv[0] == '\0')
    {
        std::printf("gui worker: skipped (set RIVEN_TEST_DATA)\n");
        return 0;
    }

    const QString dest =
        QStringLiteral("%1/riven-gui-worker-test").arg(QDir::tempPath());
    QDir(dest).removeRecursively();

    QThread thread;
    auto *worker = new ConversionWorker;
    worker->moveToThread(&thread);
    thread.start();

    const auto uiThread = QThread::currentThread();

    // --- scan ---------------------------------------------------------------
    {
        QSignalSpy scanned(worker, &ConversionWorker::scanned);
        bool onUiThread = true;
        QObject::connect(worker, &ConversionWorker::scanned, &app,
                         [&](const riven::SourceInfo &) {
                             // The whole point of a queued connection: the slot
                             // must run where the receiver lives, not on the
                             // worker. If this ever fails, every widget touch
                             // in the real window is a data race.
                             onUiThread = QThread::currentThread() == uiThread;
                         });

        QMetaObject::invokeMethod(worker, "scan", Qt::QueuedConnection,
                                  Q_ARG(QString, QString::fromUtf8(dataEnv)));
        check(scanned.wait(120000), "the scan completes and reports back");
        check(onUiThread, "scanned is delivered on the receiving thread");
        if (!scanned.isEmpty())
        {
            const auto info = scanned.at(0).at(0).value<riven::SourceInfo>();
            check(info.ok(), "the scan found Riven data");
            check(info.totalCards > 0, "the scan counted cards");
        }
    }

    // --- a real (small) conversion -----------------------------------------
    {
        riven::Options opts;
        opts.source = dataEnv;
        opts.dest = dest.toStdString();
        opts.cards = true;
        opts.images = opts.hires = opts.water = false;
        opts.stacks.insert(rivendata::StackId::Aspit);
        opts.normalise();

        QSignalSpy progress(worker, &ConversionWorker::progress);
        QSignalSpy finished(worker, &ConversionWorker::finished);

        QElapsedTimer clock;
        clock.start();
        QMetaObject::invokeMethod(worker, "run", Qt::QueuedConnection,
                                  Q_ARG(riven::Options, opts));
        check(finished.wait(300000), "the conversion finishes");

        if (!finished.isEmpty())
        {
            const int outcome = finished.at(0).at(0).toInt();
            check(outcome == static_cast<int>(riven::ConversionResult::Outcome::Ok),
                  "the conversion reports success");
        }
        check(progress.count() > 0, "progress was reported");

        // Throttling: the core emits one progress call per asset, and aspit
        // alone is several hundred. If that reached the event queue unthrottled
        // a full run would flood the UI, so assert the rate is bounded rather
        // than merely "some progress happened".
        const qint64 elapsed = std::max<qint64>(clock.elapsed(), 1);
        const double perSecond = 1000.0 * progress.count() / static_cast<double>(elapsed);
        std::printf("  %lld progress events in %lld ms (%.1f/s)\n",
                    static_cast<long long>(progress.count()),
                    static_cast<long long>(elapsed), perSecond);
        check(perSecond < 60.0, "progress emission stays under the throttle rate");

        check(QFile::exists(dest + QStringLiteral("/_nds/riven_nds/data/stacks/aspit.bin")),
              "the conversion actually wrote aspit.bin");
    }

    // --- cancellation -------------------------------------------------------
    {
        riven::Options opts;
        opts.source = dataEnv;
        opts.dest = dest.toStdString();
        opts.cards = opts.images = opts.hires = true;
        opts.water = false;
        opts.force = true; // make sure there is real work to interrupt
        opts.normalise();

        QSignalSpy finished(worker, &ConversionWorker::finished);
        QMetaObject::invokeMethod(worker, "run", Qt::QueuedConnection,
                                  Q_ARG(riven::Options, opts));

        // Let it get going, then stop it. Cancellation is cooperative, so this
        // also proves the token is actually polled inside the hot loop.
        QThread::msleep(1500);
        worker->cancelToken().cancel();

        check(finished.wait(60000), "a cancelled conversion still finishes promptly");
        if (!finished.isEmpty())
        {
            const int outcome = finished.at(0).at(0).toInt();
            check(outcome == static_cast<int>(riven::ConversionResult::Outcome::Cancelled),
                  "the outcome is reported as cancelled");
        }
    }

    thread.quit();
    thread.wait();
    QDir(dest).removeRecursively();

    if (g_failures == 0)
        std::printf("gui worker: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}

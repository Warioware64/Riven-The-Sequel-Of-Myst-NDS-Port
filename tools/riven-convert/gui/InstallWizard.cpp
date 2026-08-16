#include "InstallWizard.hpp"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QSettings>
#include <QTimer>

#include "Banner.hpp"
#include "WizardPages.hpp"

namespace
{
    /// Debounce on the source path. Scanning opens every archive in the
    /// install, so it must not fire on every keystroke.
    constexpr int kScanDebounceMs = 400;
} // namespace

QString findRomBesideConverter()
{
    const QString name = QStringLiteral("riven-nds-port.nds");

    // Beside the binary first, which is the release layout, then upwards, which
    // is running out of build/convert/gui/ during development -- three levels
    // gets from there to the repository root, and the fourth is slack.
    QDir dir(QCoreApplication::applicationDirPath());
    for (int up = 0; up <= 4; ++up)
    {
        const QString candidate = dir.filePath(name);
        if (QFileInfo(candidate).isFile())
            return QDir::toNativeSeparators(QFileInfo(candidate).absoluteFilePath());
        if (!dir.cdUp())
            break;
    }
    return {};
}

InstallWizard::InstallWizard()
{
    setWindowTitle(tr("Riven for Nintendo DS"));

    qRegisterMetaType<LogLine>("LogLine");
    qRegisterMetaType<riven::SourceInfo>("riven::SourceInfo");
    qRegisterMetaType<riven::Options>("riven::Options");

    // --- the chrome ---------------------------------------------------------
    //
    // ModernStyle is the one that gives a header strip with a title and a
    // subtitle over the interior pages and a watermark down the side of the
    // pages that ask for one. Which page gets which is decided by the page
    // itself: a subtitle produces the header, a watermark pixmap produces the
    // watermark, and Welcome and Finished set the second rather than the first.
    setWizardStyle(QWizard::ModernStyle);
    setPixmap(QWizard::BannerPixmap, banner::headerBanner());
    // The header is sized to the banner, so a Welcome page narrower than it
    // would snap wider the moment the first header appeared. Start at the
    // figure the rest of the wizard is going to be anyway.
    resize(banner::headerBannerWidth(), 620);
    setPixmap(QWizard::LogoPixmap, banner::headerLogo());
    setOption(QWizard::NoBackButtonOnStartPage, true);
    setOption(QWizard::NoCancelButtonOnLastPage, true);
    // Every page reads fields from every other page -- the Ready recap is the
    // whole of the user's answers on one screen -- so pages must not be rebuilt
    // when they are revisited.
    setOption(QWizard::IndependentPages, false);
    setButtonText(QWizard::CommitButton, tr("Install"));

    // --- the worker ---------------------------------------------------------
    worker_ = new ConversionWorker;
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &ConversionWorker::scanned, this, &InstallWizard::onScanned);
    workerThread_.start();

    scanDebounce_ = new QTimer(this);
    scanDebounce_->setSingleShot(true);
    scanDebounce_->setInterval(kScanDebounceMs);
    connect(scanDebounce_, &QTimer::timeout, this, &InstallWizard::rescan);

    // --- the pages ----------------------------------------------------------
    auto *install = new InstallPage(this);
    setPage(Page_Welcome, new WelcomePage(this));
    setPage(Page_Source, new SourcePage(this));
    setPage(Page_Dest, new DestPage(this));
    setPage(Page_Stages, new StagesPage(this));
    setPage(Page_Ready, new ReadyPage(this));
    setPage(Page_Install, install);
    setPage(Page_Finish, new FinishPage(this, install));
    setStartId(Page_Welcome);

    loadSettings();
    // Nothing is computed here on purpose. The first scan runs on the worker and
    // everything else follows from it, so the window appears immediately even
    // when the saved source is a full install on a slow disk.
    if (!field(QStringLiteral("source")).toString().isEmpty())
        scanDebounce_->start();
}

InstallWizard::~InstallWizard()
{
    workerThread_.quit();
    workerThread_.wait();
}

void InstallWizard::reject()
{
    if (running_ && !finished_)
    {
        // Cancellation is cooperative, so the worker needs a moment to unwind.
        // Partial output is complete and valid, so there is nothing to lose by
        // stopping -- but the thread must not be killed mid-write.
        worker_->cancelToken().cancel();
        workerThread_.quit();
        if (!workerThread_.wait(5000))
        {
            QMessageBox::warning(this, tr("Still finishing"),
                                 tr("The conversion is still stopping. Give it a moment "
                                    "and try again."));
            return;
        }
        running_ = false;
    }
    saveSettings();
    QWizard::reject();
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

void InstallWizard::requestScan()
{
    // The path is read off the field when the timer fires rather than captured
    // here, so a burst of keystrokes ends in one scan of the final text.
    scanDebounce_->start();
}

void InstallWizard::rescan()
{
    if (running_)
        return;
    QMetaObject::invokeMethod(worker_, "scan", Qt::QueuedConnection,
                              Q_ARG(QString, field(QStringLiteral("source")).toString()));
}

void InstallWizard::onScanned(const riven::SourceInfo &info)
{
    scanned_ = info;
    haveScan_ = true;
    emit scanFinished(info);
}

// ---------------------------------------------------------------------------
// Fields -> Options
// ---------------------------------------------------------------------------

riven::Options InstallWizard::currentOptions() const
{
    const auto str = [this](const char *name) {
        return field(QLatin1String(name)).toString().toStdString();
    };
    const auto flag = [this](const char *name) {
        return field(QLatin1String(name)).toBool();
    };

    riven::Options o;
    o.source = str("source");
    o.dest = str("dest");
    o.cards = flag("cards");
    o.images = flag("images");
    o.hires = flag("hires");
    o.water = flag("water");
    o.audio = flag("audio");
    o.video = flag("video");
    o.cursors = flag("cursors");
    o.extras = flag("extras");
    o.force = flag("force");
    o.ffmpegPath = str("ffmpeg");
    o.copyRom = flag("copyRom");
    if (o.copyRom)
        o.romPath = str("romPath");
    o.makeImage = flag("makeImage");
    if (o.makeImage)
        o.imagePath = str("imagePath");
    o.normalise();
    return o;
}

void InstallWizard::applyStages(const riven::Options &o)
{
    setField(QStringLiteral("cards"), o.cards);
    setField(QStringLiteral("images"), o.images);
    setField(QStringLiteral("hires"), o.hires);
    setField(QStringLiteral("water"), o.water);
    setField(QStringLiteral("audio"), o.audio);
    setField(QStringLiteral("video"), o.video);
    setField(QStringLiteral("cursors"), o.cursors);
    setField(QStringLiteral("extras"), o.extras);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void InstallWizard::loadSettings()
{
    QSettings s;
    // Every read is type-checked. A settings file written by a newer version
    // must not be able to push an unexpected type into a widget -- the Myst
    // converter's prefs module learned that one and it is cheap to keep.
    const auto boolOr = [&s](const char *key, bool fallback) {
        const QVariant v = s.value(QLatin1String(key));
        return v.canConvert<bool>() ? v.toBool() : fallback;
    };
    const auto stringOr = [&s](const char *key) {
        const QVariant v = s.value(QLatin1String(key));
        return v.canConvert<QString>() ? v.toString() : QString();
    };

    // The keys are the ones the single window used, so anyone who already
    // converted once keeps their paths and their stage selection.
    setField(QStringLiteral("source"), stringOr("source"));
    setField(QStringLiteral("dest"), stringOr("dest"));
    setField(QStringLiteral("ffmpeg"), stringOr("ffmpeg"));
    setField(QStringLiteral("cards"), boolOr("cards", true));
    setField(QStringLiteral("images"), boolOr("images", true));
    setField(QStringLiteral("hires"), boolOr("hires", true));
    setField(QStringLiteral("water"), boolOr("water", true));
    setField(QStringLiteral("audio"), boolOr("audio", true));
    setField(QStringLiteral("video"), boolOr("video", true));
    setField(QStringLiteral("cursors"), boolOr("cursors", true));
    setField(QStringLiteral("extras"), boolOr("extras", true));
    setField(QStringLiteral("force"), boolOr("force", false));

    // The ROM is found rather than remembered when there is nothing saved: a
    // release folder holds the converter and the .nds side by side, and asking
    // where the game is when it is in the same directory is a silly question.
    const QString savedRom = stringOr("romPath");
    const QString rom = savedRom.isEmpty() ? findRomBesideConverter() : savedRom;
    setField(QStringLiteral("romPath"), rom);
    setField(QStringLiteral("copyRom"),
             boolOr("copyRom", !rom.isEmpty() && QFileInfo(rom).isFile()));

    // Off by default, unlike the ROM: an emulator image is a second copy of a
    // multi-gigabyte conversion, and nobody should get one they did not ask for.
    setField(QStringLiteral("imagePath"), stringOr("imagePath"));
    setField(QStringLiteral("makeImage"), boolOr("makeImage", false));

    restoreGeometry(s.value(QStringLiteral("geometry")).toByteArray());
}

void InstallWizard::saveSettings()
{
    QSettings s;
    s.setValue(QStringLiteral("source"), field(QStringLiteral("source")));
    s.setValue(QStringLiteral("dest"), field(QStringLiteral("dest")));
    s.setValue(QStringLiteral("ffmpeg"), field(QStringLiteral("ffmpeg")));
    s.setValue(QStringLiteral("cards"), field(QStringLiteral("cards")));
    s.setValue(QStringLiteral("images"), field(QStringLiteral("images")));
    s.setValue(QStringLiteral("hires"), field(QStringLiteral("hires")));
    s.setValue(QStringLiteral("water"), field(QStringLiteral("water")));
    s.setValue(QStringLiteral("audio"), field(QStringLiteral("audio")));
    s.setValue(QStringLiteral("video"), field(QStringLiteral("video")));
    s.setValue(QStringLiteral("cursors"), field(QStringLiteral("cursors")));
    s.setValue(QStringLiteral("extras"), field(QStringLiteral("extras")));
    s.setValue(QStringLiteral("force"), field(QStringLiteral("force")));
    s.setValue(QStringLiteral("copyRom"), field(QStringLiteral("copyRom")));
    s.setValue(QStringLiteral("romPath"), field(QStringLiteral("romPath")));
    s.setValue(QStringLiteral("makeImage"), field(QStringLiteral("makeImage")));
    s.setValue(QStringLiteral("imagePath"), field(QStringLiteral("imagePath")));
    s.setValue(QStringLiteral("geometry"), saveGeometry());
}

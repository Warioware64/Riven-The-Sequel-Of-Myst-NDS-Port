#include "MainWindow.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "riven/Preflight.hpp"

namespace
{
    /// The log can run to tens of thousands of lines. QPlainTextEdit's own
    /// block cap does for free what the Myst GUI had to do by hand at
    /// 4000/2500 lines, and for the same reason: an unbounded text widget makes
    /// the whole window sluggish long before the conversion finishes.
    constexpr int kMaxLogBlocks = 5000;

    /// Debounce on the source path. Scanning opens every archive in the
    /// install, so it must not fire on every keystroke.
    constexpr int kScanDebounceMs = 400;

    QString human(quint64 bytes)
    {
        return QString::fromStdString(riven::humanBytes(bytes));
    }

    QString hms(qint64 ms)
    {
        const qint64 s = ms / 1000;
        if (s >= 3600)
            return QStringLiteral("%1h %2m").arg(s / 3600).arg((s % 3600) / 60);
        if (s >= 60)
            return QStringLiteral("%1m %2s").arg(s / 60).arg(s % 60);
        return QStringLiteral("%1s").arg(s);
    }
} // namespace

MainWindow::MainWindow()
{
    setWindowTitle(tr("Riven -> Nintendo DS asset converter"));
    resize(980, 860);

    qRegisterMetaType<LogLine>("LogLine");
    qRegisterMetaType<riven::SourceInfo>("riven::SourceInfo");
    qRegisterMetaType<riven::Options>("riven::Options");

    worker_ = new ConversionWorker;
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &ConversionWorker::progress, this, &MainWindow::onProgress);
    connect(worker_, &ConversionWorker::logged, this, &MainWindow::onLogged);
    connect(worker_, &ConversionWorker::finished, this, &MainWindow::onFinished);
    connect(worker_, &ConversionWorker::scanned, this, &MainWindow::onScanned);
    workerThread_.start();

    scanDebounce_ = new QTimer(this);
    scanDebounce_->setSingleShot(true);
    scanDebounce_->setInterval(kScanDebounceMs);
    connect(scanDebounce_, &QTimer::timeout, this, &MainWindow::rescanSource);

    auto *central = new QWidget;
    auto *layout = new QVBoxLayout(central);
    layout->addWidget(buildSourceGroup());
    layout->addWidget(buildDestGroup());
    layout->addWidget(buildFFmpegGroup());
    layout->addWidget(buildStagesGroup());

    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(buildChecksGroup());
    splitter->addWidget(buildLogGroup());
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    layout->addWidget(buildProgressGroup());
    setCentralWidget(central);

    loadSettings();
    // Nothing is computed here on purpose. The first scan runs on the worker
    // and everything else follows from it, so the window appears immediately
    // even when the saved source is a full install on a slow disk.
    refreshEstimate();
    if (!sourceEdit_->text().isEmpty())
    {
        sourceStatus_->setText(tr("Looking..."));
        scanDebounce_->start();
    }
}

MainWindow::~MainWindow()
{
    workerThread_.quit();
    workerThread_.wait();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (running_)
    {
        // Cancellation is cooperative, so the worker needs a moment to unwind.
        // Partial output is complete and valid, so there is nothing to lose by
        // closing -- but the thread must not be killed mid-write.
        worker_->cancelToken().cancel();
        workerThread_.quit();
        if (!workerThread_.wait(5000))
        {
            QMessageBox::warning(this, tr("Still finishing"),
                                 tr("The conversion is still stopping. Give it a moment "
                                    "and close again."));
            event->ignore();
            return;
        }
    }
    saveSettings();
    event->accept();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

QWidget *MainWindow::buildSourceGroup()
{
    auto *group = new QGroupBox(tr("Riven"));
    auto *layout = new QVBoxLayout(group);

    auto *row = new QHBoxLayout;
    sourceEdit_ = new QLineEdit;
    sourceEdit_->setPlaceholderText(
        tr("The folder holding Data/ and All/ -- an install or a mounted disc"));
    auto *browse = new QPushButton(tr("Browse..."));
    row->addWidget(sourceEdit_, 1);
    row->addWidget(browse);
    layout->addLayout(row);

    sourceStatus_ = new QLabel(tr("Choose your copy of Riven."));
    sourceStatus_->setWordWrap(true);
    layout->addWidget(sourceStatus_);

    connect(browse, &QPushButton::clicked, this, &MainWindow::browseSource);
    connect(sourceEdit_, &QLineEdit::textChanged, this, &MainWindow::sourcePathEdited);
    return group;
}

QWidget *MainWindow::buildDestGroup()
{
    auto *group = new QGroupBox(tr("Destination"));
    auto *layout = new QVBoxLayout(group);

    auto *row = new QHBoxLayout;
    destEdit_ = new QLineEdit;
    destEdit_->setPlaceholderText(tr("The root of your SD card, not the _nds folder"));
    auto *browse = new QPushButton(tr("Browse..."));
    row->addWidget(destEdit_, 1);
    row->addWidget(browse);
    layout->addLayout(row);

    destNote_ = new QLabel(tr("Files are written to <destination>/_nds/riven_nds/data/."));
    destNote_->setWordWrap(true);
    layout->addWidget(destNote_);

    connect(browse, &QPushButton::clicked, this, &MainWindow::browseDest);
    connect(destEdit_, &QLineEdit::textChanged, this, [this] { refreshChecks(); });
    return group;
}

QWidget *MainWindow::buildFFmpegGroup()
{
    // Its own group rather than a line in Destination: ffmpeg is the one thing
    // the converter needs that is not part of the game or the card, and it is
    // the one failure a user can fix without changing anything about the
    // conversion. Reporting it here, before Start, is the whole point --
    // discovering it at movie 1 of 1055 is two hours too late.
    auto *group = new QGroupBox(tr("ffmpeg"));
    auto *layout = new QVBoxLayout(group);

    auto *row = new QHBoxLayout;
    ffmpegEdit_ = new QLineEdit;
    ffmpegEdit_->setPlaceholderText(tr("Leave empty to use the ffmpeg on your PATH"));
    auto *browse = new QPushButton(tr("Browse..."));
    row->addWidget(ffmpegEdit_, 1);
    row->addWidget(browse);
    layout->addLayout(row);

    ffmpegStatus_ = new QLabel;
    ffmpegStatus_->setWordWrap(true);
    layout->addWidget(ffmpegStatus_);

    connect(browse, &QPushButton::clicked, this, &MainWindow::browseFFmpeg);
    connect(ffmpegEdit_, &QLineEdit::textChanged, this, [this] { refreshChecks(); });
    return group;
}

QWidget *MainWindow::buildStagesGroup()
{
    auto *group = new QGroupBox(tr("What to convert"));
    auto *layout = new QVBoxLayout(group);

    auto *presetRow = new QHBoxLayout;
    presetRow->addWidget(new QLabel(tr("Preset:")));
    presetBox_ = new QComboBox;
    for (const auto &p : riven::presets())
        presetBox_->addItem(QString::fromUtf8(p.name), QString::fromUtf8(p.description));
    presetRow->addWidget(presetBox_);
    presetRow->addStretch(1);
    layout->addLayout(presetRow);

    auto *grid = new QGridLayout;
    // One line each, with the detail as a tooltip. A wrapped paragraph in a
    // settings grid clips as soon as the window is anything but wide, and the
    // detail is not something anyone reads twice.
    const auto addStage = [&](QCheckBox *&box, int row, const QString &label,
                              const QString &help, const QString &detail,
                              bool implemented) {
        box = new QCheckBox(label);
        box->setEnabled(implemented);
        box->setToolTip(detail.isEmpty() ? help : detail);
        auto *hint = new QLabel(help);
        hint->setEnabled(implemented);
        hint->setToolTip(box->toolTip());
        grid->addWidget(box, row, 0);
        grid->addWidget(hint, row, 1);
        if (implemented)
            connect(box, &QCheckBox::toggled, this, &MainWindow::stageToggled);
    };
    grid->setColumnStretch(1, 1);

    addStage(cardsBox_, 0, tr("Cards and scripts"),
             tr("Navigation, hotspots and puzzle logic. Small and fast."), {}, true);
    addStage(imagesBox_, 1, tr("Card art"), tr("Every view, scaled to the DS screen."), {},
             true);
    addStage(hiresBox_, 2, tr("Zoom art"), tr("Readable close-ups. The largest stage."),
             tr("Full-resolution copies of every view, so the in-game zoom can read "
                "keypads, journals and dome combinations. This is the largest single "
                "thing on the card; turn it off if space is tight."),
             true);
    addStage(waterBox_, 3, tr("Water effects"), tr("The rippling water animations."), {},
             true);
    addStage(audioBox_, 4, tr("Sound"), tr("Ambient tracks and sound effects."),
             tr("Every sound in the game, as mono ADPCM the DS decodes in hardware. "
                "The CD release's audio is copied through without being re-encoded, "
                "so it takes about as much room on the card as it does in the game."),
             true);
    addStage(videoBox_, 5, tr("Movies"), tr("Every cutscene and animation."),
             tr("Riven's 1055 movies, re-encoded for the DS. By far the longest stage "
                "of a conversion, and the largest thing on the card after the zoom art."),
             true);

    addStage(cursorsBox_, 6, tr("Cursors"), tr("Riven's own hand and pointer cursors."),
             tr("Read from riven.exe, or from program/arcriven.z on the CD release, "
                "where the executable is not a file. Absent on a Mac install, in which "
                "case the game falls back to a plain pointer."),
             true);
    addStage(extrasBox_, 7, tr("Inventory art"), tr("The books you carry."),
             tr("Atrus's journal, Catherine's journal and the trap book, read from "
                "extras.mhk -- which on the CD release is also inside arcriven.z. "
                "Without these there is no way to open a journal."),
             true);

    forceBox_ = new QCheckBox(tr("Rebuild everything"));
    forceBox_->setToolTip(
        tr("Off by default: anything already converted and still up to date is skipped, "
           "so a stopped conversion resumes and a re-run costs seconds."));
    grid->addWidget(forceBox_, 8, 0);
    connect(forceBox_, &QCheckBox::toggled, this, [this] { refreshChecks(); });

    layout->addLayout(grid);

    estimateLabel_ = new QLabel;
    layout->addWidget(estimateLabel_);

    connect(presetBox_, &QComboBox::currentIndexChanged, this, &MainWindow::presetChanged);
    return group;
}

QWidget *MainWindow::buildChecksGroup()
{
    auto *group = new QGroupBox(tr("Before starting"));
    auto *layout = new QVBoxLayout(group);
    checksList_ = new QListWidget;
    checksList_->setAlternatingRowColors(true);
    // Check details are sentences, not identifiers: wrapping them is far more
    // useful than a horizontal scrollbar the user has to drag.
    checksList_->setWordWrap(true);
    checksList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    checksList_->setMinimumHeight(120);
    layout->addWidget(checksList_);
    return group;
}

QWidget *MainWindow::buildLogGroup()
{
    logTabs_ = new QTabWidget;

    logView_ = new QPlainTextEdit;
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(kMaxLogBlocks);
    logView_->setLineWrapMode(QPlainTextEdit::NoWrap);
    logTabs_->addTab(logView_, tr("Log"));

    problemsView_ = new QListWidget;
    problemsView_->setAlternatingRowColors(true);
    // A conversion of a damaged copy of Riven emits warnings scattered through
    // tens of thousands of log lines. Collecting them here is the difference
    // between the user seeing them and not.
    logTabs_->addTab(problemsView_, tr("Problems"));

    return logTabs_;
}

QWidget *MainWindow::buildProgressGroup()
{
    auto *widget = new QWidget;
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);

    progressBar_ = new QProgressBar;
    progressBar_->setRange(0, 1000);
    progressBar_->setValue(0);
    layout->addWidget(progressBar_);

    auto *row = new QHBoxLayout;
    statusLabel_ = new QLabel(tr("Idle."));
    etaLabel_ = new QLabel;
    row->addWidget(statusLabel_, 1);
    row->addWidget(etaLabel_);
    layout->addLayout(row);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    openButton_ = new QPushButton(tr("Open output"));
    openButton_->setEnabled(false);
    startButton_ = new QPushButton(tr("Start"));
    startButton_->setDefault(true);
    buttons->addWidget(openButton_);
    buttons->addWidget(startButton_);
    layout->addLayout(buttons);

    connect(startButton_, &QPushButton::clicked, this, &MainWindow::startOrStop);
    connect(openButton_, &QPushButton::clicked, this, &MainWindow::openOutput);
    return widget;
}

// ---------------------------------------------------------------------------
// Options <-> widgets
// ---------------------------------------------------------------------------

riven::Options MainWindow::currentOptions() const
{
    riven::Options o;
    o.source = sourceEdit_->text().toStdString();
    o.dest = destEdit_->text().toStdString();
    o.cards = cardsBox_->isChecked();
    o.images = imagesBox_->isChecked();
    o.hires = hiresBox_->isChecked();
    o.water = waterBox_->isChecked();
    o.audio = audioBox_->isChecked();
    o.video = videoBox_->isChecked();
    o.cursors = cursorsBox_->isChecked();
    o.extras = extrasBox_->isChecked();
    o.force = forceBox_->isChecked();
    o.ffmpegPath = ffmpegEdit_->text().toStdString();
    o.normalise();
    return o;
}

void MainWindow::applyOptions(const riven::Options &o)
{
    cardsBox_->setChecked(o.cards);
    imagesBox_->setChecked(o.images);
    hiresBox_->setChecked(o.hires);
    waterBox_->setChecked(o.water);
    audioBox_->setChecked(o.audio);
    videoBox_->setChecked(o.video);
    cursorsBox_->setChecked(o.cursors);
    extrasBox_->setChecked(o.extras);
}

void MainWindow::presetChanged(int index)
{
    if (updatingPreset_ || index < 0)
        return;
    const auto &all = riven::presets();
    if (index >= static_cast<int>(all.size()))
        return;

    presetBox_->setToolTip(presetBox_->itemData(index).toString());
    if (QString::fromUtf8(all[static_cast<std::size_t>(index)].name) == QLatin1String("Custom"))
        return;

    updatingPreset_ = true;
    applyOptions(all[static_cast<std::size_t>(index)].apply(currentOptions()));
    updatingPreset_ = false;

    refreshEstimate();
    refreshChecks();
}

void MainWindow::stageToggled()
{
    if (updatingPreset_)
        return;

    // Zoom art implies card art -- pics_hi/ on its own gives a game with no
    // cards. normalise() owns that rule; reflect it back so the boxes never
    // claim something the pipeline will not do.
    const riven::Options o = currentOptions();
    updatingPreset_ = true;
    applyOptions(o);
    const QString match = QString::fromStdString(riven::matchingPresetName(o));
    const int idx = presetBox_->findText(match);
    if (idx >= 0)
        presetBox_->setCurrentIndex(idx);
    updatingPreset_ = false;

    refreshEstimate();
    refreshChecks();
}

// ---------------------------------------------------------------------------
// Scanning and checks
// ---------------------------------------------------------------------------

void MainWindow::browseSource()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Where is Riven?"),
                                                          sourceEdit_->text());
    if (!dir.isEmpty())
        sourceEdit_->setText(dir);
}

void MainWindow::browseFFmpeg()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Locate ffmpeg"),
                                                      ffmpegEdit_->text());
    if (!file.isEmpty())
        ffmpegEdit_->setText(file);
}

void MainWindow::browseDest()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Card root"),
                                                          destEdit_->text());
    if (!dir.isEmpty())
        destEdit_->setText(dir);
}

void MainWindow::sourcePathEdited()
{
    sourceStatus_->setText(tr("Looking..."));
    scanDebounce_->start();
}

void MainWindow::rescanSource()
{
    if (running_)
        return;
    QMetaObject::invokeMethod(worker_, "scan", Qt::QueuedConnection,
                              Q_ARG(QString, sourceEdit_->text()));
}

void MainWindow::onScanned(const riven::SourceInfo &info)
{
    scanned_ = info;
    haveScan_ = true;

    sourceStatus_->setText(QString::fromStdString(info.summary()));
    sourceStatus_->setStyleSheet(info.ok() ? QStringLiteral("color: #2e9e4f;")
                                           : QStringLiteral("color: #d9534f;"));

    for (const auto id : info.source.missingStacks)
    {
        auto *item = new QListWidgetItem(
            tr("source: %1 is absent or unreadable")
                .arg(QString::fromUtf8(rivendata::stackName(id))));
        item->setForeground(QColor(0xc9, 0x8a, 0x00));
        problemsView_->addItem(item);
        ++problemCount_;
    }
    for (const auto &u : info.unreadable)
    {
        auto *item = new QListWidgetItem(
            tr("source: %1 could not be opened").arg(QString::fromStdString(u)));
        item->setForeground(QColor(0xc9, 0x8a, 0x00));
        problemsView_->addItem(item);
        ++problemCount_;
    }
    if (problemCount_ > 0)
        logTabs_->setTabText(1, tr("Problems (%1)").arg(problemCount_));

    refreshEstimate();
    refreshChecks();
}

void MainWindow::refreshEstimate()
{
    // From the cached census, never by re-reading the archives: this runs on
    // every checkbox toggle and a real scan takes seconds.
    if (!haveScan_)
    {
        estimateLabel_->setText(tr("Estimated output: -"));
        return;
    }
    const quint64 bytes = riven::estimateOutput(scanned_, currentOptions());
    estimateLabel_->setText(tr("Estimated output: about %1").arg(human(bytes)));
}

void MainWindow::refreshChecks()
{
    checksList_->clear();
    if (running_)
        return;

    // Independent of the scan: whether ffmpeg is there has nothing to do with
    // whether the source folder has been read yet, and it is the one thing worth
    // saying before anything else has been chosen.
    const riven::Check ff = riven::checkFFmpeg(currentOptions());
    ffmpegStatus_->setText(QString::fromStdString(ff.detail));
    ffmpegStatus_->setStyleSheet(ff.level == riven::Check::Level::Fail
                                     ? QStringLiteral("color: #d9534f;")
                                     : QString());

    if (!haveScan_)
        return; // nothing to judge until the first scan lands
    const auto checks = riven::runChecks(scanned_, currentOptions());

    for (const auto &c : checks)
    {
        const char *mark = c.level == riven::Check::Level::Fail   ? "✗"
                           : c.level == riven::Check::Level::Warn ? "!"
                                                                  : "✓";
        auto *item = new QListWidgetItem(
            QStringLiteral("%1  %2: %3")
                .arg(QString::fromUtf8(mark), QString::fromStdString(c.title),
                     QString::fromStdString(c.detail)));
        if (c.level == riven::Check::Level::Fail)
            item->setForeground(QColor(0xd9, 0x53, 0x4f));
        else if (c.level == riven::Check::Level::Warn)
            item->setForeground(QColor(0xc9, 0x8a, 0x00));
        checksList_->addItem(item);
    }

    startButton_->setEnabled(riven::checksPass(checks));
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

void MainWindow::startOrStop()
{
    if (running_)
    {
        worker_->cancelToken().cancel();
        startButton_->setEnabled(false);
        statusLabel_->setText(tr("Stopping..."));
        return;
    }

    logView_->clear();
    problemsView_->clear();
    problemCount_ = 0;
    logTabs_->setTabText(1, tr("Problems"));
    rateSamples_.clear();
    runClock_.start();

    setRunning(true);
    QMetaObject::invokeMethod(worker_, "run", Qt::QueuedConnection,
                              Q_ARG(riven::Options, currentOptions()));
}

void MainWindow::setRunning(bool running)
{
    running_ = running;
    startButton_->setText(running ? tr("Stop") : tr("Start"));
    startButton_->setEnabled(true);
    sourceEdit_->setEnabled(!running);
    destEdit_->setEnabled(!running);
    ffmpegEdit_->setEnabled(!running);
    presetBox_->setEnabled(!running);
    for (auto *box :
         {cardsBox_, imagesBox_, hiresBox_, waterBox_, audioBox_, videoBox_,
          cursorsBox_, extrasBox_, forceBox_})
        box->setEnabled(!running);
    if (running)
        openButton_->setEnabled(false);
}

void MainWindow::onProgress(quint64 done, quint64 total, const QString &stage,
                            const QString &detail)
{
    if (total == 0)
        return;
    const int permille = static_cast<int>(done * 1000 / total);
    progressBar_->setValue(permille);
    statusLabel_->setText(tr("%1 - %2  (%3%)")
                              .arg(stage, detail)
                              .arg(done * 100 / total));

    // ETA from a trailing window, so a slow stack early on stops dominating the
    // estimate once a fast one starts.
    const qint64 now = runClock_.elapsed();
    rateSamples_.append({now, done});
    while (rateSamples_.size() > 240)
        rateSamples_.removeFirst();

    if (rateSamples_.size() >= 8 && done < total)
    {
        const auto &oldest = rateSamples_.first();
        const qint64 dt = now - oldest.first;
        const quint64 dDone = done - oldest.second;
        if (dt > 0 && dDone > 0)
        {
            const double rate = static_cast<double>(dDone) / static_cast<double>(dt);
            const auto remain = static_cast<qint64>(static_cast<double>(total - done) / rate);
            etaLabel_->setText(tr("elapsed %1, about %2 left").arg(hms(now), hms(remain)));
        }
    }
}

void MainWindow::appendLog(const LogLine &line)
{
    const QString text = QStringLiteral("%1: %2").arg(line.stage, line.message);
    logView_->appendPlainText(text);

    if (line.severity == static_cast<int>(riven::Severity::Info))
        return;

    auto *item = new QListWidgetItem(text);
    item->setForeground(line.severity == static_cast<int>(riven::Severity::Error)
                            ? QColor(0xd9, 0x53, 0x4f)
                            : QColor(0xc9, 0x8a, 0x00));
    problemsView_->addItem(item);
    ++problemCount_;
    logTabs_->setTabText(1, tr("Problems (%1)").arg(problemCount_));
}

void MainWindow::onLogged(const LogLine &line)
{
    appendLog(line);
}

void MainWindow::onFinished(int outcome, const QString &message, const QString &summary)
{
    setRunning(false);
    progressBar_->setValue(outcome == static_cast<int>(riven::ConversionResult::Outcome::Ok)
                               ? 1000
                               : progressBar_->value());
    statusLabel_->setText(message);
    etaLabel_->clear();

    if (outcome == static_cast<int>(riven::ConversionResult::Outcome::Ok))
        statusLabel_->setStyleSheet(QStringLiteral("color: #2e9e4f;"));
    else if (outcome == static_cast<int>(riven::ConversionResult::Outcome::Cancelled))
        statusLabel_->setStyleSheet(QStringLiteral("color: #c98a00;"));
    else
        statusLabel_->setStyleSheet(QStringLiteral("color: #d9534f;"));

    appendLog({static_cast<int>(riven::Severity::Info), QStringLiteral("done"), summary});
    openButton_->setEnabled(!destEdit_->text().isEmpty());
    refreshChecks();
    saveSettings();
}

void MainWindow::openOutput()
{
    const auto dir = riven::Converter::dataDir(destEdit_->text().toStdString());
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(dir.string())));
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void MainWindow::loadSettings()
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

    sourceEdit_->setText(stringOr("source"));
    destEdit_->setText(stringOr("dest"));
    ffmpegEdit_->setText(stringOr("ffmpeg"));
    cardsBox_->setChecked(boolOr("cards", true));
    imagesBox_->setChecked(boolOr("images", true));
    hiresBox_->setChecked(boolOr("hires", true));
    waterBox_->setChecked(boolOr("water", true));
    audioBox_->setChecked(boolOr("audio", true));
    videoBox_->setChecked(boolOr("video", true));
    cursorsBox_->setChecked(boolOr("cursors", true));
    extrasBox_->setChecked(boolOr("extras", true));
    forceBox_->setChecked(boolOr("force", false));
    restoreGeometry(s.value(QStringLiteral("geometry")).toByteArray());

    updatingPreset_ = true;
    const int idx = presetBox_->findText(
        QString::fromStdString(riven::matchingPresetName(currentOptions())));
    if (idx >= 0)
        presetBox_->setCurrentIndex(idx);
    updatingPreset_ = false;
}

void MainWindow::saveSettings()
{
    QSettings s;
    s.setValue(QStringLiteral("source"), sourceEdit_->text());
    s.setValue(QStringLiteral("dest"), destEdit_->text());
    s.setValue(QStringLiteral("ffmpeg"), ffmpegEdit_->text());
    s.setValue(QStringLiteral("cards"), cardsBox_->isChecked());
    s.setValue(QStringLiteral("images"), imagesBox_->isChecked());
    s.setValue(QStringLiteral("hires"), hiresBox_->isChecked());
    s.setValue(QStringLiteral("water"), waterBox_->isChecked());
    s.setValue(QStringLiteral("audio"), audioBox_->isChecked());
    s.setValue(QStringLiteral("video"), videoBox_->isChecked());
    s.setValue(QStringLiteral("cursors"), cursorsBox_->isChecked());
    s.setValue(QStringLiteral("extras"), extrasBox_->isChecked());
    s.setValue(QStringLiteral("force"), forceBox_->isChecked());
    s.setValue(QStringLiteral("geometry"), saveGeometry());
}

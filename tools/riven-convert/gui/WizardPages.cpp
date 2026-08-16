#include "WizardPages.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "Banner.hpp"
#include "InstallWizard.hpp"
#include "riven/Converter.hpp"

namespace
{
    /// The log can run to tens of thousands of lines. QPlainTextEdit's own
    /// block cap does for free what the Myst GUI had to do by hand at
    /// 4000/2500 lines, and for the same reason: an unbounded text widget makes
    /// the whole window sluggish long before the conversion finishes.
    constexpr int kMaxLogBlocks = 5000;

    const QString kGreen = QStringLiteral("color: #2e9e4f;");
    const QString kAmber = QStringLiteral("color: #c98a00;");
    const QString kRed = QStringLiteral("color: #d9534f;");

    const QColor kAmberInk{0xc9, 0x8a, 0x00};
    const QColor kRedInk{0xd9, 0x53, 0x4f};

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

    /// A path field with a Browse button beside it, which is three of the four
    /// things this wizard asks for.
    QHBoxLayout *pathRow(QLineEdit *edit, QPushButton *browse)
    {
        auto *row = new QHBoxLayout;
        row->addWidget(edit, 1);
        row->addWidget(browse);
        return row;
    }
} // namespace

// ---------------------------------------------------------------------------
// Welcome
// ---------------------------------------------------------------------------

WelcomePage::WelcomePage(InstallWizard *wizard)
{
    Q_UNUSED(wizard)
    setTitle(tr("Riven for Nintendo DS"));
    // No subtitle, on purpose: ModernStyle draws the header strip only for
    // pages that have one, and this is a watermark page instead.
    setPixmap(QWizard::WatermarkPixmap, banner::watermark());

    auto *blurb = new QLabel(
        tr("<p>This converts your own copy of Riven into the form the DS port reads, "
           "and puts it on your card.</p>"
           "<p>You will need:</p>"
           "<ul>"
           "<li>Riven itself &mdash; an installed copy, a mounted disc, or the "
           "DVD/GOG release. Nothing from the game is included here.</li>"
           "<li><b>ffmpeg</b>, which the movies are decoded through. Any recent "
           "version on your PATH will do.</li>"
           "<li>Room on the card. A full conversion is several gigabytes and takes "
           "<b>hours</b> &mdash; the movies alone are 1055 of them.</li>"
           "</ul>"
           "<p>Stopping is safe at any point: what has been converted is complete, and "
           "starting again picks up where you left off.</p>"));
    blurb->setWordWrap(true);
    blurb->setTextFormat(Qt::RichText);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(blurb);
    layout->addStretch(1);
}

// ---------------------------------------------------------------------------
// Source
// ---------------------------------------------------------------------------

SourcePage::SourcePage(InstallWizard *wizard) : wizard_(wizard)
{
    setTitle(tr("Locate Riven"));
    setSubTitle(tr("Point the converter at your copy of the game."));

    edit_ = new QLineEdit;
    edit_->setPlaceholderText(
        tr("The folder holding Data/ and All/ -- an install or a mounted disc"));
    auto *browse = new QPushButton(tr("Browse..."));

    status_ = new QLabel(tr("Choose your copy of Riven."));
    status_->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(pathRow(edit_, browse));
    layout->addWidget(status_);
    layout->addStretch(1);

    registerField(QStringLiteral("source"), edit_);

    connect(browse, &QPushButton::clicked, this, &SourcePage::browse);
    connect(edit_, &QLineEdit::textChanged, this, [this] {
        status_->setText(tr("Looking..."));
        status_->setStyleSheet(QString());
        // Next must go dead the moment the path changes, not when the scan
        // comes back: between those two the wizard would still be holding the
        // previous install's census and would happily walk on with it.
        wizard_->requestScan();
        emit completeChanged();
    });
    connect(wizard_, &InstallWizard::scanFinished, this,
            [this](const riven::SourceInfo &info) {
                status_->setText(QString::fromStdString(info.summary()));
                status_->setStyleSheet(info.ok() ? kGreen : kRed);
                emit completeChanged();
            });
}

void SourcePage::browse()
{
    const QString dir =
        QFileDialog::getExistingDirectory(this, tr("Where is Riven?"), edit_->text());
    if (!dir.isEmpty())
        edit_->setText(QDir::toNativeSeparators(dir));
}

void SourcePage::initializePage()
{
    // Coming back to this page after editing elsewhere must not leave a stale
    // "Looking..." on screen when the scan already landed.
    if (wizard_->haveScan() && !edit_->text().isEmpty())
    {
        status_->setText(QString::fromStdString(wizard_->scan().summary()));
        status_->setStyleSheet(wizard_->scan().ok() ? kGreen : kRed);
    }
}

bool SourcePage::isComplete() const
{
    // A scan that has not landed is not a scan of THIS path, so an empty field
    // is disqualified separately: haveScan() stays true from a previous one.
    return !edit_->text().isEmpty() && wizard_->haveScan() && wizard_->scan().ok();
}

// ---------------------------------------------------------------------------
// Destination
// ---------------------------------------------------------------------------

DestPage::DestPage(InstallWizard *wizard) : wizard_(wizard)
{
    setTitle(tr("Choose the card"));
    setSubTitle(tr("Where the converted game is written."));

    edit_ = new QLineEdit;
    edit_->setPlaceholderText(tr("The root of your SD card, not the _nds folder"));
    auto *browse = new QPushButton(tr("Browse..."));

    note_ = new QLabel;
    note_->setWordWrap(true);

    // --- the game itself ----------------------------------------------------
    //
    // Its own group because it is the one thing on this page that is not about
    // the data. A card with the data and no .nds boots nothing, and "now copy
    // the ROM yourself" was the step this wizard existed to stop being a step.
    auto *romGroup = new QGroupBox(tr("The game"));
    auto *romLayout = new QVBoxLayout(romGroup);

    copyRom_ = new QCheckBox(tr("Also copy the game to the card"));
    copyRom_->setToolTip(tr("Copies the .nds to the root of the card, beside _nds/, "
                            "where a flashcart's loader looks for it. Done last, after "
                            "everything else, so the card is never left carrying the "
                            "game without its data."));
    romLayout->addWidget(copyRom_);

    romEdit_ = new QLineEdit;
    romEdit_->setPlaceholderText(tr("riven-nds-port.nds"));
    romBrowse_ = new QPushButton(tr("Browse..."));
    romLayout->addLayout(pathRow(romEdit_, romBrowse_));

    romStatus_ = new QLabel;
    romStatus_->setWordWrap(true);
    romLayout->addWidget(romStatus_);

    // --- the emulator image -------------------------------------------------
    //
    // A DS emulator has no card slot and will not read the folder above, so
    // without this the port is hardware-only. The image is packed FROM that
    // folder at the end of the run, which is why it lives on this page and not
    // as an alternative to it.
    auto *imageGroup = new QGroupBox(tr("For an emulator"));
    auto *imageLayout = new QVBoxLayout(imageGroup);

    makeImage_ = new QCheckBox(tr("Also make a card image an emulator can mount"));
    makeImage_->setToolTip(tr("One FAT file holding everything above, which melonDS and "
                              "DeSmuME can mount as the SD card. Packed after the "
                              "conversion from the folder above, so the run needs room "
                              "for both at once."));
    imageLayout->addWidget(makeImage_);

    imageEdit_ = new QLineEdit;
    imageEdit_->setPlaceholderText(tr("riven-card.bin"));
    imageBrowse_ = new QPushButton(tr("Browse..."));
    imageLayout->addLayout(pathRow(imageEdit_, imageBrowse_));

    imageStatus_ = new QLabel;
    imageStatus_->setWordWrap(true);
    imageLayout->addWidget(imageStatus_);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(pathRow(edit_, browse));
    layout->addWidget(note_);
    layout->addSpacing(8);
    layout->addWidget(romGroup);
    layout->addWidget(imageGroup);
    layout->addStretch(1);

    registerField(QStringLiteral("dest"), edit_);
    registerField(QStringLiteral("copyRom"), copyRom_);
    registerField(QStringLiteral("romPath"), romEdit_);
    registerField(QStringLiteral("makeImage"), makeImage_);
    registerField(QStringLiteral("imagePath"), imageEdit_);

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir =
            QFileDialog::getExistingDirectory(this, tr("Card root"), edit_->text());
        if (!dir.isEmpty())
            edit_->setText(QDir::toNativeSeparators(dir));
    });
    connect(romBrowse_, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(
            this, tr("Where is the game?"), romEdit_->text(),
            tr("Nintendo DS ROMs (*.nds);;All files (*)"));
        if (!file.isEmpty())
            romEdit_->setText(QDir::toNativeSeparators(file));
    });

    connect(imageBrowse_, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(
            this, tr("Where should the image go?"), imageEdit_->text(),
            tr("Card images (*.bin *.img);;All files (*)"));
        if (!file.isEmpty())
            imageEdit_->setText(QDir::toNativeSeparators(file));
    });

    connect(edit_, &QLineEdit::textChanged, this, &DestPage::refresh);
    connect(romEdit_, &QLineEdit::textChanged, this, &DestPage::refresh);
    connect(copyRom_, &QCheckBox::toggled, this, &DestPage::refresh);
    connect(imageEdit_, &QLineEdit::textChanged, this, &DestPage::refresh);
    connect(makeImage_, &QCheckBox::toggled, this, &DestPage::refresh);
}

void DestPage::initializePage()
{
    refresh();
}

void DestPage::refresh()
{
    const bool wantRom = copyRom_->isChecked();
    romEdit_->setEnabled(wantRom);
    romBrowse_->setEnabled(wantRom);

    note_->setText(edit_->text().isEmpty()
                       ? tr("Files are written to <destination>/_nds/riven_nds/data/.")
                       : tr("Files are written to %1.")
                             .arg(QDir::toNativeSeparators(QString::fromStdString(
                                 riven::Converter::dataDir(edit_->text().toStdString())
                                     .string()))));

    // Reported from the core rather than judged here, so this page and the
    // Ready page cannot disagree about whether the ROM is findable.
    riven::Options probe;
    probe.copyRom = wantRom;
    probe.romPath = romEdit_->text().toStdString();
    probe.makeImage = makeImage_->isChecked();
    probe.imagePath = imageEdit_->text().toStdString();
    probe.dest = edit_->text().toStdString();

    const riven::Check rom = riven::checkRom(probe);
    romStatus_->setText(wantRom ? QString::fromStdString(rom.detail) : QString());
    romStatus_->setStyleSheet(rom.level == riven::Check::Level::Fail ? kRed : QString());

    const bool wantImage = makeImage_->isChecked();
    imageEdit_->setEnabled(wantImage);
    imageBrowse_->setEnabled(wantImage);
    const riven::Check img = riven::checkImage(probe);
    imageStatus_->setText(wantImage ? QString::fromStdString(img.detail) : QString());
    imageStatus_->setStyleSheet(img.level == riven::Check::Level::Fail ? kRed : QString());

    emit completeChanged();
}

bool DestPage::isComplete() const
{
    if (edit_->text().isEmpty())
        return false;

    riven::Options probe;
    probe.dest = edit_->text().toStdString();
    probe.copyRom = copyRom_->isChecked();
    probe.romPath = romEdit_->text().toStdString();
    probe.makeImage = makeImage_->isChecked();
    probe.imagePath = imageEdit_->text().toStdString();

    return riven::checkRom(probe).level != riven::Check::Level::Fail
        && riven::checkImage(probe).level != riven::Check::Level::Fail;
}

// ---------------------------------------------------------------------------
// Stages
// ---------------------------------------------------------------------------

StagesPage::StagesPage(InstallWizard *wizard) : wizard_(wizard)
{
    setTitle(tr("What to install"));
    setSubTitle(tr("Everything fits on a large card. Leave things out to save room."));

    auto *layout = new QVBoxLayout(this);

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
            connect(box, &QCheckBox::toggled, this, &StagesPage::stageToggled);
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

    layout->addLayout(grid);

    estimateLabel_ = new QLabel;
    layout->addWidget(estimateLabel_);
    layout->addStretch(1);

    registerField(QStringLiteral("cards"), cardsBox_);
    registerField(QStringLiteral("images"), imagesBox_);
    registerField(QStringLiteral("hires"), hiresBox_);
    registerField(QStringLiteral("water"), waterBox_);
    registerField(QStringLiteral("audio"), audioBox_);
    registerField(QStringLiteral("video"), videoBox_);
    registerField(QStringLiteral("cursors"), cursorsBox_);
    registerField(QStringLiteral("extras"), extrasBox_);
    registerField(QStringLiteral("force"), forceBox_);

    connect(forceBox_, &QCheckBox::toggled, this, &StagesPage::refreshEstimate);
    connect(presetBox_, &QComboBox::currentIndexChanged, this, &StagesPage::presetChanged);
    // A scan can land while this page is already on screen -- editing the
    // source, then walking forward faster than a full install takes to read.
    // Without this the estimate would sit on "-" until something was toggled.
    connect(wizard_, &InstallWizard::scanFinished, this,
            [this] { refreshEstimate(); });
}

void StagesPage::initializePage()
{
    // The preset box follows the boxes rather than the other way round on entry:
    // the checkboxes may have come from saved settings, which name no preset.
    updatingPreset_ = true;
    const int idx = presetBox_->findText(
        QString::fromStdString(riven::matchingPresetName(wizard_->currentOptions())));
    if (idx >= 0)
        presetBox_->setCurrentIndex(idx);
    updatingPreset_ = false;

    refreshEstimate();
}

void StagesPage::presetChanged(int index)
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
    wizard_->applyStages(all[static_cast<std::size_t>(index)].apply(wizard_->currentOptions()));
    updatingPreset_ = false;

    refreshEstimate();
}

void StagesPage::stageToggled()
{
    if (updatingPreset_)
        return;

    // Zoom art implies card art -- pics_hi/ on its own gives a game with no
    // cards. normalise() owns that rule; reflect it back so the boxes never
    // claim something the pipeline will not do.
    const riven::Options o = wizard_->currentOptions();
    updatingPreset_ = true;
    wizard_->applyStages(o);
    const int idx = presetBox_->findText(QString::fromStdString(riven::matchingPresetName(o)));
    if (idx >= 0)
        presetBox_->setCurrentIndex(idx);
    updatingPreset_ = false;

    refreshEstimate();
}

void StagesPage::refreshEstimate()
{
    // From the cached census, never by re-reading the archives: this runs on
    // every checkbox toggle and a real scan takes seconds.
    if (!wizard_->haveScan())
    {
        estimateLabel_->setText(tr("Estimated output: -"));
        return;
    }
    const quint64 bytes = riven::estimateOutput(wizard_->scan(), wizard_->currentOptions());
    estimateLabel_->setText(tr("Estimated output: about %1").arg(human(bytes)));
}

// ---------------------------------------------------------------------------
// Ready
// ---------------------------------------------------------------------------

ReadyPage::ReadyPage(InstallWizard *wizard) : wizard_(wizard)
{
    setTitle(tr("Ready to install"));
    setSubTitle(tr("Everything that can be known before the run starts."));
    setCommitPage(true);

    auto *layout = new QVBoxLayout(this);

    recap_ = new QLabel;
    recap_->setWordWrap(true);
    recap_->setTextFormat(Qt::RichText);
    layout->addWidget(recap_);

    // ffmpeg lives HERE rather than on a page of its own, and can be edited on
    // the spot. It is the one condition a user can fix without changing
    // anything about the conversion, and sending them back three pages to fix
    // it -- or letting them find out at movie 1 of 1055 -- are both worse.
    auto *ffmpegGroup = new QGroupBox(tr("ffmpeg"));
    auto *ffmpegLayout = new QVBoxLayout(ffmpegGroup);
    ffmpegEdit_ = new QLineEdit;
    ffmpegEdit_->setPlaceholderText(tr("Leave empty to use the ffmpeg on your PATH"));
    auto *ffmpegBrowse = new QPushButton(tr("Browse..."));
    ffmpegLayout->addLayout(pathRow(ffmpegEdit_, ffmpegBrowse));
    layout->addWidget(ffmpegGroup);

    checksList_ = new QListWidget;
    checksList_->setAlternatingRowColors(true);
    // Check details are sentences, not identifiers: wrapping them is far more
    // useful than a horizontal scrollbar the user has to drag.
    checksList_->setWordWrap(true);
    checksList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    checksList_->setMinimumHeight(140);
    layout->addWidget(checksList_, 1);

    registerField(QStringLiteral("ffmpeg"), ffmpegEdit_);

    connect(ffmpegBrowse, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, tr("Locate ffmpeg"),
                                                          ffmpegEdit_->text());
        if (!file.isEmpty())
            ffmpegEdit_->setText(QDir::toNativeSeparators(file));
    });
    connect(ffmpegEdit_, &QLineEdit::textChanged, this, &ReadyPage::refresh);
    // Same reason as the estimate on the stages page: a scan that lands while
    // this page is showing must turn the check list into something, and the
    // Install button with it.
    connect(wizard_, &InstallWizard::scanFinished, this, [this] { refresh(); });
}

void ReadyPage::initializePage()
{
    refresh();
}

void ReadyPage::refresh()
{
    checksList_->clear();

    const riven::Options o = wizard_->currentOptions();

    QStringList stages;
    const auto note = [&stages](bool on, const QString &name) {
        if (on)
            stages << name;
    };
    note(o.cards, tr("cards"));
    note(o.images, tr("card art"));
    note(o.hires, tr("zoom art"));
    note(o.water, tr("water"));
    note(o.audio, tr("sound"));
    note(o.video, tr("movies"));
    note(o.cursors, tr("cursors"));
    note(o.extras, tr("inventory art"));

    QString recap = tr("<b>From:</b> %1<br><b>To:</b> %2<br><b>Installing:</b> %3")
                        .arg(QString::fromStdString(o.source.string()).toHtmlEscaped(),
                             QString::fromStdString(o.dest.string()).toHtmlEscaped(),
                             stages.isEmpty() ? tr("nothing") : stages.join(tr(", ")));
    if (o.makeImage)
    {
        recap += tr("<br><b>Emulator image:</b> %1")
                     .arg(QString::fromStdString(o.imagePath.string()).toHtmlEscaped());
    }
    recap_->setText(recap);

    if (!wizard_->haveScan())
    {
        pass_ = false;
        emit completeChanged();
        return;
    }

    const auto checks = riven::runChecks(wizard_->scan(), o);
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
            item->setForeground(kRedInk);
        else if (c.level == riven::Check::Level::Warn)
            item->setForeground(kAmberInk);
        checksList_->addItem(item);
    }

    pass_ = riven::checksPass(checks);
    emit completeChanged();
}

bool ReadyPage::isComplete() const
{
    return pass_;
}

// ---------------------------------------------------------------------------
// Installing
// ---------------------------------------------------------------------------

InstallPage::InstallPage(InstallWizard *wizard) : wizard_(wizard)
{
    setTitle(tr("Installing"));
    setSubTitle(tr("This runs for a long time. Stopping is safe."));

    runClock_ = new QElapsedTimer;

    auto *layout = new QVBoxLayout(this);

    progressBar_ = new QProgressBar;
    progressBar_->setRange(0, 1000);
    progressBar_->setValue(0);
    layout->addWidget(progressBar_);

    auto *row = new QHBoxLayout;
    statusLabel_ = new QLabel(tr("Starting..."));
    statusLabel_->setWordWrap(true);
    etaLabel_ = new QLabel;
    row->addWidget(statusLabel_, 1);
    row->addWidget(etaLabel_);
    layout->addLayout(row);

    // --- Details ------------------------------------------------------------
    //
    // This is the whole reason a wizard is acceptable here. The single window
    // this replaced argued that a long, partially-failing conversion needs the
    // source, the settings and the errors visible at the same moment, and that
    // hiding them behind a progress page was the Myst wizard's mistake. So they
    // are all still on the progress page -- one click away rather than always
    // on screen, which is the only thing that actually changed.
    auto *detailsRow = new QHBoxLayout;
    detailsButton_ = new QPushButton;
    detailsButton_->setCheckable(true);
    detailsRow->addWidget(detailsButton_);
    detailsRow->addStretch(1);
    layout->addLayout(detailsRow);

    details_ = new QWidget;
    auto *detailsLayout = new QVBoxLayout(details_);
    detailsLayout->setContentsMargins(0, 0, 0, 0);

    recap_ = new QLabel;
    recap_->setWordWrap(true);
    recap_->setTextFormat(Qt::RichText);
    detailsLayout->addWidget(recap_);

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
    detailsLayout->addWidget(logTabs_, 1);

    layout->addWidget(details_, 1);
    setDetailsVisible(false);

    connect(detailsButton_, &QPushButton::toggled, this, &InstallPage::setDetailsVisible);
}

void InstallPage::setDetailsVisible(bool visible)
{
    details_->setVisible(visible);
    detailsButton_->setChecked(visible);
    detailsButton_->setText(visible ? tr("Details <<") : tr("Details >>"));
}

void InstallPage::initializePage()
{
    logView_->clear();
    problemsView_->clear();
    problemCount_ = 0;
    logTabs_->setTabText(1, tr("Problems"));
    rateSamples_.clear();
    done_ = false;
    runClock_->start();

    const riven::Options o = wizard_->currentOptions();
    recap_->setText(tr("<b>From:</b> %1<br><b>To:</b> %2")
                        .arg(QString::fromStdString(o.source.string()).toHtmlEscaped(),
                             QString::fromStdString(o.dest.string()).toHtmlEscaped()));

    // Connected here rather than in the constructor so that the signals arrive
    // only while this page owns the run.
    ConversionWorker *worker = wizard_->worker();
    connect(worker, &ConversionWorker::progress, this, &InstallPage::onProgress,
            Qt::UniqueConnection);
    connect(worker, &ConversionWorker::logged, this, &InstallPage::onLogged,
            Qt::UniqueConnection);
    connect(worker, &ConversionWorker::finished, this, &InstallPage::onFinished,
            Qt::UniqueConnection);

    wizard_->setRunning(true);
    wizard_->setFinished(false);
    QMetaObject::invokeMethod(worker, "run", Qt::QueuedConnection, Q_ARG(riven::Options, o));
}

void InstallPage::onProgress(quint64 done, quint64 total, const QString &stage,
                             const QString &detail)
{
    if (total == 0)
        return;
    const int permille = static_cast<int>(done * 1000 / total);
    progressBar_->setValue(permille);
    statusLabel_->setText(tr("%1 - %2  (%3%)").arg(stage, detail).arg(done * 100 / total));

    // ETA from a trailing window, so a slow stack early on stops dominating the
    // estimate once a fast one starts.
    const qint64 now = runClock_->elapsed();
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

void InstallPage::appendLog(const LogLine &line)
{
    const QString text = QStringLiteral("%1: %2").arg(line.stage, line.message);
    logView_->appendPlainText(text);

    if (line.severity == static_cast<int>(riven::Severity::Info))
        return;

    auto *item = new QListWidgetItem(text);
    item->setForeground(line.severity == static_cast<int>(riven::Severity::Error) ? kRedInk
                                                                                 : kAmberInk);
    problemsView_->addItem(item);
    ++problemCount_;
    logTabs_->setTabText(1, tr("Problems (%1)").arg(problemCount_));
}

void InstallPage::onLogged(const LogLine &line)
{
    appendLog(line);
}

void InstallPage::onFinished(int outcome, const QString &message, const QString &summary)
{
    outcome_ = outcome;
    message_ = message;
    summary_ = summary;

    wizard_->setRunning(false);
    wizard_->setFinished(true);

    if (outcome == static_cast<int>(riven::ConversionResult::Outcome::Ok))
    {
        progressBar_->setValue(1000);
        statusLabel_->setStyleSheet(kGreen);
    }
    else if (outcome == static_cast<int>(riven::ConversionResult::Outcome::Cancelled))
    {
        statusLabel_->setStyleSheet(kAmber);
    }
    else
    {
        statusLabel_->setStyleSheet(kRed);
    }
    statusLabel_->setText(message);
    etaLabel_->clear();

    appendLog({static_cast<int>(riven::Severity::Info), QStringLiteral("done"), summary});

    // Nothing auto-advances. A run that logged problems has them on screen and
    // scrollable at the moment it ends, which is precisely what the single
    // window was right to insist on; a clean run needs one click on Next and
    // has nothing to read anyway.
    if (problemCount_ > 0)
    {
        setDetailsVisible(true);
        logTabs_->setCurrentWidget(problemsView_);
    }

    done_ = true;
    emit completeChanged();
}

// ---------------------------------------------------------------------------
// Finished
// ---------------------------------------------------------------------------

FinishPage::FinishPage(InstallWizard *wizard, InstallPage *install)
    : wizard_(wizard), install_(install)
{
    setTitle(tr("Finished"));
    setPixmap(QWizard::WatermarkPixmap, banner::watermark());

    headline_ = new QLabel;
    headline_->setWordWrap(true);
    QFont bold = headline_->font();
    bold.setBold(true);
    headline_->setFont(bold);

    detail_ = new QLabel;
    detail_->setWordWrap(true);

    openButton_ = new QPushButton(tr("Open the card folder"));

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(openButton_);
    buttons->addStretch(1);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(headline_);
    layout->addWidget(detail_);
    layout->addSpacing(8);
    layout->addLayout(buttons);
    layout->addStretch(1);

    connect(openButton_, &QPushButton::clicked, this, [this] {
        const auto dir = riven::Converter::dataDir(
            wizard_->currentOptions().dest);
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(QString::fromStdString(dir.string())));
    });
}

void FinishPage::initializePage()
{
    const int outcome = install_->outcome();
    const bool ok = outcome == static_cast<int>(riven::ConversionResult::Outcome::Ok);
    const bool cancelled =
        outcome == static_cast<int>(riven::ConversionResult::Outcome::Cancelled);

    if (ok && install_->problemCount() == 0)
        headline_->setText(tr("Riven is on your card."));
    else if (ok)
        headline_->setText(tr("Riven is on your card, with %1 thing(s) reported.")
                               .arg(install_->problemCount()));
    else if (cancelled)
        headline_->setText(tr("Stopped."));
    else
        headline_->setText(tr("It did not finish."));

    headline_->setStyleSheet(ok ? kGreen : cancelled ? kAmber : kRed);

    const riven::Options o = wizard_->currentOptions();

    QString text = install_->outcomeMessage();
    if (!install_->outcomeSummary().isEmpty())
        text += QStringLiteral("\n\n") + install_->outcomeSummary();
    if (ok && !o.copyRom)
    {
        text += QStringLiteral("\n\n")
              + tr("The game itself was not copied. Put the .nds on the card yourself, "
                   "or run this again with the copy turned on.");
    }
    if (ok && o.makeImage)
    {
        // Worth naming rather than leaving in the summary line: it is the file
        // an emulator user is about to go looking for, and it is not on the
        // card folder they were shown three pages ago.
        text += QStringLiteral("\n\n")
              + tr("The emulator image is %1. Point melonDS or DeSmuME at it as the SD "
                   "card.")
                    .arg(QDir::toNativeSeparators(
                        QString::fromStdString(o.imagePath.string())));
    }
    detail_->setText(text);

    openButton_->setEnabled(!o.dest.empty());
}

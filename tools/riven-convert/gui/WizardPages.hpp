#pragma once

// The seven pages. All state that outlives a page lives in InstallWizard; what
// is here is layout, plus the isComplete() rules that decide when Next lights
// up.
//
// Those rules are the wizard's whole safety argument and are worth listing in
// one place, because between them they are what the old window's disabled Start
// button used to do on its own:
//
//   Source   -- a scan has landed and it found a readable Riven.
//   Card     -- a destination exists, and if the game is to be copied, the .nds
//               is where it was said to be.
//   Ready    -- riven::checksPass(), which is the same test the CLI applies.
//   Install  -- the run has ended, however it ended.
//
// Nothing else blocks. In particular the stage page never blocks: choosing to
// convert nothing is caught on Ready, where the reason can be spelled out next
// to everything else that is wrong.

#include <QWizardPage>

#include "ConversionWorker.hpp"
#include "riven/Preflight.hpp"

class InstallWizard;

class QCheckBox;
class QComboBox;
class QElapsedTimer;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTabWidget;

/// What this is, and what you need before starting.
class WelcomePage : public QWizardPage
{
    Q_OBJECT
public:
    explicit WelcomePage(InstallWizard *wizard);
};

/// Where Riven is. Next waits for a scan.
class SourcePage : public QWizardPage
{
    Q_OBJECT
public:
    explicit SourcePage(InstallWizard *wizard);

    bool isComplete() const override;
    void initializePage() override;

private:
    void browse();

    InstallWizard *wizard_ = nullptr;
    QLineEdit *edit_ = nullptr;
    QLabel *status_ = nullptr;
};

/// Where the card is, and whether to put the game on it as well as the data.
class DestPage : public QWizardPage
{
    Q_OBJECT
public:
    explicit DestPage(InstallWizard *wizard);

    bool isComplete() const override;
    void initializePage() override;

private:
    void refresh();

    InstallWizard *wizard_ = nullptr;
    QLineEdit *edit_ = nullptr;
    QLabel *note_ = nullptr;
    QCheckBox *copyRom_ = nullptr;
    QLineEdit *romEdit_ = nullptr;
    QPushButton *romBrowse_ = nullptr;
    QLabel *romStatus_ = nullptr;

    QCheckBox *makeImage_ = nullptr;
    QLineEdit *imageEdit_ = nullptr;
    QPushButton *imageBrowse_ = nullptr;
    QLabel *imageStatus_ = nullptr;
};

/// Preset, stages, and what it will cost on the card.
class StagesPage : public QWizardPage
{
    Q_OBJECT
public:
    explicit StagesPage(InstallWizard *wizard);

    void initializePage() override;

private:
    void presetChanged(int index);
    void stageToggled();
    void refreshEstimate();

    InstallWizard *wizard_ = nullptr;
    QComboBox *presetBox_ = nullptr;
    QCheckBox *cardsBox_ = nullptr;
    QCheckBox *imagesBox_ = nullptr;
    QCheckBox *hiresBox_ = nullptr;
    QCheckBox *waterBox_ = nullptr;
    QCheckBox *audioBox_ = nullptr;
    QCheckBox *videoBox_ = nullptr;
    QCheckBox *cursorsBox_ = nullptr;
    QCheckBox *extrasBox_ = nullptr;
    QCheckBox *forceBox_ = nullptr;
    QLabel *estimateLabel_ = nullptr;

    /// Guards the two-way traffic between the preset box and the checkboxes, so
    /// applying a preset does not look like the user unticking things.
    bool updatingPreset_ = false;
};

/// The last gate: every check, a recap, and the one thing worth being able to
/// fix without walking back -- ffmpeg.
class ReadyPage : public QWizardPage
{
    Q_OBJECT
public:
    explicit ReadyPage(InstallWizard *wizard);

    bool isComplete() const override;
    void initializePage() override;

private:
    void refresh();

    InstallWizard *wizard_ = nullptr;
    QLineEdit *ffmpegEdit_ = nullptr;
    QLabel *recap_ = nullptr;
    QListWidget *checksList_ = nullptr;
    bool pass_ = false;
};

/// The run. Progress on top, everything the old single window showed behind
/// Details.
class InstallPage : public QWizardPage
{
    Q_OBJECT
public:
    explicit InstallPage(InstallWizard *wizard);

    bool isComplete() const override { return done_; }
    void initializePage() override;

    /// What to put on the Finished page. Read once the run has ended.
    QString outcomeMessage() const { return message_; }
    QString outcomeSummary() const { return summary_; }
    int outcome() const { return outcome_; }
    int problemCount() const { return problemCount_; }

private slots:
    void onProgress(quint64 done, quint64 total, const QString &stage,
                    const QString &detail);
    void onLogged(const LogLine &line);
    void onFinished(int outcome, const QString &message, const QString &summary);

private:
    void appendLog(const LogLine &line);
    void setDetailsVisible(bool visible);

    InstallWizard *wizard_ = nullptr;
    QProgressBar *progressBar_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QLabel *etaLabel_ = nullptr;
    QPushButton *detailsButton_ = nullptr;
    QWidget *details_ = nullptr;
    QLabel *recap_ = nullptr;
    QTabWidget *logTabs_ = nullptr;
    QPlainTextEdit *logView_ = nullptr;
    QListWidget *problemsView_ = nullptr;

    bool done_ = false;
    int outcome_ = 0;
    QString message_;
    QString summary_;
    int problemCount_ = 0;

    /// Trailing-window rate samples for the ETA, as (elapsed ms, done).
    QElapsedTimer *runClock_ = nullptr;
    QList<QPair<qint64, quint64>> rateSamples_;
};

/// What happened, and the way to the files.
class FinishPage : public QWizardPage
{
    Q_OBJECT
public:
    FinishPage(InstallWizard *wizard, InstallPage *install);

    void initializePage() override;

private:
    InstallWizard *wizard_ = nullptr;
    InstallPage *install_ = nullptr;
    QLabel *headline_ = nullptr;
    QLabel *detail_ = nullptr;
    QPushButton *openButton_ = nullptr;
};

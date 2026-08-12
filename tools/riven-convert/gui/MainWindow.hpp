#pragma once

// The converter window: source, destination, what to convert, the pre-run
// checks, progress and the log, all visible at once.
//
// A single window rather than a wizard because a Riven conversion runs for a
// long time and can partially fail per stack. Being able to see the source you
// chose, the settings you chose and what is going wrong at the same moment is
// worth more than the tidiness of pages -- the Myst wizard hid all three behind
// its progress page.

#include <QMainWindow>
#include <QThread>

#include "ConversionWorker.hpp"
#include "riven/Options.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTabWidget;
class QTimer;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow();
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void browseSource();
    void browseDest();
    void sourcePathEdited();
    void rescanSource();
    void presetChanged(int index);
    void stageToggled();
    void startOrStop();
    void openOutput();

    void onScanned(const riven::SourceInfo &info);
    void onProgress(quint64 done, quint64 total, const QString &stage,
                    const QString &detail);
    void onLogged(const LogLine &line);
    void onFinished(int outcome, const QString &message, const QString &summary);

private:
    QWidget *buildSourceGroup();
    QWidget *buildDestGroup();
    QWidget *buildStagesGroup();
    QWidget *buildChecksGroup();
    QWidget *buildProgressGroup();
    QWidget *buildLogGroup();

    riven::Options currentOptions() const;
    void applyOptions(const riven::Options &o);
    void refreshChecks();
    void refreshEstimate();
    void setRunning(bool running);
    void loadSettings();
    void saveSettings();
    void appendLog(const LogLine &line);

    // --- widgets ---------------------------------------------------------
    QLineEdit *sourceEdit_ = nullptr;
    QLabel *sourceStatus_ = nullptr;
    QLineEdit *destEdit_ = nullptr;
    QLabel *destNote_ = nullptr;

    QComboBox *presetBox_ = nullptr;
    QCheckBox *cardsBox_ = nullptr;
    QCheckBox *imagesBox_ = nullptr;
    QCheckBox *hiresBox_ = nullptr;
    QCheckBox *waterBox_ = nullptr;
    QCheckBox *audioBox_ = nullptr;
    QCheckBox *videoBox_ = nullptr;
    QCheckBox *forceBox_ = nullptr;
    QLabel *estimateLabel_ = nullptr;

    QListWidget *checksList_ = nullptr;

    QProgressBar *progressBar_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QLabel *etaLabel_ = nullptr;

    QTabWidget *logTabs_ = nullptr;
    QPlainTextEdit *logView_ = nullptr;
    QListWidget *problemsView_ = nullptr;

    QPushButton *startButton_ = nullptr;
    QPushButton *openButton_ = nullptr;

    // --- state -----------------------------------------------------------
    QThread workerThread_;
    ConversionWorker *worker_ = nullptr;
    QTimer *scanDebounce_ = nullptr;

    /// The last completed source scan. Every estimate and every check is
    /// computed from this rather than by re-reading the archives: scanning a
    /// full install takes seconds, and it must never happen on the UI thread.
    riven::SourceInfo scanned_;
    bool haveScan_ = false;

    bool running_ = false;
    bool updatingPreset_ = false;
    int problemCount_ = 0;

    /// Trailing-window rate samples for the ETA, as (elapsed ms, done).
    QElapsedTimer runClock_;
    QList<QPair<qint64, quint64>> rateSamples_;
};

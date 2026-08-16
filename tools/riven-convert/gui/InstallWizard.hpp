#pragma once

// The converter, as an installer: Welcome, Riven, card, what to install, ready,
// installing, finished.
//
// This replaced a single window that showed source, destination, settings,
// checks, log and progress all at once, and the argument that window carried in
// its header is worth keeping because it was right about something. It said a
// Riven conversion runs for hours and can partially fail per stack, so being
// able to see the source you chose, the settings you chose and what is going
// wrong at the same moment beats the tidiness of pages -- and that "the Myst
// wizard hid all three behind its progress page" was the failure to avoid.
//
// That is a complaint about the PROGRESS PAGE, not about pages. So the shape
// changed and the complaint was answered rather than dropped: the Installing
// page carries a Details disclosure holding the whole log, the Problems list
// and a recap of every path and stage the run was given. Nothing the old window
// showed while running became unreachable; what went away is being asked about
// six things at once before you have answered the first.
//
// Everything that is not layout lives in the Qt-free core, exactly as before,
// so the CLI and the wizard still cannot drift.
//
// STATE LIVES HERE, NOT IN THE PAGES. Pages come and go and are constructed in
// an order the user does not control, so the worker thread, the cached source
// scan and the settings belong to the wizard, and the values the user types are
// QWizard fields that any page can read. currentOptions() is the single place
// those fields become a riven::Options.

#include <QThread>
#include <QWizard>

#include "ConversionWorker.hpp"
#include "riven/Options.hpp"
#include "riven/Preflight.hpp"

class QTimer;

class InstallWizard : public QWizard
{
    Q_OBJECT

public:
    /// Explicit ids rather than QWizard's auto-numbering, because nextId() and
    /// the settings both name pages and an implicit ordering would make adding
    /// a page in the middle a silent renumbering.
    enum PageId
    {
        Page_Welcome,
        Page_Source,
        Page_Dest,
        Page_Stages,
        Page_Ready,
        Page_Install,
        Page_Finish,
    };

    InstallWizard();
    ~InstallWizard() override;

    /// The fields as the pipeline wants them. normalise() has run, so this is
    /// what the conversion will actually do rather than what was ticked.
    riven::Options currentOptions() const;

    /// Write stage flags back into the checkboxes. The Stages page uses this to
    /// stop the boxes claiming something normalise() will overrule.
    void applyStages(const riven::Options &o);

    /// The last completed scan, and whether there is one. Every estimate and
    /// every check is computed from this rather than by re-reading the
    /// archives: scanning a full install takes seconds and must never happen on
    /// the UI thread.
    const riven::SourceInfo &scan() const { return scanned_; }
    bool haveScan() const { return haveScan_; }

    /// Ask for a scan of whatever the source field currently holds, debounced.
    /// Safe to call on every keystroke.
    void requestScan();

    ConversionWorker *worker() const { return worker_; }
    bool running() const { return running_; }
    void setRunning(bool running) { running_ = running; }

    /// The run has ended, whatever the outcome. Closing after this must not go
    /// through the "still stopping" path.
    void setFinished(bool finished) { finished_ = finished; }

signals:
    /// A scan finished. The source page listens; nothing else has to.
    void scanFinished(const riven::SourceInfo &info);

protected:
    /// Cancel and the window's close button both arrive here.
    void reject() override;

private slots:
    void rescan();
    void onScanned(const riven::SourceInfo &info);

private:
    void loadSettings();
    void saveSettings();

    QThread workerThread_;
    ConversionWorker *worker_ = nullptr;
    QTimer *scanDebounce_ = nullptr;

    riven::SourceInfo scanned_;
    bool haveScan_ = false;
    bool running_ = false;
    /// Set once the run has ended, so closing the finished wizard does not go
    /// through the "still stopping" path.
    bool finished_ = false;
};

/// The .nds to offer copying, or empty if there is not one to be found.
///
/// Looked for beside the converter and a few directories above it, which covers
/// both a release layout (everything in one folder) and running straight out of
/// build/convert/gui/ during development.
QString findRomBesideConverter();

#pragma once

// Global process/init singleton. Brings up the filesystem (NitroFS + FAT) and
// exposes the on-SD data paths the rest of the engine builds on. Forked from
// the Myst port, which in turn modeled it on Skyjo DS's `Process` singleton.
//
// The path set differs from Myst's in one telling way: there is no nodesDir()
// and no yasNodeDir(). Myst shipped JSON on the card and baked it to yas on
// first boot, so it needed both. Here the converter writes the yas archive
// directly, so there is exactly one directory of card data and no bake step.

#include "global_header.hpp"

class Global
{
public:
    const char *fatDevice = nullptr;   ///< libfat default drive, e.g. "fat:" / "sd:"
    std::string fatDeviceCPP;          ///< same as a std::string, for path building
    bool hasFat = false;               ///< FAT init succeeded and a drive is mounted
    bool hasNitroFS = false;           ///< NitroFS (ROM filesystem) available
    /// The top-screen console, or null if it would not come up. Every
    /// diagnostic in the engine is a std::printf, and without this they go
    /// nowhere.
    ///
    /// The POINTER and not a bool, because the debug console has to move the
    /// text window around it -- consoleSetWindow takes a mutable PrintConsole *
    /// and consoleGetDefault() hands back a const one. Nothing else needs it,
    /// and testing it against null is the same question the bool answered.
    PrintConsole *console = nullptr;

    /// nitroFSInit + fatInitDefault + fatGetDefaultDrive, then create the data
    /// directories. Safe to call once at startup.
    void Init();

    /// Create the directories the game itself writes to (saves/). The converter
    /// creates everything it writes. No-op without FAT.
    void EnsureDataDirs();

    // SD data paths (only meaningful when hasFat). Mirrors the layout the asset
    // converter writes: <drive>/_nds/riven_nds/data/{stacks,pics,...}.
    std::string dataDir() const { return fatDeviceCPP + "_nds/riven_nds/data/"; }
    /// yas-serialized per-stack card graphs, written by the converter.
    std::string stacksDir() const { return dataDir() + "stacks/"; }
    /// Downscaled ARGB1555 card stills (.rpic).
    std::string picsDir() const { return dataDir() + "pics/"; }
    /// Full-resolution 8bpp+CLUT twins for the zoom viewer (.rpiz).
    std::string picsHiDir() const { return dataDir() + "pics_hi/"; }
    std::string videoDir() const { return dataDir() + "video/"; }
    std::string soundDir() const { return dataDir() + "sound/"; }
    /// Water-effect (SFXE) frame scripts.
    std::string sfxeDir() const { return dataDir() + "sfxe/"; }
    std::string cursorsDir() const { return dataDir() + "cursors/"; }
    /// Inventory / marble / credits art, recovered from extras.MHK or arcriven.z.
    std::string extrasDir() const { return dataDir() + "extras/"; }
    /// The port's own UI art: topbg.rpiz, the top-screen picture. Separate from
    /// extras/ because nothing in it is a Mohawk resource -- it is built from
    /// AUTORUN.BMP, which sits loose on the disc.
    std::string uiDir() const { return dataDir() + "ui/"; }
    /// Written by the game, never by the converter.
    std::string savesDir() const { return dataDir() + "saves/"; }
    /// The player's notebook -- captures and the scribbles on them. Written by
    /// the game too, and deliberately NOT under saves/: a note is global rather
    /// than part of a slot (BookNotes.hpp), and putting it in the save
    /// directory would invite a "delete my saves" from taking it with them.
    std::string notesDir() const { return dataDir() + "notes/"; }
    std::string dataPath(const std::string &rel) const { return dataDir() + rel; }

    /// Why the game cannot start. Everything below Ok means the card does not
    /// carry a usable conversion, which would otherwise show up as a black
    /// screen rather than as anything the player could act on.
    enum class DataStatus
    {
        Ok,
        NoCard,      ///< no FAT device at all (no SD, or the flashcard's DLDI failed)
        NoDataDir,   ///< the card is readable but _nds/riven_nds/data/ isn't there
        NoStacks,    ///< data/ exists but holds no card graph: not a conversion
        NoAspit,     ///< stacks exist but aspit is missing, so there is nothing to boot into
        NoImages,    ///< the graph is there but the card art isn't: partial conversion
    };

    /// Look for the pieces the engine cannot start without. Cheap (a handful of
    /// filesystem probes), so main() can call it before anything else reads data.
    DataStatus CheckData() const;

    /// Report the pieces the engine can start WITHOUT, to the top-screen console.
    ///
    /// Deliberately not part of CheckData: a conversion run with --no-video is a
    /// playable game with no movies, and halting on it would be worse than the
    /// silence it replaces. But an absent video/ directory is also exactly what a
    /// player who thinks video is broken needs to be told, and nothing else in the
    /// engine is in a position to say it -- by the time a card wants a movie, the
    /// only symptom is one missing file at a time.
    void ReportOptionalData() const;

    /// Two lines of player-facing explanation for a CheckData() result: what is
    /// wrong (line 1) and what to do about it (line 2). Never null.
    static const char *DataStatusTitle(DataStatus s);
    static const char *DataStatusHint(DataStatus s);
};

extern Global global;

#include "MainMenu.hpp"

#include <cstdio>
#include <string>

#include "DebugLog.hpp"
#include "Global.hpp"
#include "SaveGame.hpp"
#include "ScreenTakeover.hpp"
#include "Settings.hpp"
#include "engine/Engine.hpp"
#include "global_header.hpp"
#include "render/BgSurface.hpp"
#include "render/Cursor.hpp"
#include "render/HintBar.hpp"
#include "render/Inventory.hpp"
#include "render/TextLayer.hpp"

namespace rivenrt
{

MainMenu mainMenu;

namespace
{
    // Layout. Rows are full-width for the hit test even though the label is not,
    // because a menu row on a touch screen is a band, not a word.
    constexpr int kTitleY = 24;
    /// SEVEN rows have to fit -- the settings screen is the longest and grew one
    /// when the control hints got a row of their own: kFirstY + 6*kRowStep +
    /// kLineHeight must land inside the 192-row screen, and 56 + 120 + 14 is
    /// 190. It has been lost twice now, both times the same way: at 72 and 24
    /// the last row started AT 192, and at 56 and 22 it started at 188, so
    /// "Back" was four pixels tall with its selection mark cut off beside it.
    ///
    /// kFirstY stays at 56 rather than absorbing the step: kNoteY is 38 and a
    /// line of this font is 14, so the pickers that draw a note under the title
    /// need the first row to begin at 52 or later.
    constexpr int kFirstY = 56;
    constexpr int kRowStep = 20;
    constexpr int kTextX = 40;
    /// The selection marker sits in the margin, so a row's text does not move
    /// when the selection lands on it.
    constexpr int kMarkX = 20;

    /// Steps on the volume row, and the one place the byte and the number on
    /// screen are tied together.
    ///
    /// Twenty steps across the whole gain range, which now runs to twice unity
    /// (Settings.hpp), so a step is 10% and level 10 is 100%. Shown as a
    /// percentage rather than as "10": the useful thing to know about this
    /// control is where normal is, and a bare number does not say.
    ///
    /// The control used to step masterVolume itself by 255/kVolumeSteps and let
    /// the label re-derive the number. An integer step does not divide 255, so
    /// the ladder missed both ends -- from full it stopped at 5, never 0 -- and
    /// the label showed the same number at either side of the wrap. Stepping the
    /// LEVEL and deriving the byte from it cannot drift: level*255/kVolumeSteps
    /// puts volumeLevel() back on the same level.
    constexpr int kVolumeSteps = 20;
    int volumeLevel(std::uint8_t v)
    {
        return (v * kVolumeSteps + 127) / 255;
    }

    int rowAt(int y, int rows)
    {
        if (y < kFirstY)
            return -1;
        const int row = (y - kFirstY) / kRowStep;
        return row < rows ? row : -1;
    }

    /// One frame of a menu: read input, and redraw only when something moved.
    ///
    /// The redraw is gated because it is not cheap -- clear() is a 128 KB fill
    /// of VRAM -- and because a menu that redraws every frame would flip the
    /// two buffers 60 times a second for no reason.
    struct Screen
    {
        int sel = 0;
        int rows = 0;
        bool dirty = true;

        /// Move the selection, and report whether input asked to activate the
        /// row it is on. `down` is keysDown() for this frame.
        bool step(int down, int touchX, int touchY, bool touched)
        {
            if (down & KEY_UP)
            {
                sel = (sel + rows - 1) % rows;
                dirty = true;
            }
            if (down & KEY_DOWN)
            {
                sel = (sel + 1) % rows;
                dirty = true;
            }
            if (touched)
            {
                const int row = rowAt(touchY, rows);
                (void)touchX;
                if (row >= 0)
                {
                    // A touch selects AND activates, the way a button does.
                    // Moving to the row first is what makes the D-pad and the
                    // stylus agree about where the player is.
                    sel = row;
                    dirty = true;
                    return true;
                }
            }
            return (down & KEY_A) != 0;
        }
    };

    // The screen guard and its four helpers live in ScreenTakeover.hpp now: the
    // credits roll is a port screen too, and it wanted the same eight things in
    // the same order. These are the spellings this file already used.
    inline void frame() { screenFrame(); }
    inline bool usable() { return screenUsable(); }
    inline void showPointer(bool on) { screenShowPointer(on); }
    inline void showInventory(bool on) { screenShowInventory(on); }

    /// Where a picker's one line of explanation goes: between the title and the
    /// first row, in the gap kFirstY already leaves.
    constexpr int kNoteY = 38;

    /// Draw a titled list of rows, with a marker on the selection and a note
    /// under the title. The tail of every screen in this file.
    void paint(const char *title, const std::string *labels, int rows, int sel,
               const std::string &note)
    {
        textLayer.target(bgs.backBuffer());
        textLayer.clear();
        textLayer.draw(kTextX, kTitleY, title);
        if (!note.empty())
            textLayer.draw(kTextX, kNoteY, note);
        for (int i = 0; i < rows; ++i)
        {
            const int y = kFirstY + i * kRowStep;
            if (i == sel)
                textLayer.draw(kMarkX, y, ">");
            textLayer.draw(kTextX, y, labels[i]);
        }
        // Shown at the top of the next turn, by frame().
        bgs.requestFlip();
    }
} // namespace

void MainMenu::runSettings()
{
    if (!usable())
    {
        DebugLog::warn("settings need the menu font; not showing them");
        return;
    }

    enum Row
    {
        RowZip = 0,
        RowTransitions,
        RowWater,
        RowVolume,
        RowHints,
        RowDebug,
        RowBack,
        kRowCount,
    };

    Screen s;
    s.rows = kRowCount;
    bool changed = false;
    bool leaving = false;

    ScreenTakeover screen;
    // Scoped alongside the takeover, and for the same reason it is: this loop
    // has several ways out and every one of them owes the card legend back.
    HintScope hint{HintBar::kMenuRow0, HintBar::kMenuRow1, HintBar::kMenuRow2};

    while (true)
    {
        frame();
        scanKeys();
        const int down = keysDown();

        touchPosition t = {};
        const bool touched = (down & KEY_TOUCH) != 0;
        if (touched)
            touchRead(&t);

        const bool activate = s.step(down, t.px, t.py, touched);

        if (down & KEY_B)
            break;

        if (activate || (down & (KEY_LEFT | KEY_RIGHT)))
        {
            // A/LEFT/RIGHT all toggle, which is what makes a two-state row feel
            // like a switch rather than like a list. Straight from the Myst
            // port's settings screen (InitialMenu.cpp:279-295).
            switch (s.sel)
            {
            case RowZip:
                settings.zipMode = !settings.zipMode;
                changed = true;
                break;
            case RowTransitions:
                settings.transitions = !settings.transitions;
                changed = true;
                break;
            case RowWater:
                settings.water = !settings.water;
                changed = true;
                break;
            case RowVolume:
            {
                // Twenty steps, wrapping, so one control works for both a D-pad
                // and a touch. LEFT goes down, everything else goes up.
                int level = volumeLevel(settings.masterVolume);
                level += (down & KEY_LEFT) != 0 ? -1 : 1;
                level = level < 0 ? kVolumeSteps : (level > kVolumeSteps ? 0 : level);
                settings.masterVolume =
                    static_cast<std::uint8_t>(level * 255 / kVolumeSteps);
                changed = true;
                // Live, so the slider is heard while it moves.
                settings.apply();
                break;
            }
            case RowHints:
                settings.controlHints = !settings.controlHints;
                changed = true;
                // Live, through settings.apply() below -- but the band is on the
                // OTHER screen, so unlike every other row here the player can
                // watch this one take effect while the settings are still up.
                settings.apply();
                break;
            case RowDebug:
                settings.debugMode = !settings.debugMode;
                changed = true;
                break;
            case RowBack:
                // LEFT/RIGHT on "Back" is a cursor movement that landed on a
                // row with nothing to toggle, not a request to leave.
                leaving = activate;
                break;
            default:
                break;
            }
            if (leaving)
                break;
            s.dirty = true;
        }

        if (!s.dirty)
            continue;
        s.dirty = false;

        const char *const onOff[2] = {"Off", "On"};
        const std::string labels[kRowCount] = {
            std::string("Zip mode: ") + onOff[settings.zipMode],
            std::string("Transitions: ") + onOff[settings.transitions],
            std::string("Water: ") + onOff[settings.water],
            "Volume: " + std::to_string(volumeLevel(settings.masterVolume) * 10) + "%",
            std::string("Control hints: ") + onOff[settings.controlHints],
            // The only row that does not take effect where it is set:
            // DebugLog::begin reads it once, at startup, because it takes the
            // top screen away from the picture as it does so.
            std::string("Debug log: ") + onOff[settings.debugMode] + " (on restart)",
            "Back",
        };
        paint("Settings", labels, kRowCount, s.sel, std::string());
    }

    if (changed)
    {
        settings.save();
        settings.apply();
    }
}

namespace
{
    /// Let the player pick one of the five slots. Returns 1..5, or 0 for cancel.
    ///
    /// `forSaving` changes three things and nothing else: the title, whether an
    /// occupied slot asks before it is used, and whether an empty one is a
    /// refusal or the obvious choice.
    ///
    /// The caller owns the display -- every route in here is already inside a
    /// ScreenTakeover -- so this only draws and reads input.
    int pickSlot(bool forSaving)
    {
        // Read once, on the way in. Five file headers is five seeks; doing it in
        // the repaint would put them in the frame loop for a list that only
        // changes when this screen itself writes a slot.
        SaveGame::SlotInfo info[SaveGame::kSlotCount];
        std::string labels[SaveGame::kSlotCount + 1];
        const int rows = SaveGame::kSlotCount + 1;
        const int rowBack = SaveGame::kSlotCount;

        const auto rescan = [&]() {
            for (int i = 0; i < SaveGame::kSlotCount; ++i)
            {
                info[i] = SaveGame::readSlotInfo(i + 1);
                labels[i] = SaveGame::slotLabel(i + 1, info[i]);
            }
            labels[rowBack] = "Back";
        };
        rescan();

        Screen s;
        s.rows = rows;
        std::string note;
        // Which slot the player has been asked to confirm overwriting, or -1.
        // An in-place prompt rather than a modal: the list is the context for
        // the question, and covering it up to ask would take that away.
        int confirming = -1;

        while (true)
        {
            frame();
            scanKeys();
            const int down = keysDown();

            touchPosition t = {};
            const bool touched = (down & KEY_TOUCH) != 0;
            if (touched)
                touchRead(&t);

            if (confirming >= 0)
            {
                // The prompt swallows the D-pad: moving the selection out from
                // under a question about a particular slot would leave the
                // answer attached to the wrong one.
                if (down & KEY_A)
                    return confirming;
                if (down & (KEY_B | KEY_START))
                {
                    confirming = -1;
                    note.clear();
                    s.dirty = true;
                }
                if (!s.dirty)
                    continue;
                s.dirty = false;
                paint(forSaving ? "Save game" : "Load game", labels, rows, s.sel, note);
                continue;
            }

            const bool activate = s.step(down, t.px, t.py, touched);
            if (down & (KEY_B | KEY_START))
                return 0;

            if (activate)
            {
                if (s.sel == rowBack)
                    return 0;

                const int slot = s.sel + 1;
                const SaveGame::SlotInfo &si = info[s.sel];
                if (forSaving)
                {
                    // A damaged slot counts as occupied. The file may be the
                    // only copy of a game the player still wants to look at on a
                    // PC, so it gets the same question a good one does.
                    if (si.used || si.damaged)
                    {
                        confirming = slot;
                        note = "Overwrite?  A: yes   B: no";
                    }
                    else
                    {
                        return slot;
                    }
                }
                else if (si.damaged)
                {
                    // Three failures, three notes. "Load failed" for all of them
                    // tells the player nothing they can do anything about.
                    note = "Damaged - save over it to reuse";
                }
                else if (!si.used)
                {
                    // A row that simply ignored the button would read as a
                    // frozen menu.
                    note = "That slot is empty";
                }
                else
                {
                    return slot;
                }
                s.dirty = true;
            }

            if (!s.dirty)
                continue;
            s.dirty = false;
            paint(forSaving ? "Save game" : "Load game", labels, rows, s.sel, note);
        }
    }
    /// One line of bad news and a way out.
    ///
    /// On the screen the player is looking at, and not only on the console: with
    /// debug mode off the console says nothing at all (DebugLog.hpp), so a save
    /// or load that silently did not happen would be found out about at the
    /// worst possible moment. The caller already owns the display.
    void showNotice(const char *title, const char *text)
    {
        Screen s;
        s.rows = 1;
        const std::string labels[1] = {"Back"};
        while (true)
        {
            frame();
            scanKeys();
            const int down = keysDown();
            touchPosition t = {};
            const bool touched = (down & KEY_TOUCH) != 0;
            if (touched)
                touchRead(&t);
            if (s.step(down, t.px, t.py, touched) || (down & (KEY_B | KEY_START)))
                break;
            if (!s.dirty)
                continue;
            s.dirty = false;
            paint(title, labels, 1, 0, text);
        }
    }
} // namespace

void MainMenu::runSavePicker()
{
    if (!usable() || !global.hasFat)
    {
        DebugLog::warn("saves need the menu font and a card");
        return;
    }
    ScreenTakeover screen;
    HintScope hint{HintBar::kMenuRow0, HintBar::kMenuRow1, HintBar::kMenuRow2};

    // Riven's own menu and its journals are aspit cards, and Riven's Save button
    // is ON one of them -- so this is reachable with the engine standing
    // somewhere that is not a place in the game. Saving it would write a slot
    // that loads back to the menu, and it could overwrite a real playthrough to
    // do it.
    //
    // Refused rather than redirected: where the player came from is in
    // ReturnStackId/ReturnCardId, but ReturnCardId is an RMAP GLOBAL id and
    // turning one into the local id a save stores needs the target stack's own
    // table, which is not the stack that is loaded. Saving the return position
    // properly means storing global ids, and that is a format change rather
    // than a fix.
    if (engine.booted() && engine.stack().id == rivendata::StackId::Aspit)
    {
        showNotice("Save game", "Not from the menu - save in the game");
        return;
    }

    const int slot = pickSlot(true);
    if (slot == 0)
        return;

    const bool ok = SaveGame::writeSlot(slot, engine.buildSaveState());
    DebugLog::note("SAVE slot %d %s", slot, ok ? "ok" : "FAILED");
    if (!ok)
        showNotice("Save game", "Could not write to the card");
}

bool MainMenu::runLoadPicker()
{
    if (!usable() || !global.hasFat)
    {
        DebugLog::warn("saves need the menu font and a card");
        return false;
    }
    ScreenTakeover screen;
    HintScope hint{HintBar::kMenuRow0, HintBar::kMenuRow1, HintBar::kMenuRow2};

    const int slot = pickSlot(false);
    if (slot == 0)
        return false;

    SaveGame::SaveState state;
    if (!SaveGame::readSlot(slot, state))
    {
        // The slot listed as usable -- its header parsed -- and the payload
        // behind it did not. That is the length or the checksum catching a file
        // the menu had already offered, so the player has to be told here; the
        // list will keep showing it as a save until it is written over.
        DebugLog::warn("load: slot %d would not read", slot);
        showNotice("Load game", "That save is damaged");
        return false;
    }

    if (!engine.booted())
    {
        // The boot menu. There is no engine to restore into yet, so the state is
        // held for main() to hand to boot() -- which is what keeps a load from
        // entering aspit card 1 and starting the intro on its way past.
        load_ = std::move(state);
        haveLoad_ = true;
        return true;
    }

    // Mid-game. Note that this may be DEFERRED rather than done: opened from
    // Riven's own Restore button we are inside a script, and restoreFrom holds
    // the load until the interpreter is out of the stack it would replace. True
    // means accepted either way, which is what the caller needs to know.
    if (!engine.restoreFrom(state))
    {
        DebugLog::warn("load: slot %d could not be entered", slot);
        return false;
    }
    return true;
}

void MainMenu::runInGameMenu()
{
    if (!usable())
    {
        DebugLog::warn("the in-game menu needs the menu font");
        return;
    }

    enum Row
    {
        RowSave = 0,
        RowLoad,
        RowNotebook,
        RowSettings,
        RowResume,
        kRowCount,
    };

    bool wantNotebook = false;
    {
        ScreenTakeover screen;
        HintScope hint{HintBar::kMenuRow0, HintBar::kMenuRow1, HintBar::kMenuRow2};
        Screen s;
        s.rows = kRowCount;

        while (true)
        {
            frame();
            scanKeys();
            const int down = keysDown();

            touchPosition t = {};
            const bool touched = (down & KEY_TOUCH) != 0;
            if (touched)
                touchRead(&t);

            if (down & (KEY_B | KEY_START))
                break;

            if (s.step(down, t.px, t.py, touched))
            {
                bool leave = false;
                switch (s.sel)
                {
                case RowSave:
                    runSavePicker();
                    break;
                case RowLoad:
                    // A load replaces the card this menu is sitting on top of,
                    // so there is nothing left to come back to: close on
                    // success. Asking the picker rather than watching the card
                    // id, because a card id is stack-LOCAL -- loading Jungle
                    // island's card 155 while standing on Temple island's card
                    // 155 would look like nothing had happened.
                    leave = runLoadPicker();
                    break;
                case RowNotebook:
                    // The notebook takes the screen ITSELF, and not the way a
                    // port screen does: it claims a buffer and stops flipping
                    // (NoteView.cpp), which cannot be nested inside this
                    // screen's takeover. So this menu closes first and the
                    // notebook is opened below, outside the guard.
                    leave = true;
                    wantNotebook = true;
                    break;
                case RowSettings:
                    runSettings();
                    break;
                case RowResume:
                    leave = true;
                    break;
                default:
                    break;
                }
                if (leave)
                    break;
                s.dirty = true;
            }

            if (!s.dirty)
                continue;
            s.dirty = false;

            const std::string labels[kRowCount] = {"Save game", "Load game", "Notebook",
                                                   "Settings", "Resume"};
            paint("Riven", labels, kRowCount, s.sel, std::string());
        }
    }

    if (wantNotebook)
        engine.runNotebook();
}

void MainMenu::run()
{
    if (!usable())
    {
        DebugLog::warn("no menu: starting the game directly");
        return;
    }

    enum Row
    {
        RowNewGame = 0,
        RowLoad,
        RowSettings,
        kRowCount,
    };

    Screen s;
    s.rows = kRowCount;

    // The band is already carrying the card legend by now (HintBar::create sets
    // it as the resting state), and none of it is true yet -- there is no card.
    HintScope hint{HintBar::kMenuRow0, HintBar::kMenuRow1};

    bgs.setLetterbox(false);
    showPointer(false);

    while (true)
    {
        frame();
        scanKeys();
        const int down = keysDown();

        touchPosition t = {};
        const bool touched = (down & KEY_TOUCH) != 0;
        if (touched)
            touchRead(&t);

        if (s.step(down, t.px, t.py, touched))
        {
            if (s.sel == RowNewGame)
                break;
            if (s.sel == RowLoad)
            {
                runLoadPicker();
                if (haveLoad_)
                    break; // main() boots straight into it
                // runLoadPicker left the screen its own way; take it back.
                bgs.setLetterbox(false);
                showPointer(false);
                s.dirty = true;
            }
            if (s.sel == RowSettings)
            {
                runSettings();
                // runSettings left the screen its own way; take it back.
                bgs.setLetterbox(false);
                showPointer(false);
                s.dirty = true;
            }
        }

        if (!s.dirty)
            continue;
        s.dirty = false;

        const std::string labels[kRowCount] = {"New game", "Load game", "Settings"};
        paint("Riven", labels, kRowCount, s.sel, std::string());
        // Shown at the top of the next turn, by frame().
        bgs.requestFlip();
    }

    // The engine draws the first card over whatever is here, and it draws into
    // the buffer it thinks is clean -- so every buffer has to be given back the
    // way create() left it rather than holding half a menu. resetBuffer and not
    // textLayer.clear(): the clear is opaque, and the rows below the card view
    // owe the renderer transparency (BgSurface::resetBuffer).
    for (int b = 0; b < BgSurface::kBuffers; ++b)
        bgs.resetBuffer(b);
    bgs.setLetterbox(true);
    showPointer(true);
}

} // namespace rivenrt

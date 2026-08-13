#include "MainMenu.hpp"

#include <cstdio>
#include <string>

#include "Global.hpp"
#include "Settings.hpp"
#include "engine/Engine.hpp"
#include "global_header.hpp"
#include "render/BgSurface.hpp"
#include "render/Cursor.hpp"
#include "render/TextLayer.hpp"

namespace rivenrt
{

MainMenu mainMenu;

namespace
{
    // Layout. Rows are full-width for the hit test even though the label is not,
    // because a menu row on a touch screen is a band, not a word.
    constexpr int kTitleY = 24;
    constexpr int kFirstY = 72;
    constexpr int kRowStep = 24;
    constexpr int kTextX = 40;
    /// The selection marker sits in the margin, so a row's text does not move
    /// when the selection lands on it.
    constexpr int kMarkX = 20;

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

    /// Draw into the buffer that is not on screen, then flip. The engine is not
    /// running, so nothing else is publishing and this has to drive the vblank
    /// itself.
    void present()
    {
        bgs.requestFlip();
        bgs.vblank();
    }

    void frame() { NEA_WaitForVBL(static_cast<NEA_UpdateFlags>(0)); }

    /// True when the port's screens can be drawn at all.
    bool usable() { return bgs.exists() && textLayer.ready(); }

    /// Put the pointer away, or bring it back. setVisible only raises a flag --
    /// it is Cursor::flush that writes OAM, and nothing is calling flush while
    /// a menu owns the frame, so this has to.
    void showPointer(bool on)
    {
        Cursor &c = engine.cursor();
        if (!c.exists())
            return;
        c.setVisible(on);
        c.flush();
    }
} // namespace

void MainMenu::runSettings()
{
    if (!usable())
    {
        std::printf("settings need the menu font; not showing them\n");
        return;
    }

    enum Row
    {
        RowZip = 0,
        RowTransitions,
        RowWater,
        RowVolume,
        RowDebug,
        RowBack,
        kRowCount,
    };

    Screen s;
    s.rows = kRowCount;
    bool changed = false;
    bool leaving = false;

    // The card view, if there is one, is on the front buffer and must survive:
    // the settings screen can be opened mid-game from Riven's own Options
    // button, and the card has to come back afterwards without being reloaded.
    // This is the same trick a fullscreen movie uses (BgSurface.hpp:94-100).
    const bool inGame = engine.booted();
    if (inGame)
        bgs.beginMovieTakeover();
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
                // Ten steps, wrapping, so one control works for both a D-pad
                // and a touch. LEFT goes down, everything else goes up.
                const int step = 255 / 10;
                int v = settings.masterVolume;
                if (down & KEY_LEFT)
                    v = v < step ? 255 : v - step;
                else
                    v = v > 255 - step ? 0 : v + step;
                settings.masterVolume = static_cast<std::uint8_t>(v);
                changed = true;
                // Live, so the slider is heard while it moves.
                settings.apply();
                break;
            }
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

        textLayer.target(bgs.backBuffer());
        textLayer.clear();
        textLayer.draw(kTextX, kTitleY, "Settings");

        const char *const onOff[2] = {"Off", "On"};
        const std::string labels[kRowCount] = {
            std::string("Zip mode: ") + onOff[settings.zipMode],
            std::string("Transitions: ") + onOff[settings.transitions],
            std::string("Water: ") + onOff[settings.water] + " (not yet animated)",
            "Volume: " + std::to_string((settings.masterVolume * 10 + 127) / 255),
            // The only row that does not take effect where it is set:
            // DebugLog::begin reads it once, at startup, because it takes the
            // top screen away from the picture as it does so.
            std::string("Debug log: ") + onOff[settings.debugMode] + " (on restart)",
            "Back",
        };
        for (int i = 0; i < kRowCount; ++i)
        {
            const int y = kFirstY + i * kRowStep;
            if (i == s.sel)
                textLayer.draw(kMarkX, y, ">");
            textLayer.draw(kTextX, y, labels[i]);
        }
        present();
    }

    if (changed)
    {
        settings.save();
        settings.apply();
    }

    bgs.setLetterbox(true);
    if (inGame)
    {
        // Hands the card back and names the buffer this screen scribbled on, so
        // the card view can rebuild it rather than trusting what is there.
        const int stale = bgs.endMovieTakeover();
        engine.surface().invalidate(stale);
        engine.applyScreenUpdate(true);
        showPointer(true);
    }
}

void MainMenu::run()
{
    if (!usable())
    {
        std::printf("no menu: starting the game directly\n");
        return;
    }

    enum Row
    {
        RowNewGame = 0,
        RowSettings,
        // Load save belongs here, and does not exist yet: saves are the other
        // half of milestone 9. A row that said so and did nothing would be
        // worse than the gap.
        kRowCount,
    };

    Screen s;
    s.rows = kRowCount;

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

        textLayer.target(bgs.backBuffer());
        textLayer.clear();
        textLayer.draw(kTextX, kTitleY, "Riven");

        const char *const labels[kRowCount] = {"New game", "Settings"};
        for (int i = 0; i < kRowCount; ++i)
        {
            const int y = kFirstY + i * kRowStep;
            if (i == s.sel)
                textLayer.draw(kMarkX, y, ">");
            textLayer.draw(kTextX, y, labels[i]);
        }
        present();
    }

    // The engine draws the first card over whatever is here, and it draws into
    // the buffer it thinks is clean -- so both buffers have to be given back
    // empty rather than holding half a menu.
    for (int b = 0; b < BgSurface::kBuffers; ++b)
    {
        textLayer.target(b);
        textLayer.clear();
    }
    bgs.setLetterbox(true);
    showPointer(true);
}

} // namespace rivenrt

// Garden Island's external commands -- ScummVM's riven_stacks/gspit.cpp.
//
// The island of viewing devices, and four of them need native code:
//
//   * the PIN MAP, a scale model of Riven whose sections rise on pins -- one
//     movie per section, one slice of it per rotation of the table, and a click
//     anywhere on a 6x5 grid that has to be un-rotated before it means anything;
//   * the RIGHT VIEWER, which shows the marble grid's colours;
//   * the LEFT VIEWER, which shows the village or Catherine's cage -- and which
//     is the only thing in the game that keeps playing movies at the player on
//     a timer for as long as they stand and watch; and
//   * the WHARK, who comes to the lake when the lights go out, and stops coming
//     after five visits.
//
// Plus the dome, whose five sliders are shared with four other islands, the
// pools, which are one line, and the scribe, who is on a forty-second clock.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "DebugLog.hpp"
#include "engine/DomeSpit.hpp"
#include "engine/Engine.hpp"
#include "engine/Externals.hpp"

using namespace rivendata;

namespace rivenrt
{
namespace
{
    // --- the pin map ---------------------------------------------------------
    //
    // A model of the island under glass. Press a section and it rises on a pin;
    // turn the table and the whole thing rotates a quarter turn. Both are the
    // same long movie played a slice at a time -- one movie per raisable
    // section, whose timeline is the pin going up at each of the four rotations
    // followed by the table turning -- so every command here is a seek and a
    // half-second of playback.
    //
    // gupmoov is which of those movies is currently holding a section up. It is
    // NOT in any stack's NAME 4 list; ScummVM's variable map invents it and
    // RivenVars.hpp carries it by hand for the same reason (see the header).

    /// GSpit::lowerPins (gspit.cpp:58-83). Put whatever is up back down.
    void lowerPins(Engine &e)
    {
        if (e.vars().get(VarId::GPinUp) == 0)
            return;

        // 1..4, and provably: it starts at 1 (Vars::kDefaults) and xgrotatepins
        // is the only thing that writes it, having clamped it first. So no
        // bounds test here, where an unsigned 0 would wrap the subtraction.
        const std::uint32_t pinPos = e.vars().get(VarId::GPinPos);
        const std::uint32_t startTime = (pinPos - 1) * 600 + 4830;
        e.vars().set(VarId::GPinUp, 0);

        e.playEffect(13, 256); // the pins going down

        const std::uint16_t movie = static_cast<std::uint16_t>(e.vars().get(VarId::GUpMoov));
        e.playMovieRange(movie, startTime, startTime + 550);
        // disable(), which bakes the last frame into the card: the section has
        // to stay down rather than snap back to whatever the still shows.
        e.enableMovie(movie, false);

        e.vars().set(VarId::GUpMoov, 0);
    }

    /// GSpit::xgrotatepins (gspit.cpp:90-118). A quarter turn of the table, out
    /// of the tail of the same movie the raised section is holding.
    void xgrotatepins(Engine &e)
    {
        if (e.vars().get(VarId::GPinUp) == 0)
            return; // nothing is up, so there is nothing to rotate

        // Eight entries for four positions, because the turn out of position 4
        // wraps: it runs from 3616 to 4846 and the table is then back at 1
        // (gspit.cpp:98-107). Slot 0 is only reachable from a variable that was
        // never initialised, and ScummVM would play it as an 8416..0 range.
        static const std::uint32_t kPinPosTimes[] = {8416, 0,    1216, 2416,
                                                     3616, 4846, 6016, 7216};

        std::uint32_t &pinPos = e.vars().at(VarId::GPinPos);
        // The table has four positions and the variable is the only thing that
        // says which. This is the one place it is written, so clamping here is
        // what makes 1..4 true everywhere else -- and a corrupt one would
        // otherwise index the table twice, once past its end.
        if (pinPos < 1 || pinPos > 4)
            pinPos = 1;

        const std::uint32_t startTime = kPinPosTimes[pinPos];
        ++pinPos;
        const std::uint32_t endTime = kPinPosTimes[pinPos];
        if (pinPos > 4)
            pinPos = 1;

        e.playEffect(12, 256); // the table turning

        const std::uint16_t movie = static_cast<std::uint16_t>(e.vars().get(VarId::GUpMoov));
        e.playMovieRange(movie, startTime, endTime);
        e.enableMovie(movie, false);
    }

    /// GSpit::xgpincontrols (gspit.cpp:120-218). A click on the map.
    void xgpincontrols(Engine &e)
    {
        const Hotspot *const panel = e.hotspotByBlstId(13);
        if (panel == nullptr)
        {
            DebugLog::warn("xgpincontrols: card %u has no hotspot 13",
                           static_cast<unsigned>(e.cardId()));
            return;
        }

        // Which cell of the 6x5 grid was pressed. The model is drawn at an
        // angle, so the cells are 10 wide and 11 tall rather than square.
        const Rect &r = e.hotspotRect(panel);
        int x = (e.pointerCardX() - r.left) / 10;
        int y = (e.pointerCardY() - r.top) / 11;

        // And now un-rotate it. The map turns under the glass but the grid does
        // not, so the same cell means a different piece of the island at each of
        // the four positions (gspit.cpp:134-156). The multiplications by five
        // are what turn a row into an offset -- the island is five slots wide.
        const std::uint32_t pinPos = e.vars().get(VarId::GPinPos);
        switch (pinPos)
        {
        case 1:
            x = 5 - x;
            y = (4 - y) * 5;
            break;
        case 2:
            x = (4 - x) * 5;
            y = 1 + y;
            break;
        case 3:
            x = 1 + x;
            y = y * 5;
            break;
        case 4:
            x = x * 5;
            y = 5 - y;
            break;
        default:
            // ScummVM calls error() here, which quits the game (gspit.cpp:155).
            // The position is a save-file variable; a bad one is a reason for
            // the map not to respond, not to stop playing.
            DebugLog::warn("xgpincontrols: gpinpos is %lu, not 1..4",
                           static_cast<unsigned long>(pinPos));
            return;
        }

        const std::uint32_t islandIndex = e.vars().get(VarId::GLkBtns);
        if (islandIndex == 0 || islandIndex > 5)
        {
            // No island selected, which ScummVM notes happens when the card is
            // jumped to rather than walked to (gspit.cpp:160-164).
            DebugLog::warn("xgpincontrols: no island selected (glkbtns %lu)",
                           static_cast<unsigned long>(islandIndex));
            return;
        }

        const int imagePos = x + y;

        // Which of the twenty-five slots each island actually occupies
        // (gspit.cpp:168-174). Ragged: Temple Island is one slot and Jungle
        // Island is eleven, and gimagemax is how many of each row are real.
        static const std::uint16_t kIslandImages[5][11] = {
            {1, 2, 6, 7},
            {11, 16, 21, 22},
            {12, 13, 14, 15, 17, 18, 19, 20, 23, 24, 25},
            {5},
            {3, 4, 8, 9, 10},
        };

        // The scripts set gimagemax to the length of the row above.
        std::uint32_t imageCount = e.vars().get(VarId::GImageMax);
        if (imageCount > 11)
            imageCount = 11;

        std::uint32_t image = 0;
        for (; image < imageCount; ++image)
            if (kIslandImages[islandIndex - 1][image] == imagePos)
                break;

        // Past the end: the click was on a part of the glass with no island
        // under it.
        if (image == imageCount)
            return;

        if (e.vars().get(VarId::GPinUp) == 1)
        {
            lowerPins(e);
            // Pressing the section that is already up puts it down and stops
            // there -- that is how the map is cleared.
            if (e.vars().get(VarId::GImageCurr) == image)
                return;
        }

        // Which movie holds which slot. Eleven movies for twenty-five slots,
        // because the sections of one island share a recording (gspit.cpp:200).
        static const std::uint16_t kPinMovieCodes[25] = {1, 2, 1, 2, 1, 3, 4, 3, 4,
                                                         5, 1, 1, 2, 3, 4, 2, 5, 6,
                                                         7, 8, 3, 4, 9, 10, 11};
        // imagePos indexes it, and it comes out of the arithmetic above rather
        // than out of the table -- so it is checked here and not taken on trust.
        if (imagePos < 1 || imagePos > 25)
        {
            DebugLog::warn("xgpincontrols: slot %d is off the map", imagePos);
            return;
        }
        const std::uint16_t movie = kPinMovieCodes[imagePos - 1];

        e.playEffect(14, 256); // the pins coming up

        // The rise, at this rotation. Counting DOWN from 9630 because the movie
        // runs the four rotations in order and position 1 is the last of them.
        const std::uint32_t startTime = 9630 - pinPos * 600;
        e.playMovieRange(movie, startTime, startTime + 550);
        e.enableMovie(movie, false);

        e.vars().set(VarId::GUpMoov, movie);
        e.vars().set(VarId::GPinUp, 1);
        e.vars().set(VarId::GImageCurr, image);
    }

    // --- the pools -----------------------------------------------------------

    /// GSpit::xgplateau3160_dopools (gspit.cpp:244-250). Turn the previous
    /// island's pool off before the script turns the new one on. The odd codes
    /// are the pools lighting and the even ones are them going out, so the one
    /// to play is the selection doubled.
    void xgplateau3160_dopools(Engine &e)
    {
        const std::uint32_t selected = e.vars().get(VarId::GLkBtns);
        if (selected != 0)
            e.playMovie(static_cast<std::uint16_t>(selected * 2), true);
    }

    // --- the scribe ----------------------------------------------------------
    //
    // The villager writing at the far end of the water tunnel. Look at him for
    // forty seconds and he notices; look away and come back and the clock keeps
    // running, which is why this is a stamp rather than a countdown.

    /// GSpit::xgwt200_scribetime (gspit.cpp:252-255) and xgwt900_scribe
    /// (gspit.cpp:257-262).
    ///
    /// ScummVM stamps getMillis(), a wall clock. Engine::clock() is engine
    /// frames, which is the port's only clock and the one bspit's Ytram trap is
    /// already written against -- and unlike a wall clock it is in the save
    /// file, so a scribe watched across a save still notices at the right time.
    constexpr std::uint32_t kScribeDelayMs = 40000;

    // --- the viewers ---------------------------------------------------------

    /// Where each of the twelve stops sits in the viewers' movie, in the
    /// original's milliseconds (gspit.cpp:264). Twelve for six positions,
    /// because the movie holds two revolutions -- a viewer at position 4 asked
    /// to advance 5 needs somewhere past the end of the first to run to.
    constexpr std::uint16_t kViewerStops[] = {0,    816,  1617, 2416, 3216, 4016,
                                              4816, 5616, 6416, 7216, 8016, 8816};

    /// How far the button that was pressed moves a viewer.
    ///
    /// ScummVM reads the last character of the hotspot's own name and subtracts
    /// '0' (gspit.cpp:284-285, :377-378) -- the buttons are called "pos1"
    /// through "pos5" and the number IS the number of stops. Zero when there is
    /// no current hotspot or its name does not end in a digit, which means the
    /// viewer does not move.
    std::uint32_t viewerButtonStep(Engine &e)
    {
        const Hotspot *const h = e.currentHotspot();
        if (h == nullptr || h->nameRes < 0)
            return 0;
        const std::string name =
            e.nameFromList(kHotspotNames, static_cast<std::uint16_t>(h->nameRes));
        if (name.empty())
            return 0;
        const char last = name[name.size() - 1];
        if (last < '0' || last > '9')
            return 0;
        return static_cast<std::uint32_t>(last - '0');
    }

    /// Turn a viewer `step` stops on, playing the movie of it turning. Returns
    /// the new position, 0..5.
    ///
    /// The shared half of xgrviewer and xglviewer, which differ only in the
    /// variable and in what they do afterwards (gspit.cpp:287-300, :380-395).
    std::uint32_t turnViewer(Engine &e, VarId posVar, std::uint32_t step)
    {
        std::uint32_t &curPos = e.vars().at(posVar);
        if (curPos > 5)
            curPos = 0; // a save-file variable; six positions and no more
        const std::uint32_t newPos = curPos + step;
        if (newPos >= sizeof(kViewerStops) / sizeof(kViewerStops[0]))
        {
            DebugLog::warn("viewer: %lu + %lu is past the end of the movie",
                           static_cast<unsigned long>(curPos),
                           static_cast<unsigned long>(step));
            return curPos;
        }

        e.playMovieRange(1, kViewerStops[curPos], kViewerStops[newPos]);
        // disable() then stop() (gspit.cpp:295-296): the wheel stays where it
        // stopped, and the movie is released rather than left holding its slot
        // -- the next press seeks it somewhere else entirely.
        e.enableMovie(1, false);
        e.stopMovie(1);

        curPos = newPos % 6;
        return curPos;
    }

    /// GSpit::xgrviewer (gspit.cpp:266-301). The right-hand viewer, which shows
    /// the marble grid's colours -- and which has to be switched off before it
    /// can be turned.
    void xgrviewer(Engine &e)
    {
        // BEFORE the refresh below, and this is the one line here that is not
        // ScummVM's order (gspit.cpp:284). Its _curHotspot belongs to the
        // engine and survives a card re-entry; this port's belongs to the card
        // and resetCardState clears it -- so reading the button afterwards
        // would find nothing and the viewer would never turn.
        const std::uint32_t step = viewerButtonStep(e);

        if (e.vars().get(VarId::GRView) == 1)
        {
            e.vars().set(VarId::GRView, 0);
            e.playCardSound("gScpBtnUp");
            e.refreshCard(); // the card draws the lamp from grview

            // Let the button's own click finish before the wheel starts moving,
            // which is what makes the two read as one action (gspit.cpp:278-280).
            while (e.isEffectPlaying() && !e.quitRequested())
                e.pumpIdleFrame();
        }

        turnViewer(e, VarId::GRViewPos, step);

        // enter(false): the card draws the new colours from grviewpos.
        e.refreshCard();
    }

    /// GSpit::xglviewer (gspit.cpp:372-396). The left-hand viewer, which shows
    /// the village from the middle of the lake. No lamp to switch off, and the
    /// picture is drawn here rather than by the card.
    void xglviewer(Engine &e)
    {
        const std::uint32_t pos = turnViewer(e, VarId::GLViewPos, viewerButtonStep(e));
        e.activatePlst(static_cast<std::uint16_t>(pos + 2));
    }

    // --- Catherine in the viewer ---------------------------------------------
    //
    // The left viewer in prison mode. Catherine is in her cage on Prison Island
    // and the imager shows her live, which means a movie every ten to eighty
    // seconds for as long as the player watches -- sixteen recordings, and which
    // are available depends on where she is standing.
    //
    // gcathstate is that: 1 is one side of the cage, 2 the other, 3 out of
    // sight. Every movie both requires a state and leaves her in one, which is
    // what stops her teleporting across the cage between two clips.

    void catherineViewerIdleTimer(Engine &e);

    /// Start a Catherine movie and arm the timer for the next one. The tail
    /// shared by the idle timer and by switching the viewer on
    /// (gspit.cpp:434-440, :475-480).
    ///
    /// Code 30 and not the movie's own: the imager's picture is one playback
    /// slot that every one of the sixteen recordings is activated into.
    void playCatherineMovie(Engine &e, std::uint16_t index)
    {
        e.activateMlst(index, false);
        e.playMovie(30, false); // NOT blocking: she plays behind the card

        const std::uint32_t next = e.movieDurationMs(30)
                                   + static_cast<std::uint32_t>(randomBetween(0, 60)) * 1000;
        e.installTimer(catherineViewerIdleTimer, next);
    }

    /// GSpit::catherineViewerIdleTimer (gspit.cpp:410-441).
    void catherineViewerIdleTimer(Engine &e)
    {
        std::uint32_t &cathState = e.vars().at(VarId::GCathState);

        // Which recordings can follow from where she is. The repeats are the
        // original's weighting -- 19 and 21 are twice as likely as 9 and 10 out
        // of state 1, and 17 is four times as likely as 12 out of state 3.
        std::uint16_t movie = 0;
        if (cathState == 1)
        {
            static const std::uint16_t kMovies[] = {9, 10, 19, 19, 21, 21};
            movie = kMovies[randomBetween(0, 5)];
        }
        else if (cathState == 2)
        {
            static const std::uint16_t kMovies[] = {18, 20, 22};
            movie = kMovies[randomBetween(0, 2)];
        }
        else
        {
            static const std::uint16_t kMovies[] = {11, 11, 12, 17, 17, 17, 17, 23};
            movie = kMovies[randomBetween(0, 7)];
        }

        // And where it leaves her.
        if (movie == 10 || movie == 17 || movie == 18 || movie == 20)
            cathState = 1;
        else if (movie == 19 || movie == 21 || movie == 23)
            cathState = 2;
        else
            cathState = 3;

        playCatherineMovie(e, movie);
    }

    /// GSpit::xglview_prisonon (gspit.cpp:443-489). Switch the imager to the
    /// cage.
    void xglview_prisonon(Engine &e)
    {
        e.vars().set(VarId::GLView, 1);

        // Where she happens to be when the imager warms up, which is also which
        // "turning on" recording plays: two of the sixteen have their own.
        const std::uint16_t cathMovie = static_cast<std::uint16_t>(randomBetween(8, 23));
        std::uint16_t turnOnMovie = 4;
        if (cathMovie == 14)
            turnOnMovie = 6;
        else if (cathMovie == 15)
            turnOnMovie = 7;

        std::uint32_t &cathState = e.vars().at(VarId::GCathState);
        if (cathMovie == 9 || cathMovie == 11 || cathMovie == 12 || cathMovie == 22)
            cathState = 3;
        else if (cathMovie == 19 || cathMovie == 21 || cathMovie == 23 || cathMovie == 14)
            cathState = 2;
        else
            cathState = 1;

        e.playMovie(turnOnMovie, true);

        // She is in shot already: start her straight away. Otherwise the imager
        // shows the empty cage and waits for her to walk back into it.
        if (cathMovie == 8 || (cathMovie >= 13 && cathMovie <= 16))
        {
            playCatherineMovie(e, cathMovie);
            return;
        }

        e.activatePlst(8);
        e.installTimer(catherineViewerIdleTimer,
                       static_cast<std::uint32_t>(randomBetween(10, 20)) * 1000);
    }

    /// GSpit::xglview_prisonoff (gspit.cpp:491-507).
    void xglview_prisonoff(Engine &e)
    {
        e.vars().set(VarId::GLView, 0);
        e.removeTimer();

        // Whatever Catherine was doing stops first (gspit.cpp:501).
        // disableAllMovies and not closeAllMovies: ScummVM calls the former,
        // which is opcode 29 and bakes -- and it does not matter which, because
        // the turn-off movie plays over the same rows and the picture below
        // replaces them anyway. Kept as the original's call so it reads as one.
        e.disableAllMovies();
        e.playMovie(5, true);

        e.activatePlst(1);
    }

    // --- the whark -----------------------------------------------------------

    /// GSpit::xgplaywhark (gspit.cpp:303-348). Turn the lake lights out and
    /// something comes to look. Five times, and then it stops coming.
    void xgplaywhark(Engine &e)
    {
        // One visit per darkening: the scripts set gwharktime to 1 when the
        // lights go out and this is what spends it.
        if (e.vars().get(VarId::GWharkTime) != 1)
            return;
        e.vars().set(VarId::GWharkTime, 0);

        std::uint32_t &visits = e.vars().at(VarId::GWhark);
        ++visits;
        if (visits >= 5)
        {
            visits = 5;
            return;
        }

        switch (visits)
        {
        case 1:
            e.activateMlst(3, false);
            break;
        case 2:
            e.activateMlst(static_cast<std::uint16_t>(4 + randomBetween(0, 1)), false);
            break;
        case 3:
            e.activateMlst(static_cast<std::uint16_t>(6 + randomBetween(0, 1)), false);
            break;
        case 4:
            e.activateMlst(8, false); // the one where it notices you
            break;
        default:
            break;
        }

        // "For whatever reason the devs felt fit, code 31 is used for all of the
        // videos" (gspit.cpp:345). Every one of the records above is activated
        // into that one slot, so this plays whichever it was.
        e.playMovie(31, true);
    }

    /// GSpit::xgwharksnd (gspit.cpp:350-370). The whark somewhere out in the
    /// dark, heard and not seen. Roughly one time in four, after a moment.
    void xgwharksnd(Engine &e)
    {
        if (e.vars().get(VarId::GWhark) >= 5)
            return;

        // 1..36 and only 1..9 sound: nine recordings and a three-in-four chance
        // of silence, written as one roll (gspit.cpp:358-362).
        const int soundId = randomBetween(1, 36);
        if (soundId >= 10)
            return;

        // 121..150 MILLISECONDS. It reads like a frame count and is not -- the
        // original's delay() takes milliseconds, and this is the beat before the
        // call rather than a wait for the creature to arrive.
        e.delay(static_cast<std::uint32_t>(randomBetween(1, 30)) + 120);

        // "gWharkSolo1" through "gWharkSolo9". Sized for the format's widest
        // possible expansion rather than for the nine names that can actually
        // come out of it, so the compiler does not have to take the range on
        // trust.
        char name[24];
        std::snprintf(name, sizeof(name), "gWharkSolo%d", soundId);
        e.playCardSound(name);
    }

    // --- the dome ------------------------------------------------------------

    /// gspit's dome, on card 25. The slots are BLST 11..35 and the artwork is
    /// named for card 190, where it was first used (GSpit's constructor,
    /// gspit.cpp:33-34).
    constexpr DomeConfig kGspitDome{11, "gsliders.190", "gsliderbg.190"};

} // namespace

bool runGspitExternal(Engine &e, const std::string &key, const std::uint16_t *args,
                      std::size_t argCount)
{
    (void)args;
    (void)argCount;

    // --- the pin map ---------------------------------------------------------
    if (key == "xgresetpins")
    {
        lowerPins(e);
        return true;
    }
    if (key == "xgrotatepins")
    {
        xgrotatepins(e);
        return true;
    }
    if (key == "xgpincontrols")
    {
        xgpincontrols(e);
        return true;
    }

    // --- the pools and the scribe --------------------------------------------
    if (key == "xgplateau3160_dopools")
    {
        xgplateau3160_dopools(e);
        return true;
    }
    if (key == "xgwt200_scribetime")
    {
        e.vars().set(VarId::GScribeTime, e.clock());
        return true;
    }
    if (key == "xgwt900_scribe")
    {
        std::uint32_t &scribe = e.vars().at(VarId::GScribe);
        if (scribe == 1
            && e.clock() > e.vars().get(VarId::GScribeTime)
                               + Engine::msToFrames(kScribeDelayMs))
            scribe = 2;
        return true;
    }

    // --- the viewers ---------------------------------------------------------
    if (key == "xgrviewer")
    {
        xgrviewer(e);
        return true;
    }
    if (key == "xglviewer")
    {
        xglviewer(e);
        return true;
    }
    if (key == "xglview_villageon")
    {
        e.vars().set(VarId::GLView, 2);
        e.activatePlst(static_cast<std::uint16_t>(e.vars().get(VarId::GLViewPos) + 2));
        return true;
    }
    if (key == "xglview_villageoff")
    {
        // "(why is this external?)" -- gspit.cpp:404. It is, so here it is.
        e.vars().set(VarId::GLView, 0);
        e.activatePlst(1);
        return true;
    }
    if (key == "xglview_prisonon")
    {
        xglview_prisonon(e);
        return true;
    }
    if (key == "xglview_prisonoff")
    {
        xglview_prisonoff(e);
        return true;
    }

    // --- the whark -----------------------------------------------------------
    if (key == "xgplaywhark")
    {
        xgplaywhark(e);
        return true;
    }
    if (key == "xgwharksnd")
    {
        xgwharksnd(e);
        return true;
    }

    // --- the dome ------------------------------------------------------------
    if (key == "xgisland25_resetsliders")
    {
        resetDomeSliders(e, kGspitDome);
        return true;
    }
    if (key == "xgisland25_slidermd")
    {
        dragDomeSlider(e, kGspitDome);
        return true;
    }
    if (key == "xgisland25_slidermw")
    {
        checkSliderCursorChange(e, kGspitDome);
        return true;
    }
    if (key == "xgisland25_opencard")
    {
        checkDomeSliders(e);
        return true;
    }
    if (key == "xgscpbtn")
    {
        runDomeButtonMovie(e);
        return true;
    }
    if (key == "xgisland1490_domecheck")
    {
        runDomeCheck(e);
        return true;
    }

    return false;
}

} // namespace rivenrt

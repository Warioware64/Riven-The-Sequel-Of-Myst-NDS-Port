#pragma once

// What a conversion run is asked to do.
//
// This struct owns the coupling between stages, so a UI cannot present a
// combination the pipeline will not honour. The Myst converter learned this the
// hard way and ended up duplicating its Options coercion inside the GUI's
// switch handler to stop the switches from lying about what would happen;
// normalise() here is the single place that rule lives.

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "RivenData.hpp"

namespace riven
{

/// One selectable stage of the pipeline.
enum class Stage
{
    Cards,   ///< stacks/<stack>.bin -- the card graph, scripts and per-card lists
    Images,  ///< pics/<stack>/<id>.rpic -- downscaled ARGB1555 stills
    Hires,   ///< pics_hi/<stack>/<id>.rpiz -- full-resolution twins for the zoom viewer
    Water,   ///< sfxe/<stack>/<id>.rsfx -- water-effect frame scripts
    Audio,   ///< sound/<stack>/<id>.rsnd -- mono IMA ADPCM
    Video,   ///< video/<stack>/<id>.rvid -- the RVID codec
    Cursors, ///< cursors/cursors.rcur -- Riven's own cursors, from riven.exe
    Extras,  ///< extras/inventory.rcur -- the inventory books, from extras.MHK
};

const char *stageName(Stage s);
/// False for stages that are declared but not yet built. The GUI shows these
/// disabled rather than hiding them, so the roadmap is visible.
bool stageImplemented(Stage s);

struct Options
{
    std::filesystem::path source;
    std::filesystem::path dest; ///< card ROOT; data lands in <dest>/_nds/riven_nds/data/

    bool cards = true;
    bool images = true;
    bool hires = true;
    bool water = true;
    bool audio = true;
    bool video = true;
    /// Not per-stack, unlike everything above: both come from files that
    /// belong to the game as a whole rather than to an age.
    bool cursors = true;
    bool extras = true;

    /// Spend Riven's crest factor to make the game audible on DS speakers:
    /// compress and lift the sounds and the movie soundtracks
    /// (SoundPipeline.hpp's Loudness note). On by default because without it the
    /// port is quiet in a way no runtime volume control can fix -- every peak in
    /// the source is already at full scale. Off is the untouched material, for
    /// comparing the two.
    bool compressAudio = true;


    /// Where ffmpeg is. The video stage decodes through it (riven/FFmpeg.hpp),
    /// so this is the one external tool the converter needs. Empty means "search
    /// PATH", which is what it is on any machine that installed ffmpeg normally;
    /// it may be the binary itself or the directory holding it.
    std::filesystem::path ffmpegPath;

    /// Reconvert assets whose output is already up to date. Off by default:
    /// skipping finished work is what makes a cancelled run resumable and a
    /// re-run fast.
    bool force = false;

    /// Restrict the run to these stacks. Empty means every stack found.
    std::set<rivendata::StackId> stacks;

    bool enabled(Stage s) const;
    void setEnabled(Stage s, bool on);

    /// Clamp to what is actually buildable and resolve stage dependencies.
    /// Called by the converter before anything else, so a caller that forgets
    /// still gets a coherent run.
    void normalise();

    /// True when nothing at all would be produced.
    bool empty() const
    {
        return !cards && !images && !hires && !water && !audio && !video && !cursors
            && !extras;
    }
};

/// Named stage combinations offered in the UI.
struct Preset
{
    const char *name;
    const char *description;
    Options (*apply)(Options base);
};

/// The presets, in display order. The last entry is always "Custom", which
/// applies nothing -- selecting it is what the UI does when the checkboxes stop
/// matching any named preset.
const std::vector<Preset> &presets();

/// Name of the preset whose stage flags match `o`, or "Custom".
std::string matchingPresetName(const Options &o);

} // namespace riven

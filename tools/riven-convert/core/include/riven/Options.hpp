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

    /// RVID quantiser scale, as a percentage: 100 is the tuned default, lower
    /// is smaller and softer, higher is larger and sharper. Only the three AC
    /// coefficients follow it -- see rvidQuantSteps.
    int videoQuality = 100;

    /// Movies to encode at once. 0 picks one per core, less one. Only the
    /// video stage uses it; everything else is fast enough to stay serial.
    int jobs = 0;

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
    bool empty() const { return !cards && !images && !hires && !water && !audio && !video; }
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

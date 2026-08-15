#include "riven/Converter.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>

#include <yas/mem_streams.hpp>
#include <yas/binary_oarchive.hpp>

#include "RivenData.hpp"
#include "RivenSfxe.hpp"
#include "RivenVideo.hpp"
#include "riven/Archive.hpp"
#include "riven/AtomicWrite.hpp"
#include "riven/CardParse.hpp"
#include "riven/CursorPipeline.hpp"
#include "riven/Installer.hpp"
#include "riven/ImagePipeline.hpp"
#include "riven/Marbles.hpp"
#include "riven/MovieList.hpp"
#include "riven/SoundPipeline.hpp"
#include "riven/TopBg.hpp"
#include "riven/VideoPipeline.hpp"
#include "riven/WaterEffect.hpp"

namespace fs = std::filesystem;

namespace riven
{
namespace
{
    // Same archive flags the ARM9 reads with. See RivenData.hpp: the file
    // carries our own versioned StackFileHeader instead of yas's.
    constexpr std::size_t kYasFlags = yas::mem | yas::binary | yas::no_header;

    /// True if `path` is a stack file this build's ARM9 would accept.
    ///
    /// A .format stamp cannot answer this and must not be used here, which is
    /// worth spelling out because every other stage does use one. A stamp
    /// describes a DIRECTORY, and pics/, pics_hi/, video/ and sound/ each get
    /// one directory PER STACK -- but all eight stack files share stacks/. A
    /// run restricted with --stack would stamp that shared directory after
    /// converting one of them, and every later run would then read the stamp,
    /// conclude the whole directory was current, and skip the seven files still
    /// written to the old schema. They would never be redone, and the ROM
    /// rejects them the moment the player links to one.
    ///
    /// The file answers for itself instead: StackFileHeader carries the schema
    /// version it was written with, and headerLooksValid is the same check the
    /// ARM9 makes (RivenData.hpp:74-78, StackFile.cpp:35-42).
    bool stackFileIsCurrent(const fs::path &path)
    {
        std::FILE *f = std::fopen(path.string().c_str(), "rb");
        if (f == nullptr)
            return false;
        rivendata::StackFileHeader hdr{};
        const bool read = std::fread(&hdr, 1, sizeof(hdr), f) == sizeof(hdr);
        std::fclose(f);
        return read && rivendata::headerLooksValid(hdr);
    }

    // -----------------------------------------------------------------------
    // One asset at a time
    // -----------------------------------------------------------------------
    //
    // This stage used to run a thread pool, one job per core, each worker with its
    // own libvaht handles. It was removed on purpose and the cost is real, so it is
    // written down here rather than rediscovered: the work is embarrassingly
    // parallel (1055 movies, ~162k frames, no job touching another's output) and
    // CPU-bound, so a serial run gives most of the machine back to nobody.
    //
    // Measured on an eight-core machine, rspit's 28 movies and 14831 frames: 32.7 s
    // at 185% CPU. The other ~6 cores are idle. ffmpeg cannot take them either --
    // its Cinepak decoder is single-threaded and 1038 of the 1055 movies are
    // Cinepak -- so removing `-threads 1` from it changed nothing measurable
    // (32.3 s pinned, 32.7 s not). The 185% is this process quantising the last
    // frame while ffmpeg's decodes the next through the pipe.
    //
    // What is bought with that: peak memory is one movie rather than one per core
    // (64 MB resident for the run above, against workers each holding a tMOV of up
    // to 25 MB), the log lines come out in resource order instead of interleaved,
    // and there is nothing left in here that needs a lock. The GUI is unaffected --
    // it runs Converter::run on its own QThread and always did.

    /// Work units. Weighted so the bar moves at a roughly even rate: an image
    /// costs far more than a card, and a hi-res twin costs more again because
    /// of the LZ77 pass.
    constexpr std::uint64_t kCardWork = 1;
    constexpr std::uint64_t kImageWork = 8;
    constexpr std::uint64_t kHiresWork = 12;
    constexpr std::uint64_t kEffectWork = 2;
    /// A sound is one archive read plus one decode pass over a few hundred KB.
    /// Cheaper than an image, dearer than a card.
    constexpr std::uint64_t kSoundWork = 3;
    /// A movie is hundreds of frames, each decoded, downscaled and re-encoded.
    /// It dwarfs everything else in the pipeline, and the weighting is what
    /// stops the progress bar sitting at 90% for most of a run.
    constexpr std::uint64_t kMovieWork = 400;

    std::string stackDir(const rivendata::StackId id) { return rivendata::stackName(id); }

    /// ASCII fold, matching Vars::normalise on the ARM9 side. The NAME lists are
    /// mixed case ("aAtrusBook") and RivenVars.hpp stores them folded; std::tolower
    /// is avoided because it is locale-dependent and these are resource strings.
    std::string toLowerAscii(const std::string &s)
    {
        std::string out;
        out.reserve(s.size());
        for (const char c : s)
            out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
        return out;
    }

    /// Slurp a file. Only the executable goes through this -- everything else in
    /// the conversion arrives through an archive reader -- so it is deliberately
    /// simple and deliberately here rather than in a header.
    bool readFileInto(const fs::path &path, std::vector<std::uint8_t> &out)
    {
        std::FILE *f = std::fopen(path.string().c_str(), "rb");
        if (f == nullptr)
            return false;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size <= 0)
        {
            std::fclose(f);
            return false;
        }
        out.resize(static_cast<std::size_t>(size));
        const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
        std::fclose(f);
        return ok;
    }

} // namespace

std::string ConversionResult::summary() const
{
    char buf[448];
    std::snprintf(buf, sizeof(buf),
                  "%d stacks, %d cards, %d images, %d zoom twins, %d water effects, "
                  "%d sounds, %d movies (%llu frames), %d cursors, %d inventory images; "
                  "%d skipped, %d warnings, %d errors",
                  stacksConverted, cardsWritten, imagesWritten, hiresWritten,
                  effectsWritten, soundsWritten, moviesWritten,
                  static_cast<unsigned long long>(videoFrames), cursorsWritten,
                  extrasWritten, skipped, warnings, errors);
    return buf;
}

fs::path Converter::dataDir(const fs::path &dest)
{
    return dest / "_nds" / "riven_nds" / "data";
}

ConversionResult Converter::run(Options opts, ProgressSink &sink, CancelToken &cancel)
{
    ConversionResult result;
    opts.normalise();

    CountingProgressSink counting(sink);
    ProgressSink &out = counting;

    if (opts.empty())
    {
        result.message = "Nothing was selected to convert.";
        out.error("setup", result.message);
        return result;
    }

    const SourceInfo info = scanSource(opts.source);
    if (!info.ok())
    {
        result.message = "No Riven data found in " + opts.source.string();
        out.error("setup", result.message);
        return result;
    }

    // ffmpeg, found once for the whole run rather than per movie: the video
    // stage decodes through it, and a run that cannot decode a single movie
    // should say so here instead of a thousand times over the next two hours.
    FFmpegPaths ffmpeg;
    if (opts.video)
    {
        ffmpeg = findFFmpeg(opts.ffmpegPath);
        std::string version;
        std::string ffError;
        if (!probeFFmpeg(ffmpeg, version, ffError))
        {
            result.message = ffError
                           + ". Install ffmpeg, point --ffmpeg at it, or convert "
                             "with --no-video.";
            out.error("setup", result.message);
            return result;
        }
        out.info("setup", "using " + version);
    }

    const fs::path root = dataDir(opts.dest);
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec && !fs::is_directory(root))
    {
        result.message = "Cannot create " + root.string() + ": " + ec.message();
        out.error("setup", result.message);
        return result;
    }

    // A previous run that was killed rather than cancelled can leave .tmp files
    // around. They are never read, but clearing them keeps the card tidy and
    // stops them being mistaken for output.
    if (const int stale = cleanStaleTempFiles(root); stale > 0)
        out.logf(Severity::Info, "setup", "removed %d leftover temporary file(s)", stale);

    out.info("setup", info.summary());
    if (!info.source.missingStacks.empty())
    {
        std::string names;
        for (const auto id : info.source.missingStacks)
        {
            if (!names.empty())
                names += " ";
            names += rivendata::stackName(id);
        }
        out.warn("setup", "stacks absent or unreadable, skipping: " + names);
    }

    // --- total work, so the bar is honest from the first tick ---------------
    std::uint64_t total = 0;
    for (const auto &s : info.stacks)
    {
        if (!opts.stacks.empty() && opts.stacks.find(s.id) == opts.stacks.end())
            continue;
        if (opts.cards)
            total += static_cast<std::uint64_t>(s.cards) * kCardWork;
        if (opts.images)
            total += static_cast<std::uint64_t>(s.images) * kImageWork;
        if (opts.hires)
            total += static_cast<std::uint64_t>(s.images) * kHiresWork;
        if (opts.water)
            total += static_cast<std::uint64_t>(s.effects) * kEffectWork;
        if (opts.audio)
            total += static_cast<std::uint64_t>(s.sounds) * kSoundWork;
        if (opts.video)
            total += static_cast<std::uint64_t>(s.movies) * kMovieWork;
    }
    if (total == 0)
        total = 1;

    std::uint64_t done = 0;
    std::string stage;

    // The single cancellation checkpoint, co-located with the progress
    // bookkeeping exactly as the Myst converter's bump() was.
    const auto bump = [&](std::uint64_t units, const std::string &detail) {
        cancel.throwIfCancelled();
        done += units;
        out.progress(done, total, stage, detail);
    };

    try
    {
        // --- cursors and inventory art --------------------------------------
        //
        // Outside the per-stack loop, because neither belongs to a stack: the
        // cursors are PE resources in riven.exe and the inventory books are
        // tBMPs in extras.MHK, and both describe the game rather than an age.
        //
        // On the CD release neither file exists on its own -- both are inside
        // program/arcriven.z, which is why this converter can read that at all.
        // An install without any of the three is not an error: the game runs
        // with a plain pointer and no inventory strip, and says so.
        if (opts.cursors || opts.extras)
        {
            stage = "cursors";
            InstallerArchive installer;
            if (!info.source.installerArchive.empty())
                installer = InstallerArchive::open(info.source.installerArchive);

            const auto fromInstaller = [&](const char *name) {
                std::vector<std::uint8_t> bytes;
                if (!installer.isOpen())
                    return bytes;
                std::string err;
                bytes = installer.read(name, err);
                if (bytes.empty() && !err.empty())
                    out.warn(stage, err);
                return bytes;
            };

            if (opts.cursors)
            {
                // A loose executable wins: an installed copy has one, and so does
                // the DVD/GOG release. ScummVM orders it the same way.
                std::vector<std::uint8_t> exe;
                if (!info.source.executable.empty())
                    readFileInto(info.source.executable, exe);
                if (exe.empty())
                    exe = fromInstaller("riven.exe");
                if (exe.empty())
                    exe = fromInstaller("rivendmo.exe");

                if (exe.empty())
                {
                    out.warn(stage, "no Riven executable found; the game will use a "
                                    "plain pointer. A Mac install keeps its cursors in "
                                    "a resource fork, which this converter does not read.");
                }
                else
                {
                    std::vector<std::string> warnings;
                    const auto r = convertCursors(exe, root / "cursors" / "cursors.rcur",
                                                  warnings);
                    for (const std::string &w : warnings)
                        out.warn(stage, w);
                    if (!r.ok)
                        out.error(stage, r.error);
                    else
                    {
                        result.cursorsWritten = r.cels;
                        result.bytesWritten += r.bytes;
                        out.logf(Severity::Info, stage, "%d cursors", r.cels);
                    }
                }
            }

            if (opts.extras)
            {
                stage = "inventory art";
                fs::path extras = info.source.extrasArchive;
                if (extras.empty())
                {
                    // libvaht opens by path, so the archive has to become a file.
                    // Kept rather than deleted: it makes the 221 KB inflate happen
                    // once, and the marbles and credits will want it next.
                    const auto bytes = fromInstaller("extras.mhk");
                    if (!bytes.empty())
                    {
                        const fs::path unpacked = root / "extras" / "extras.mhk";
                        std::string err;
                        if (writeFileAtomic(unpacked, bytes, err))
                            extras = unpacked;
                        else
                            out.error(stage, err);
                    }
                }

                if (extras.empty())
                {
                    out.warn(stage, "extras.mhk was not found; Atrus's journal will "
                                    "not be reachable");
                }
                else
                {
                    std::vector<std::string> warnings;
                    const auto r =
                        convertInventory(extras, root / "extras" / "inventory.rcur",
                                         warnings);
                    for (const std::string &w : warnings)
                        out.warn(stage, w);
                    if (!r.ok)
                        out.error(stage, r.error);
                    else
                    {
                        result.extrasWritten = r.cels;
                        result.bytesWritten += r.bytes;
                        out.logf(Severity::Info, stage, "%d inventory images", r.cels);
                    }

                    // --- the marbles -------------------------------------
                    //
                    // Same archive, same stage, and deliberately not fatal: a
                    // conversion without them is a Temple Island whose marble
                    // grid draws nothing, which is worth a line rather than a
                    // failed run.
                    stage = "marbles";
                    std::vector<std::string> marbleWarnings;
                    const auto m = convertMarbles(extras, root / "extras" / "marbles.rpic",
                                                  marbleWarnings);
                    for (const std::string &w : marbleWarnings)
                        out.warn(stage, w);
                    if (!m.ok)
                    {
                        out.warn(stage, m.error + "; the marble puzzle will draw nothing");
                    }
                    else
                    {
                        result.extrasWritten += kMarbleCount;
                        result.bytesWritten += m.bytes;
                        out.logf(Severity::Info, stage, "%d marbles, %dx%d each",
                                 kMarbleCount, m.cellW, m.cellH);
                    }
                }

                // --- the top-screen background ---------------------------
                //
                // Not from an archive at all: AUTORUN.BMP sits loose on the
                // disc, put there by the autorun shell rather than by the
                // game. It is here in the extras stage because it is the same
                // kind of thing as the inventory art -- one picture that
                // describes the port rather than an age -- and because that
                // spares it a stage, a CLI flag and a preset entry for 34 KB.
                stage = "top-screen background";
                if (info.source.autorunBitmap.empty())
                {
                    out.warn(stage, "Autorun/AUTORUN.BMP was not found; the top "
                                    "screen will be black behind the log");
                }
                else
                {
                    const auto r = convertTopBackground(info.source.autorunBitmap,
                                                        root / "ui" / "topbg.rpiz");
                    if (!r.ok)
                        out.error(stage, r.error);
                    else
                    {
                        result.bytesWritten += r.bytes;
                        out.logf(Severity::Info, stage, "%d colours", r.colours);
                    }
                }
            }
        }

        for (const auto &si : info.stacks)
        {
            if (!opts.stacks.empty() && opts.stacks.find(si.id) == opts.stacks.end())
                continue;

            const riven::StackSource *ss = info.source.find(si.id);
            if (ss == nullptr)
                continue;

            const char *name = rivendata::stackName(si.id);
            ArchiveSet set;
            std::vector<std::string> failures;
            set.openAll(ss->dataArchives, failures);
            for (const auto &f : failures)
                out.warn(name, "could not open " + f);
            if (set.empty())
            {
                out.error(name, "no readable archive; skipping this stack");
                continue;
            }

            // --- cards ------------------------------------------------------
            if (opts.cards)
            {
                stage = std::string(name) + " cards";
                const fs::path outFile = root / "stacks" / (std::string(name) + ".bin");

                bool upToDate = !opts.force && stackFileIsCurrent(outFile);
                if (upToDate)
                    for (const auto &a : ss->dataArchives)
                        if (!isUpToDate(outFile, a))
                            upToDate = false;

                if (upToDate)
                {
                    ++result.skipped;
                    out.info(stage, "up to date, skipped");
                    bump(static_cast<std::uint64_t>(si.cards) * kCardWork, "skipped");
                }
                else
                {
                    rivendata::Stack stack;
                    stack.id = si.id;

                    const auto rmapIds = set.resourceIds("RMAP");
                    if (!rmapIds.empty())
                    {
                        const auto bytes = set.read("RMAP", rmapIds.front());
                        ResourceReader r(bytes);
                        stack.rmap = parseRmap(r);
                    }

                    // NAME resources are ids 1..5 and map onto the five lists
                    // the schema carries (card, hotspot, external command,
                    // variable, stack names).
                    for (int i = 0; i < rivendata::kNameListCount; ++i)
                    {
                        const auto id = static_cast<std::uint16_t>(i + 1);
                        if (set.find("NAME", id) == nullptr)
                            continue;
                        const auto bytes = set.read("NAME", id);
                        ResourceReader r(bytes);
                        stack.names[i] = parseName(r);
                    }

                    // Resolve the variable names to the shared enum HERE, once,
                    // so the ROM never sees a variable name at all
                    // (shared/RivenVars.hpp). A name the enum does not know is
                    // not an error -- the game still runs, that one variable just
                    // reads and writes nowhere -- but it is always a bug in
                    // RivenVars.hpp, so say so per occurrence.
                    {
                        const auto &vnames = stack.names[rivendata::kVariableNames].names;
                        stack.variableIds.reserve(vnames.size());
                        for (const std::string &n : vnames)
                        {
                            const rivendata::VarId v = rivendata::parseVarName(toLowerAscii(n));
                            if (v == rivendata::VarId::Unknown)
                                out.logf(Severity::Warning, stage,
                                         "variable \"%s\" is not in RivenVars.hpp; "
                                         "scripts using it will do nothing",
                                         n.c_str());
                            stack.variableIds.push_back(v);
                        }
                    }

                    // The archive's own resource names, for the few lookups
                    // Riven does by name -- the dome's slider strip and the
                    // sounds the scripts do not name by id. See RivenData.hpp
                    // for why one half keeps strings and the other a hash.
                    {
                        for (auto &nv : set.names("tWAV"))
                            stack.soundNames.push_back(
                                rivendata::NamedRes{std::move(nv.first), nv.second});
                        std::sort(stack.soundNames.begin(), stack.soundNames.end(),
                                  [](const rivendata::NamedRes &a,
                                     const rivendata::NamedRes &b) { return a.name < b.name; });

                        std::vector<std::string> bitmapNamesForReport;
                        for (auto &nv : set.names("tBMP"))
                        {
                            stack.bitmapNames.push_back(rivendata::HashedRes{
                                rivendata::hashResourceName(nv.first), nv.second});
                            bitmapNamesForReport.push_back(std::move(nv.first));
                        }
                        // Sort the two together so a duplicate hash can name
                        // both strings it came from.
                        std::vector<std::size_t> order(stack.bitmapNames.size());
                        for (std::size_t i = 0; i < order.size(); ++i)
                            order[i] = i;
                        std::sort(order.begin(), order.end(),
                                  [&](std::size_t a, std::size_t b) {
                                      return stack.bitmapNames[a].hash
                                             < stack.bitmapNames[b].hash;
                                  });
                        std::vector<rivendata::HashedRes> sorted;
                        sorted.reserve(order.size());
                        for (const std::size_t i : order)
                            sorted.push_back(stack.bitmapNames[i]);
                        stack.bitmapNames = std::move(sorted);

                        // A 32-bit hash over ~1400 names collides with
                        // probability ~0.02%, which is small but not zero, and a
                        // collision would silently draw the wrong picture on the
                        // device. Here it is a build error naming both culprits.
                        for (std::size_t i = 1; i < order.size(); ++i)
                            if (stack.bitmapNames[i].hash == stack.bitmapNames[i - 1].hash)
                                out.logf(Severity::Error, stage,
                                         "tBMP name hash collision: \"%s\" and \"%s\"",
                                         bitmapNamesForReport[order[i - 1]].c_str(),
                                         bitmapNamesForReport[order[i]].c_str());

                        out.logf(Severity::Info, stage, "%zu sound names, %zu bitmap names",
                                 stack.soundNames.size(), stack.bitmapNames.size());
                    }

                    int damaged = 0;
                    for (const std::uint16_t id : set.resourceIds("CARD"))
                    {
                        const auto cardBytes = set.read("CARD", id);
                        if (looksDamaged(cardBytes))
                        {
                            ++damaged;
                            bump(kCardWork, "card " + std::to_string(id));
                            continue;
                        }

                        rivendata::Card card;
                        card.id = id;
                        ResourceReader cr(cardBytes);
                        if (!parseCard(cr, card))
                        {
                            out.logf(Severity::Warning, stage, "card %u did not parse", id);
                            bump(kCardWork, "card " + std::to_string(id));
                            continue;
                        }

                        // Each resource is held in a named local: ResourceReader
                        // is a view over the bytes, not an owner.
                        const auto plst = set.read("PLST", id);
                        ResourceReader pr(plst);
                        card.plst = parsePlst(pr);

                        const auto blst = set.read("BLST", id);
                        ResourceReader br(blst);
                        card.blst = parseBlst(br);

                        const auto flst = set.read("FLST", id);
                        ResourceReader fr(flst);
                        card.flst = parseFlst(fr);

                        const auto mlst = set.read("MLST", id);
                        ResourceReader mr(mlst);
                        card.mlst = parseMlst(mr);

                        const auto slst = set.read("SLST", id);
                        ResourceReader sr(slst);
                        card.slst = parseSlst(sr);

                        const auto hspt = set.read("HSPT", id);
                        ResourceReader hr(hspt);
                        card.hotspots = parseHspt(hr);

                        stack.cards.push_back(std::move(card));
                        bump(kCardWork, "card " + std::to_string(id));
                    }

                    if (damaged > 0)
                        out.logf(Severity::Warning, stage,
                                 "%d card(s) are zero-filled in this copy of the game "
                                 "and were skipped",
                                 damaged);

                    std::sort(stack.cards.begin(), stack.cards.end(),
                              [](const auto &a, const auto &b) { return a.id < b.id; });

                    yas::mem_ostream os;
                    yas::binary_oarchive<yas::mem_ostream, kYasFlags> oa(os);
                    oa &stack;
                    const auto buf = os.get_intrusive_buffer();

                    rivendata::StackFileHeader hdr{};
                    std::memcpy(hdr.magic, rivendata::kStackMagic, 4);
                    hdr.schemaVersion = rivendata::kSchemaVersion;
                    hdr.stackId = static_cast<std::uint8_t>(si.id);
                    hdr.cardCount = static_cast<std::uint32_t>(stack.cards.size());
                    hdr.payloadBytes = static_cast<std::uint32_t>(buf.size);

                    std::vector<std::uint8_t> file(sizeof(hdr) + buf.size);
                    std::memcpy(file.data(), &hdr, sizeof(hdr));
                    std::memcpy(file.data() + sizeof(hdr), buf.data, buf.size);

                    std::string err;
                    if (!writeFileAtomic(outFile, file, err))
                    {
                        out.error(stage, err);
                    }
                    else
                    {
                        result.cardsWritten += static_cast<int>(stack.cards.size());
                        result.bytesWritten += file.size();
                        out.logf(Severity::Info, stage, "%zu cards -> %s.bin",
                                 stack.cards.size(), name);
                        // No format stamp here: the file's own header is the
                        // version, and a stamp on the shared stacks/ directory
                        // would let a --stack run mark the seven files it did
                        // not touch as current. See stackFileIsCurrent.
                    }
                }
            }

            // --- images -----------------------------------------------------
            if (opts.images || opts.hires)
            {
                stage = std::string(name) + " art";
                const fs::path picDir = root / "pics" / stackDir(si.id);
                const fs::path hiDir = root / "pics_hi" / stackDir(si.id);
                const fs::path &archive = ss->dataArchives.front();

                // Art written by a build with a different .rpic layout is as
                // unreadable as a stale movie, and mtimes cannot see it either.
                const bool redoPics =
                    opts.force || formatStampIsStale(picDir, rivendata::kImageVersion);
                const bool redoHires =
                    opts.force || formatStampIsStale(hiDir, rivendata::kImageVersion);

                struct ImageJob
                {
                    std::uint16_t id = 0;
                    fs::path rpic;
                    fs::path rpiz;
                    bool needRpic = false;
                    bool needRpiz = false;
                    std::uint64_t work = 0;
                };

                const auto report = [&](ImageJob &j, ImageResult &ir) {
                    if (!ir.ok)
                    {
                        out.logf(Severity::Warning, stage, "image %u: %s", j.id,
                                 ir.error.c_str());
                    }
                    else
                    {
                        if (ir.rpicBytes > 0)
                            ++result.imagesWritten;
                        if (ir.rpizBytes > 0)
                            ++result.hiresWritten;
                        result.bytesWritten += ir.rpicBytes + ir.rpizBytes;
                    }
                    bump(j.work, "image " + std::to_string(j.id));
                };

                for (const std::uint16_t id : set.resourceIds("tBMP"))
                {
                    const std::uint64_t work = (opts.images ? kImageWork : 0)
                                             + (opts.hires ? kHiresWork : 0);
                    ImageJob job;
                    job.id = id;
                    job.rpic = picDir / (std::to_string(id) + ".rpic");
                    job.rpiz = hiDir / (std::to_string(id) + ".rpiz");
                    job.needRpic = opts.images && (redoPics || !isUpToDate(job.rpic, archive));
                    job.needRpiz = opts.hires && (redoHires || !isUpToDate(job.rpiz, archive));
                    job.work = work;

                    if (!job.needRpic && !job.needRpiz)
                    {
                        ++result.skipped;
                        bump(work, "image " + std::to_string(id) + " (up to date)");
                        continue;
                    }

                    ImageResult ir = convertBitmap(set, job.id, job.rpic, job.needRpic,
                                                   job.rpiz, job.needRpiz);
                    report(job, ir);
                }

                std::string e;
                if (opts.images && !writeFormatStamp(picDir, rivendata::kImageVersion, e))
                    out.warn(stage, e);
                if (opts.hires && !writeFormatStamp(hiDir, rivendata::kImageVersion, e))
                    out.warn(stage, e);
            }

            // --- water effects ----------------------------------------------
            if (opts.water)
            {
                stage = std::string(name) + " water";
                const fs::path sfxDir = root / "sfxe" / stackDir(si.id);
                const fs::path &archive = ss->dataArchives.front();

                for (const std::uint16_t id : set.resourceIds("SFXE"))
                {
                    const fs::path outFile = sfxDir / (std::to_string(id) + ".rsfx");
                    if (!opts.force && isUpToDate(outFile, archive))
                    {
                        ++result.skipped;
                        bump(kEffectWork, "effect " + std::to_string(id) + " (up to date)");
                        continue;
                    }

                    const auto bytes = set.read("SFXE", id);
                    if (looksDamaged(bytes))
                    {
                        out.logf(Severity::Warning, stage,
                                 "effect %u is zero-filled in this copy of the game", id);
                        bump(kEffectWork, "effect " + std::to_string(id));
                        continue;
                    }

                    ResourceReader r(bytes);
                    const SfxeParse parsed = parseSfxe(r, id);
                    if (!parsed.ok())
                    {
                        out.logf(Severity::Warning, stage, "effect %u did not parse", id);
                        bump(kEffectWork, "effect " + std::to_string(id));
                        continue;
                    }
                    if (!parsed.complete())
                    {
                        out.logf(Severity::Warning, stage,
                                 "effect %u: salvaged %zu of %d frames (source damaged)",
                                 id, parsed.effect->frames.size(), parsed.framesClaimed);
                    }

                    yas::mem_ostream os;
                    yas::binary_oarchive<yas::mem_ostream, kYasFlags> oa(os);
                    oa &*parsed.effect;
                    const auto buf = os.get_intrusive_buffer();

                    std::string err;
                    if (!writeFileAtomic(outFile, buf.data, buf.size, err))
                        out.error(stage, err);
                    else
                    {
                        ++result.effectsWritten;
                        result.bytesWritten += buf.size;
                    }
                    bump(kEffectWork, "effect " + std::to_string(id));
                }
            }

            // --- movies -----------------------------------------------------
            if (opts.video)
            {
                stage = std::string(name) + " movies";
                const fs::path videoDir = root / "video" / stackDir(si.id);
                const fs::path &archive = ss->dataArchives.front();

                struct MovieJob
                {
                    std::uint16_t id = 0;
                    fs::path out;
                };

                int unsupported = 0;
                int trackScaled = 0;

                // A .rvid written by an older build is rejected outright by the
                // ARM9 (isRvid is a hard version equality), and mtimes cannot see
                // that: the outputs are still newer than a 1997 CD, so every movie
                // would be skipped as up to date while none of them plays. The
                // stamp is the only thing that knows.
                const bool formatChanged =
                    formatStampIsStale(videoDir, rivendata::kVideoVersion);
                if (formatChanged)
                    out.logf(Severity::Warning, stage,
                             "video was converted by an older build: redoing it");
                const bool redoAll = opts.force || formatChanged;

                // Where each movie is drawn. A movie's DS size follows from its
                // MLST position, not from the movie alone -- VideoPipeline's
                // geometry comment says why. Read here rather than in the card
                // stage: --no-cards skips that block entirely and the video
                // stage has to stand on its own.
                int placementClashes = 0;
                const auto placements = collectMoviePlacements(set, &placementClashes);
                if (placementClashes > 0)
                    out.logf(Severity::Warning, stage,
                             "%d movie(s) are placed at more than one position in this "
                             "copy of the game; the first was used and the others will "
                             "be a pixel out",
                             placementClashes);

                const auto report = [&](MovieJob &j, VideoResult &vr) {
                    if (!vr.ok)
                    {
                        if (vr.unsupported)
                            ++unsupported;
                        out.logf(Severity::Warning, stage, "%s", vr.error.c_str());
                    }
                    else
                    {
                        if (!vr.audioError.empty())
                            out.logf(Severity::Warning, stage,
                                     "movie %u converted without its soundtrack: %s",
                                     j.id, vr.audioError.c_str());
                        // The count of these is what says the track matrix
                        // reached everything: 79 of a 5-CD install's 1054
                        // movies carry one, and before it was applied every one
                        // of them was drawn at twice or four times its size.
                        if (vr.trackScaled)
                        {
                            ++trackScaled;
                            out.logf(Severity::Info, stage,
                                     "movie %u is authored larger than it is shown: "
                                     "%dx%d after the track matrix",
                                     j.id, vr.width, vr.height);
                        }
                        ++result.moviesWritten;
                        result.videoFrames += static_cast<std::uint64_t>(vr.frames);
                        result.bytesWritten += vr.bytes;
                    }
                    bump(kMovieWork, "movie " + std::to_string(j.id));
                };

                for (const std::uint16_t id : set.resourceIds("tMOV"))
                {
                    MovieJob job;
                    job.id = id;
                    job.out = videoDir / (std::to_string(id) + ".rvid");
                    if (!redoAll && isUpToDate(job.out, archive))
                    {
                        ++result.skipped;
                        bump(kMovieWork, "movie " + std::to_string(id) + " (up to date)");
                        continue;
                    }

                    const auto pit = placements.find(job.id);
                    const MoviePlacement *place =
                        pit == placements.end() ? nullptr : &pit->second;
                    VideoResult vr =
                        convertMovie(set, job.id, job.out, ffmpeg, place, &cancel);
                    report(job, vr);
                }

                // Stamped only now: a stamp written before the stage would claim
                // movies a cancelled run never wrote.
                if (std::string err; !writeFormatStamp(videoDir, rivendata::kVideoVersion, err))
                    out.warn(stage, err);

                if (unsupported > 0)
                    out.logf(Severity::Warning, stage,
                             "%d movie(s) use a codec this converter does not decode",
                             unsupported);
                if (trackScaled > 0)
                    out.logf(Severity::Info, stage,
                             "%d movie(s) were scaled down by their track matrix",
                             trackScaled);
            }

            // --- sound ------------------------------------------------------
            if (opts.audio)
            {
                stage = std::string(name) + " sound";
                const fs::path soundDir = root / "sound" / stackDir(si.id);
                const bool redoSounds =
                    opts.force || formatStampIsStale(soundDir, rivendata::kSoundVersion);

                // tWAV lives in TWO places: the effects a script plays are in
                // the data archives, the ambient tracks are in <stack>_Sounds.mhk
                // (Layout.hpp:44-45), and the two id spaces overlap. ScummVM
                // resolves that by searching every archive of the stack in
                // order and taking the first hit (riven.cpp:405-415), so a set
                // holding the data archives followed by the sound ones
                // reproduces its lookup exactly -- patch first, base next,
                // sounds last.
                ArchiveSet soundSet;
                std::vector<std::string> soundFailures;
                soundSet.openAll(ss->dataArchives, soundFailures);
                soundSet.openAll(ss->soundArchives, soundFailures);
                for (const auto &f : soundFailures)
                    out.warn(stage, "could not open " + f);

                struct SoundJob
                {
                    std::uint16_t id = 0;
                    fs::path out;
                };

                int mpeg = 0;

                const auto report = [&](SoundJob &j, SoundResult &sr) {
                    if (!sr.ok)
                    {
                        out.logf(Severity::Warning, stage, "%s", sr.error.c_str());
                    }
                    else
                    {
                        ++result.soundsWritten;
                        result.bytesWritten += sr.bytes;
                        if (sr.wasMpeg)
                            ++mpeg;
                    }
                    bump(kSoundWork, "sound " + std::to_string(j.id));
                };

                for (const std::uint16_t id : soundSet.resourceIds("tWAV"))
                {
                    SoundJob job;
                    job.id = id;
                    job.out = soundDir / (std::to_string(id) + ".rsnd");

                    // Freshness is measured against the archive the sound
                    // actually came from, which may be either kind.
                    const Archive *owner = soundSet.find("tWAV", id);
                    if (owner == nullptr)
                    {
                        bump(kSoundWork, "sound " + std::to_string(id));
                        continue;
                    }

                    if (!redoSounds && isUpToDate(job.out, owner->path()))
                    {
                        ++result.skipped;
                        bump(kSoundWork, "sound " + std::to_string(id) + " (up to date)");
                        continue;
                    }

                    SoundResult sr = convertSound(soundSet, job.id, job.out, opts.compressAudio);
                    report(job, sr);
                }

                if (std::string e; !writeFormatStamp(soundDir, rivendata::kSoundVersion, e))
                    out.warn(stage, e);

                if (mpeg > 0)
                {
                    result.mpegSounds += mpeg;
                    out.logf(Severity::Info, stage,
                             "%d sound(s) were MPEG-2 Layer II and were re-encoded", mpeg);
                }
            }

            ++result.stacksConverted;
        }

        result.outcome = ConversionResult::Outcome::Ok;
        result.message = "Finished.";
    }
    catch (const ConversionCancelled &)
    {
        result.outcome = ConversionResult::Outcome::Cancelled;
        result.message = "Stopped. What was already converted is complete and will be "
                         "kept; running again resumes where this left off.";
    }
    catch (const std::exception &e)
    {
        result.outcome = ConversionResult::Outcome::Failed;
        result.message = std::string("Failed: ") + e.what();
        out.error("run", result.message);
    }

    result.warnings = counting.warnings();
    result.errors = counting.errors();
    out.progress(done, total, "done", result.message);
    return result;
}

} // namespace riven

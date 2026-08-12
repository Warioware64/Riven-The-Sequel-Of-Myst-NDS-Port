#include "riven/Converter.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#include <yas/mem_streams.hpp>
#include <yas/binary_oarchive.hpp>

#include "RivenData.hpp"
#include "RivenSfxe.hpp"
#include "riven/Archive.hpp"
#include "riven/AtomicWrite.hpp"
#include "riven/CardParse.hpp"
#include "riven/ImagePipeline.hpp"
#include "riven/MovieList.hpp"
#include "riven/SoundPipeline.hpp"
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
} // namespace

std::string ConversionResult::summary() const
{
    char buf[448];
    std::snprintf(buf, sizeof(buf),
                  "%d stacks, %d cards, %d images, %d zoom twins, %d water effects, "
                  "%d sounds, %d movies (%llu frames); %d skipped, %d warnings, %d errors",
                  stacksConverted, cardsWritten, imagesWritten, hiresWritten,
                  effectsWritten, soundsWritten, moviesWritten,
                  static_cast<unsigned long long>(videoFrames), skipped, warnings, errors);
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

    // How many movies to encode at once. Auto means "every core but one", so
    // the machine stays usable through what is otherwise a half-hour of full
    // load; the cap keeps peak memory sane, since each worker holds a whole
    // tMOV and the largest is 25 MB.
    int workers = opts.jobs;
    if (workers <= 0)
    {
        const unsigned hw = std::thread::hardware_concurrency();
        workers = hw > 1 ? static_cast<int>(hw) - 1 : 1;
    }
    workers = std::clamp(workers, 1, 16);
    if (opts.video && workers > 1)
        out.logf(Severity::Info, "setup", "encoding movies on %d threads", workers);

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

                bool upToDate = !opts.force;
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

                for (const std::uint16_t id : set.resourceIds("tBMP"))
                {
                    const std::uint64_t work = (opts.images ? kImageWork : 0)
                                             + (opts.hires ? kHiresWork : 0);
                    const fs::path rpic = picDir / (std::to_string(id) + ".rpic");
                    const fs::path rpiz = hiDir / (std::to_string(id) + ".rpiz");

                    const bool needRpic =
                        opts.images && (opts.force || !isUpToDate(rpic, archive));
                    const bool needRpiz =
                        opts.hires && (opts.force || !isUpToDate(rpiz, archive));

                    if (!needRpic && !needRpiz)
                    {
                        ++result.skipped;
                        bump(work, "image " + std::to_string(id) + " (up to date)");
                        continue;
                    }

                    const ImageResult ir =
                        convertBitmap(set, id, rpic, needRpic, rpiz, needRpiz);
                    if (!ir.ok)
                    {
                        out.logf(Severity::Warning, stage, "image %u: %s", id,
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
                    bump(work, "image " + std::to_string(id));
                }
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

                // Encoding is the only stage worth spreading across cores: a
                // stack of movies takes minutes where everything else takes
                // seconds. libvaht is not thread-safe, so the reads stay here
                // and only convertMovieBytes goes to a worker.
                //
                // Work is handed out a batch at a time rather than through a
                // queue. That keeps cancellation and the progress bar exactly
                // as they were -- both happen on this thread, between batches
                // -- and it bounds memory, which matters when a single tMOV can
                // be 25 MB and the biggest batch holds one per worker.
                struct Job
                {
                    std::uint16_t id = 0;
                    fs::path out;
                    std::vector<std::uint8_t> bytes;
                };

                int unsupported = 0;
                const auto ids = set.resourceIds("tMOV");
                std::size_t next = 0;

                while (next < ids.size())
                {
                    // Batches are several jobs per worker so the pull loop has
                    // something to balance, but bounded by total bytes as well
                    // as by count: a batch of 25 MB movies is the one way this
                    // stage could run a machine out of memory.
                    constexpr std::size_t kBatchBytes = 192u * 1024 * 1024;
                    const std::size_t batchLimit = static_cast<std::size_t>(workers) * 4;
                    std::size_t batchBytes = 0;

                    std::vector<Job> batch;
                    while (next < ids.size() && batch.size() < batchLimit
                           && batchBytes < kBatchBytes)
                    {
                        const std::uint16_t id = ids[next++];
                        const fs::path outFile = videoDir / (std::to_string(id) + ".rvid");
                        if (!opts.force && isUpToDate(outFile, archive))
                        {
                            ++result.skipped;
                            bump(kMovieWork, "movie " + std::to_string(id) + " (up to date)");
                            continue;
                        }
                        Job job;
                        job.id = id;
                        job.out = outFile;
                        job.bytes = set.readMovie(id);
                        if (job.bytes.empty())
                        {
                            out.logf(Severity::Warning, stage,
                                     "tMOV %u is not a readable QuickTime movie", id);
                            bump(kMovieWork, "movie " + std::to_string(id));
                            continue;
                        }
                        batchBytes += job.bytes.size();
                        batch.push_back(std::move(job));
                    }
                    if (batch.empty())
                        continue;

                    std::vector<VideoResult> results(batch.size());
                    if (batch.size() == 1)
                    {
                        results[0] = convertMovieBytes(batch[0].bytes, batch[0].id,
                                                       batch[0].out, opts.videoQuality);
                    }
                    else
                    {
                        // Workers pull the next job rather than being handed
                        // one each. Riven's movies differ in length by two
                        // orders of magnitude -- 1225 frames next to 23 -- so a
                        // thread per job leaves most of the machine waiting on
                        // whichever cutscene landed in the batch.
                        std::atomic<std::size_t> nextJob{0};
                        std::vector<std::thread> pool;
                        pool.reserve(static_cast<std::size_t>(workers));
                        for (int t = 0; t < workers; ++t)
                            pool.emplace_back([&] {
                                for (;;)
                                {
                                    const std::size_t i = nextJob.fetch_add(1);
                                    if (i >= batch.size())
                                        return;
                                    results[i] =
                                        convertMovieBytes(batch[i].bytes, batch[i].id,
                                                          batch[i].out, opts.videoQuality);
                                }
                            });
                        for (auto &t : pool)
                            t.join();
                    }

                    for (std::size_t i = 0; i < batch.size(); ++i)
                    {
                        const VideoResult &vr = results[i];
                        if (!vr.ok)
                        {
                            if (vr.unsupported)
                                ++unsupported;
                            out.logf(Severity::Warning, stage, "%s", vr.error.c_str());
                        }
                        else
                        {
                            ++result.moviesWritten;
                            result.videoFrames += static_cast<std::uint64_t>(vr.frames);
                            result.bytesWritten += vr.bytes;
                        }
                        bump(kMovieWork, "movie " + std::to_string(batch[i].id));
                    }
                }

                if (unsupported > 0)
                    out.logf(Severity::Warning, stage,
                             "%d movie(s) use a codec this converter does not decode",
                             unsupported);
            }

            // --- sound ------------------------------------------------------
            if (opts.audio)
            {
                stage = std::string(name) + " sound";
                const fs::path soundDir = root / "sound" / stackDir(si.id);

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

                int mpeg = 0;
                for (const std::uint16_t id : soundSet.resourceIds("tWAV"))
                {
                    const fs::path outFile = soundDir / (std::to_string(id) + ".rsnd");

                    // Freshness is measured against the archive the sound
                    // actually came from, which may be either kind.
                    const Archive *owner = soundSet.find("tWAV", id);
                    if (owner == nullptr)
                    {
                        bump(kSoundWork, "sound " + std::to_string(id));
                        continue;
                    }

                    if (!opts.force && isUpToDate(outFile, owner->path()))
                    {
                        ++result.skipped;
                        bump(kSoundWork, "sound " + std::to_string(id) + " (up to date)");
                        continue;
                    }

                    const SoundResult sr = convertSound(soundSet, id, outFile);
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
                    bump(kSoundWork, "sound " + std::to_string(id));
                }

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

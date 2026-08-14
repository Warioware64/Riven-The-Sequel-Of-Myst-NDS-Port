// The notebook files the DS writes, compiled for the host.
//
// Not a converter test. It compiles source/BookNotes.cpp -- which is free of
// <nds.h> so that it can be -- writes a note, reads it back, and checks the
// pixels are the pixels. On hardware nothing can check that, and a note is the
// one file here the player MADE: losing a save costs an evening, losing the
// page you wrote the dome combination on costs the puzzle.
//
// The claims it is really here to defend:
//
//   * EVERY ALLOCATION IS BOUNDED BY A VALIDATED FIELD. readPixels sizes a
//     buffer from the width and height in the header and readStrokes sizes one
//     from a point count, so a corrupt file is an out-of-memory on a 4 MB
//     console rather than a failed read. Each of those bounds is checked here.
//   * AN INDEX IS A NAME, NOT A POSITION. capture() takes the first free slot,
//     so deleting note 1 of 3 and taking another puts it back at index 1 -- and
//     lastOpened() stores the index precisely because positions move.
//   * ERASED AND NEVER-DRAWN-ON ARE THE SAME THING. writeStrokes with an empty
//     vector must leave no file, or a fully erased note would be distinguishable
//     from a fresh one for no reason anybody could see.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "BookNotes.hpp"

namespace fs = std::filesystem;
using namespace rivenrt;

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void check(bool cond, const std::string &what)
    {
        ++g_checks;
        if (!cond)
        {
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
            ++g_failures;
        }
    }

    /// A picture with a different value in every pixel, so a stride or an
    /// offset mistake cannot look like a pass.
    std::vector<std::uint16_t> makePicture(int w, int h)
    {
        std::vector<std::uint16_t> px(static_cast<std::size_t>(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                px[static_cast<std::size_t>(y) * w + x] =
                    static_cast<std::uint16_t>(0x8000 | ((y * 31 + x) & 0x7FFF));
        return px;
    }

    std::vector<std::uint8_t> readFile(const std::string &path)
    {
        std::vector<std::uint8_t> out;
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (f == nullptr)
            return out;
        std::fseek(f, 0, SEEK_END);
        out.resize(static_cast<std::size_t>(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(out.data(), 1, out.size(), f) != out.size())
            out.clear();
        std::fclose(f);
        return out;
    }

    void writeFile(const std::string &path, const std::vector<std::uint8_t> &bytes)
    {
        std::FILE *f = std::fopen(path.c_str(), "wb");
        if (f == nullptr)
            return;
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }

    BookNotes::Stroke makeStroke(std::uint8_t colour, std::uint8_t width, int n, int seed)
    {
        BookNotes::Stroke s;
        s.color = colour;
        s.width = width;
        for (int i = 0; i < n; ++i)
        {
            s.x.push_back(static_cast<std::uint8_t>((seed + i * 3) & 0xFF));
            s.y.push_back(static_cast<std::uint8_t>((seed + i * 7) & 0xFF));
        }
        return s;
    }
} // namespace

int main()
{
    const fs::path dir = fs::temp_directory_path() / "riven_notes_arm9_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    BookNotes::setDirectory(dir.string() + "/");

    check(BookNotes::scan().empty(), "an empty directory holds no notes");
    check(BookNotes::lastOpened() == -1, "nothing has been opened yet");

    // --- capture and read back ---------------------------------------------
    constexpr int kW = 256;
    constexpr int kH = 165; // the card view: what captureNote actually grabs
    const std::vector<std::uint16_t> picture = makePicture(kW, kH);

    const int first = BookNotes::capture(picture.data(), kW, kH, 4 /*Tspit*/, 155);
    check(first == 0, "the first note takes index 0");

    std::vector<std::uint16_t> got;
    BookNotes::NoteInfo info;
    check(BookNotes::readPixels(first, got, info), "the note reads back");
    check(got == picture, "every pixel survived the round trip");
    check(info.w == kW && info.h == kH, "the size survived");
    check(info.stackId == 4 && info.cardId == 155, "the place survived");
    check(info.index == first, "the info knows its own index");
    check(BookNotes::label(info).find("Temple Island") != std::string::npos,
          "the caption names the island");
    check(BookNotes::lastOpened() == -1, "capture alone does not set lastOpened");

    check(BookNotes::scan().size() == 1, "one note is listed");

    // --- strokes ------------------------------------------------------------
    check(BookNotes::readStrokes(first).empty(), "a fresh note has no strokes");
    check(!fs::exists(BookNotes::strokePath(first)), "and no stroke file");

    std::vector<BookNotes::Stroke> strokes;
    strokes.push_back(makeStroke(0, 2, 5, 10));
    strokes.push_back(makeStroke(3, 3, 120, 200));
    check(BookNotes::writeStrokes(first, strokes), "strokes are written");

    const std::vector<BookNotes::Stroke> back = BookNotes::readStrokes(first);
    check(back.size() == 2, "both strokes come back");
    check(back.size() == 2 && back[0].color == 0 && back[0].width == 2
              && back[0].x == strokes[0].x && back[0].y == strokes[0].y,
          "the first stroke is unchanged");
    check(back.size() == 2 && back[1].color == 3 && back[1].points() == 120
              && back[1].y == strokes[1].y,
          "the second stroke is unchanged");

    // The picture is written once and must not be touched by a stroke save.
    check(BookNotes::readPixels(first, got, info) && got == picture,
          "writing strokes did not disturb the capture");

    // Erasing everything must leave no file at all.
    check(BookNotes::writeStrokes(first, {}), "an empty stroke list is accepted");
    check(!fs::exists(BookNotes::strokePath(first)),
          "a fully erased note has no stroke file, exactly like a fresh one");
    check(BookNotes::readStrokes(first).empty(), "and reads back as no strokes");

    // --- a note header that lies --------------------------------------------
    //
    // Every one of these must be REFUSED, and refused without allocating from
    // the number that is wrong.
    const std::string notePath = BookNotes::notePath(first);
    const std::vector<std::uint8_t> good = readFile(notePath);
    check(good.size() == 32 + static_cast<std::size_t>(kW) * kH * 2,
          "the file is a 32-byte header plus the pixels");

    struct Corruption
    {
        std::size_t offset;
        std::uint8_t value[2];
        int bytes;
        const char *what;
    };
    const Corruption kBad[] = {
        {0, {0xFF, 0}, 1, "a bad magic is refused"},
        {4, {0x99, 0x99}, 2, "an unknown version is refused"},
        {6, {0x01, 0x04}, 2, "a width past the screen is refused"},   // 1025
        {8, {0x01, 0x04}, 2, "a height past the screen is refused"},  // 1025
        {6, {0x00, 0x00}, 2, "a zero width is refused"},
        {8, {0x00, 0x00}, 2, "a zero height is refused"},
        {22, {0xFF, 0xFF}, 2, "a payload length disagreeing with w*h is refused"},
    };
    for (const Corruption &c : kBad)
    {
        std::vector<std::uint8_t> bad = good;
        for (int i = 0; i < c.bytes; ++i)
            bad[c.offset + i] = c.value[i];
        writeFile(notePath, bad);
        check(!BookNotes::readPixels(first, got, info), c.what);
        check(BookNotes::scan().empty(), std::string("scan drops it: ") + c.what);
    }

    // A header that is fine over a picture that has been cut short.
    {
        writeFile(notePath, std::vector<std::uint8_t>(good.begin(), good.end() - 2));
        check(!BookNotes::readPixels(first, got, info), "a truncated picture is refused");
        // The HEADER is still good, so scan still lists it -- that is correct,
        // and it is why the browser has to handle readPixels failing on a note
        // the list gave it.
        check(BookNotes::scan().size() == 1, "but its header still lists");
    }
    writeFile(notePath, good);
    check(BookNotes::readPixels(first, got, info) && got == picture, "and restores");

    // --- a stroke file that lies --------------------------------------------
    check(BookNotes::writeStrokes(first, strokes), "strokes written again");
    const std::vector<std::uint8_t> goodStrokes = readFile(BookNotes::strokePath(first));
    {
        // An absurd stroke count must not make readStrokes reserve from it.
        std::vector<std::uint8_t> bad = goodStrokes;
        bad[6] = 0xFF;
        bad[7] = 0xFF;
        writeFile(BookNotes::strokePath(first), bad);
        check(BookNotes::readStrokes(first).empty(), "an absurd stroke count is refused");
    }
    {
        // An absurd POINT count on the first stroke, same argument.
        std::vector<std::uint8_t> bad = goodStrokes;
        bad[16 + 2] = 0xFF;
        bad[16 + 3] = 0xFF;
        writeFile(BookNotes::strokePath(first), bad);
        check(BookNotes::readStrokes(first).empty(), "an absurd point count is refused");
    }
    {
        // Truncated mid-stroke: keep what was whole, drop the rest, never crash.
        writeFile(BookNotes::strokePath(first),
                  std::vector<std::uint8_t>(goodStrokes.begin(), goodStrokes.end() - 40));
        const std::vector<BookNotes::Stroke> partial = BookNotes::readStrokes(first);
        check(partial.size() == 1, "a truncated stroke file keeps the whole strokes");
        check(partial.size() == 1 && partial[0].x == strokes[0].x,
              "and the one it kept is intact");
    }
    {
        std::vector<std::uint8_t> bad = goodStrokes;
        bad[0] ^= 0xFF;
        writeFile(BookNotes::strokePath(first), bad);
        check(BookNotes::readStrokes(first).empty(), "a bad stroke magic is refused");
    }

    // --- a page with more strokes than the format holds ----------------------
    //
    // The writer caps the count. The bug this guards is the header and the body
    // disagreeing about that cap: a count of 512 followed by all 600 strokes
    // reads back as 512 good ones and 88 bytes of garbage the reader would walk
    // into. Whatever is written has to be exactly what the header describes.
    {
        std::vector<BookNotes::Stroke> many;
        for (int i = 0; i < 600; ++i)
            many.push_back(makeStroke(static_cast<std::uint8_t>(i % 6), 2, 3, i));
        check(BookNotes::writeStrokes(first, many), "600 strokes are accepted");

        const std::vector<BookNotes::Stroke> readBack = BookNotes::readStrokes(first);
        check(!readBack.empty(), "and read back");
        check(readBack.size() <= 512, "capped at the format's limit");
        // The kept ones must be the FIRST ones, in order, byte for byte.
        bool inOrder = true;
        for (std::size_t i = 0; i < readBack.size(); ++i)
            if (readBack[i].x != many[i].x || readBack[i].y != many[i].y
                || readBack[i].color != many[i].color)
                inOrder = false;
        check(inOrder, "the strokes kept are the first ones, unchanged");

        // The file must contain nothing beyond what its header describes.
        const std::vector<std::uint8_t> f = readFile(BookNotes::strokePath(first));
        const std::size_t declared = static_cast<std::size_t>(f[8]) | (f[9] << 8)
                                     | (static_cast<std::size_t>(f[10]) << 16)
                                     | (static_cast<std::size_t>(f[11]) << 24);
        check(f.size() == 16 + declared,
              "the file is exactly its header plus the payload the header declares");
    }
    check(BookNotes::writeStrokes(first, {}), "and the overflow page can be cleared");

    // --- oversized captures are refused on the way in -----------------------
    {
        const std::vector<std::uint16_t> big = makePicture(8, 8);
        check(BookNotes::capture(big.data(), 257, 8, 4, 1) < 0, "a too-wide capture is refused");
        check(BookNotes::capture(big.data(), 8, 193, 4, 1) < 0, "a too-tall capture is refused");
        check(BookNotes::capture(big.data(), 0, 8, 4, 1) < 0, "a zero-width capture is refused");
        check(BookNotes::capture(nullptr, 8, 8, 4, 1) < 0, "a null capture is refused");
    }

    // --- indices are names, not positions ------------------------------------
    const std::vector<std::uint16_t> small = makePicture(64, 48);
    const int second = BookNotes::capture(small.data(), 64, 48, 7 /*Jspit*/, 42);
    const int third = BookNotes::capture(small.data(), 64, 48, 5 /*Bspit*/, 99);
    check(second == 1 && third == 2, "notes take the next free index in order");
    check(BookNotes::scan().size() == 3, "three notes are listed");

    check(BookNotes::remove(second), "the middle note is removed");
    check(BookNotes::scan().size() == 2, "two are left");
    const int fourth = BookNotes::capture(small.data(), 64, 48, 6 /*Gspit*/, 7);
    check(fourth == 1, "a new capture REUSES the freed index rather than appending");

    // Which is exactly why lastOpened stores an index: note `third` is still at
    // index 2 even though it is now the last of three again.
    BookNotes::setLastOpened(third);
    check(BookNotes::lastOpened() == third, "lastOpened remembers an index");
    check(fs::exists(dir / "last.dat"), "and it is on the card");

    BookNotes::remove(third);
    check(BookNotes::lastOpened() != third,
          "removing the remembered note forgets it, so it cannot reopen on whatever "
          "lands at that index next");

    // Removing a note takes its strokes with it.
    check(BookNotes::writeStrokes(first, strokes), "strokes for the survivor");
    check(fs::exists(BookNotes::strokePath(first)), "which exist");
    BookNotes::remove(first);
    check(!fs::exists(BookNotes::strokePath(first)), "and go with the note");

    // A reused index must not inherit the dead note's scribbles.
    {
        const int reused = BookNotes::capture(small.data(), 64, 48, 4, 1);
        check(reused == 0, "index 0 is free again");
        check(BookNotes::readStrokes(reused).empty(),
              "a reused index starts with no strokes");
    }

    // --- a full notebook is refused, not wrapped ----------------------------
    {
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        for (int i = 0; i < BookNotes::kMaxNotes; ++i)
            check(BookNotes::capture(small.data(), 64, 48, 4, 1) == i,
                  i == 0 ? "filling the notebook" : "");
        check(BookNotes::capture(small.data(), 64, 48, 4, 1) < 0,
              "note 25 is refused rather than overwriting the oldest");
        check(BookNotes::scan().size() == BookNotes::kMaxNotes,
              "and the 24 that were there are all still there");
    }

    // --- bounds --------------------------------------------------------------
    check(BookNotes::notePath(-1).empty() && BookNotes::notePath(BookNotes::kMaxNotes).empty(),
          "an out-of-range index has no path");
    check(!BookNotes::readPixels(-1, got, info), "and does not read");
    check(!BookNotes::remove(BookNotes::kMaxNotes), "and does not remove");

    fs::remove_all(dir, ec);

    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks,
                g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

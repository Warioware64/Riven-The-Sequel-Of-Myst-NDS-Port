#pragma once

// The wizard's chrome, drawn rather than shipped.
//
// An installer wants a left-hand watermark and a header strip, and this repo has
// no artwork to put there: resources/bg/ is empty and Logo.bmp is a 32x32 icon.
// The obvious fix -- lift a still out of Riven -- is the one thing
// docs/licensing.md rules out, and it would put game data in a binary that is
// distributed. So these are painted with QPainter at startup from a handful of
// colours. Nothing is committed, nothing is licensed, and the whole thing costs
// about a millisecond once.
//
// The colours are the ones the GUI already uses for its status text, so the
// chrome and the messages inside it belong to the same palette.
//
// Swapping in real art later is one Qt resource file and one QPixmap
// constructor per function; nothing else needs to know.

#include <QColor>
#include <QPixmap>

namespace banner
{

/// The port's palette, shared with the check and status colours.
inline const QColor kInk{0x0b, 0x18, 0x21};   ///< deepest background
inline const QColor kDeep{0x1a, 0x3a, 0x49};  ///< the water
inline const QColor kGold{0xc9, 0x8a, 0x00};  ///< the domes, and every warning
inline const QColor kPaper{0xf4, 0xf1, 0xe8}; ///< the header strip

/// The tall pixmap down the left of the Welcome and Finished pages.
///
/// ModernStyle only shows a watermark on pages that carry one, which is exactly
/// the classic installer arrangement: art on the first and last page, a header
/// strip on everything between.
QPixmap watermark();

/// The header strip behind the title and subtitle of the interior pages.
///
/// Deliberately light. Qt draws the header's title and subtitle in a dark text
/// colour whatever the system theme is, so a dark banner here would be
/// unreadable on exactly the machines that most want a dark one.
///
/// Its WIDTH sets the wizard's width; see the note beside kBannerWidth.
QPixmap headerBanner();

/// The width headerBanner() is drawn at, and therefore the width the wizard
/// opens at. InstallWizard reads this so the two cannot drift apart.
int headerBannerWidth();

/// The small mark at the right of the header.
QPixmap headerLogo();

} // namespace banner

/* The single translation unit that instantiates minimp3.
 *
 * Kept apart from Mp2.cpp, and compiled as C with warnings off, for the same
 * reason libvaht is (CMakeLists.txt: "third-party: not our warnings to fix").
 * Mp2.cpp includes the same header WITHOUT MINIMP3_IMPLEMENTATION and gets only
 * the declarations, which the header wraps in extern "C" itself.
 *
 * minimp3 is CC0-1.0 (public domain); see docs/licensing.md.
 */

#define MINIMP3_IMPLEMENTATION
#include <minimp3/minimp3.h>

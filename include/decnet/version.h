// decnet/version.h -- version strings.  Port of version.py.

#ifndef DECNET_VERSION_H
#define DECNET_VERSION_H

#include <string>

namespace decnet::version {

inline constexpr const char *stage   = "V";
inline constexpr const char *vernum  = "0.2";
inline constexpr const char *patch   = "0";
// Empty for a release; "-dev" while it is still being worked on.
inline constexpr const char *suffix  = "-dev";
inline constexpr const char *cyear   = "2026";
// Copyright of this port.  PyDECnet's copyright (Paul Koning) is in
// LICENSE.
inline constexpr const char *authors = "Richard";

// Set by the build from git; see DN_GITREV in mk/config.mk.
#ifndef DN_GITREV
#define DN_GITREV ""
#endif

std::string kit_version ();     // "V0.2.0-dev"
std::string full_version ();    // "V0.2.0-dev (abc1234)"
std::string ident ();           // "DECnet/C++ V0.2.0-dev (abc1234)"
std::string banner ();          // ident plus copyright

}   // namespace decnet::version

#endif  // DECNET_VERSION_H

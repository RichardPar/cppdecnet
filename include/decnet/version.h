// decnet/version.h -- version strings.  Port of version.py.

#ifndef DECNET_VERSION_H
#define DECNET_VERSION_H

#include <string>

namespace decnet::version {

inline constexpr const char *stage   = "V";
inline constexpr const char *vernum  = "0.1";
inline constexpr const char *patch   = "0";
inline constexpr const char *cyear   = "2026";
// The copyright in this port is the port author's: the C++ work here is
// an original expression, even though the protocol behaviour it implements
// was worked out in the Python it derives from.  Paul Koning's copyright
// in that Python is retained in LICENSE, which is what its BSD 3-clause
// terms require and what the credit line below acknowledges.
inline constexpr const char *authors = "Richard";

// Set by the build from git; see DN_GITREV in mk/config.mk.
#ifndef DN_GITREV
#define DN_GITREV ""
#endif

std::string kit_version ();     // "V0.1.0"
std::string full_version ();    // "V0.1.0 (abc1234)"
std::string ident ();           // "DECnet/C++ V0.1.0 (abc1234)"
std::string banner ();          // ident plus copyright

}   // namespace decnet::version

#endif  // DECNET_VERSION_H

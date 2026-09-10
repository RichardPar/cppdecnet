#include "decnet/version.h"

namespace decnet::version {

std::string kit_version ()
{
    return std::string (stage) + vernum + "." + patch;
}

std::string full_version ()
{
    std::string rev = DN_GITREV;
    if (rev.empty ()) return kit_version ();
    return kit_version () + " (" + rev + ")";
}

std::string ident ()
{
    return "DECnet/C++ " + full_version ();
}

std::string banner ()
{
    // The port carries the upstream attribution: the protocol work and the
    // reference implementation this is derived from are Paul Koning's.
    return ident () + " © 2013-" + cyear + " by " + authors
        + "; C++ port derived from PyDECnet";
}

}   // namespace decnet::version

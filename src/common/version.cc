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
    // Kept short, since it is sent to other nodes.
    return ident () + " © " + cyear + " by " + authors
        + "; after Paul Koning's Python";
}

}   // namespace decnet::version

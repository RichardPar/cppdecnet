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
    // This string is what the node tells other nodes about itself, so it
    // stays short.  The copyright is the port author's; the credit is to
    // the author of the Python this was ported from, whose own copyright
    // and licence are in LICENSE.
    return ident () + " © " + cyear + " by " + authors
        + "; after Paul Koning's Python";
}

}   // namespace decnet::version

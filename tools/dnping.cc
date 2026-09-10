// tools/dnping.cc -- port of applications/dnping.
//
// PORT: the real tool opens an NSP connection to the MIRROR object on the
// target node and times the round trip.  It needs the session control API,
// so for now it only parses its arguments and reports what it would do.

#include "decnet/common/types.h"
#include "decnet/version.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

int main (int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf (stderr, "usage: %s node [count]\n", argv[0]);
        return 2;
    }
    std::string target = argv[1];
    int count = argc > 2 ? std::atoi (argv[2]) : 4;

    try {
        // Accept either a node name or an address, as the Python tool does.
        decnet::Nodeid id = decnet::Nodeid::parse (target);
        std::printf ("%s: would ping %s %d time(s) via the MIRROR object\n",
                     decnet::version::ident ().c_str (), id.str ().c_str (),
                     count);
    } catch (const std::invalid_argument &) {
        std::printf ("%s: would ping node named %s %d time(s)\n",
                     decnet::version::ident ().c_str (), target.c_str (), count);
    }
    std::fprintf (stderr, "%s: not implemented yet (needs the session layer)\n",
                  argv[0]);
    return 1;
}

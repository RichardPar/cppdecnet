// decnet/session/cterm.h -- the CHOST gate: local programs for SET HOST.
//
// The host end of CTERM (DNA Command Terminal over Terminal Foundation
// Services), on object 42.  A VMS, RSX or Ultrix user types SET HOST to
// this node and gets one of the programs the configuration's chost lines
// name, running on a pseudo terminal.  With several chost lines the user
// picks one from a menu first.
//
// The terminal end does as little as CTERM lets it: every read asks for a
// single character with no echo and no editing, and every character
// terminates it, so what the user types goes straight to the pseudo
// terminal.  Echo, line editing and ^C are the pseudo terminal's, as for a
// program run under ssh.  The program's output goes back as Write
// messages, untouched.
//
// The same gate answers object 23 too, in the remote terminal protocol
// that came before CTERM: RSX's RMT speaks only that.
//
// Not available on Windows, which has no pseudo terminals of this kind.
//
// An extension: PyDECnet has no CTERM host.

#ifndef DECNET_SESSION_CTERM_H
#define DECNET_SESSION_CTERM_H

#include "decnet/config.h"
#include "decnet/session/session.h"

#include <vector>

namespace decnet {
class Node;
}

namespace decnet::session {

// The CTERM object number, and the remote terminal protocol's: the one
// before CTERM, which RSX's RMT uses for SET HOST.
inline constexpr std::uint8_t CTERM_OBJECT = 42;
inline constexpr std::uint8_t RMT_OBJECT = 23;

// Whether this system can run the gate.
bool cterm_gate_supported () noexcept;

// The factory for object 42, offering these programs.
ApplicationFactory cterm_gate (Node *node, std::vector<ChostConfig> hosts);

// The same programs for object 23, in the remote terminal protocol's RSX
// variant.
ApplicationFactory rmt_gate (Node *node, std::vector<ChostConfig> hosts);

}   // namespace decnet::session

#endif  // DECNET_SESSION_CTERM_H

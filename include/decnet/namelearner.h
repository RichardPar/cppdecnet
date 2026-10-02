// decnet/namelearner.h -- node names from the network itself.
//
// "node @neighbours" in the configuration.  A node name database is only as
// good as whoever wrote it, and on HECnet plenty of nodes are in nobody's
// list.  Their neighbours know them, though, and every node knows its own
// name.  So this asks:
//
//  - each neighbour, once its adjacency is up and every refresh interval
//    after, for its known nodes (NCP's TELL n SHOW KNOWN NODES), and
//  - any node a link runs to while we have no name for it, for its own
//    name (TELL n SHOW EXECUTOR).
//
// What it learns only fills gaps.  A name from the configuration or a
// fetched list is never replaced, and a name another address already has is
// not given to a second one.  Learned names are not kept across restarts;
// the neighbours are asked again.
//
// Not in PyDECnet.  Node thread only.

#ifndef DECNET_NAMELEARNER_H
#define DECNET_NAMELEARNER_H

#include "decnet/common/timers.h"
#include "decnet/common/types.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <set>

namespace decnet {

class Node;

class NameLearner {
public:
    NameLearner (Node *node, unsigned refresh);

    void start ();
    void stop ();

    // A logical link to this node is running.  If we have no name for it,
    // ask it.
    void link_running (Nodeid id);

    // How many names have been learned so far, for the log and the tests.
    std::size_t learned () const noexcept { return learned_; }

    // For a query: what it found, and that it is over.
    void found (Nodeid id, const std::string &name);
    void finished (Nodeid id);

private:
    using Clock = std::chrono::steady_clock;

    void tick ();                           // look at the neighbours
    void ask (Nodeid id, bool known);       // known: all its names, else its own
    bool asked_lately (Nodeid id) const;

    Node                                 *node_;
    std::chrono::seconds                  refresh_;
    CallbackTimer                         timer_;
    std::map<std::uint16_t, Clock::time_point> asked_;
    std::set<std::uint16_t>               busy_;
    std::size_t                           learned_ = 0;
    std::size_t                           learned_before_ = 0;   // for the log
};

}   // namespace decnet

#endif  // DECNET_NAMELEARNER_H

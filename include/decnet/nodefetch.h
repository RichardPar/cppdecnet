// decnet/nodefetch.h -- fetching node name lists over HTTP.
//
// "node @hecnet --cache FILE" in the configuration names a list to fetch.
// The configuration itself only ever reads the cache, so the node starts
// with the names it had last time whether or not the network is up; this
// element does the fetching afterwards, on a thread of its own, and hands
// the result to the node thread to apply.
//
// A failed fetch is a logged warning and nothing more.  Node names are
// cosmetic -- they change what a page and an event record say, never
// routing -- so a stale list is a much better outcome than a node that
// will not start.
//
// The cache is replaced only when a fetch parses completely, so a
// truncated or error response cannot empty a good list.

#ifndef DECNET_NODEFETCH_H
#define DECNET_NODEFETCH_H

#include "decnet/common/element.h"
#include "decnet/common/types.h"
#include "decnet/config.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace decnet {

class Node;

// One address and name from a list.
using NodeName = std::pair<Nodeid, std::string>;

// Parse a node name list: lines of "<address> <NAME>", which is the format
// of the HECnet nodenames.dat and of the hecnet.dat that Johnny Billquist
// maintains for PyDECnet.  Blank lines and "#" comments are skipped, and a
// line that does not parse is skipped with a debug line rather than
// failing the whole list.
//
// Returns false if the text yielded nothing at all, which is what tells a
// caller that a response was not a node list.
bool parse_node_list (const std::string &text, std::vector<NodeName> &out);

// What a fetch produced.
struct FetchedList {
    std::vector<NodeName> names;
    // The Last-Modified the server gave, to send back next time.
    std::string           last_modified;
    // True when the server answered "not modified": names is then empty and
    // the cache on disk is already current.
    bool                  unchanged = false;
};

// Fetch one list.  if_modified_since is a validator kept from a previous
// fetch, normally read out of the cache file by read_cached_validator; when
// the server recognises it and the list has not changed, the result has
// unchanged set and no names.
//
// Blocking; used by the element below and by the one-shot command line
// option.
bool fetch_node_list (const std::string &url, FetchedList &out,
                      std::string &error, int timeout_seconds = 30,
                      const std::string &if_modified_since = std::string ());

// The validator recorded in a cache file by a previous write, or empty if
// there is none -- including when the file does not exist.
std::string read_cached_validator (const std::string &path);

// Write a list to a file, through a temporary and a rename so that a
// crash or a full disk cannot leave a half written cache behind.  The
// validator is written as a comment, which the parser skips, so the cache
// stays a plain node name list that anything else can read.
bool write_node_list (const std::string &path,
                      const std::vector<NodeName> &names,
                      const std::string &last_modified, std::string &error);

class NodeFetcher : public Element {
public:
    NodeFetcher (Element *parent, std::vector<NodeSourceConfig> sources);
    ~NodeFetcher () override;

    void start ();
    void stop ();

    void dispatch (Work &) override {}

    bool running () const noexcept { return thread_.joinable (); }

private:
    void run ();                        // the fetch loop
    void fetch_all ();                  // one pass over every source

    std::vector<NodeSourceConfig> sources_;
    std::thread                   thread_;
    std::mutex                    mutex_;
    std::condition_variable       wake_;
    bool                          stopping_ = false;
};

}   // namespace decnet

#endif  // DECNET_NODEFETCH_H

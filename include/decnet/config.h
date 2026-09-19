// decnet/config.h -- the configuration file.
//
// Port of config.py.  Each line is a command followed by Unix style
// options.  The syntax is the same as PyDECnet's, parsed by a small option
// parser here instead of argparse.
//
//     circuit eth-0 Ethernet tap:/dev/tap0 --console Plugh
//     routing 9.54 --type l2router
//     node 9.54 SAMPLE
//     http --http-port 8102 --https-port 0

#ifndef DECNET_CONFIG_H
#define DECNET_CONFIG_H

#include "decnet/common/types.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace decnet {

// One parsed configuration line, before a layer interprets it.
struct ConfigLine {
    std::string                                  command;   // "circuit"
    std::vector<std::string>                     positional;
    std::map<std::string, std::vector<std::string>> options; // "--console"
    std::string                                  source;    // file:line
};

// circuit <name> <type> <device> [options]
struct CircuitConfig {
    std::string name;
    std::string type;        // Ethernet, Multinet, DDCMP, GRE
    std::string device;
    unsigned    cost = 4;
    unsigned    t1 = 0;
    unsigned    t3 = 0;
    bool        mop = false;
    bool        random_address = false;
    // Broadcast circuits only: the designated router election priority and
    // the size of the router table.  Ports of --priority and --nr.
    unsigned    priority = 0;
    unsigned    maxrouters = 0;
    std::string console;
    std::string verify;
    ConfigLine  raw;
};

// routing <id> --type <type> ...
struct RoutingConfig {
    Nodeid   id;
    NodeType type = NodeType::l2router;
    unsigned maxnodes = 1023;
    unsigned maxarea = 63;
    unsigned maxhops = 16;
    unsigned maxcost = 1022;
    unsigned maxvisits = 32;
    // The same limits for the area routing table.  Ports of --amaxhops and
    // --amaxcost, whose defaults differ from the level 1 ones.
    unsigned amaxhops = 16;
    unsigned amaxcost = 128;
    unsigned t1 = 600;
    unsigned bct1 = 10;
};

// node <id> <name> [--inbound-verification x] [--outbound-verification y]
struct NodeConfig {
    Nodeid      id;
    std::string name;
    std::string inbound_verification;
    std::string outbound_verification;

    // True when this entry came from a fetched list rather than from the
    // configuration itself.  A later refresh may replace such a name; a
    // name the operator wrote is left alone.
    bool        from_source = false;
};

// node @<url> [--cache FILE] [--refresh SECONDS]
//
// A node name list fetched over HTTP rather than read from a file.  The
// cache is what the configuration actually loads, so the node starts with
// whatever names it had last time even when the fetch fails or the network
// is down; the fetch happens in the background and rewrites the cache.
struct NodeSourceConfig {
    std::string url;
    std::string cache;
    // How often to re-fetch.  The list changes by a couple of dozen entries
    // a year, so the default is weekly; a refresh whose validator still
    // matches costs one small exchange and no rewrite.  Zero fetches once
    // at startup only.
    unsigned    refresh = 604800;
};

// object --number N --name X --file PROG [--argument A]...
//
// An object run as a separate process.  --module is not supported.
struct ObjectConfig {
    unsigned                 number = 0;
    std::string              name;
    std::string              file;
    std::vector<std::string> arguments;
};

// nsp [--max-connections N] [--qmax N]
struct NspConfig {
    unsigned max_connections = 4095;
    // Unacknowledged data segments allowed in flight at once.
    unsigned qmax = 20;

    // NSP retransmission timing.  The timer follows the measured round trip
    // time to each node.
    //
    // weight: each measurement contributes 1/(weight+1) to the estimate.
    // delay_factor: the timeout is the estimate multiplied by this.
    unsigned weight = 3;
    double   delay_factor = 2.0;

    // How many times a packet is sent before the link is given up on.
    unsigned retransmits = 5;
};

// logging <type> [--sink-node N] [--events E] [--sink-file F] ...
//
// One event sink.  Local sinks are console, file and monitor; a remote sink
// is another node's event logger.  Lines naming the same sink node share
// one connection.
struct LoggingConfig {
    std::string type;            // console, file, monitor
    std::string sink_node;       // empty for a local sink
    std::string sink_username;
    std::string sink_password;
    std::string sink_account;
    std::string sink_file = "events.dat";
    std::string events;          // event list, empty for the default
};

class Config {
public:
    // Read a PyDECnet configuration file.  Throws std::runtime_error with a
    // file:line prefix on a syntax error, as config.py does.
    static Config from_file (const std::string &path);

    // Parse already read text; used by the unit tests.
    static Config from_string (const std::string &text,
                               const std::string &source = "<string>");

    const std::vector<CircuitConfig> &circuits () const noexcept { return circuits_; }
    const std::vector<NodeConfig>    &nodes    () const noexcept { return nodes_; }
    const std::vector<NodeSourceConfig> &node_sources () const noexcept
    { return node_sources_; }
    const std::vector<ObjectConfig>  &objects  () const noexcept { return objects_; }
    const std::vector<LoggingConfig> &logging  () const noexcept { return logging_; }
    const NspConfig &nsp () const noexcept { return nsp_; }
    const std::optional<RoutingConfig> &routing () const noexcept { return routing_; }

    const std::string &identification () const noexcept { return identification_; }
    const std::string &node_name () const noexcept { return node_name_; }

    // HTTP port.  Zero means not configured.
    unsigned http_port () const noexcept { return http_port_; }

    // Lines no layer has claimed.  Kept rather than rejected so PyDECnet
    // configuration files load.
    const std::vector<ConfigLine> &unhandled () const noexcept { return unhandled_; }

    // The URL "@hecnet" stands for, which is the list Johnny Billquist
    // maintains for PyDECnet in exactly this format.
    static const char *hecnet_url () noexcept;

private:
    void apply (ConfigLine line);

    // Read a file and apply each of its lines with a component name in
    // front, which is what "node @file" means: the file holds the arguments
    // of a node line, not whole configuration lines.  Port of the prefix
    // argument to config.py's scanconfig.
    void include_prefixed (const std::string &path, const std::string &prefix);

    // Directory of the file being read.  "@file" includes are relative to it,
    // as in config.py.
    std::string base_dir_;

    // Set while reading a fetched list's cache, so the entries it creates
    // are marked as coming from a source rather than from the operator.
    bool        in_source_include_ = false;

    std::vector<CircuitConfig>   circuits_;
    std::vector<NodeConfig>      nodes_;
    std::vector<NodeSourceConfig> node_sources_;
    std::vector<ObjectConfig>    objects_;
    std::vector<LoggingConfig>   logging_;
    NspConfig                    nsp_;
    std::optional<RoutingConfig> routing_;
    std::vector<ConfigLine>      unhandled_;
    std::string                  identification_;
    std::string                  node_name_;
    unsigned                     http_port_ = 0;
};

// Split a config file line into words, honouring quotes and '#' comments.
// Exposed because the tests exercise it directly.
std::vector<std::string> split_config_line (const std::string &line);

// Turn a word list into a ConfigLine, separating --options from positionals.
ConfigLine parse_config_line (const std::vector<std::string> &words,
                              const std::string &source);

}   // namespace decnet

#endif  // DECNET_CONFIG_H

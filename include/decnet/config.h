// decnet/config.h -- the configuration file.
//
// Port of config.py.  the Python reuses argparse: each line of the config
// file is a command word followed by Unix style options, and each layer
// registers a subparser.  We keep the syntax exactly -- existing the Python
// configuration files must work unchanged -- but parse it with a small
// hand written option parser rather than dragging in a dependency.
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
};

// object --number N --name X --file PROG [--argument A]...
//
// An object implemented as a separate process.  the Python also supports
// --module for one implemented inside the daemon; here the built-in
// objects are registered in code, so only --file is read.
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
};

// logging <type> [--sink-node N] [--events E] [--sink-file F] ...
//
// One sink for event records.  A local sink is the console, a file or the
// monitoring interface; a remote sink is another node's event logger,
// reached over a logical link.  Several logging lines naming the same sink
// node make one connection with a filter per sink type at the far end.
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
    // Read a the Python configuration file.  Throws std::runtime_error with a
    // file:line prefix on a syntax error, as config.py does.
    static Config from_file (const std::string &path);

    // Parse already read text; used by the unit tests.
    static Config from_string (const std::string &text,
                               const std::string &source = "<string>");

    const std::vector<CircuitConfig> &circuits () const noexcept { return circuits_; }
    const std::vector<NodeConfig>    &nodes    () const noexcept { return nodes_; }
    const std::vector<ObjectConfig>  &objects  () const noexcept { return objects_; }
    const std::vector<LoggingConfig> &logging  () const noexcept { return logging_; }
    const NspConfig &nsp () const noexcept { return nsp_; }
    const std::optional<RoutingConfig> &routing () const noexcept { return routing_; }

    const std::string &identification () const noexcept { return identification_; }
    const std::string &node_name () const noexcept { return node_name_; }

    // The monitoring server.  Zero means "not configured", which is also
    // what "--http-port 0" means to the Python: the port is how the feature
    // is turned on and off.
    unsigned http_port () const noexcept { return http_port_; }

    // Lines whose command no layer has claimed yet.  Everything the port
    // has not reached is parsed and kept here rather than rejected, so a
    // real configuration file still loads.
    const std::vector<ConfigLine> &unhandled () const noexcept { return unhandled_; }

private:
    void apply (ConfigLine line);

    // Directory of the file being read.  A "@file" include resolves against
    // it, not against the working directory, which is what config.py does
    // (os.path.join (os.path.dirname (f.name), ifn)) and what lets a
    // configuration directory be moved as a unit.
    std::string base_dir_;

    std::vector<CircuitConfig>   circuits_;
    std::vector<NodeConfig>      nodes_;
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

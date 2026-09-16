#include "decnet/config.h"

#include "decnet/common/logging.h"

#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace decnet {

namespace {

unsigned to_uint (const std::string &s, const char *what)
{
    unsigned v = 0;
    auto [ptr, ec] = std::from_chars (s.data (), s.data () + s.size (), v);
    if (ec != std::errc () || ptr != s.data () + s.size ())
        throw std::runtime_error (std::string ("bad ") + what + ": " + s);
    return v;
}

// First option value, or a default.
const std::string &opt (const ConfigLine &l, const std::string &name,
                        const std::string &dflt)
{
    auto it = l.options.find (name);
    if (it == l.options.end () || it->second.empty ()) return dflt;
    return it->second.front ();
}

bool has (const ConfigLine &l, const std::string &name)
{
    return l.options.count (name) != 0;
}

NodeType node_type (const std::string &s)
{
    if (s == "l2router")       return NodeType::l2router;
    if (s == "l1router")       return NodeType::l1router;
    if (s == "endnode")        return NodeType::endnode;
    if (s == "phase3router")   return NodeType::phase3router;
    if (s == "phase3endnode")  return NodeType::phase3endnode;
    if (s == "phase2")         return NodeType::phase2;
    throw std::runtime_error ("unknown node type: " + s);
}

}   // namespace

// ------------------------------------------------------------- tokenizing

std::vector<std::string> split_config_line (const std::string &line)
{
    std::vector<std::string> words;
    std::string cur;
    bool in_word = false;
    char quote = 0;

    for (std::size_t i = 0; i < line.size (); ++i) {
        char c = line[i];
        if (quote) {
            if (c == quote) quote = 0;
            else            cur += c;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; in_word = true; continue; }
        // A '#' outside quotes starts a comment, as in PyDECnet's files.
        if (c == '#') break;
        if (c == ' ' || c == '\t') {
            if (in_word) { words.push_back (cur); cur.clear (); in_word = false; }
            continue;
        }
        cur += c;
        in_word = true;
    }
    if (quote) throw std::runtime_error ("unterminated quote");
    if (in_word) words.push_back (cur);
    return words;
}

ConfigLine parse_config_line (const std::vector<std::string> &words,
                              const std::string &source)
{
    ConfigLine line;
    line.source = source;
    if (words.empty ()) return line;
    line.command = words[0];

    for (std::size_t i = 1; i < words.size (); ++i) {
        const std::string &w = words[i];
        if (w.size () > 2 && w[0] == '-' && w[1] == '-') {
            std::string name = w.substr (2);
            std::string value;
            // Both "--opt value" and "--opt=value" spellings, as argparse
            // accepts both.
            if (auto eq = name.find ('='); eq != std::string::npos) {
                value = name.substr (eq + 1);
                name = name.substr (0, eq);
                line.options[name].push_back (value);
                continue;
            }
            // A following word that is not itself an option is the value;
            // otherwise this is a flag.
            if (i + 1 < words.size ()
                && !(words[i + 1].size () > 2 && words[i + 1][0] == '-'
                     && words[i + 1][1] == '-')) {
                line.options[name].push_back (words[++i]);
            } else {
                line.options[name];          // present, no value: a flag
            }
        } else {
            line.positional.push_back (w);
        }
    }
    return line;
}

// ---------------------------------------------------------------- Config

Config Config::from_string (const std::string &text, const std::string &source)
{
    Config cfg;
    if (auto slash = source.rfind ('/'); slash != std::string::npos)
        cfg.base_dir_ = source.substr (0, slash + 1);
    std::istringstream in (text);
    std::string raw;
    unsigned lineno = 0;

    while (std::getline (in, raw)) {
        ++lineno;
        std::string where = source + ":" + std::to_string (lineno);
        std::vector<std::string> words;
        try {
            words = split_config_line (raw);
        } catch (const std::exception &e) {
            throw std::runtime_error (where + ": " + e.what ());
        }
        if (words.empty ()) continue;
        try {
            cfg.apply (parse_config_line (words, where));
        } catch (const std::exception &e) {
            throw std::runtime_error (where + ": " + e.what ());
        }
    }
    return cfg;
}

Config Config::from_file (const std::string &path)
{
    std::ifstream f (path);
    if (!f) throw std::runtime_error ("cannot open config file: " + path);
    std::ostringstream buf;
    buf << f.rdbuf ();
    Config cfg = from_string (buf.str (), path);
    return cfg;
}

void Config::apply (ConfigLine line)
{
    static const std::string empty;

    if (line.command == "circuit") {
        if (line.positional.size () < 3)
            throw std::runtime_error ("circuit needs name, type and device");
        CircuitConfig c;
        c.name   = circname (line.positional[0]);
        c.type   = line.positional[1];
        c.device = line.positional[2];
        if (has (line, "cost")) c.cost = to_uint (opt (line, "cost", empty), "cost");
        if (has (line, "t1"))   c.t1   = to_uint (opt (line, "t1", empty), "t1");
        if (has (line, "t3"))   c.t3   = to_uint (opt (line, "t3", empty), "t3");
        c.mop            = has (line, "mop");
        if (has (line, "priority"))
            c.priority = to_uint (opt (line, "priority", empty), "priority");
        if (has (line, "nr"))
            c.maxrouters = to_uint (opt (line, "nr", empty), "nr");
        c.random_address = has (line, "random-address")
                        || has (line, "random");
        c.console        = opt (line, "console", empty);
        c.verify         = opt (line, "verify", empty);
        c.raw            = std::move (line);
        circuits_.push_back (std::move (c));
        return;
    }

    if (line.command == "http") {
        // --https-port is accepted and ignored.
        if (has (line, "http-port"))
            http_port_ = to_uint (opt (line, "http-port", empty), "http-port");
        return;
    }

    if (line.command == "routing") {
        if (line.positional.empty ())
            throw std::runtime_error ("routing needs a node address");
        RoutingConfig r;
        r.id = Nodeid::parse (line.positional[0]);
        if (has (line, "type")) r.type = node_type (opt (line, "type", empty));
        if (has (line, "maxnodes"))
            r.maxnodes = to_uint (opt (line, "maxnodes", empty), "maxnodes");
        if (has (line, "maxarea"))
            r.maxarea = to_uint (opt (line, "maxarea", empty), "maxarea");
        if (has (line, "maxhops"))
            r.maxhops = to_uint (opt (line, "maxhops", empty), "maxhops");
        if (has (line, "maxcost"))
            r.maxcost = to_uint (opt (line, "maxcost", empty), "maxcost");
        if (has (line, "maxvisits"))
            r.maxvisits = to_uint (opt (line, "maxvisits", empty), "maxvisits");
        if (has (line, "t1"))
            r.t1 = to_uint (opt (line, "t1", empty), "t1");
        if (has (line, "bct1"))
            r.bct1 = to_uint (opt (line, "bct1", empty), "bct1");
        if (has (line, "amaxhops"))
            r.amaxhops = to_uint (opt (line, "amaxhops", empty), "amaxhops");
        if (has (line, "amaxcost"))
            r.amaxcost = to_uint (opt (line, "amaxcost", empty), "amaxcost");
        routing_ = r;
        return;
    }

    if (line.command == "node") {
        if (line.positional.empty ())
            throw std::runtime_error ("node needs an address");
        // "node @file" pulls in a node name database.
        if (line.positional[0].size () > 1 && line.positional[0][0] == '@') {
            std::string inc = line.positional[0].substr (1);
            if (!inc.empty () && inc[0] != '/') inc = base_dir_ + inc;
            Config sub = from_file (inc);
            for (auto &n : sub.nodes_) nodes_.push_back (std::move (n));
            return;
        }
        NodeConfig n;
        n.id = Nodeid::parse (line.positional[0]);
        if (line.positional.size () > 1)
            n.name = nodename (line.positional[1]);
        n.inbound_verification  = opt (line, "inbound-verification", empty);
        n.outbound_verification = opt (line, "outbound-verification", empty);
        // The node line naming our own address also names this system.
        nodes_.push_back (std::move (n));
        return;
    }

    if (line.command == "nsp") {
        if (has (line, "max-connections"))
            nsp_.max_connections =
                to_uint (opt (line, "max-connections", empty),
                         "max-connections");
        if (has (line, "qmax"))
            nsp_.qmax = to_uint (opt (line, "qmax", empty), "qmax");
        if (nsp_.qmax < 1 || nsp_.qmax > 4095)
            throw std::runtime_error ("qmax out of range");
        if (has (line, "nsp-weight")) {
            nsp_.weight = to_uint (opt (line, "nsp-weight", empty),
                                   "nsp-weight");
            if (nsp_.weight < 1 || nsp_.weight > 255)
                throw std::runtime_error ("nsp-weight out of range");
        }
        if (has (line, "nsp-delay")) {
            nsp_.delay_factor =
                std::stod (opt (line, "nsp-delay", empty));
            if (nsp_.delay_factor < 1.0 || nsp_.delay_factor > 15.94)
                throw std::runtime_error ("nsp-delay out of range");
        }
        if (has (line, "retransmits")) {
            nsp_.retransmits = to_uint (opt (line, "retransmits", empty),
                                        "retransmits");
            if (nsp_.retransmits < 1)
                throw std::runtime_error ("retransmits out of range");
        }
        return;
    }

    if (line.command == "object") {
        ObjectConfig o;
        if (has (line, "number"))
            o.number = to_uint (opt (line, "number", empty), "object number");
        o.name = opt (line, "name", empty);
        o.file = opt (line, "file", empty);
        auto it = line.options.find ("argument");
        if (it != line.options.end ()) o.arguments = it->second;
        if (!o.number && o.name.empty ())
            throw std::runtime_error ("an object needs a number or a name");
        if (o.file.empty ())
            throw std::runtime_error ("object needs --file "
                                      "(--module is not supported)");
        objects_.push_back (std::move (o));
        return;
    }

    if (line.command == "logging") {
        if (line.positional.size () != 1)
            throw std::runtime_error ("logging needs a sink type");
        LoggingConfig l;
        l.type = line.positional[0];
        if (l.type != "console" && l.type != "file" && l.type != "monitor")
            throw std::runtime_error ("logging type must be console, file "
                                      "or monitor");
        l.sink_node     = opt (line, "sink-node", empty);
        l.sink_username = opt (line, "sink-username", empty);
        l.sink_password = opt (line, "sink-password", empty);
        l.sink_account  = opt (line, "sink-account", empty);
        l.sink_file     = opt (line, "sink-file", l.sink_file);
        l.events        = opt (line, "events", empty);
        logging_.push_back (std::move (l));
        return;
    }

    if (line.command == "system") {
        identification_ = opt (line, "ident", identification_);
        return;
    }

    // PORT: commands for unported layers (bridge, api, ...) are kept so
    // PyDECnet configuration files load.
    unhandled_.push_back (std::move (line));
}

}   // namespace decnet

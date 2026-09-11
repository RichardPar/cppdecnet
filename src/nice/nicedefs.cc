// src/nice/nicedefs.cc -- what each NICE parameter means.
//
// Port of the parameter tables in nicepackets.py: the _layout of each reply
// class, plus the counter tables from nice_coding.py.  pydecnet uses these
// both to decode a request (where the type code is missing) and to format a
// reply; here decoding never needs them -- a reply is self describing -- so
// they exist for display, and for the reply builders to know which number
// carries which fact.
//
// The tables are in parameter number order, which is also the order NCP
// prints them in.

#include "decnet/nice/packets.h"

namespace decnet::nice {

namespace {

// ---------------------------------------------------------------- labels

// The node type codes, shared by "Type" as an adjacency property (810) and
// as a node characteristic (901).  Note these are not the routing layer's
// type numbers.
constexpr const char *const rvalues[] = {
    "Routing III", "Non-Routing III", "Phase II", "Area",
    "Routing IV", "Non-Routing IV"
};

constexpr const char *const ed_values[] = { "Enabled", "Disabled" };

constexpr const char *const node_state[] = {
    "On", "Off", "Shut", "Restricted", "Reachable", "Unreachable"
};

constexpr const char *const area_state[] = {
    nullptr, nullptr, nullptr, nullptr, "Reachable", "Unreachable"
};

constexpr const char *const circuit_state[] = {
    "On", "Off", "Service", "Cleared"
};

constexpr const char *const substate[] = {
    "Starting", "Reflecting", "Looping", "Loading", "Dumping",
    "Triggering", "Autoservice", "Autoloading", "Autodumping",
    "Autotriggering", "Synchronizing", "Failed"
};

constexpr const char *const cpu_values[] = {
    "PDP8", "PDP11", "DECSystem-10/20", "VAX"
};

constexpr const char *const service_node_version[] = {
    "Phase III", "Phase IV"
};

constexpr const char *const software_type[] = {
    "Secondary loader", "Tertiary loader", "System"
};

constexpr const char *const loop_with[] = { "Zeroes", "Ones", "Mixed" };

constexpr const char *const loop_help[] = { "Transmit", "Receive", "Full" };

constexpr const char *const circuit_usage[] = {
    "Permanent", "Incoming", "Outgoing"
};

constexpr const char *const circuit_type[] = {
    "DDCMP point", "DDCMP control", "DDCMP tributary", "X.25",
    "DDCMP DMC", nullptr, "Ethernet", "CI", "QP2 (DTE20)", "BISYNC"
};

constexpr const char *const line_protocol[] = {
    "DDCMP point", "DDCMP control", "DDCMP tributary", nullptr,
    "DDCMP DMC", "LAPB", "Ethernet", "CI", "QP2 (DTE20)"
};

constexpr const char *const polling_state[] = {
    "Automatic", "Active", "Inactive", "Dying", "Dead"
};

constexpr const char *const controller_values[] = { "Normal", "Loopback" };
constexpr const char *const duplex_values[] = { "Full", "Half" };
constexpr const char *const clock_values[] = { "External", "Internal" };
constexpr const char *const surveillance[] = { "Enabled", "Disabled" };

// MOP functions, one per element of the module's "Functions" list.
constexpr const char *const mop_functions[] = {
    "Loop", "Dump", "Primary loader", "Secondary loader", "Boot",
    "Console carrier", "Counters"
};

// Qualifier names for the mapped counters.  The bit number indexes these.
constexpr const char *const data_errors_in[] = {
    "NAKs sent, header block check error",
    "NAKs sent, data field block check error",
    "NAKs sent, REP response"
};

constexpr const char *const data_errors_out[] = {
    "NAKs received, header block check error",
    "NAKs received, data field block check error",
    "NAKs received, REP response"
};

constexpr const char *const remote_buffer_errors[] = {
    "NAKs received buffer unavailable",
    "NAKs received buffer too small",
    "RNR received, buffer unavailable"
};

constexpr const char *const local_buffer_errors[] = {
    "NAKs sent buffer unavailable",
    "NAKs sent buffer too small",
    "RNR sent, buffer unavailable"
};

constexpr const char *const send_failure[] = {
    "Excessive collisions", "Carrier check failed", "Short circuit",
    "Open circuit", "Frame too long", "Remote failure to defer"
};

constexpr const char *const receive_failure[] = {
    "Block check error", "Framing error", "Frame too long"
};

// ------------------------------------------------------------ the tables

// Shorthands, so a table row reads like the Python tuple it came from.
constexpr ParamDef P (std::uint16_t n, const char *d,
                      Labels l = {}, Style s = Style::plain)
{
    return ParamDef { n, false, d, l, s };
}

constexpr ParamDef C (std::uint16_t n, const char *d, Labels l = {})
{
    return ParamDef { n, true, d, l, Style::plain };
}

// Node parameters.  Everything NCP can report about a node, whether it is
// this one (the executor) or another.
constexpr ParamDef node_defs[] = {
    P (   0, "State", node_state),
    P (  10, "Physical address"),
    P ( 100, "Identification"),
    P ( 101, "Management Version", {}, Style::version),
    P ( 110, "Service circuit"),
    P ( 111, "Service password"),
    P ( 112, "Service device"),
    P ( 113, "CPU", cpu_values),
    P ( 114, "Hardware address"),
    P ( 115, "Service node version", service_node_version),
    P ( 120, "Load file"),
    P ( 121, "Secondary loader"),
    P ( 122, "Tertiary loader"),
    P ( 123, "Diagnostic file"),
    P ( 125, "Software type", software_type),
    P ( 126, "Software identification"),
    P ( 130, "Dump file"),
    P ( 131, "Secondary dumper"),
    P ( 135, "Dump address"),
    P ( 136, "Dump count"),
    P ( 140, "Host", {}, Style::node),
    P ( 150, "Loop count"),
    P ( 151, "Loop length"),
    P ( 152, "Loop with", loop_with),
    P ( 153, "Loop assistant physical address"),
    P ( 154, "Loop help", loop_help),
    P ( 155, "Loop node", {}, Style::node),
    P ( 156, "Loop assistant node", {}, Style::node),
    P ( 160, "Counter timer"),
    P ( 501, "Circuit"),
    P ( 502, "Address"),
    P ( 510, "Incoming timer"),
    P ( 511, "Outgoing timer"),
    P ( 522, "Incoming proxy", ed_values),
    P ( 523, "Outgoing proxy", ed_values),
    P ( 600, "Active links"),
    P ( 601, "Delay"),
    P ( 700, "ECL version", {}, Style::version),
    P ( 710, "Maximum links"),
    P ( 720, "Delay factor"),
    P ( 721, "Delay weight"),
    P ( 722, "Inactivity timer"),
    P ( 723, "Retransmit factor"),
    P ( 810, "Type", rvalues),
    P ( 820, "Cost"),
    P ( 821, "Hops"),
    P ( 822, "Circuit"),
    P ( 830, "Next node", {}, Style::node),
    P ( 900, "Routing version", {}, Style::version),
    P ( 901, "Type", rvalues),
    P ( 910, "Routing timer"),
    P ( 911, "Subaddresses"),
    P ( 912, "Broadcast routing timer"),
    P ( 920, "Maximum address"),
    P ( 921, "Maximum circuits"),
    P ( 922, "Maximum cost"),
    P ( 923, "Maximum hops"),
    P ( 924, "Maximum visits"),
    P ( 925, "Maximum area"),
    P ( 926, "Max broadcast nonrouters"),
    P ( 927, "Max broadcast routers"),
    P ( 928, "Area maximum cost"),
    P ( 929, "Area maximum hops"),
    P ( 930, "Maximum buffers"),
    P ( 931, "Buffer size"),
    P ( 932, "Segment buffer size"),
    P ( 933, "Maximum path splits"),
    // The counters.  Bit 15 of the number separates them from the
    // parameters above, so 600 as a parameter and 600 as a counter are
    // different things and both may be present.
    C (   0, "Seconds since last zeroed"),
    C ( 600, "User bytes received"),
    C ( 601, "User bytes sent"),
    C ( 602, "User messages received"),
    C ( 603, "User messages sent"),
    C ( 608, "Total bytes received"),
    C ( 609, "Total bytes sent"),
    C ( 610, "Total messages received"),
    C ( 611, "Total messages sent"),
    C ( 620, "Connects received"),
    C ( 621, "Connects sent"),
    C ( 630, "Response timeouts"),
    C ( 640, "Received connect resource errors"),
    C ( 700, "Maximum logical links active"),
    C ( 900, "Aged packet loss"),
    C ( 901, "Node unreachable packet loss"),
    C ( 902, "Node out-of-range packet loss"),
    C ( 903, "Oversized packet loss"),
    C ( 910, "Packet format error"),
    C ( 920, "Partial routing update loss"),
    C ( 930, "Verification reject"),
};

// Circuit parameters.  The read-write ones -- state, cost, priority and the
// timers -- are the same list a SET CIRCUIT request carries.
constexpr ParamDef circuit_defs[] = {
    P (   0, "State", circuit_state),
    P (   1, "Substate", substate),
    P ( 100, "Service", ed_values),
    P ( 110, "Counter timer"),
    P ( 120, "Service physical address"),
    P ( 121, "Service substate"),
    P ( 200, "Connected node", {}, Style::node),
    P ( 201, "Connected object"),
    P ( 400, "Loopback name"),
    P ( 800, "Adjacent node", {}, Style::node),
    P ( 801, "Designated router", {}, Style::node),
    P ( 810, "Block size"),
    P ( 811, "Originating queue limit"),
    P ( 900, "Cost"),
    P ( 901, "Maximum routers"),
    P ( 902, "Router priority"),
    P ( 904, "Level 2 cost"),
    P ( 906, "Hello timer"),
    P ( 907, "Listen timer"),
    P ( 910, "Blocking", ed_values),
    P ( 920, "Maximum recalls"),
    P ( 921, "Recall timer"),
    P ( 930, "Number"),
    P (1000, "User"),
    P (1010, "Polling state", polling_state),
    P (1011, "Polling substate", polling_state),
    P (1100, "Owner"),
    P (1110, "Line"),
    P (1111, "Usage", circuit_usage),
    P (1112, "Type", circuit_type),
    P (1120, "Dte"),
    P (1121, "Channel"),
    P (1122, "Maximum data"),
    P (1123, "Maximum window"),
    P (1140, "Tributary"),
    P (1141, "Babble timer"),
    P (1142, "Transmit timer"),
    P (1145, "Maximum buffers"),
    P (1146, "Maximum transmits"),
    P (1150, "Active base"),
    P (1151, "Active increment"),
    P (1152, "Inactive base"),
    P (1153, "Inactive increment"),
    P (1154, "Inactive threshold"),
    P (1155, "Dying base"),
    P (1156, "Dying increment"),
    P (1157, "Dying threshold"),
    P (1158, "Dead threshold"),
    C (   0, "Seconds since last zeroed"),
    C ( 800, "Terminating packets received"),
    C ( 801, "Originating packets sent"),
    C ( 802, "Terminating congestion loss"),
    C ( 805, "Corruption loss"),
    C ( 810, "Transit packets received"),
    C ( 811, "Transit packets sent"),
    C ( 812, "Transit congestion loss"),
    C ( 820, "Circuit down"),
    C ( 821, "Initialization failure"),
    C ( 900, "Peak adjacencies"),
    C (1000, "Bytes received"),
    C (1001, "Bytes sent"),
    // A circuit read picks up the port's counters as well as routing's, so
    // the two multicast counters the broadcast datalink keeps have to be
    // named here too, not only under line.  Without them a circuit's
    // counters read "Counter #1002", which is what the monitoring pages
    // showed.
    C (1002, "Multicast bytes received"),
    C (1010, "Data blocks received"),
    C (1011, "Data blocks sent"),
    C (1012, "Multicast blocks received"),
    C (1020, "Data errors inbound", data_errors_in),
    C (1021, "Data errors outbound", data_errors_out),
    C (1030, "Remote reply timeouts"),
    C (1031, "Local reply timeouts"),
    C (1040, "Remote buffer errors", remote_buffer_errors),
    C (1041, "Local buffer errors", local_buffer_errors),
    C (1050, "Selection intervals elapsed"),
    C (1065, "User buffer unavailable"),
    // pydecnet's own: how long the circuit has been up, which is not
    // architected but is the first thing anyone wants to know.
    C (3900, "Seconds since last circuit up"),
};

// Line parameters.  A line is the hardware under a circuit; on an Ethernet
// the two are one to one, which is why so much of this is about DDCMP.
constexpr ParamDef line_defs[] = {
    P (   0, "State", circuit_state),
    P (   1, "Substate", substate),
    P ( 100, "Service", ed_values),
    P ( 110, "Counter timer"),
    P (1100, "Device"),
    P (1105, "Receive buffers"),
    P (1110, "Controller", controller_values),
    P (1111, "Duplex", duplex_values),
    P (1112, "Protocol", line_protocol),
    P (1113, "Clock", clock_values),
    P (1120, "Service timer"),
    P (1121, "Retransmit timer"),
    P (1122, "Holdback timer"),
    P (1130, "Maximum block"),
    P (1131, "Maximum retransmits"),
    P (1132, "Maximum window"),
    P (1150, "Scheduling timer"),
    P (1151, "Dead timer"),
    P (1152, "Delay timer"),
    P (1153, "Stream timer"),
    P (1160, "Hardware address"),
    C (   0, "Seconds since last zeroed"),
    C (1000, "Bytes received"),
    C (1001, "Bytes sent"),
    C (1002, "Multicast bytes received"),
    C (1010, "Data blocks received"),
    C (1011, "Data blocks sent"),
    C (1012, "Multicast blocks received"),
    C (1013, "Blocks sent, initially deferred"),
    C (1014, "Blocks sent, single collision"),
    C (1015, "Blocks sent, multiple collisions"),
    C (1020, "Data errors inbound", data_errors_in),
    C (1021, "Data errors outbound", data_errors_out),
    C (1030, "Remote reply timeouts"),
    C (1031, "Local reply timeouts"),
    C (1040, "Remote buffer errors", remote_buffer_errors),
    C (1041, "Local buffer errors", local_buffer_errors),
    C (1060, "Send failure", send_failure),
    C (1061, "Collision detect check failure"),
    C (1062, "Receive failure", receive_failure),
    C (1063, "Unrecognized frame destination"),
    C (1064, "Data overrun"),
    C (1065, "System buffer unavailable"),
    C (1066, "User buffer unavailable"),
};

// Area parameters: a short list, because an area is only ever reachable or
// not, and if it is, by what route.
constexpr ParamDef area_defs[] = {
    P (   0, "State", area_state),
    P ( 820, "Cost"),
    P ( 821, "Hops"),
    P ( 822, "Circuit"),
    P ( 830, "Next node", {}, Style::node),
};

// Module parameters.  The only module here is the MOP configurator, which
// reports what it has heard other stations announce about themselves.
constexpr ParamDef module_defs[] = {
    P ( 100, "Circuit"),
    P ( 110, "Surveillance", surveillance),
    P ( 111, "Elapsed time"),
    P ( 120, "Physical address"),
    P ( 130, "Last report"),
    P (1001, "Maintenance version", {}, Style::version),
    P (1002, "Functions", mop_functions),
    P (1003, "Console user"),
    P (1004, "Reservation timer"),
    P (1005, "Command size"),
    P (1006, "Response size"),
    P (1007, "Hardware address"),
    P (1100, "Device"),
    P (1200, "Software identification"),
    P (1300, "System processor"),
    P (1400, "Data link"),
    P (1401, "Data link buffer size"),
};

}   // namespace

ParamDefs node_params ()    { return node_defs; }
ParamDefs circuit_params () { return circuit_defs; }
ParamDefs line_params ()    { return line_defs; }
ParamDefs area_params ()    { return area_defs; }
ParamDefs module_params ()  { return module_defs; }
ParamDefs logging_params () { return {}; }

ParamDefs params_for (std::uint8_t entity_kind)
{
    switch (entity_kind) {
    case Entity::node:    return node_params ();
    case Entity::circuit: return circuit_params ();
    case Entity::line:    return line_params ();
    case Entity::area:    return area_params ();
    case Entity::module:  return module_params ();
    default:              return {};
    }
}

Labels node_type_labels () { return rvalues; }

}   // namespace decnet::nice

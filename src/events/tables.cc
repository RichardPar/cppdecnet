// The event catalogue: names, severities and parameter meanings.
//
// The data part of events.py.  Shared parameter tables are spans
// referenced by several rows.  Events with unknown parameters still
// decode and display, with numbers in place of names.

#include "decnet/events/events.h"

namespace decnet::events {

using nice::ParamDef;
using L = logging::Level;
using nice::Style;

namespace {

// ------------------------------------------------------- shared labels

constexpr const char *const status_labels[] = { "Reachable", "Unreachable" };

constexpr const char *const reason_labels[] = {
    "Operator command", "Normal operation"
};

constexpr const char *const password_labels[] = { "Set" };

// ------------------------------------------------- class 0, network management

constexpr const char *const service_labels[]   = { "Load", "Dump" };
constexpr const char *const operation_labels[] = { "Initiated", "Terminated" };

constexpr const char *const netman_reason_labels[] = {
    "Receive timeout",
    "Receive error",
    "Line state change by higher level",
    "Unrecognized request",
    "Line open error"
};

constexpr const char *const software_labels[] = {
    "Secondary loader", "Tertiary loader", "System"
};

constexpr ParamDef netman_params[] = {
    { 0, false, "Service",       service_labels },
    { 1, false, "Status",        {} },
    { 2, false, "Operation",     operation_labels },
    { 3, false, "Reason",        netman_reason_labels },
    { 4, false, "Qualifier",     {} },
    { 5, false, "Node",          {}, Style::node },
    { 6, false, "DTE",           {} },
    { 7, false, "Filespec",      {} },
    { 8, false, "Software type", software_labels },
};

// ------------------------------------------------------- class 2, session

constexpr const char *const se_state_labels[] = {
    "On", "Off", "Shut", "Restricted"
};

constexpr ParamDef session_params[] = {
    { 0, false, "Reason",              reason_labels },
    { 1, false, "Old state",           se_state_labels },
    { 2, false, "New state",           se_state_labels },
    { 3, false, "Source node",         {}, Style::node },
    { 4, false, "Source process",      {} },
    { 5, false, "Destination process", {} },
    { 6, false, "User",                {} },
    { 7, false, "Password",            password_labels },
    { 8, false, "Account",             {} },
};

// ---------------------------------------------------------- node counters

constexpr ParamDef node_counters[] = {
    { 0,    false, "Message",                           {} },
    { 1,    false, "Current flow control request count", {} },
    { 2,    false, "Source node",                       {}, Style::node },
    { 0,    true,  "Seconds since last zeroed",         {} },
    { 600,  true,  "User bytes received",               {} },
    { 601,  true,  "User bytes sent",                   {} },
    { 602,  true,  "User messages received",            {} },
    { 603,  true,  "User messages sent",                {} },
    { 608,  true,  "Total bytes received",              {} },
    { 609,  true,  "Total bytes sent",                  {} },
    { 610,  true,  "Total messages received",           {} },
    { 611,  true,  "Total messages sent",               {} },
    { 620,  true,  "Connects received",                 {} },
    { 621,  true,  "Connects sent",                     {} },
    { 630,  true,  "Response timeouts",                 {} },
    { 640,  true,  "Received connect resource errors",  {} },
    { 700,  true,  "Maximum logical links active",      {} },
    { 900,  true,  "Aged packet loss",                  {} },
    { 901,  true,  "Node unreachable packet loss",      {} },
    { 902,  true,  "Node out-of-range packet loss",     {} },
    { 903,  true,  "Oversized packet loss",             {} },
    { 910,  true,  "Packet format error",               {} },
    { 920,  true,  "Partial routing update loss",       {} },
    { 930,  true,  "Verification reject",               {} },
    { 2200, true,  "Current reachable nodes",           {} },
    { 2201, true,  "Maximum reachable nodes",           {} },
    { 2300, true,  "Maximum logical links",             {} },
    { 2310, true,  "Received connect resource errors",  {} },
};

// ------------------------------------------------------- class 4, routing

constexpr const char *const routing_reason_labels[] = {
    "Circuit synchronization lost",
    "Data errors",
    "Unexpected packet type",
    "Routing update checksum error",
    "Adjacency address change",
    "Verification receive timeout",
    "Version skew",
    "Adjacency address out of range",
    "Adjacency block size too small",
    "Invalid verification seed value",
    "Adjacency listener receive timeout",
    "Adjacency listener received invalid data",
    "Call failed",
    "Verification password require for Phase III node",
    "Dropped by adjacent node"
};

constexpr ParamDef routing_params[] = {
    { 0, false, "Packet header",    {} },
    { 1, false, "Packet beginning", {} },
    { 2, false, "Highest address",  {} },
    { 3, false, "Node",             {}, Style::node },
    { 4, false, "Expected node",    {}, Style::node },
    { 5, false, "Reason",           routing_reason_labels },
    { 6, false, "Received version", {}, Style::version },
    { 7, false, "Status",           status_labels },
    { 8, false, "Adjacent node",    {}, Style::node },
};

// A reachability change names only the status.
constexpr ParamDef reach_params[] = {
    { 7, false, "Status", status_labels },
};

// ----------------------------------------------------- class 5, data link

constexpr const char *const dl_state_labels[] = {
    "Halted", "IStrt", "AStrt", "Running", "Maintenance"
};

constexpr const char *const dl_state11_labels[] = { "On", "Off", "Shut" };

constexpr const char *const trib_status_labels[] = {
    "Streaming",
    "Continued send after timeout",
    "Continued send after deselect",
    "Ended streaming"
};

constexpr const char *const failure_labels[] = {
    "Excessive collisions",
    "Carrier check failed",
    "(OBSOLETE)",
    "Short circuit",
    "Open circuit",
    "Frame too long",
    "Remote failure to defer",
    "Block check error",
    "Framing error",
    "Data overrun",
    "System buffer unavailable",
    "User buffer unavailable",
    "Unrecognized frame destination"
};

// Qualifier names for the mapped circuit counters.
constexpr const char *const in_err_labels[] = {
    nullptr,
    "NAKs sent, data field block check error",
    "NAKs sent, REP response"
};

constexpr const char *const out_err_labels[] = {
    "NAKs received, header block check error",
    "NAKs received, data field block check error",
    "NAKs received, REP response"
};

constexpr const char *const remote_buf_labels[] = {
    "NAKs received buffer unavailable",
    "NAKs received buffer too small"
};

constexpr const char *const local_buf_labels[] = {
    "NAKs sent buffer unavailable",
    "NAKs sent buffer too small"
};

constexpr const char *const sel_timeout_labels[] = {
    "No reply to select", "Incomplete reply to select"
};

constexpr ParamDef datalink_params[] = {
    { 0,  false, "Old state",           dl_state_labels },
    { 1,  false, "New state",           dl_state_labels },
    { 2,  false, "Header",              {} },
    { 3,  false, "Selected tributary",  {} },
    { 4,  false, "Previous tributary",  {} },
    { 5,  false, "Tributary status",    trib_status_labels },
    { 6,  false, "Received tributary",  {} },
    { 7,  false, "Block length",        {} },
    { 8,  false, "Buffer length",       {} },
    { 9,  false, "DTE",                 {} },
    { 10, false, "Reason",              reason_labels },
    { 11, false, "Old state",           dl_state11_labels },
    { 12, false, "New state",           dl_state11_labels },
    { 13, false, "Parameter type",      {} },
    { 14, false, "Cause",               {} },
    { 15, false, "Diagnostic",          {} },
    { 16, false, "Failure reason",      failure_labels },
    { 17, false, "Distance",            {} },
    { 18, false, "Ethernet header",     {} },
    { 19, false, "Hardware status",     {} },
    // Circuit counters are documented as arguments to events 5.3 to 5.5.
    { 0,    true, "Seconds since last zeroed",    {} },
    { 800,  true, "Terminating packets received", {} },
    { 801,  true, "Originating packets sent",     {} },
    { 802,  true, "Terminating congestion loss",  {} },
    { 805,  true, "Corruption loss",              {} },
    { 810,  true, "Transit packets received",     {} },
    { 811,  true, "Transit packets sent",         {} },
    { 812,  true, "Transit congestion loss",      {} },
    { 820,  true, "Circuit down",                 {} },
    { 821,  true, "Initialization failure",       {} },
    { 900,  true, "Peak adjacencies",             {} },
    { 1000, true, "Bytes received",               {} },
    { 1001, true, "Bytes sent",                   {} },
    { 1010, true, "Data blocks received",         {} },
    { 1011, true, "Data blocks sent",             {} },
    { 1020, true, "Data errors inbound",          in_err_labels },
    { 1021, true, "Data errors outbound",         out_err_labels },
    { 1030, true, "Remote reply timeouts",        {} },
    { 1031, true, "Local reply timeouts",         {} },
    { 1040, true, "Remote buffer errors",         remote_buf_labels },
    { 1041, true, "Local buffer errors",          local_buf_labels },
    { 1050, true, "Selection intervals elapsed",  {} },
    { 1051, true, "Selection timeouts",           sel_timeout_labels },
    { 1065, true, "User buffer unavailable",      {} },
    { 1240, true, "Locally initiated resets",     {} },
    { 1241, true, "Remotely initiated resets",    {} },
    { 1242, true, "Network initiated resets",     {} },
    { 3900, true, "Seconds since last circuit up", {} },
};

// A DTE event names only the DTE.
constexpr ParamDef dte_params[] = {
    { 9, false, "DTE", {} },
};

// ------------------------------------------------------ class 6, physical

constexpr const char *const off_on_labels[] = { "Off", "On" };

constexpr ParamDef physical_params[] = {
    { 0, false, "Device register", {} },
    { 1, false, "New state",       off_on_labels },
};

// ------------------------------------------- classes 33 and 34, DECnet/E

constexpr const char *const access_labels[] = { "Local", "Remote" };

constexpr const char *const function_labels[] = {
    "Illegal", "Open/read", "Open/write", "Rename", "Delete",
    "Reserved", "Directory", "Submit", "Execute"
};

constexpr ParamDef rsts_app_params[] = {
    { 0, false, "Access",         access_labels },
    { 1, false, "Function",       function_labels },
    { 3, false, "Remote node",    {}, Style::node },
    { 4, false, "Remote process", {} },
    { 5, false, "Local process",  {} },
    { 6, false, "User",           {} },
    { 7, false, "Password",       password_labels },
    { 8, false, "Account",        {} },
    { 9, false, "File accessed",  {} },
};

constexpr const char *const rsts_session_reason_labels[] = {
    "I/O error on Object database",
    "Spawn Directive failed",
    "Unknown Object identification"
};

constexpr ParamDef rsts_session_params[] = {
    { 0, false, "Reason",              rsts_session_reason_labels },
    { 3, false, "Source node",         {}, Style::node },
    { 4, false, "Source process",      {} },
    { 5, false, "Destination process", {} },
    { 6, false, "User",                {} },
    { 7, false, "Password",            password_labels },
    { 8, false, "Account",             {} },
};

// --------------------------------------------------------- the catalogue

constexpr EventDef all_events[] = {
    // Class 0, network management.
    { { 0,  0 }, "Event records lost",            L::info,    netman_params },
    { { 0,  1 }, "Automatic node counters",       L::info,    node_counters },
    { { 0,  2 }, "Automatic line counters",       L::info,    netman_params },
    { { 0,  3 }, "Automatic service",             L::info,    netman_params },
    { { 0,  4 }, "Line counters zeroed",          L::info,    netman_params },
    { { 0,  5 }, "Node counters zeroed",          L::info,    node_counters },
    { { 0,  6 }, "Passive loopback",              L::info,    netman_params },
    { { 0,  7 }, "Aborted service request",       L::info,    netman_params },
    { { 0,  8 }, "Automatic counters",            L::info,    netman_params },
    { { 0,  9 }, "Counters zeroed",               L::info,    netman_params },

    // Class 2, session control.
    { { 2,  0 }, "Local node state change",       L::info,    session_params },
    { { 2,  1 }, "Access control reject",         L::warning, session_params },

    // Class 3, end communication (NSP).
    { { 3,  0 }, "Invalid message",               L::debug,   node_counters },
    { { 3,  1 }, "Invalid flow control",          L::debug,   node_counters },
    { { 3,  2 }, "Data base reused",              L::info,    node_counters },

    // Class 4, routing.
    { { 4,  0 }, "Aged packet loss",              L::debug,   routing_params },
    { { 4,  1 }, "Node unreachable packet loss",  L::debug,   routing_params },
    { { 4,  2 }, "Node out-of-range packet loss", L::info,    routing_params },
    { { 4,  3 }, "Oversized packet loss",         L::info,    routing_params },
    { { 4,  4 }, "Packet format error",           L::debug,   routing_params },
    { { 4,  5 }, "Partial routing update loss",   L::info,    routing_params },
    { { 4,  6 }, "Verification reject",           L::warning, routing_params },
    { { 4,  7 }, "Circuit down, circuit fault",   L::warning, routing_params },
    { { 4,  8 }, "Circuit down",                  L::warning, routing_params },
    { { 4,  9 }, "Circuit down, operator initiated", L::info, routing_params },
    { { 4, 10 }, "Circuit up",                    L::info,    routing_params },
    { { 4, 11 }, "Initialization failure, line fault",     L::info, routing_params },
    { { 4, 12 }, "Initialization failure, software fault", L::info, routing_params },
    { { 4, 13 }, "Initialization failure, operator fault", L::info, routing_params },
    { { 4, 14 }, "Node reachability change",      L::info,    reach_params },
    { { 4, 15 }, "Adjacency up",                  L::info,    routing_params },
    { { 4, 16 }, "Adjacency rejected",            L::info,    routing_params },
    { { 4, 17 }, "Area reachability change",      L::info,    reach_params },
    { { 4, 18 }, "Adjacency down",                L::warning, routing_params },
    { { 4, 19 }, "Adjacency down, operator initiated", L::info, routing_params },

    // Class 5, data link.
    { { 5,  0 }, "Locally initiated state change",  L::info, datalink_params },
    { { 5,  1 }, "Remotely initiated state change", L::info, datalink_params },
    { { 5,  2 }, "Protocol restart received in maintenance mode",
                                                    L::info, datalink_params },
    { { 5,  3 }, "Send error threshold",            L::info, datalink_params },
    { { 5,  4 }, "Receive error threshold",         L::info, datalink_params },
    { { 5,  5 }, "Select error threshold",          L::info, datalink_params },
    { { 5,  6 }, "Block header format error",       L::info, datalink_params },
    { { 5,  7 }, "Selection address error",         L::info, datalink_params },
    { { 5,  8 }, "Streaming tributary",             L::info, datalink_params },
    { { 5,  9 }, "Local buffer too small",          L::info, datalink_params },
    { { 5, 10 }, "Restart",                         L::info, datalink_params },
    { { 5, 11 }, "State change",                    L::info, datalink_params },
    { { 5, 12 }, "Retransmit maximum exceeded",     L::info, datalink_params },
    { { 5, 13 }, "Initialization failure",          L::info, datalink_params },
    { { 5, 14 }, "Send failed",                     L::info, datalink_params },
    { { 5, 15 }, "Receive failed",                  L::info, datalink_params },
    { { 5, 16 }, "Collision detect check failed",   L::info, datalink_params },
    { { 5, 17 }, "DTE up",                          L::info, dte_params },
    { { 5, 18 }, "DTE down",                        L::info, dte_params },

    // Class 6, physical link.
    { { 6,  0 }, "Data set ready transition",       L::info, physical_params },
    { { 6,  1 }, "Ring indicator transition",       L::info, physical_params },
    { { 6,  2 }, "Unexpected carrier transition",   L::info, physical_params },
    { { 6,  3 }, "Memory access error",             L::info, physical_params },
    { { 6,  4 }, "Communications interface error",  L::info, physical_params },
    { { 6,  5 }, "Performance error",               L::info, physical_params },

    // Classes 33 and 34, DECnet/E.  We never raise these; they are here so
    // that a record from an RSTS/E node reads properly.
    { { 33, 0 }, "Remote file access",              L::info, rsts_app_params },
    { { 34, 0 }, "Object spawned",                  L::info, rsts_session_params },
    { { 34, 1 }, "Object spawn failure",            L::warning, rsts_session_params },

    // Classes 64, 93, 94: RSX.  The manuals give the codes but not the
    // parameters.
    { { 64, 1 }, "Routing database corrupt",        L::info, {} },
    { { 64, 2 }, "Routing database restored",       L::info, {} },
    { { 93, 0 }, "State change",                    L::info, {} },
    { { 94, 0 }, "DCE detected packet error",       L::info, {} },

    // Classes 128 and 353: VMS.  Likewise.
    { { 128,  1 }, "DAP CRC error detected",        L::info, {} },
    { { 128,  2 }, "Duplicate PHASE 2 address error", L::info, {} },
    { { 128,  3 }, "Process created",               L::info, {} },
    { { 128,  4 }, "Process terminated",            L::info, {} },
    { { 353,  5 }, "DECdns clerk unable to communicate with server",
                                                    L::info, {} },
    { { 353, 20 }, "Local DECdns Advertiser error", L::info, {} },
};

}   // namespace

const EventDef *find_event (EventId id)
{
    for (const EventDef &d : all_events)
        if (d.id == id) return &d;
    return nullptr;
}

std::span<const EventDef> known_events ()
{
    return { all_events, std::size (all_events) };
}

}   // namespace decnet::events

// decnet/mop/names.h -- names for the codes in a MOP system ID.
//
// The device, processor and datalink codes, as NCP and PyDECnet's API
// show them.  Port of MOPdevices2, MOPCPUs and MOPdatalinks in
// nicepackets.py.

#ifndef DECNET_MOP_NAMES_H
#define DECNET_MOP_NAMES_H

#include <string>

namespace decnet::mop {

// "DEUNA UNIBUS CSMA/CD communication link", or the number if unknown.
std::string device_name (unsigned code);
// "UNA", the device's short name, or the number if unknown.
std::string device_short_name (unsigned code);
std::string processor_name (unsigned code);
std::string datalink_name (unsigned code);

}   // namespace decnet::mop

#endif  // DECNET_MOP_NAMES_H

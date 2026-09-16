// decnet/datalink/ethernet.h -- Ethernet circuits.
//
// Port of ethernet.py.  Device strings:
//
//   udp:<localport>:<host>:<remoteport>   Ethernet frames in UDP datagrams
//   bridge:<same>                         same as udp
//   tap:/dev/tapN                         TAP device
//   pcap:<interface>                      real interface, if libpcap is
//                                         available
//
// pcap needs CAP_NET_RAW.  It sends with the DECnet address
// AA-00-04-00-xx-xx as source without changing the interface address.

#ifndef DECNET_DATALINK_ETHERNET_H
#define DECNET_DATALINK_ETHERNET_H

#include "decnet/datalink/bc.h"

#include <mutex>

namespace decnet::datalink {

// The parsed --device argument.
struct EthernetDevice {
    enum class Mode { udp, tap, pcap };

    Mode          mode = Mode::udp;
    std::string   destination;      // udp: peer host; tap/pcap: the device
    std::uint16_t dest_port = 0;
    std::uint16_t source_port = 0;

    static EthernetDevice parse (const std::string &device);
    std::string str () const;
};

// Common behaviour: a receive thread that turns frames into work items.
class Ethernet : public BcDatalink {
public:
    Ethernet (Element *owner, std::string name, EthernetDevice dev,
              bool random_address);
    ~Ethernet () override;

    void open () override;
    void close () override;

    // Must be called from each concrete class's destructor.  The receive
    // thread runs derived class code, so it has to be stopped before the
    // derived object is destroyed.
    void shutdown ();

    static std::unique_ptr<Datalink> create (Element *owner,
                                             const std::string &name,
                                             const std::string &device,
                                             bool random_address);

protected:
    // Open and close whatever carries the frames.  Return false if the
    // circuit cannot be brought up, which is logged and not fatal.
    virtual bool start_transport () = 0;
    virtual void stop_transport () = 0;

    // Runs on the receive thread until stopping() becomes true.
    virtual void receive_loop () = 0;

    bool stopping () const noexcept { return stopnow_.load (); }

    EthernetDevice dev_;
    Socket         socket_;

private:
    void run ();

    std::thread       thread_;
    std::atomic<bool> stopnow_ { false };
};

// Frames in UDP datagrams.  Port of ethernet._BridgeEth.
class BridgeEthernet : public Ethernet {
public:
    BridgeEthernet (Element *owner, std::string name, EthernetDevice dev,
                    bool random_address);

    ~BridgeEthernet () override { shutdown (); }

    void send_frame (const Bytes &frame) override;

protected:
    bool start_transport () override;
    void stop_transport () override;
    void receive_loop () override;

private:
    SourceAddress source_;
    HostAddress   dest_;
};

// A real interface via libpcap.  Port of ethernet._PcapEth.
//
// Opened in promiscuous mode, since the DECnet addresses are not the
// interface's own.  Capture is limited to inbound frames so our own
// transmissions are not received.
#if DN_HAVE_PCAP
class PcapEthernet : public Ethernet {
public:
    PcapEthernet (Element *owner, std::string name, EthernetDevice dev,
                  bool random_address);
    ~PcapEthernet () override;

    void send_frame (const Bytes &frame) override;

    // Rebuild the kernel filter when a port's addresses change.
    void filter_changed () override;

    // Hardware address of a named interface, or a null address if it has none
    // or does not exist.
    static Macaddr interface_address (const std::string &ifname);

protected:
    bool start_transport () override;
    void stop_transport () override;
    void receive_loop () override;

private:
    void install_filter ();

    // pcap_t, kept as void * so that libpcap's headers do not have to be
    // included by everything that includes this one.
    void       *pcap_ = nullptr;
    std::mutex  pcap_mutex_;      // send and filter changes race the reader
};
#endif  // DN_HAVE_PCAP

// A TAP device.  Port of ethernet._TapEth.
class TapEthernet : public Ethernet {
public:
    using Ethernet::Ethernet;
    ~TapEthernet () override { shutdown (); }

    void send_frame (const Bytes &frame) override;

protected:
    bool start_transport () override;
    void stop_transport () override;
    void receive_loop () override;
};

}   // namespace decnet::datalink

#endif  // DECNET_DATALINK_ETHERNET_H

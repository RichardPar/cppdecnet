// decnet/datalink/ethernet.h -- Ethernet circuits.
//
// Port of ethernet.py.  Three ways to reach a LAN, matching pydecnet's
// device syntax:
//
//   udp:<localport>:<host>:<remoteport>   Ethernet frames in UDP datagrams,
//   bridge:<same>                         to Johnny Billquist's bridge or
//                                         directly to another node
//   tap:/dev/tapN                         a TAP device
//   pcap:<interface>                      a real interface, if libpcap is
//                                         available (make features says)
//
// The UDP form is the one that needs no privilege and no kernel device, so
// it is what the tests use: two nodes pointed at each other are a
// degenerate two station LAN, which is enough to exercise everything the
// routing sublayer above does.
//
// The pcap form is the one that reaches real hardware.  It needs
// CAP_NET_RAW, and it does not change the interface's hardware address --
// it cannot -- so it sends frames whose source is the DECnet address
// AA-00-04-00-xx-xx while the interface keeps its own.  Everything on the
// segment is captured and filtered, in the kernel where it can be and in
// software where it cannot.

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

    // Every concrete Ethernet class must call this from its own
    // destructor.
    //
    // The receive thread runs receive_loop(), which is the derived class's
    // code operating on the derived class's members.  By the time the base
    // destructor runs, that object no longer exists -- so stopping the
    // thread there would be far too late, and calling close() there reaches
    // a pure virtual.  Stopping it in the derived destructor, while the
    // derived object is still whole, is the only correct place.
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

// A real interface, through libpcap.  Port of ethernet._PcapEth.
//
// Two things about this are worth knowing.  The interface is opened
// promiscuously, because the addresses DECnet answers to -- the derived
// AA-00-04-00-xx-xx and the routing multicasts -- are not the ones the
// interface was configured with, so the card would filter our own traffic
// out.  And capture is restricted to inbound frames, because otherwise
// every frame we transmit is handed straight back to us.
#if DN_HAVE_PCAP
class PcapEthernet : public Ethernet {
public:
    PcapEthernet (Element *owner, std::string name, EthernetDevice dev,
                  bool random_address);
    ~PcapEthernet () override;

    void send_frame (const Bytes &frame) override;

    // Rebuild the kernel filter when a port's addresses change.
    void filter_changed () override;

    // The hardware address of a named interface, or a null address if it
    // has none or does not exist.  Public because it is worth testing on
    // its own: everything else here needs a live capture handle.
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

#include "decnet/datalink/datalink.h"

#include "decnet/common/logging.h"
#include "decnet/config.h"
#include "decnet/datalink/ethernet.h"
#include "decnet/datalink/ddcmp.h"
#include "decnet/datalink/multinet.h"
#include "decnet/node.h"

namespace decnet::datalink {

// ------------------------------------------------------------------ Port

Port::Port (Datalink *dl, Element *owner) noexcept
    : Element (dl), datalink_ (dl), owner_ (owner)
{
}

void Port::open ()
{
    DN_TRACE ("datalink {} open port", datalink_->name ());
    if (node ()) node ()->add_work (std::make_unique<Start> (datalink_));
}

void Port::close ()
{
    DN_TRACE ("datalink {} close port", datalink_->name ());
    if (node ()) node ()->add_work (std::make_unique<Stop> (datalink_));
}

void Port::restart ()
{
    DN_TRACE ("datalink {} restart port", datalink_->name ());
    if (node ()) node ()->add_work (std::make_unique<Restart> (datalink_));
}

// -------------------------------------------------------------- Datalink

Datalink::Datalink (Element *owner, std::string name) noexcept
    : Element (owner), name_ (std::move (name))
{
}

// --------------------------------------------------------- DatalinkLayer

std::unique_ptr<Datalink> DatalinkLayer::create (Element *owner,
                                                 const CircuitConfig &c)
{
    try {
        if (c.type == "Multinet")
            return Multinet::create (owner, c.name, c.device);
        if (c.type == "Ethernet")
            return Ethernet::create (owner, c.name, c.device,
                                     c.random_address);
        if (c.type == "DDCMP")
            return Ddcmp::create (owner, c.name, c.device);
        // PORT: GRE joins this switch as it is ported.
        DN_ERROR ("invalid datalink type {} for circuit {}", c.type, c.name);
    } catch (const std::exception &e) {
        DN_ERROR ("error initializing {} datalink {}: {}",
                  c.type, c.name, e.what ());
    }
    return nullptr;
}

DatalinkLayer::DatalinkLayer (Element *owner, const Config &config)
    : Element (owner)
{
    DN_DEBUG ("initializing data link layer");
    for (const CircuitConfig &c : config.circuits ()) {
        std::unique_ptr<Datalink> dl = create (this, c);
        if (!dl) continue;      // create() has already logged why
        Datalink *raw = dl.get ();
        circuits_[c.name] = std::move (dl);
        order_.push_back (raw);
        DN_DEBUG ("initialized {} datalink {}", c.type, c.name);
    }
}

DatalinkLayer::~DatalinkLayer () = default;

Datalink *DatalinkLayer::circuit (const std::string &name) const
{
    // Circuit names are canonicalised to upper case when the configuration
    // is read, so canonicalise the lookup too rather than making every
    // caller remember.  pydecnet does the same thing at its NICE entry
    // point, with an explicit .upper ().
    std::string key;
    try {
        key = circname (name);
    } catch (const std::invalid_argument &) {
        return nullptr;
    }
    auto it = circuits_.find (key);
    return it == circuits_.end () ? nullptr : it->second.get ();
}

void DatalinkLayer::start ()
{
    DN_DEBUG ("starting datalink layer");
    for (Datalink *c : order_) {
        try {
            c->open ();
            DN_DEBUG ("started datalink {}", c->name ());
        } catch (const std::exception &e) {
            DN_ERROR ("error starting datalink {}: {}", c->name (), e.what ());
        }
    }
}

void DatalinkLayer::stop ()
{
    DN_DEBUG ("stopping datalink layer");
    for (Datalink *c : order_) {
        try {
            c->close ();
            DN_DEBUG ("stopped datalink {}", c->name ());
        } catch (const std::exception &e) {
            DN_ERROR ("error stopping datalink {}: {}", c->name (), e.what ());
        }
    }
}

}   // namespace decnet::datalink

#include "decnet/common/element.h"

#include "decnet/node.h"

namespace decnet {

void Work::dispatch ()
{
    if (owner_) owner_->dispatch (*this);
}

void Element::post (WorkPtr w)
{
    if (node_) node_->add_work (std::move (w));
}

}   // namespace decnet

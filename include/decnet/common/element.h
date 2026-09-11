// decnet/common/element.h -- the layer object base class.
//
// Port of common.Element: every protocol layer object knows its parent and,
// through it, the node it belongs to, which is how the Python reaches the
// work queue, the timer wheel and the event logger from anywhere.

#ifndef DECNET_COMMON_ELEMENT_H
#define DECNET_COMMON_ELEMENT_H

#include "decnet/common/work.h"

namespace decnet {

class Node;

class Element {
public:
    explicit Element (Element *parent) noexcept
        : parent_ (parent), node_ (parent ? parent->node_ : nullptr) {}
    virtual ~Element () = default;

    Element *parent () const noexcept { return parent_; }
    Node    *node   () const noexcept { return node_; }

    // Handle one work item addressed to this element.  Layers override it;
    // state machine based layers forward to StateMachine::dispatch.
    virtual void dispatch (Work &w) = 0;

    // Queue work to this element's node from any thread.
    void post (WorkPtr w);

protected:
    // Only Node uses this, to make itself the root of the tree.
    void set_node (Node *n) noexcept { node_ = n; }

private:
    Element *parent_;
    Node    *node_;
};

}   // namespace decnet

#endif  // DECNET_COMMON_ELEMENT_H

// decnet/common/backoff.h -- binary exponential backoff.
//
// Port of common.Backoff.  Used for connection retries and datalink
// restart holdoff, so that a peer that is down is not hammered.

#ifndef DECNET_COMMON_BACKOFF_H
#define DECNET_COMMON_BACKOFF_H

#include "decnet/common/exceptions.h"

namespace decnet {

class Backoff {
public:
    // Like the Python constructor: one argument is the upper bound with a
    // lower bound of 1, two arguments are the bounds.
    explicit Backoff (double high) : Backoff (1.0, high) {}

    Backoff (double low, double high)
        : low_ (low), high_ (high), current_ (low)
    {
        if (!(low > 0 && low < high))
            throw InternalError ("bad backoff bounds");
    }

    // The next delay, doubling each time up to the ceiling.
    double next () noexcept
    {
        double ret = current_;
        current_ = (current_ * 2 > high_) ? high_ : current_ * 2;
        ++tries_;
        return ret;
    }

    void reset () noexcept { current_ = low_; tries_ = 0; }

    unsigned tries () const noexcept { return tries_; }

private:
    double   low_, high_, current_;
    unsigned tries_ = 0;
};

}   // namespace decnet

#endif  // DECNET_COMMON_BACKOFF_H

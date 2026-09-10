// decnet/common/exceptions.h -- the DNAException hierarchy from common.py.

#ifndef DECNET_COMMON_EXCEPTIONS_H
#define DECNET_COMMON_EXCEPTIONS_H

#include <stdexcept>
#include <string>

namespace decnet {

struct DNAException : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct InternalError : DNAException { using DNAException::DNAException; };

// Everything a packet decoder can complain about.  The routing and NSP
// layers distinguish these when deciding whether to count a format error
// event or simply drop the packet.
struct DecodeError   : DNAException { using DNAException::DNAException; };
struct WrongValue    : DecodeError  { using DecodeError::DecodeError; };
struct ExtraData     : DecodeError  { using DecodeError::DecodeError; };
struct MissingData   : DecodeError  { using DecodeError::DecodeError; };
struct FieldOverflow : DecodeError  { using DecodeError::DecodeError; };
struct InvalidTag    : DecodeError  { using DecodeError::DecodeError; };

}   // namespace decnet

#endif  // DECNET_COMMON_EXCEPTIONS_H

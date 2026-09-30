// decnet/dap/fal.h -- File Access Listener: the server side of DAP.
//
// Port of PyDECnet's applications/fal.py, extended with create, erase and
// rename.  One FalServer serves one logical link: it answers directory
// listings and reads, and, if allowed, writes, deletes and renames, all
// within a root directory.
//
// File specifications may be Unix ("sub/file.txt") or VMS style
// ("DEV:[SUB]FILE.TXT;1"): device and version are ignored, directories
// map to subdirectories of the root, and names match case-insensitively
// when there is no exact match.  Nothing outside the root is reachable,
// through ".." or through a symbolic link.
//
// Files are reported as PyDECnet reports them -- Stream_LF (fixed 512 for
// DAP before version 7), with the end of file block and first free byte
// giving the exact size -- so the same requesters work with both.
//
// PORT: no access control.  The server runs as whoever started it; the
// connect's user name and password are not checked.

#ifndef DECNET_DAP_FAL_H
#define DECNET_DAP_FAL_H

#include "decnet/dap/messages.h"

#include <optional>
#include <string>
#include <vector>

namespace decnet::dap {

// Where DAP messages come from and go to: a logical link, in practice.
class Transport {
public:
    virtual ~Transport () = default;

    // The next session control message, or nothing once the link is gone.
    virtual std::optional<Bytes> recv () = 0;

    virtual void send (const Bytes &msg) = 0;
};

struct FalOptions {
    std::string root;               // the directory served
    bool        writable = false;   // allow create, erase and rename
    bool        trace = false;      // log each message to stderr
    // What the Configuration message says we are: VAX/VMS and RMS-32.
    // PyDECnet says user-defined and ULTRIX-32 (192 and 13), but VMS COPY
    // will not send a file without carriage control, a binary file, to a
    // file system it thinks is ULTRIX's.
    std::uint8_t ostype = 7;
    std::uint8_t filesys = 3;
};

class FalServer {
public:
    FalServer (Transport &t, FalOptions o);

    // Serve until the requester disconnects.
    void run ();

    // A file specification as a path under the root, split into
    // components, or nothing if it tries to leave the root.  Wildcards are
    // kept.  Exposed for the tests.
    static std::optional<std::vector<std::string>> split_spec (
        const std::string &spec);

private:
    struct Found {
        std::string path;           // full path
        std::string rel;            // relative to the root, "" for the root
        std::string name;           // last component
        bool        is_dir = false;
    };

    std::optional<Message> next ();
    void send (const Message &m);
    void status (unsigned mac, unsigned mic);

    void access (const Access &a);
    void directory (const Access &a);
    void get (const Access &a);
    void create (const Access &a);
    void erase (const Access &a);
    void rename (const Access &a);

    // Files matching a specification.  Sends the error and returns
    // nothing if the directory does not exist or the spec is not allowed.
    std::optional<std::vector<Found>> find (const std::string &spec,
                                            bool want_dirs);

    // Where a new file named by spec would go, or nothing (error sent).
    std::optional<Found> new_file (const std::string &spec);

    // The description of one file: its Name, Attributes, Date and
    // Protection messages, as the Access's display bits ask.
    std::vector<Message> describe (const Found &f, const Access &a,
                                   bool dirop, bool wild);

    bool inside_root (const std::string &path) const;

    // A file's full specification, as a Name message.
    static Name spec_name (const Found &f);

    Transport              &t_;
    FalOptions              o_;
    std::string             root_real_;
    Config                  remote_;
    bool                    v7_ = false;
    std::vector<Message>    pending_;
    std::optional<Attributes> attr_;    // sent before the Access
};

}   // namespace decnet::dap

#endif  // DECNET_DAP_FAL_H

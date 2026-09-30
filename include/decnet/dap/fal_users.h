// decnet/dap/fal_users.h -- who may use the FAL, and where.
//
// dnfal checks the user name and password in a connect against a file of
// its own rather than the system's accounts, so decnetd does not need to
// run as root.  One line per user:
//
//     # user    password hash                 directory       access
//     RICHARD   $y$j9T$...                    richard         rw
//     GUEST     -                             pub             ro
//     *         -                             pub             ro
//
//   - Names match without regard to case, since VMS sends them in upper
//     case.
//   - The hash is anything crypt(3) accepts ("openssl passwd -6",
//     "mkpasswd", or "dnfal --hash").  "-" means no password.
//   - A relative directory is under the root dnfal was given.
//   - "*" is for connections that name no user.  Without it, they are
//     refused.
//
// Proxy lines let users on other nodes in without a password, as
// DECnet-VAX's proxy database does:
//
//     proxy  VMSNOD::RICHARD  richard      # that user, from that node
//     proxy  VMSNOD::*        guest        # anyone from that node
//     proxy  *::SYSTEM        -            # never: "-" refuses
//
// A node is a name or an address.  A proxy connect (no password, the proxy
// flag set) uses the most specific match: node and user, node, user, then
// "*::*".  With none it falls back to the "*" entry, as VMS falls back to
// its default account.  The name a proxy connect gives says who the user
// is at the far node; it is never taken as a user in this file.
//
// VMS upper-cases a password typed without quotes, so a password that
// does not match as sent is tried again in lower case.

#ifndef DECNET_DAP_FAL_USERS_H
#define DECNET_DAP_FAL_USERS_H

#include <optional>
#include <string>
#include <vector>

namespace decnet::dap {

struct FalUser {
    std::string name;               // as in the file; "*" for anonymous
    std::string hash;               // "-" for none
    std::string root;               // absolute
    bool        writable = false;
};

// Who is asking, from the connect.
struct Requester {
    std::string user, password;
    bool        proxy = false;
    std::string source_user;        // the source end user, "[g,u]NAME"
    std::string node_name;          // the requesting node, if known
    std::string node_address;       // "1.2"
};

class FalUsers {
public:
    // Read a user file.  Throws std::runtime_error naming the line of the
    // first mistake.
    static FalUsers load (const std::string &path, const std::string &base);
    static FalUsers parse (const std::string &text, const std::string &base,
                           const std::string &source = "users");

    // The entry a connect's user name and password give, or nothing.
    std::optional<FalUser> authenticate (const std::string &user,
                                         const std::string &password) const;
    std::optional<FalUser> authenticate (const Requester &r) const;

    std::size_t size () const noexcept { return users_.size (); }

private:
    struct Proxy {
        std::string node, user;     // upper case; "*" for any
        std::string local;          // user entry; "-" to refuse
    };

    const FalUser *find (const std::string &name) const;

    std::vector<FalUser> users_;
    std::vector<Proxy>   proxies_;
};

// Does password match a crypt(3) hash?
bool check_password (const std::string &password, const std::string &hash);

// A new hash for password, with the system's preferred method.
std::string hash_password (const std::string &password);

}   // namespace decnet::dap

#endif  // DECNET_DAP_FAL_USERS_H

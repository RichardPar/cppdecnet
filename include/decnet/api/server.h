// decnet/api/server.h -- the PyDECnet API over a Unix socket.
//
// Port of apiserver.py and of session.py's ApiConnector.  Clients send one
// JSON object per line and get one back per reply or event, in PyDECnet's
// format, so PyDECnet's connectors.py and async_connectors.py work against
// this server unchanged.
//
//     {}                                          list of systems
//     {"api":"session","type":"connect","dest":"MIM","remuser":17,"tag":1}
//     {"api":"session","type":"data","handle":3,"data":"..."}
//
// Implemented: the system list; the "session" API (connect, bind,
// accept, reject, data, interrupt, disconnect, abort); and the "mop" API
// (get, sysid, counters, loop), with a "dest" on sysid to ask one station,
// and node names accepted as stations.  A node with MOP but no routing
// still has the API, with only "mop".
//
// PORT: the node, nsp, routing and ncp APIs, and MOP's console carrier.
//
// Threading: an accept thread, and one reader thread per client.  Readers
// parse requests and post them to the node thread, which does all the work
// and queues replies, which a writer thread per client sends.  Writes never
// block the node thread; a client that falls 64 MB behind is disconnected.

#ifndef DECNET_API_SERVER_H
#define DECNET_API_SERVER_H

#include "decnet/common/element.h"
#include "decnet/common/socket.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace decnet {

class Node;

namespace api {

class Client;

class Server : public Element {
public:
    Server (Node *node, std::string path, unsigned mode);
    ~Server () override;

    // Bind the socket and start accepting.  False if the socket could not be
    // created, for instance because another server is using the path.
    bool start ();

    // Stop accepting, drop every client and remove the socket.  Call while
    // the node loop still runs: clients post their clean up to it.
    void stop ();

    // Nothing is addressed to this element; the reader threads post
    // CallbackWork.  Required by Element.
    void dispatch (Work &) override {}

    const std::string &path () const noexcept { return path_; }

    // Number of connected clients.  For tests.
    std::size_t client_count ();

private:
    void run ();                        // the accept loop

    // Join and forget clients whose reader has finished.
    void reap ();

    Node             *node_;
    std::string       path_;
    unsigned          mode_;
    Socket            listener_;
    std::thread       thread_;
    std::atomic<bool> stopping_ { false };

    std::mutex                           clients_m_;
    std::vector<std::shared_ptr<Client>> clients_;
};

}   // namespace api
}   // namespace decnet

#endif  // DECNET_API_SERVER_H

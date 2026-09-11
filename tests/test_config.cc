// Port of tests/test_config.py: reading a pydecnet configuration file.

#include "harness.h"

#include "decnet/config.h"

using namespace decnet;

DN_TEST (config, tokenizing)
{
    auto w = split_config_line ("circuit eth-0 Ethernet tap:/dev/tap0  # comment");
    DN_ASSERT_EQ (w.size (), 4u);
    DN_ASSERT_EQ (w[0], std::string ("circuit"));
    DN_ASSERT_EQ (w[3], std::string ("tap:/dev/tap0"));

    auto q = split_config_line ("system --ident \"Sample config file\"");
    DN_ASSERT_EQ (q.size (), 3u);
    DN_ASSERT_EQ (q[2], std::string ("Sample config file"));

    DN_ASSERT (split_config_line ("   # just a comment").empty ());
    DN_ASSERT (split_config_line ("").empty ());
}

DN_TEST (config, circuit_line)
{
    Config c = Config::from_string (
        "circuit eth-0 Ethernet tap:/dev/tap0 --console Plugh --random-address\n"
        "circuit dmc-0 DDCMP tcp:12345:localhost:32154 --cost 3\n");
    DN_ASSERT_EQ (c.circuits ().size (), 2u);

    // Circuit and node names are canonicalised to upper case at read-in,
    // as common.circname and common.nodename do.
    const auto &e = c.circuits ()[0];
    DN_ASSERT_EQ (e.name, std::string ("ETH-0"));
    DN_ASSERT_EQ (e.type, std::string ("Ethernet"));
    DN_ASSERT_EQ (e.device, std::string ("tap:/dev/tap0"));
    DN_ASSERT_EQ (e.console, std::string ("Plugh"));
    DN_ASSERT (e.random_address);
    DN_ASSERT (!e.mop);

    DN_ASSERT_EQ (c.circuits ()[1].cost, 3u);
}

DN_TEST (config, routing_and_node_lines)
{
    Config c = Config::from_string (
        "routing 9.54 --type l2router\n"
        "node 9.54 SAMPLE\n"
        "node 9.55 OTHER --inbound-verification secret\n"
        "system --ident \"Sample PyDECnet configuration\"\n");

    DN_ASSERT (c.routing ().has_value ());
    DN_ASSERT_EQ (c.routing ()->id, Nodeid::parse ("9.54"));
    DN_ASSERT (c.routing ()->type == NodeType::l2router);

    DN_ASSERT_EQ (c.nodes ().size (), 2u);
    DN_ASSERT_EQ (c.nodes ()[0].name, std::string ("SAMPLE"));
    DN_ASSERT_EQ (c.nodes ()[1].inbound_verification, std::string ("secret"));
    DN_ASSERT_EQ (c.identification (),
                  std::string ("Sample PyDECnet configuration"));
}

DN_TEST (config, option_equals_form)
{
    Config c = Config::from_string ("routing 1.1 --type=endnode --maxhops=8\n");
    DN_ASSERT (c.routing ()->type == NodeType::endnode);
    DN_ASSERT_EQ (c.routing ()->maxhops, 8u);
}

DN_TEST (config, unknown_commands_are_kept_not_rejected)
{
    // Layers that are not ported yet must not make a real config file fail.
    // This used to use "http" as its example, which stopped being one when
    // the monitoring server landed and claimed the line.
    Config c = Config::from_string ("api /tmp/decnet.sock\n"
                                    "bridge br-0 --pcap eth0\n");
    DN_ASSERT_EQ (c.unhandled ().size (), 2u);
    DN_ASSERT_EQ (c.unhandled ()[0].command, std::string ("api"));
}

DN_TEST (config, names_are_validated)
{
    // A node name is at most six characters and must contain a letter; a
    // real pydecnet node rejects anything else, so accepting it here would
    // only defer the failure.
    DN_ASSERT_THROWS (std::runtime_error,
                      Config::from_string ("node 1.1 TOOLONGNAME\n"));
    DN_ASSERT_THROWS (std::runtime_error,
                      Config::from_string ("node 1.1 12345\n"));
    DN_ASSERT_THROWS (std::runtime_error,
                      Config::from_string ("node 1.1 BAD_NAME\n"));
    DN_ASSERT_EQ (Config::from_string ("node 1.1 sample\n").nodes ()[0].name,
                  std::string ("SAMPLE"));
}

DN_TEST (config, errors_name_the_line)
{
    bool caught = false;
    try {
        Config::from_string ("routing 9.54 --type nosuchtype\n", "test.conf");
    } catch (const std::exception &e) {
        caught = true;
        DN_ASSERT (std::string (e.what ()).find ("test.conf:1") == 0);
    }
    DN_ASSERT (caught);
}

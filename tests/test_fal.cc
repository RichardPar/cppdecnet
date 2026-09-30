// Tests for the FAL server, driven through a scripted transport: the
// requester's messages are queued up front and the server's answers
// collected.

#include "harness.h"

#include "decnet/dap/fal.h"

#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>

#include <sys/stat.h>
#include <unistd.h>

using namespace decnet;
using namespace decnet::dap;

namespace {

class Script : public Transport {
public:
    std::deque<Bytes>    in;
    std::vector<Message> out;

    std::optional<Bytes> recv () override
    {
        if (in.empty ()) return std::nullopt;
        Bytes b = std::move (in.front ());
        in.pop_front ();
        return b;
    }
    void send (const Bytes &msg) override
    {
        for (Message &m : decode (msg)) out.push_back (std::move (m));
    }

    Script &operator<< (const Message &m) { in.push_back (encode (m)); return *this; }

    // The types of everything sent, "Config Attributes Ack ...".
    std::string types () const
    {
        std::string s;
        for (const Message &m : out) {
            if (!s.empty ()) s += ' ';
            s += std::to_string (type_of (m));
        }
        return s;
    }

    template <typename T> std::vector<T> all () const
    {
        std::vector<T> v;
        for (const Message &m : out)
            if (auto *p = std::get_if<T> (&m)) v.push_back (*p);
        return v;
    }
};

// A directory tree for one test, removed afterwards.
struct Tree {
    std::string root;
    Tree ()
    {
        char tmpl[] = "/tmp/dnfal-test-XXXXXX";
        root = ::mkdtemp (tmpl);
    }
    ~Tree () { if (std::system (("rm -rf '" + root + "'").c_str ())) {} }
    void file (const std::string &name, const std::string &data) const
    {
        std::ofstream (root + "/" + name, std::ios::binary) << data;
    }
    void dir (const std::string &name) const
    { ::mkdir ((root + "/" + name).c_str (), 0755); }
    std::string read (const std::string &name) const
    {
        std::ifstream f (root + "/" + name, std::ios::binary);
        std::stringstream s;
        s << f.rdbuf ();
        return s.str ();
    }
    bool exists (const std::string &name) const
    {
        struct stat st;
        return ::lstat ((root + "/" + name).c_str (), &st) == 0;
    }
};

Config v7 ()
{
    Config c;
    c.bufsiz = 4096;
    return c;
}

Access access (std::uint8_t fn, const std::string &spec)
{
    Access a;
    a.accfunc = fn;
    a.filespec = spec;
    a.display.set (Access::d_main);
    return a;
}

Control control (std::uint8_t fn)
{
    Control c;
    c.ctlfunc = fn;
    if (fn == Control::get || fn == Control::put) {
        c.menu.set (Control::m_rac);
        c.rac = Control::rb_seqf;
    }
    return c;
}

AccessComplete complete (std::uint8_t fn)
{
    AccessComplete a;
    a.cmpfunc = fn;
    return a;
}

Bytes bytes_of (const std::string &s) { return Bytes (s.begin (), s.end ()); }

// The last status sent, as "maccode:miccode" in octal.
std::string last_status (const Script &s)
{
    auto st = s.all<Status> ();
    if (st.empty ()) return "none";
    char buf[32];
    std::snprintf (buf, sizeof buf, "%o:%o", st.back ().maccode,
                   st.back ().miccode);
    return buf;
}

}   // namespace

// ------------------------------------------------------------- file names

DN_TEST (fal, specs_map_under_the_root)
{
    using V = std::vector<std::string>;
    auto s = [] (const char *spec) { return *FalServer::split_spec (spec); };
    DN_ASSERT_EQ (s ("hello.txt"), (V { "hello.txt" }));
    DN_ASSERT_EQ (s ("/sub/x.dat"), (V { "sub", "x.dat" }));
    DN_ASSERT_EQ (s ("DKA0:[SUB.DEEP]FILE.TXT;3"), (V { "SUB", "DEEP", "FILE.TXT" }));
    DN_ASSERT_EQ (s ("[000000]LOGIN.COM"), (V { "LOGIN.COM" }));
    DN_ASSERT_EQ (s ("[SUB]*.*;*"), (V { "SUB", "*" }));
    DN_ASSERT_EQ (s ("README."), (V { "README" }));
    DN_ASSERT_EQ (s ("NODE::sub/"), (V { "sub", "" }));
    DN_ASSERT_EQ (s (""), (V { "" }));

    DN_ASSERT (!FalServer::split_spec ("../etc/passwd"));
    DN_ASSERT (!FalServer::split_spec ("sub/../../x"));
    DN_ASSERT (!FalServer::split_spec ("[-]X.TXT"));
    DN_ASSERT (!FalServer::split_spec ("[SUB]a/b"));
}

// -------------------------------------------------------------- reading

DN_TEST (fal, configuration_comes_first)
{
    Tree t;
    Script s;
    s << Ack {};
    FalServer (s, { t.root }).run ();
    DN_ASSERT_EQ (last_status (s), std::string ("12:6"));   // out of sync

    Script ok;
    ok << v7 ();
    FalServer (ok, { t.root }).run ();
    DN_ASSERT_EQ (ok.types (), std::string ("1"));
    auto c = ok.all<Config> ();
    DN_ASSERT (c[0].syscap[Config::cap_dir]);
    DN_ASSERT (!c[0].syscap[Config::cap_delete]);   // read only
}

DN_TEST (fal, directory_lists_like_pydecnet)
{
    Tree t;
    t.file ("b.txt", "bee");
    t.file ("a.txt", std::string (600, 'x'));
    t.file (".hidden", "no");
    t.dir ("sub");
    Script s;
    Access a = access (Access::directory, "*");
    a.display.set (Access::d_date).set (Access::d_fprot);
    s << v7 () << a;
    FalServer (s, { t.root }).run ();

    // Config; volume and directory names; then per entry: name,
    // attributes, date, protection, acknowledge; then access complete.
    auto names = s.all<Name> ();
    DN_ASSERT_EQ (names.size (), 5u);
    DN_ASSERT (names[0].nametype[Name::volname]);
    DN_ASSERT_EQ (names[1].namespec, std::string ("/"));
    DN_ASSERT_EQ (names[2].namespec, std::string ("a.txt"));
    DN_ASSERT_EQ (names[3].namespec, std::string ("b.txt"));
    DN_ASSERT_EQ (names[4].namespec, std::string ("sub/"));
    auto at = s.all<Attributes> ();
    DN_ASSERT_EQ (at.size (), 3u);
    DN_ASSERT_EQ (*at[0].size (), 600u);
    DN_ASSERT_EQ (at[0].rfm, Attributes::fb_slf);
    DN_ASSERT_EQ (s.all<DateTime> ().size (), 3u);
    DN_ASSERT_EQ (s.all<Protection> ().size (), 3u);
    DN_ASSERT_EQ (s.all<Ack> ().size (), 3u);
    DN_ASSERT (std::holds_alternative<AccessComplete> (s.out.back ()));
}

DN_TEST (fal, a_missing_directory_or_file_is_reported)
{
    Tree t;
    Script s;
    s << v7 () << access (Access::directory, "[NOSUCH]*.*")
      << access (Access::open, "nosuch.txt");
    FalServer (s, { t.root }).run ();
    auto st = s.all<Status> ();
    DN_ASSERT_EQ (st.size (), 2u);
    DN_ASSERT_EQ (st[0].miccode, 040u);         // directory not found
    DN_ASSERT_EQ (st[1].miccode, 062u);         // file not found
}

DN_TEST (fal, get_sends_blocks_then_end_of_file)
{
    Tree t;
    std::string data (700, 'q');
    t.file ("f.bin", data);
    Script s;
    s << v7 () << access (Access::open, "F.BIN") << control (Control::connect)
      << control (Control::get) << complete (AccessComplete::close);
    FalServer (s, { t.root }).run ();

    auto d = s.all<Data> ();
    DN_ASSERT_EQ (d.size (), 2u);
    DN_ASSERT_EQ (d[0].payload.size (), 512u);
    DN_ASSERT_EQ (d[1].payload.size (), 512u);             // padded
    DN_ASSERT_EQ (*s.all<Attributes> ()[0].size (), 700u);  // the real size
    auto st = s.all<Status> ();
    DN_ASSERT_EQ (st.size (), 1u);
    DN_ASSERT (st[0].eof ());
    DN_ASSERT (std::holds_alternative<AccessComplete> (s.out.back ()));
}

DN_TEST (fal, get_as_text_sends_a_record_per_line)
{
    Tree t;
    t.file ("t.txt", "one\ntwo\nthree");
    Attributes want;
    want.menu.set (Attributes::m_rfm).set (Attributes::m_rat);
    want.rfm = Attributes::fb_var;
    want.rat.set (Attributes::rat_cr);
    Script s;
    s << v7 () << want << access (Access::open, "t.txt")
      << control (Control::connect) << control (Control::get)
      << complete (AccessComplete::close);
    FalServer (s, { t.root }).run ();
    auto d = s.all<Data> ();
    DN_ASSERT_EQ (d.size (), 3u);
    DN_ASSERT_EQ (d[0].payload, bytes_of ("one"));
    DN_ASSERT_EQ (d[2].payload, bytes_of ("three"));
}

DN_TEST (fal, nothing_outside_the_root_is_reachable)
{
    Tree t;
    Tree outside;
    outside.file ("secret", "s");
    DN_ASSERT (::symlink ((outside.root + "/secret").c_str (),
                          (t.root + "/link").c_str ()) == 0);
    DN_ASSERT (::symlink (outside.root.c_str (),
                          (t.root + "/dirlink").c_str ()) == 0);
    Script s;
    s << v7 () << access (Access::open, "../x") << access (Access::open, "link")
      << access (Access::open, "dirlink/secret")
      << access (Access::directory, "dirlink/*");
    FalServer (s, { t.root }).run ();
    auto st = s.all<Status> ();
    DN_ASSERT_EQ (st.size (), 4u);
    DN_ASSERT_EQ (st[0].miccode, 063u);         // bad name
    DN_ASSERT_EQ (st[1].miccode, 062u);         // as if not there
    DN_ASSERT_EQ (st[2].miccode, 040u);
    DN_ASSERT_EQ (st[3].miccode, 040u);
    DN_ASSERT (s.all<Data> ().empty ());
}

// -------------------------------------------------------------- writing

DN_TEST (fal, writing_needs_permission)
{
    Tree t;
    t.file ("keep", "k");
    Script s;
    s << v7 () << access (Access::create, "new") << access (Access::erase, "keep");
    FalServer (s, { t.root }).run ();
    auto st = s.all<Status> ();
    DN_ASSERT_EQ (st.size (), 2u);
    DN_ASSERT_EQ (st[0].miccode, 0125u);        // privilege violation
    DN_ASSERT (!t.exists ("new"));
    DN_ASSERT (t.exists ("keep"));
}

DN_TEST (fal, create_writes_binary_and_text)
{
    Tree t;
    t.dir ("sub");
    Script s;
    Attributes bin;
    bin.menu.set (Attributes::m_datatype);
    s << v7 () << bin << access (Access::create, "[SUB]B.DAT")
      << control (Control::connect) << control (Control::put)
      << Data { 0, bytes_of ("abc") } << Data { 1, bytes_of ("def") }
      << complete (AccessComplete::close);
    Attributes txt;
    txt.menu.set (Attributes::m_datatype).set (Attributes::m_rfm)
            .set (Attributes::m_rat);
    txt.datatype = Ext ().set (Attributes::dt_ascii);
    txt.rfm = Attributes::fb_var;
    txt.rat.set (Attributes::rat_cr);
    s << txt << access (Access::create, "sub/t.txt")
      << control (Control::connect) << control (Control::put)
      << Data { 0, bytes_of ("line 1") } << Data { 1, bytes_of ("line 2\r\n") }
      << complete (AccessComplete::close);
    FalServer (s, { t.root, true }).run ();

    DN_ASSERT_EQ (last_status (s), std::string ("none"));
    // The directory matched in another case; the new name is as given.
    DN_ASSERT_EQ (t.read ("sub/B.DAT"), std::string ("abcdef"));
    DN_ASSERT_EQ (t.read ("sub/t.txt"), std::string ("line 1\nline 2\n"));
    DN_ASSERT_EQ (s.all<AccessComplete> ().size (), 2u);
}

DN_TEST (fal, an_abandoned_create_leaves_the_old_file)
{
    Tree t;
    t.file ("f", "old");
    Script s;
    s << v7 () << access (Access::create, "f") << control (Control::connect)
      << control (Control::put) << Data { 0, bytes_of ("new") };
    // ... and then the link goes.
    FalServer (s, { t.root, true }).run ();
    DN_ASSERT_EQ (t.read ("f"), std::string ("old"));

    Script p;
    p << v7 () << access (Access::create, "f") << control (Control::connect)
      << control (Control::put) << Data { 0, bytes_of ("new") }
      << complete (AccessComplete::purge);
    FalServer (p, { t.root, true }).run ();
    DN_ASSERT_EQ (t.read ("f"), std::string ("old"));

    // No temporary files left behind.
    std::string out;
    FILE *ls = ::popen (("ls -A '" + t.root + "'").c_str (), "r");
    char buf[256];
    while (std::fgets (buf, sizeof buf, ls)) out += buf;
    ::pclose (ls);
    DN_ASSERT_EQ (out, std::string ("f\n"));
}

DN_TEST (fal, erase_and_rename)
{
    Tree t;
    t.file ("a.log", "1");
    t.file ("b.log", "2");
    t.file ("keep.txt", "3");
    Script s;
    Name to;
    to.nametype.set (Name::filespec);
    to.namespec = "kept.txt";
    s << v7 () << access (Access::erase, "*.LOG")
      << access (Access::rename, "keep.txt") << to;
    FalServer (s, { t.root, true }).run ();
    DN_ASSERT_EQ (last_status (s), std::string ("none"));
    DN_ASSERT (!t.exists ("a.log"));
    DN_ASSERT (!t.exists ("b.log"));
    DN_ASSERT (!t.exists ("keep.txt"));
    DN_ASSERT_EQ (t.read ("kept.txt"), std::string ("3"));

    // Renaming onto an existing file is refused.
    t.file ("x", "x");
    Script r;
    Name onto;
    onto.nametype.set (Name::filespec);
    onto.namespec = "x";
    r << v7 () << access (Access::rename, "kept.txt") << onto;
    FalServer (r, { t.root, true }).run ();
    DN_ASSERT_EQ (last_status (r), std::string ("4:55"));   // file exists
    DN_ASSERT (t.exists ("kept.txt"));
}

DN_TEST (fal, a_wildcard_open_offers_each_file_in_turn)
{
    Tree t;
    t.file ("a.txt", "first");
    t.file ("b.txt", "second");
    t.file ("c.dat", "other");
    Script s;
    // Skip the first by closing before connecting; read the second.
    s << v7 () << access (Access::open, "*.TXT")
      << complete (AccessComplete::close)
      << control (Control::connect) << control (Control::get)
      << complete (AccessComplete::close);
    FalServer (s, { t.root }).run ();

    auto names = s.all<Name> ();
    DN_ASSERT_EQ (names.size (), 4u);           // volume, directory, a, b
    DN_ASSERT (names[0].nametype[Name::volname]);
    DN_ASSERT (names[1].nametype[Name::dirname]);
    DN_ASSERT_EQ (names[2].namespec, std::string ("a.txt"));
    DN_ASSERT_EQ (names[3].namespec, std::string ("b.txt"));
    auto d = s.all<Data> ();
    DN_ASSERT_EQ (d.size (), 1u);
    DN_ASSERT_EQ (Bytes (d[0].payload.begin (), d[0].payload.begin () + 6),
                  bytes_of ("second"));
    DN_ASSERT (std::holds_alternative<AccessComplete> (s.out.back ()));
}

DN_TEST (fal, end_of_stream_before_close_is_answered)
{
    // VMS COPY ends a write with end of stream, then close.
    Tree t;
    Script s;
    s << v7 () << access (Access::create, "f.txt") << control (Control::connect)
      << control (Control::put) << Data { 0, bytes_of ("abc") }
      << complete (AccessComplete::eos) << complete (AccessComplete::close);
    FalServer (s, { t.root, true }).run ();
    DN_ASSERT_EQ (last_status (s), std::string ("none"));
    DN_ASSERT_EQ (s.all<AccessComplete> ().size (), 2u);
    DN_ASSERT_EQ (t.read ("f.txt"), std::string ("abc"));
}

DN_TEST (fal, vms_binary_marked_ascii_is_kept_as_binary)
{
    // What VMS COPY sends for a fixed 512 byte file with no carriage
    // control: data type ASCII, and records ending in any byte at all.
    Tree t;
    Attributes a;
    a.menu.set (Attributes::m_datatype).set (Attributes::m_rfm)
          .set (Attributes::m_rat).set (Attributes::m_mrs);
    a.datatype = Ext ().set (Attributes::dt_ascii);
    a.rfm = Attributes::fb_fix;
    a.mrs = 512;
    Bytes block (512, 'x');
    block.back () = '\n';
    Script s;
    s << v7 () << a << access (Access::create, "b.bin")
      << control (Control::connect) << control (Control::put)
      << Data { 0, block } << Data { 1, block }
      << complete (AccessComplete::eos) << complete (AccessComplete::close);
    FalServer (s, { t.root, true }).run ();
    DN_ASSERT_EQ (t.read ("b.bin").size (), 1024u);
}

DN_TEST (fal, erase_and_rename_answer_names_as_vms_fal_does)
{
    // VMS DELETE and RENAME ask for names back, and take any other answer
    // as a protocol error.  These are VMS FAL's own answers.
    Tree t;
    t.file ("gone.txt", "1");
    t.file ("old.txt", "2");
    Access del = access (Access::erase, "gone.txt");
    del.display = Ext ().set (Access::d_name);
    Access ren = access (Access::rename, "old.txt");
    ren.display = Ext ().set (Access::d_name);
    Name to;
    to.nametype.set (Name::filespec);
    to.namespec = "new.txt";
    Script s;
    s << v7 () << del << ren << to;
    FalServer (s, { t.root, true }).run ();
    // Config; erase: name, ack, complete; rename: name, ack, name, ack,
    // complete.
    DN_ASSERT_EQ (s.types (), std::string ("1 15 6 7 15 6 15 6 7"));
    auto n = s.all<Name> ();
    DN_ASSERT_EQ (n[0].namespec, std::string ("/gone.txt"));
    DN_ASSERT_EQ (n[1].namespec, std::string ("/old.txt"));
    DN_ASSERT_EQ (n[2].namespec, std::string ("/new.txt"));
}

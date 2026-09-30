// dap/fal.cc -- File Access Listener: the server side of DAP.
//
// Port of PyDECnet's applications/fal.py.  The message sequences, and the
// way files are described, follow it, since VMS and RSX requesters are
// known to accept them.  Create, erase and rename are additions.

#include "decnet/dap/fal.h"

#include "decnet/common/exceptions.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace decnet::dap {

namespace {

// RMS completion codes this server returns, as DAP MICCODEs.
constexpr unsigned rms_dnf = 040;       // directory not found
constexpr unsigned rms_fex = 055;       // file already exists
constexpr unsigned rms_fnf = 062;       // file not found
constexpr unsigned rms_fnm = 063;       // error in file name
constexpr unsigned rms_prv = 0125;      // privilege violation
constexpr unsigned rms_rer = 0132;      // file read error
constexpr unsigned rms_wer = 0163;      // file write error

// DAP field codes for "unsupported" statuses: MACCODE 2 with the field.
constexpr unsigned f_accfunc = 0320;
constexpr unsigned f_bls = 0225;

bool is_wild (const std::string &s)
{
    return s.find_first_of ("*?%") != std::string::npos;
}

std::string upper (std::string s)
{
    for (char &c : s) c = static_cast<char> (std::toupper (static_cast<unsigned char> (c)));
    return s;
}

// A VMS "%" matches one character, as "?" does for fnmatch.
std::string glob_pattern (std::string p)
{
    std::replace (p.begin (), p.end (), '%', '?');
    return p;
}

// DAP's date field is 18 characters, "30-SEP-26 13:24:26": two digit year.
std::string dap_time (std::time_t t)
{
    static const char *const months[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    std::tm tm {};
    ::localtime_r (&t, &tm);
    char buf[32];
    std::snprintf (buf, sizeof buf, "%02d-%s-%02d %02d:%02d:%02d", tm.tm_mday,
                   months[tm.tm_mon], tm.tm_year % 100, tm.tm_hour, tm.tm_min,
                   tm.tm_sec);
    return buf;
}

std::string owner_name (uid_t uid)
{
    struct passwd pw {}, *res = nullptr;
    char buf[1024];
    if (::getpwuid_r (uid, &pw, buf, sizeof buf, &res) == 0 && res)
        return res->pw_name;
    return std::to_string (uid);
}

// Deny bits from Unix permission bits.  Write permission covers delete.
Ext deny_bits (mode_t mode, mode_t r, mode_t w, mode_t x)
{
    Ext e;
    if (!(mode & r)) e.set (Protection::no_read);
    if (!(mode & w)) e.set (Protection::no_write).set (Protection::no_del);
    if (!(mode & x)) e.set (Protection::no_exec);
    return e;
}

std::vector<std::string> split (const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back (cur); cur.clear (); }
        else cur += c;
    }
    out.push_back (cur);
    return out;
}

struct Dir {
    DIR *d;
    explicit Dir (const std::string &p) : d (::opendir (p.c_str ())) {}
    ~Dir () { if (d) ::closedir (d); }
};

// The name of an entry in dir that is name, or failing that is name in
// another case; empty if none.
std::string lookup (const std::string &dir, const std::string &name)
{
    struct stat st;
    if (::lstat ((dir + "/" + name).c_str (), &st) == 0) return name;
    Dir d (dir);
    if (!d.d) return {};
    std::string want = upper (name), found;
    while (dirent *e = ::readdir (d.d)) {
        if (upper (e->d_name) == want) {
            if (!found.empty ()) return {};         // ambiguous
            found = e->d_name;
        }
    }
    return found;
}

}   // namespace

// ============================================================ FalServer

FalServer::FalServer (Transport &t, FalOptions o) : t_ (t), o_ (std::move (o))
{
    char *r = ::realpath (o_.root.c_str (), nullptr);
    if (!r) throw std::runtime_error ("FAL root " + o_.root + ": "
                                      + std::strerror (errno));
    root_real_ = r;
    std::free (r);
}

std::optional<std::vector<std::string>>
FalServer::split_spec (const std::string &spec)
{
    std::string s = spec;
    if (auto p = s.rfind ("::"); p != std::string::npos) s = s.substr (p + 2);
    if (auto p = s.find (';'); p != std::string::npos) s.erase (p);

    std::vector<std::string> comps;
    std::string name;
    auto lb = s.find_first_of ("[<");
    if (lb != std::string::npos) {
        // VMS: DEV:[DIR.SUB]NAME.TYP
        char close = s[lb] == '[' ? ']' : '>';
        auto rb = s.find (close, lb);
        if (rb == std::string::npos) return std::nullopt;
        for (const std::string &c : split (s.substr (lb + 1, rb - lb - 1), '.')) {
            if (c.empty () || c == "000000") continue;
            if (c == "-" || c.find ('/') != std::string::npos) return std::nullopt;
            comps.push_back (c);
        }
        name = s.substr (rb + 1);
        if (name.find ('/') != std::string::npos) return std::nullopt;
    } else {
        // A VMS device with no directory, or a Unix path.
        auto colon = s.find (':');
        if (colon != std::string::npos && s.find ('/') > colon)
            s = s.substr (colon + 1);
        auto parts = split (s, '/');
        name = parts.back ();
        parts.pop_back ();
        for (const std::string &c : parts) {
            if (c.empty () || c == ".") continue;
            comps.push_back (c);
        }
    }
    for (const std::string &c : comps)
        if (c == "..") return std::nullopt;
    if (name == "..") return std::nullopt;
    if (name == ".") name.clear ();
    // VMS "*.*" is every file, and "NAME." a name with no type.
    if (name == "*.*") name = "*";
    else if (name.size () > 1 && name.back () == '.'
             && name.find ('.') == name.size () - 1)
        name.pop_back ();
    comps.push_back (name);
    return comps;
}

bool FalServer::inside_root (const std::string &path) const
{
    char *r = ::realpath (path.c_str (), nullptr);
    if (!r) return false;
    std::string real = r;
    std::free (r);
    return real == root_real_
        || (real.size () > root_real_.size ()
            && real.compare (0, root_real_.size (), root_real_) == 0
            && real[root_real_.size ()] == '/');
}

// ---------------------------------------------------------- messages

std::optional<Message> FalServer::next ()
{
    while (pending_.empty ()) {
        auto b = t_.recv ();
        if (!b) return std::nullopt;
        try {
            pending_ = decode (*b);
        } catch (const std::exception &e) {
            if (o_.trace) std::cerr << "fal: bad message: " << e.what () << "\n";
            status (Status::format_error, 0);
        }
    }
    Message m = std::move (pending_.front ());
    pending_.erase (pending_.begin ());
    if (o_.trace) std::cerr << "fal< " << type_name (type_of (m)) << "\n";
    return m;
}

void FalServer::send (const Message &m)
{
    if (o_.trace) std::cerr << "fal> " << type_name (type_of (m)) << "\n";
    t_.send (encode (m));
}

void FalServer::status (unsigned mac, unsigned mic)
{
    Status s;
    s.maccode = mac;
    s.miccode = mic;
    send (s);
}

// ---------------------------------------------------------- the session

void FalServer::run ()
{
    auto m = next ();
    if (!m) return;
    auto *c = std::get_if<Config> (&*m);
    if (!c) {
        status (Status::out_of_sync, type_of (*m));
        return;
    }
    remote_ = *c;
    v7_ = remote_.version[0] >= 7;

    Config ours;
    ours.bufsiz = 65535;
    ours.ostype = 192;                  // as PyDECnet
    ours.filesys = 13;
    for (unsigned b : { Config::cap_fo_seq, Config::cap_seq_xfer,
                        Config::cap_blocking, Config::cap_len2, Config::cap_dir,
                        Config::cap_dattim_xa, Config::cap_fprot_xa,
                        Config::cap_seq_ra, Config::cap_glob, Config::cap_name })
        ours.syscap.set (b);
    if (o_.writable)
        ours.syscap.set (Config::cap_delete).set (Config::cap_rename);
    send (ours);

    while ((m = next ())) {
        if (auto *a = std::get_if<Attributes> (&*m)) {
            attr_ = *a;
        } else if (std::holds_alternative<DateTime> (*m)
                   || std::holds_alternative<Protection> (*m)) {
            // Extended attributes for a create; not applied.
        } else if (auto *acc = std::get_if<Access> (&*m)) {
            access (*acc);
            attr_.reset ();
        } else if (std::holds_alternative<Config> (*m)) {
            send (ours);
        } else {
            status (Status::out_of_sync, type_of (*m));
        }
    }
}

void FalServer::access (const Access &a)
{
    switch (a.accfunc) {
    case Access::directory: return directory (a);
    case Access::open:      return get (a);
    case Access::create:    return create (a);
    case Access::erase:     return erase (a);
    case Access::rename:    return rename (a);
    default:                return status (Status::unsupported, f_accfunc);
    }
}

// ------------------------------------------------------------ finding

std::optional<std::vector<FalServer::Found>>
FalServer::find (const std::string &spec, bool want_dirs)
{
    auto comps = split_spec (spec);
    if (!comps) {
        status (Status::open_error, rms_fnm);
        return std::nullopt;
    }
    std::string pattern = comps->back ();
    comps->pop_back ();

    std::string cur = root_real_, rel;
    for (const std::string &c : *comps) {
        std::string n = lookup (cur, c);
        struct stat st;
        if (n.empty () || ::stat ((cur + "/" + n).c_str (), &st) != 0
            || !S_ISDIR (st.st_mode) || !inside_root (cur + "/" + n)) {
            status (Status::open_error, rms_dnf);
            return std::nullopt;
        }
        cur += "/" + n;
        rel += (rel.empty () ? "" : "/") + n;
    }
    if (::access (cur.c_str (), X_OK) != 0) {
        status (Status::open_error, rms_dnf);
        return std::nullopt;
    }
    // A directory named on its own means everything in it.
    if (!pattern.empty () && !is_wild (pattern)) {
        std::string n = lookup (cur, pattern);
        struct stat st;
        if (!n.empty () && ::stat ((cur + "/" + n).c_str (), &st) == 0
            && S_ISDIR (st.st_mode) && want_dirs) {
            cur += "/" + n;
            rel += (rel.empty () ? "" : "/") + n;
            pattern.clear ();
        }
    }
    if (pattern.empty ()) pattern = "*";

    std::vector<Found> out;
    auto consider = [&] (const std::string &name) {
        Found f;
        f.path = cur + "/" + name;
        f.rel = rel;
        f.name = name;
        struct stat st;
        if (::stat (f.path.c_str (), &st) != 0) return;
        if (!inside_root (f.path)) return;
        f.is_dir = S_ISDIR (st.st_mode);
        if (f.is_dir && !want_dirs) return;
        // DEC rule, as fal.py has it: what you cannot read, you cannot see.
        if (::access (f.path.c_str (), R_OK) != 0) return;
        out.push_back (std::move (f));
    };

    if (is_wild (pattern)) {
        std::string g = glob_pattern (pattern);
        Dir d (cur);
        while (d.d) {
            dirent *e = ::readdir (d.d);
            if (!e) break;
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            // Hidden files only when asked for by name, as glob does.
            if (n[0] == '.' && g[0] != '.') continue;
            if (::fnmatch (g.c_str (), n.c_str (), FNM_CASEFOLD) == 0)
                consider (n);
        }
        std::sort (out.begin (), out.end (),
                   [] (const Found &x, const Found &y) { return x.name < y.name; });
    } else {
        std::string n = lookup (cur, pattern);
        if (!n.empty ()) consider (n);
    }
    return out;
}

std::optional<FalServer::Found> FalServer::new_file (const std::string &spec)
{
    auto comps = split_spec (spec);
    if (!comps || comps->back ().empty () || is_wild (comps->back ())) {
        status (Status::open_error, rms_fnm);
        return std::nullopt;
    }
    std::string name = comps->back ();
    comps->pop_back ();
    std::string cur = root_real_, rel;
    for (const std::string &c : *comps) {
        std::string n = lookup (cur, c);
        struct stat st;
        if (n.empty () || ::stat ((cur + "/" + n).c_str (), &st) != 0
            || !S_ISDIR (st.st_mode)) {
            status (Status::open_error, rms_dnf);
            return std::nullopt;
        }
        cur += "/" + n;
        rel += (rel.empty () ? "" : "/") + n;
    }
    if (!inside_root (cur)) {
        status (Status::open_error, rms_dnf);
        return std::nullopt;
    }
    // Replace a file that differs only in case rather than make a twin.
    std::string existing = lookup (cur, name);
    Found f;
    f.name = existing.empty () ? name : existing;
    f.path = cur + "/" + f.name;
    f.rel = rel;
    struct stat st;
    if (!existing.empty () && ::lstat (f.path.c_str (), &st) == 0) {
        if (!S_ISREG (st.st_mode) || !inside_root (f.path)) {
            status (Status::open_error, rms_fex);
            return std::nullopt;
        }
    }
    return f;
}

// -------------------------------------------------------- describing

std::vector<Message> FalServer::describe (const Found &f, const Access &a,
                                          bool dirop, bool wild)
{
    std::vector<Message> out;
    struct stat st;
    if (::stat (f.path.c_str (), &st) != 0) return out;

    if (dirop || wild) {
        Name n;
        n.nametype.set (Name::filename);
        // DAP cannot say "directory"; PyDECnet appends a slash.
        n.namespec = f.is_dir ? f.name + "/" : f.name;
        out.push_back (n);
    }
    if (a.display[Access::d_main] || !dirop) {
        Attributes at;
        at.menu.set (Attributes::m_bls).set (Attributes::m_rfm)
               .set (Attributes::m_alq).set (Attributes::m_hbk)
               .set (Attributes::m_ebk).set (Attributes::m_ffb);
        auto size = static_cast<std::uint64_t> (st.st_size);
        at.bls = 512;
        at.rfm = v7_ ? Attributes::fb_slf : Attributes::fb_fix;
        at.alq = static_cast<std::uint64_t> (st.st_blocks);
        at.hbk = at.ebk = size / 512 + 1;
        at.ffb = static_cast<std::uint16_t> (size % 512);
        if (attr_) {
            // Requested format, as fal.py applies it: stream becomes
            // Stream_LF for a DAP 7 requester.
            if (attr_->has (Attributes::m_rfm)
                && (attr_->rfm != Attributes::fb_stm || !v7_))
                at.rfm = attr_->rfm;
            if (attr_->has (Attributes::m_rat)) {
                at.menu.set (Attributes::m_rat);
                at.rat.set (Attributes::rat_cr, attr_->rat[Attributes::rat_cr]);
            }
        }
        if (at.rfm == Attributes::fb_fix) {
            at.menu.set (Attributes::m_mrs);
            at.mrs = 512;
        }
        out.push_back (at);
    }
    if (a.display[Access::d_date]) {
        DateTime d;
        d.menu.set (DateTime::m_cdt).set (DateTime::m_rdt);
        d.cdt = dap_time (st.st_ctime);
        d.rdt = dap_time (st.st_mtime);
        out.push_back (d);
    }
    if (a.display[Access::d_fprot]) {
        Protection p;
        p.menu.set (Protection::m_owner).set (Protection::m_own)
              .set (Protection::m_grp).set (Protection::m_wld);
        p.owner = owner_name (st.st_uid);
        p.own = deny_bits (st.st_mode, S_IRUSR, S_IWUSR, S_IXUSR);
        p.grp = deny_bits (st.st_mode, S_IRGRP, S_IWGRP, S_IXGRP);
        p.wld = deny_bits (st.st_mode, S_IROTH, S_IWOTH, S_IXOTH);
        out.push_back (p);
    }
    // For one named file the name goes last.  VMS cares, says fal.py.
    if (!(dirop || wild) && a.display[Access::d_name]) {
        Name n;
        n.nametype.set (Name::filespec);
        n.namespec = "/" + (f.rel.empty () ? "" : f.rel + "/") + f.name;
        out.push_back (n);
    }
    return out;
}

// ---------------------------------------------------------- directory

void FalServer::directory (const Access &a)
{
    auto found = find (a.filespec, true);
    if (found) {
        auto comps = split_spec (a.filespec);
        bool wild = comps && is_wild (comps->back ());
        if (found->empty () && !wild && comps && !comps->back ().empty ()) {
            status (Status::open_error, rms_fnf);
        } else {
            // An empty volume name first: RSTS needs it, fal.py says.
            Name vol;
            vol.nametype.set (Name::volname);
            send (vol);
            std::string dir;
            bool first = true;
            for (const Found &f : *found) {
                if (first || f.rel != dir) {
                    Name dn;
                    dn.nametype.set (Name::dirname);
                    dn.namespec = "/" + (f.rel.empty () ? "" : f.rel + "/");
                    send (dn);
                    dir = f.rel;
                    first = false;
                }
                for (const Message &m : describe (f, a, true, true)) send (m);
                // DAP 7 has an Acknowledge after each file; VMS wants it.
                if (v7_) send (Ack {});
            }
        }
    }
    // As fal.py: the listing ends with Access Complete, error or not.
    AccessComplete done;
    done.cmpfunc = AccessComplete::response;
    send (done);
}

// ----------------------------------------------------------------- get

void FalServer::get (const Access &a)
{
    if (attr_ && attr_->has (Attributes::m_bls) && attr_->bls != 512)
        return status (Status::unsupported, f_bls);
    auto found = find (a.filespec, false);
    if (!found) return;
    auto comps = split_spec (a.filespec);
    bool wild = comps && is_wild (comps->back ());
    if (found->empty () && !wild) return status (Status::open_error, rms_fnf);

    bool text = attr_ && attr_->has (Attributes::m_rfm)
             && (attr_->rfm == Attributes::fb_var
                 || attr_->rfm == Attributes::fb_stm);
    // Line ends implied by the record attributes, or sent as CR LF.
    bool implied = attr_ && attr_->has (Attributes::m_rat)
                && attr_->rat[Attributes::rat_cr];

    for (const Found &f : *found) {
        for (const Message &m : describe (f, a, false, wild)) send (m);
        send (Ack {});

        // Connect to read it, or close to skip it.
        auto m = next ();
        if (!m) return;
        if (auto *ac = std::get_if<AccessComplete> (&*m);
            ac && ac->cmpfunc == AccessComplete::close)
            continue;
        auto *c = std::get_if<Control> (&*m);
        if (!c) return status (Status::out_of_sync, type_of (*m));
        if (c->ctlfunc != Control::connect)
            return status (Status::unsupported, 0420);
        send (Ack {});

        m = next ();
        if (!m) return;
        c = std::get_if<Control> (&*m);
        if (!c) return status (Status::out_of_sync, type_of (*m));
        if (c->ctlfunc != Control::get) return status (Status::unsupported, 0420);
        if (!c->menu[Control::m_rac] || c->rac != Control::rb_seqf)
            return status (Status::unsupported, 0422);

        std::unique_ptr<std::FILE, int (*) (std::FILE *)> fp (
            std::fopen (f.path.c_str (), "rb"), std::fclose);
        if (!fp) return status (Status::open_error, rms_prv);
        if (text) {
            std::string line;
            int ch;
            bool any = false;
            auto flush = [&] {
                if (!implied) line += "\r\n";
                send (Data { 0, Bytes (line.begin (), line.end ()) });
                line.clear ();
            };
            while ((ch = std::fgetc (fp.get ())) != EOF) {
                any = true;
                if (ch == '\n') flush ();
                else line += static_cast<char> (ch);
            }
            // A last line with no newline is still a record.
            if (any && !line.empty ()) flush ();
        } else {
            Bytes block (512);
            for (;;) {
                std::size_t n = std::fread (block.data (), 1, 512, fp.get ());
                if (n == 0) break;
                // Fixed 512 byte records: pad the last one.
                std::fill (block.begin () + static_cast<std::ptrdiff_t> (n),
                           block.end (), 0);
                send (Data { 0, block });
            }
        }
        if (std::ferror (fp.get ()))
            return status (Status::transfer_error, rms_rer);
        status (Status::transfer_error, Status::rms_eof);

        m = next ();
        if (!m) return;
        auto *ac = std::get_if<AccessComplete> (&*m);
        if (!ac) return status (Status::out_of_sync, type_of (*m));
        if (ac->cmpfunc == AccessComplete::eos) {
            // Some requesters say end of stream before closing.
            AccessComplete r;
            r.cmpfunc = AccessComplete::response;
            send (r);
            m = next ();
            if (!m) return;
            ac = std::get_if<AccessComplete> (&*m);
            if (!ac) return status (Status::out_of_sync, type_of (*m));
        }
        if (ac->cmpfunc != AccessComplete::close)
            return status (Status::unsupported, 0720);
    }
    AccessComplete done;
    done.cmpfunc = AccessComplete::response;
    send (done);
}

// -------------------------------------------------------------- create

void FalServer::create (const Access &a)
{
    if (!o_.writable) return status (Status::open_error, rms_prv);
    auto target = new_file (a.filespec);
    if (!target) return;

    // Text if the requester says its records are lines.
    bool text = attr_
        && ((attr_->has (Attributes::m_datatype)
             && attr_->datatype[Attributes::dt_ascii])
            || attr_->text ());

    // Written under a temporary name and renamed at close, so a transfer
    // that fails part way leaves any older file as it was.
    std::string dir = target->path.substr (0, target->path.rfind ('/'));
    std::string tmp = dir + "/." + target->name + ".dnfal-XXXXXX";
    int fd = ::mkstemp (tmp.data ());
    if (fd < 0) return status (Status::open_error, rms_prv);
    ::fchmod (fd, 0644);
    std::unique_ptr<std::FILE, int (*) (std::FILE *)> fp (
        ::fdopen (fd, "wb"), std::fclose);
    auto abandon = [&] { fp.reset (); ::unlink (tmp.c_str ()); };

    // Tell the requester what it is getting, as for an open.
    Attributes at = attr_ ? *attr_ : Attributes {};
    at.menu.set (Attributes::m_rfm).set (Attributes::m_bls);
    if (!attr_ || !attr_->has (Attributes::m_rfm))
        at.rfm = v7_ ? Attributes::fb_slf : Attributes::fb_fix;
    send (at);
    if (a.display[Access::d_name]) {
        Name n;
        n.nametype.set (Name::filespec);
        n.namespec = "/" + (target->rel.empty () ? "" : target->rel + "/")
                   + target->name;
        send (n);
    }
    send (Ack {});

    for (;;) {
        auto m = next ();
        if (!m) { abandon (); return; }
        if (auto *c = std::get_if<Control> (&*m)) {
            if (c->ctlfunc == Control::connect) send (Ack {});
            else if (c->ctlfunc != Control::put) {
                abandon ();
                return status (Status::unsupported, 0420);
            }
            // PUT: the data follows; in record mode, one per record.
        } else if (auto *d = std::get_if<Data> (&*m)) {
            ByteView r = d->payload;
            bool ok;
            if (text) {
                while (!r.empty () && (r.back () == '\n' || r.back () == '\r'))
                    r = r.first (r.size () - 1);
                ok = std::fwrite (r.data (), 1, r.size (), fp.get ()) == r.size ()
                  && std::fputc ('\n', fp.get ()) != EOF;
            } else {
                ok = std::fwrite (r.data (), 1, r.size (), fp.get ()) == r.size ();
            }
            if (!ok) {
                abandon ();
                return status (Status::transfer_error, rms_wer);
            }
        } else if (auto *ac = std::get_if<AccessComplete> (&*m)) {
            if (ac->cmpfunc == AccessComplete::purge) {
                abandon ();
            } else {
                bool ok = std::fflush (fp.get ()) == 0
                       && ::fsync (::fileno (fp.get ())) == 0;
                fp.reset ();
                if (!ok || ::rename (tmp.c_str (), target->path.c_str ()) != 0) {
                    ::unlink (tmp.c_str ());
                    return status (Status::close_error, rms_wer);
                }
            }
            AccessComplete r;
            r.cmpfunc = AccessComplete::response;
            send (r);
            return;
        } else {
            abandon ();
            return status (Status::out_of_sync, type_of (*m));
        }
    }
}

// --------------------------------------------------------------- erase

void FalServer::erase (const Access &a)
{
    if (!o_.writable) return status (Status::open_error, rms_prv);
    auto found = find (a.filespec, false);
    if (!found) return;
    if (found->empty ()) return status (Status::open_error, rms_fnf);
    for (const Found &f : *found)
        if (::unlink (f.path.c_str ()) != 0)
            return status (Status::open_error, rms_prv);
    AccessComplete done;
    done.cmpfunc = AccessComplete::response;
    send (done);
}

// -------------------------------------------------------------- rename

void FalServer::rename (const Access &a)
{
    if (!o_.writable) return status (Status::open_error, rms_prv);
    // The new name follows in a Name message.
    auto m = next ();
    if (!m) return;
    auto *n = std::get_if<Name> (&*m);
    if (!n) return status (Status::out_of_sync, type_of (*m));

    auto comps = split_spec (a.filespec);
    if (comps && is_wild (comps->back ()))
        return status (Status::open_error, rms_fnm);
    auto found = find (a.filespec, false);
    if (!found) return;
    if (found->size () != 1) return status (Status::open_error, rms_fnf);

    auto target = new_file (n->namespec);
    if (!target) return;
    struct stat st;
    if (::lstat (target->path.c_str (), &st) == 0)
        return status (Status::open_error, rms_fex);
    if (::rename ((*found)[0].path.c_str (), target->path.c_str ()) != 0)
        return status (Status::open_error, rms_prv);
    AccessComplete done;
    done.cmpfunc = AccessComplete::response;
    send (done);
}

}   // namespace decnet::dap

// decnet/dap/messages.h -- Data Access Protocol messages.
//
// Port of dap_packets.py: the DAP messages NFT and FAL exchange to list,
// read and (later) write files.  DAP is spoken over a logical link to
// object 17; each session control message carries one DAP message, or
// several "blocked" together with length fields in their headers.
//
// Two DAP rules shape the decoder:
//
//   - A message may stop early.  Fields missing at the end take their
//     default values, which is zero unless the specification gives one
//     (data type "image", record format "fixed", block size 512, ...).
//   - Many fields are present only when a bit in a preceding menu says so.
//
// Bit fields are EX-n: up to n bytes of seven bits each, the top bit of
// a byte saying another follows.  Ext holds one of any width.
//
// Messages decoded but not otherwise supported (key definition,
// allocation, summary, ACL) arrive as Unknown.  Only unblocked messages
// are encoded; any peer must accept those.

#ifndef DECNET_DAP_MESSAGES_H
#define DECNET_DAP_MESSAGES_H

#include "decnet/common/types.h"

#include <array>
#include <bitset>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace decnet::dap {

// Message type codes.
enum Type : std::uint8_t {
    t_config = 1, t_attributes = 2, t_access = 3, t_control = 4,
    t_continue = 5, t_ack = 6, t_access_complete = 7, t_data = 8,
    t_status = 9, t_key_def = 10, t_alloc = 11, t_summary = 12,
    t_date_time = 13, t_protection = 14, t_name = 15, t_acl = 16
};

// An EX-n bit field.
class Ext {
public:
    Ext () = default;
    explicit Ext (std::uint64_t bits) : b_ (bits) {}

    bool operator[] (unsigned bit) const { return bit < N && b_[bit]; }
    Ext &set (unsigned bit, bool on = true) { b_.set (bit, on); return *this; }
    bool any () const noexcept { return b_.any (); }

    // A multi-bit subfield, such as the two bit RSTS protection code.
    unsigned field (unsigned lo, unsigned width) const;

    // Encode in as few bytes as the value needs, at least one.  Throws
    // FieldOverflow if that is more than maxlen.
    void encode (Bytes &out, unsigned maxlen) const;

    friend bool operator== (const Ext &, const Ext &) = default;

    static constexpr unsigned N = 128;

private:
    friend class Reader;
    std::bitset<N> b_;
};

// ----------------------------------------------------------------- messages

// Configuration: exchanged first, in both directions.
struct Config {
    std::uint16_t bufsiz = 0;           // largest message; 0 is unlimited
    std::uint8_t  ostype = 0;
    std::uint8_t  filesys = 0;
    std::array<std::uint8_t, 5> version { 7, 0, 0, 0, 0 };
    Ext           syscap;

    // SYSCAP bits.
    enum : unsigned {
        cap_prealloc = 0, cap_fo_seq = 1, cap_fo_rel = 2, cap_seq_xfer = 5,
        cap_random_rec = 6, cap_random_blk = 7, cap_append = 13,
        cap_blocking = 18, cap_unres_blocking = 19, cap_len2 = 20,
        cap_cksum = 21, cap_dir = 25, cap_dattim_xa = 26, cap_fprot_xa = 27,
        cap_delete = 31, cap_seq_ra = 33, cap_rename = 37, cap_glob = 38,
        cap_name = 40
    };

    // Operating system types, for display.
    static const char *ostype_name (std::uint8_t t);
};

// File attributes.
struct Attributes {
    Ext           menu;
    Ext           datatype { 1u << dt_image };
    std::uint8_t  org = 0;
    std::uint8_t  rfm = fb_fix;
    Ext           rat;
    std::uint16_t bls = 512;
    std::uint16_t mrs = 0;
    std::uint64_t alq = 0;
    std::uint8_t  bks = 0;
    std::uint8_t  fsz = 0;
    std::uint64_t mrn = 0;
    std::string   runsys;
    std::uint16_t deq = 0;
    Ext           fop;
    std::uint8_t  bsz = 8;
    Ext           dev;
    Ext           sdc;
    std::uint16_t lrl = 0;
    std::uint64_t hbk = 0;
    std::uint64_t ebk = 0;
    std::uint16_t ffb = 0;
    std::uint64_t sbn = 0;

    // ATTMENU bits: which fields are present.
    enum : unsigned {
        m_datatype = 0, m_org, m_rfm, m_rat, m_bls, m_mrs, m_alq, m_bks,
        m_fsz, m_mrn, m_runsys, m_deq, m_fop, m_bsz, m_dev, m_sdc, m_lrl,
        m_hbk, m_ebk, m_ffb, m_sbn
    };
    // DATATYPE bits.
    enum : unsigned {
        dt_ascii = 0, dt_image = 1, dt_compressed = 3, dt_executable = 4,
        dt_privileged = 5, dt_sensitive = 7
    };
    // ORG values.
    enum : std::uint8_t { fb_seq = 0, fb_rel = 020, fb_idx = 040 };
    // RFM values.
    enum : std::uint8_t {
        fb_udf = 0, fb_fix = 1, fb_var = 2, fb_vfc = 3, fb_stm = 4,
        fb_slf = 5, fb_scr = 6
    };
    // RAT bits.
    enum : unsigned {
        rat_ftn = 0, rat_cr = 1, rat_prn = 2, rat_blk = 3, rat_emb = 4,
        rat_lsa = 6, rat_macy11 = 7
    };

    bool has (unsigned m) const { return menu[m]; }

    // Records carry implied line ends: FORTRAN, CR or print control.
    bool text () const
    { return rat[rat_cr] || rat[rat_ftn] || rat[rat_prn]; }

    // Size in bytes, from the end of file block and first free byte, when
    // the server gave them.
    std::optional<std::uint64_t> size () const;

    // "Variable", "Stream_LF", ...
    static const char *rfm_name (std::uint8_t rfm);
};

// Open, create, list, delete, ... a file.
struct Access {
    std::uint8_t accfunc = open;
    Ext          accopt;
    std::string  filespec;
    Ext          fac { 1u << fac_get };
    Ext          shr { 1u << fac_get };
    Ext          display;
    std::string  password;              // sent only if not empty

    // ACCFUNC values.
    enum : std::uint8_t {
        open = 1, create = 2, rename = 3, erase = 4, directory = 6,
        submit = 7, execute = 8
    };
    // FAC and SHR bits.
    enum : unsigned {
        fac_put = 0, fac_get = 1, fac_del = 2, fac_upd = 3, fac_trn = 4,
        fac_bio = 5, fac_bro = 6
    };
    // DISPLAY bits: the attribute messages wanted back.
    enum : unsigned {
        d_main = 0, d_keydef = 1, d_alloc = 2, d_summary = 3, d_date = 4,
        d_fprot = 5, d_name = 8
    };
};

// Record operations on an open file.
struct Control {
    std::uint8_t ctlfunc = get;
    Ext          menu;
    std::uint8_t rac = 0;
    Bytes        key;
    std::uint8_t krf = 0;
    Ext          rop;

    enum : std::uint8_t {
        get = 1, connect = 2, update = 3, put = 4, del = 5, rewind = 6,
        truncate = 7, free = 10, flush = 12, find = 14
    };
    enum : unsigned { m_rac = 0, m_key = 1, m_krf = 2, m_rop = 3 };
    // RAC values.
    enum : std::uint8_t {
        rb_seq = 0, rb_key = 1, rb_rfa = 2, rb_seqf = 3, rb_blk = 4,
        rb_blkf = 5
    };
};

struct Continue {
    std::uint8_t confunc = resume;
    enum : std::uint8_t { try_again = 1, skip = 2, abort = 3, resume = 4 };
};

struct Ack {};

struct AccessComplete {
    std::uint8_t  cmpfunc = close;
    Ext           fop;
    std::uint16_t check = 0;
    enum : std::uint8_t {
        close = 1, response = 2, purge = 3, eos = 4, skip = 5
    };
};

// One record, or one block in block mode.
struct Data {
    std::uint64_t recnum = 0;
    Bytes         payload;
};

// Success, failure or end of file.
struct Status {
    unsigned maccode = 0;               // 4 bits: the class
    unsigned miccode = 0;               // 12 bits: the detail
    Bytes    rest;                      // RFA, record number, STV: unused

    // MACCODE values.
    enum : unsigned {
        pending = 0, success = 1, unsupported = 2, open_error = 4,
        transfer_error = 5, transfer_warning = 6, close_error = 7,
        format_error = 8, invalid_field = 9, out_of_sync = 10
    };
    // The RMS end of file code, which arrives as a transfer error.
    static constexpr unsigned rms_eof = 047;

    bool eof () const noexcept
    { return maccode == transfer_error && miccode == rms_eof; }

    // "Open error: file not found."
    std::string str () const;
};

struct DateTime {
    Ext           menu;
    std::string   cdt, rdt, edt;        // "30-SEP-26 13:24:26", 18 bytes
    std::uint16_t rvn = 0;
    enum : unsigned { m_cdt = 0, m_rdt = 1, m_edt = 2, m_rvn = 3 };
};

struct Protection {
    Ext         menu;
    std::string owner;
    Ext         sys, own, grp, wld;     // bits deny: read, write, exec, delete
    enum : unsigned {
        m_owner = 0, m_sys = 1, m_own = 2, m_grp = 3, m_wld = 4
    };
    enum : unsigned { no_read = 0, no_write = 1, no_exec = 2, no_del = 3 };

    // "(RWED,RWED,RE,)" as VMS shows it.
    std::string vms () const;
};

struct Name {
    Ext         nametype;
    std::string namespec;
    enum : unsigned { filespec = 0, filename = 1, dirname = 2, volname = 3 };
};

// Anything else: kept whole so a session can skip it.
struct Unknown {
    std::uint8_t type = 0;
    Bytes        body;
};

using Message = std::variant<Config, Attributes, Access, Control, Continue,
                             Ack, AccessComplete, Data, Status, DateTime,
                             Protection, Name, Unknown>;

// The type code of a message.
std::uint8_t type_of (const Message &m);

// "Attributes", "Access Complete", for traces and errors.
const char *type_name (std::uint8_t type);

// Encode one message, unblocked.
Bytes encode (const Message &m);

// Decode every message in one session control message.  Throws
// DecodeError if a message is malformed.
std::vector<Message> decode (ByteView buf);

}   // namespace decnet::dap

#endif  // DECNET_DAP_MESSAGES_H

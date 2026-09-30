#include "decnet/mop/names.h"

#include <cstddef>

namespace decnet::mop {

namespace {

struct Name {
    unsigned    code;
    const char *short_name;
    const char *name;
};

// From PyDECnet's nicepackets.py: MOPdevices2, MOPdatalinks, MOPCPUs.
const Name devices[] = {
    { 0, "DP", "DP11-DA (OBSOLETE)" },
    { 1, "UNA", "DEUNA UNIBUS CSMA/CD communication link" },
    { 2, "DU", "DU11-DA synchronous line interface" },
    { 3, "CNA", "DECNA Professional CSMA/CD communication link" },
    { 4, "DL", "DL11-C, -E or -WA asynchronous line interface" },
    { 5, "QNA", "DEQNA Q-bus CSMA/CD communication link" },
    { 6, "DQ", "DQ11-DA (OBSOLETE)" },
    { 7, "CI", "Computer Interconnect interface" },
    { 8, "DA", "DA11-B or -AL UNIBUS link" },
    { 9, "PCL", "PCL11-B UNIBUS multiple CPU link" },
    { 10, "DUP", "DUP11-DA synchronous line interface" },
    { 11, "LUA", "DELUA UNIBUS CSMA/CD communication link" },
    { 12, "DMC", "DMC11-DA/AR, -FA/AR, -MA/AL or -MD/AL synchronous link" },
    { 13, "LNA", "MicroServer Lance CSMA/CD communication link" },
    { 14, "DN", "DN11-BA or -AA automatic calling unit" },
    { 16, "DLV", "DLV11-E, -F, -J, MXV11-A or -B asynchronous line interface" },
    { 17, "LCS", "LANCE/DECserver100 CSMA/CD communication link" },
    { 18, "DMP", "DMP11 UNIBUS multipoint synchronous link" },
    { 20, "DTE", "DTE20 PDP-11 to KL10 interface" },
    { 21, "DBT", "DEBET CSMA/CD communication link" },
    { 22, "DV", "DV11-AA/BA UNIBUS synchronous line multiplexer" },
    { 23, "BNA", "DEBNT BI CSMA/CD communication link" },
    { 24, "DZ", "DZ11-A, -B, -C, or -D UNIBUS asynchronous line multiplexer" },
    { 25, "LPC", "VAXmate (LANCE) CSMA/CD communication link" },
    { 26, "DSV", "DSV11 Q-bus synchronous link" },
    { 27, "CEC", "3Com 3C501, IBM-PC CSMA/CD adapter" },
    { 28, "KDP", "KMC11/DUP11-DA synchronous line multiplexer" },
    { 29, "IEC", "Micom/Interlan 5010, IBM-PC CSMA/CD adapter" },
    { 30, "KDZ", "KMC11/DZ11-A, -B, -C, or -D asynchronous line multiplexer" },
    { 31, "LQA", "DELQA CSMA/CD communication link, alternate assignment" },
    { 32, "KL", "KL8-J (OBSOLETE)" },
    { 33, "DS2", "LANCE/DECserver 200 CSMA/CD communication link" },
    { 34, "DMV", "DMV11 Q-bus synchronous link" },
    { 35, "DS5", "DECserver 500 CSMA/CD communication link" },
    { 36, "DPV", "DPV11 Q-bus synchronous line interface" },
    { 37, "LQA", "DELQA CSMA/CD communication link" },
    { 38, "DMF", "DMF-32 UNIBUS synchronous line unit" },
    { 39, "SVA", "DESVA Microvax-2000, 3100, 3300 CSMA/CD communication link" },
    { 40, "DMR", "DMR11-AA, -AB, -AC, or -AE UNIBUS interprocessor link" },
    { 41, "MUX", "MUXServer 100 CSMA/CD communication link" },
    { 42, "KMY", "KMS11-PX UNIBUS synchronous line interface with X.25 level 2 microcode" },
    { 43, "DEP", "DEPCA PCSG/IBM-PC CSMA/CD communication link" },
    { 44, "KMX", "KMS11-BD/BE UNIBUS synchronous line interface with X.25 level 2 microcode" },
    { 45, "LTM", "LTM (911) Ethernet monitor" },
    { 46, "DMB", "DMB-32 BI synchronous line multiplexer" },
    { 47, "DES", "DESNC Ethernet Encryption Module" },
    { 48, "KCP", "KCP Professional synchronous/asynchronous comm port" },
    { 49, "MX3", "MUXServer 300 CSMA/CD communication link" },
    { 50, "SYN", "MicroServer Synchronous line interface" },
    { 52, "DSB", "DSB32 BI Synchronous Line Interface" },
    { 53, "BAM", "DEBAM LANBridge-200 Data Link" },
    { 54, "DST", "DST-32 TEAMmate Synchronous Line Interface (DEC423)" },
    { 55, "FAT", "DEFAT DataKit Server CSMA/CD communication link" },
    { 58, "3C2", "3COM Etherlink II (part number 3C503)" },
    { 59, "3CM", "3COM Etherlink/MC (part number 3C523)" },
    { 60, "DS3", "DECServer 300 CSMA/CD communication link" },
    { 61, "MF2", "MicroVAX 3300 CSMA/CD communication link" },
    { 63, "VIT", "Vitalink TransLAN III/IV (NP3A) Bridge" },
    { 64, "VT5", "Vitalink TransLAN 350 (NPC25) Bridge, TransPATH 350 BRouter" },
    { 65, "BNI", "DEBNI BI CSMA/CD communication link" },
    { 66, "MNA", "DEMNA XMI CSMA/CD communication link" },
    { 67, "PMX", "DECstation-3100 CSMA/CD communication link" },
    { 68, "NI5", "Interlan NI5210-8 CSMA/CD communication link" },
    { 69, "NI9", "Interlan NI9210 CSMA/CD communication link" },
    { 70, "KMK", "KMS11-K DataKit UNIBUS adapter" },
    { 71, "3CP", "3COM Etherlink Plus (part number 3C505)" },
    { 72, "DP2", "DECserver-250 (parallel printer server) CSMA/CD communication link" },
    { 73, "ISA", "Pele SGEC-based CSMA/CD communication link" },
    { 74, "DIV", "DIV-32 Q-bus ISDN (2B+D) adapter" },
    { 75, "QTA", "DEQTA (DELQA-YM) CSMA/CD comm link" },
    { 76, "B15", "LANbridge-150 CSMA/CD comm link" },
    { 77, "WD8", "Western Digital WD8003 family CSMA/CD comm link" },
    { 78, "ILA", "BICC ISOLAN 4110-2 CSMA/CD comm link" },
    { 79, "ILM", "BICC ISOLAN 4110-3 CSMA/CD comm link" },
    { 80, "APR", "Apricot Xen-S and Qi series workstation CSMA/CD comm link" },
    { 81, "ASN", "AST EtherNode CSMA/CD comm link" },
    { 82, "ASE", "AST Ethernet CSMA/CD comm link" },
    { 83, "TRW", "TRW HC-2001 CSMA/CD comm link" },
    { 84, "EDX", "EDEN Sistemas de Computaçao Ltda ED586/32 CSMA/CD comm link" },
    { 85, "EDA", "EDEN Sistemas de Computaçao Ltda ED586/AT CSMA/CD comm link" },
    { 86, "DR2", "DECrouter-250 CSMA/CD comm link" },
    { 87, "SCC", "DECrouter-250 DUSCC serial comm link (DDCMP or HDLC)" },
    { 88, "DCA", "DCA Series 300 Network Processor CSMA/CD comm link" },
    { 89, "TIA", "Tiara Computers Systems: LANcard/E CSMA/CD controllers" },
    { 90, "FBN", "DECbridge-5xx CSMA/CD comm link" },
    { 91, "FEB", "DECbridge-5xx, -6xx FDDI comm link" },
    { 92, "FCN", "DECconcentrator-500 wiring concentrator FDDI comm link" },
    { 93, "MFA", "DEMFA XMI ~ FDDI comm link" },
    { 94, "MXE", "MIPS workstation family CSMA/CD comm links" },
    { 95, "CED", "Cabletron Ethernet Desktop Network Interface CSMA/CD comm link" },
    { 96, "C20", "3Com CS/200 terminal server CSMA/CD comm link" },
    { 97, "CS1", "3Com CS/1 terminal server CSMA/CD comm link" },
    { 98, "C2M", "3Com CS/210, CS/2000, CS/2100 terminal server CSMA/CD comm link" },
    { 99, "ACA", "Advanced Computer Applications ACA/32000 system CSMA/CD comm link" },
    { 100, "GSM", "Gandalf StarMaster 5855 Network Processor CSMA/CD comm link" },
    { 101, "DSF", "DSF-32 2 line synchronous comm link for Cirrus" },
    { 102, "CS5", "3Com CS/50 terminal server CSMA/CD comm link" },
    { 103, "XIR", "XIRCOM PE10B2 Pocket Ethernet Adapter CSMA/CD comm link" },
    { 104, "KFE", "VAXft-3000 KFE52 CSMA/CD comm link" },
    { 105, "RT3", "rtVAX-300 SGEC-based CSMA/CD comm link" },
    { 106, "SPI", "Spider Systems Inc. SPiderport M250 terminal server CSMA/CD comm link" },
    { 107, "FOR", "Forest Computer Inc. Connection System LAT gateway CSMA/CD comm link" },
    { 108, "MER", "Meridian Technology Corp CSMA/CD comm link drivers" },
    { 109, "PER", "Persoft Inc.  CSMA/CD comm link drivers" },
    { 110, "STR", "AT&T StarLan-10 twisted pair comm link" },
    { 111, "MPS", "MIPSfair SGEC CSMA/CD comm link" },
    { 112, "L20", "LPS20 print server CSMA/CD comm link" },
    { 113, "VT2", "Vitalink TransLAN 320 Bridge" },
    { 114, "DWT", "VT-1000 DECwindows terminal" },
    { 115, "WGB", "DEWGB Work Group Bridge CSMA/CD comm link" },
    { 116, "ZEN", "Zenith Z-LAN4000 Z-LAN comm link" },
    { 117, "TSS", "Thursby Software Systems CSMA/CD comm link drivers" },
    { 118, "MNE", "3MIN (KN02-BA) integral CSMA/CD comm link" },
    { 119, "FZA", "DEFZA TurboChannel FDDI comm link" },
    { 120, "90L", "DS90L terminal server CSMA/CD comm link" },
    { 121, "CIS", "cisco Systems terminal servers CSMA/CD comm link" },
    { 122, "STC", "STRTC Inc. terminal servers" },
    { 123, "UBE", "Ungermann-Bass PC2030, PC3030 CSMA/CD comm link" },
    { 124, "DW2", "DECwindows terminal II CSMA/CD comm link" },
    { 125, "FUE", "Fujitsu Etherstar MB86950 CSMA/CD comm link" },
    { 126, "M38", "MUXServer 380 CSMA/CD communication link" },
    { 127, "NTI", "NTI Group PC Ethernet card CSMA/CD comm link" },
    { 128, "LT2", "LPS20-turbo print server CSMA/CD comm link" },
    { 129, "L17", "LPS17 print server CSMA/CD comm link" },
    { 130, "RAD", "RADLINX LAN Gateway CSMA/CD comm link" },
    { 131, "INF", "Infotron Commix series terminal server CSMA/CD comm link" },
    { 132, "XMX", "Xyplex MAXserver series terminal server CSMA/CD comm link" },
    { 133, "NDI", "NDIS driver on MS-DOS" },
    { 134, "ND2", "NDIS driver on OS/2" },
    { 135, "TRN", "DEQRA token ring (802.5) comm link" },
    { 136, "DEV", "Develcon Electronics Ltr. LAT gateway CSMA/CD comm link" },
    { 137, "ACE", "Acer 5220, 5270 adapter CSMA/CD comm link" },
    { 138, "PNT", "PROnet-4/16 (802.5) comm link" },
    { 139, "ISE", "Network Integration Server 600 (Hastings) CSMA/CD line card" },
    { 140, "IST", "Network Integration Server 600 (Hastings) T1 sync line card" },
    { 141, "ISH", "Network Integration Server 600 (Hastings) 64 kb HDLC line card" },
    { 142, "ISF", "Network Integration Server 600 (Hastings) FDDI line card" },
    { 143, "DR1", "DECrouter-150 CSMA/CD comm link" },
    { 144, "SC1", "DECrouter-150 DUSCC serial comm link (DDCMP or HDLC)" },
    { 145, "FB3", "DECbridge-6xx CSMA/CD (3 port) comm link" },
    { 146, "CET", "Thomson CSMA/CD adapter for CETIA Unigraph" },
    { 147, "EIC", "ECI/FMR91515 CSMA/CD comm link" },
    { 148, "ETS", "Cabletron (Xyplex) ETSMIM terminal server CSMA/CD comm link" },
    { 149, "DSW", "DSW-21 single line serial comm link" },
    { 150, "DW4", "DSW-41/42 single/dual line serial comm link" },
    { 151, "ETW", "Etherworks (DE206) router CSMA/CD comm link" },
    { 152, "IBM", "IBM PS/2 adapter CSMA/CD comm link" },
    { 154, "ITC", "DEC/4000 (Cobra) TGEC based CSMA/CD comm link" },
    { 156, "ACS", "DECserver 700 (Whitewater) terminal server CSMA/CD comm link" },
    { 157, "9LP", "DECserver-90L+ CSMA/CD comm link" },
    { 158, "92M", "DECserver-90TL CSMA/CD comm link" },
    { 159, "SSL", "Spider Systems SL8, SL16 CSMA/CD comm link" },
    { 160, "FTA", "DEFTA Turbochannel-plus adapter FDDI comm link" },
    { 161, "FAA", "DEFAA Futurebus+ adapter FDDI comm link" },
    { 162, "FEA", "DEFEA EISA bus adapter FDDI comm link" },
    { 163, "FIA", "DEFIA ISA bus adapter FDDI comm link" },
    { 164, "FNA", "DEFNA S-bus adapter FDDI comm link" },
    { 165, "NMA", "DENMA DEChub-90 network management agent CSMA/CD comm link" },
    { 166, "M32", "MUXServer 320 CSMA/CD communication link" },
    { 167, "90W", "WANrouter-90 multiprotocol router CSMA/CD comm link" },
    { 168, "9WS", "WANrouter-90 multiprotocol router DDCMP/HDLC comm link" },
    { 169, "FQA", "DEFQA Q-bus adapter FDDI comm link" },
    { 170, "A35", "DEC/3000 model 400/500 (Sandpiper/Flamingo) Alpha AXP workstation CSMA/CD comm link" },
    { 172, "V49", "VAXstation 400 model 90 workstation CSMA/CD comm link" },
    { 173, "IS4", "NIS400 bridge/router CSMA/CD comm link" },
    { 174, "I4E", "NIS400 bridge/router Ethernet option module CSMA/CD comm link" },
    { 175, "TRA", "DETRA-AA Turbochannel 802.5 token ring comm link" },
    { 176, "TRB", "DETRA-BA Turbochannel 802.5 token ring comm link" },
    { 177, "MX9", "MUXserver 90 CSMA/CD comm link" },
    { 178, "90M", "DECserver-90M CSMA/CD comm link" },
    { 179, "M9S", "MUXserver 90 synchronous (HDLC/DDCMP) comm link" },
    { 180, "FEN", "DECserver 900-04 CSMA/CD comm link" },
    { 181, "FGL", "Gigaswitch DEFGL line card FDD comm link" },
    { 182, "ERA", "DE422 EISA-bus PC CSMA/CD comm link" },
    { 183, "RMN", "DECpacketprobe 90 Ethernet RMON agent CSMA/CD comm link" },
    { 184, "TMN", "DECpacketprobe 900 Token Ring RMON agent 802.5 comm link" },
};
const Name datalinks[] = {
    { 1, "", "CSMA-CD" },
    { 2, "", "DDCMP" },
    { 3, "", "LAPB (frame level of X.25)" },
    { 4, "", "HDLC" },
    { 5, "", "FDDI" },
    { 6, "", "Token-passing Ring (IEEE 802.5)" },
    { 11, "", "Token-passing Bus (IEEE 802.4)" },
    { 12, "", "Z-LAN 4000: Zenith 4 Megabit/second broadband CSMA/CD LAN" },
};
const Name processors[] = {
    { 1, "", "PDP-11 (UNIBUS)" },
    { 2, "", "Communication Server" },
    { 3, "", "Professional" },
};

template <std::size_t N>
const Name *find (const Name (&table)[N], unsigned code)
{
    for (const Name &n : table)
        if (n.code == code) return &n;
    return nullptr;
}

}   // namespace

std::string device_name (unsigned code)
{
    const Name *n = find (devices, code);
    return n ? n->name : std::to_string (code);
}

std::string device_short_name (unsigned code)
{
    const Name *n = find (devices, code);
    return n && *n->short_name ? n->short_name : std::to_string (code);
}

std::string processor_name (unsigned code)
{
    const Name *n = find (processors, code);
    return n ? n->name : std::to_string (code);
}

std::string datalink_name (unsigned code)
{
    const Name *n = find (datalinks, code);
    return n ? n->name : std::to_string (code);
}

}   // namespace decnet::mop

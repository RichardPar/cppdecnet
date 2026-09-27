# RSX bits

Odds and ends for poking at cppdecnet from a real RSX-11M-PLUS box.
Built and tested on BAJI (RSX-11M-PLUS V4.6 BL87, DECnet-11M-PLUS).

## TSTIME

Asks the TIMESTAMP object on one or more nodes what time it is. Sends
"UA" and prints what comes back, then "UB" and prints the 8 byte VMS
binary time as octal words, low word first.

```
>TSTIME CPPNOD A29RT1
CPPNOD:: 27-SEP-2026:14:57:50.86
CPPNOD:: 152066 003733 031031 000274
A29RT1:: 27-SEP-2026:14:57:51.74
A29RT1:: 165470 004147 031031 000274
```

Handy for checking two clocks against each other. A round trip over
HECnet is most of a second, so the second node always looks late - run
it both ways round and split the difference. Spaces or commas between
names, and "NODE::" works too. `RUN TSTIME` with no arguments asks for
the node list.

It connects by name, so RSX needs to know the node. If it doesn't:

```
>NCP SET NODE 29.150 NAME CPPNOD
```

(SET is gone after a reboot, use DEFINE if you want it to stick.)

If something fails you get the node, a step number (1 open, 2 connect,
3-6 send/receive) and the status, and it moves on to the next node.

TSTIME.TSK in here is the image I built, so you can skip the rest if
you just want to run it. FTP it over in binary mode.

## Building

You need MAC, TKB, and LB:[1,1]NETLIB.MLB from the DECnet kit. Get
TSTIME.MAC onto the box first - FTP in ASCII mode is easiest.

No FTP? `PIP TSTIME.MAC=TI:`, paste, ^Z. Paste slowly though. At
19200 on my console anything quicker than ~4ms a character gave me a
data overrun, PIP gave up, and the rest of the file got fed to MCR as
commands. Harmless but messy. Check the file size afterwards.

Assemble:

```
>MAC TSTIME,TSTIME/-SP=LB:[1,1]NETLIB/ML,SY:TSTIME
```

No output means no errors. If it says "Errors detected", look in
TSTIME.LST (`PIP TI:=TSTIME.LST` - not /SP, that sends it to the
printer, ask me how I know).

Task build:

```
>TKB
TKB>TSTIME/-FP=TSTIME
TKB>/
Enter Options:
TKB>ASG=NS0:1:2,TI0:5
TKB>//
```

LUN 1 is the network, LUN 2 the link, 5 the terminal.

Install it as ...TST so MCR hands it the command line:

```
>INS TSTIME/TASK=...TST
>TSTIME CPPNOD
```

REM ...TST before installing a new build. It won't survive a reboot
either, so put the INS in your startup file if you want it permanent.

## Getting the .TSK back off the box

FTP in binary mode. Failing that, `DMP TI:=TSTIME.TSK/BL:1:9.` and
turn the octal back into bytes (low byte first). Note the dot - DMP
assumes octal otherwise and 9 isn't an octal digit. I dumped it twice
and compared, a flipped digit on the serial line would be easy to miss.

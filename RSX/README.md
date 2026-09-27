# RSX bits

Odds and ends for poking at cppdecnet from a real RSX-11M-PLUS box.
Built and tested on BAJI (RSX-11M-PLUS V4.6 BL87, DECnet-11M-PLUS).

## TSTIME

Asks the TIMESTAMP object on a node what time it is and prints it the
way TIM does, but with the full year. Under the hood it sends "UB",
gets back a VMS quadword (100ns ticks since 17-NOV-1858), and turns
that into an RSX date and time.

```
>TSTIME CPPNOD
15:34:20 27-SEP-2026
>TSTIME CPPNOD,A29RT1
15:34:30 27-SEP-2026
15:34:31 27-SEP-2026
```

More than one node gives one line each, in the order given. Spaces or
commas between names, and "NODE::" works too. `RUN TSTIME` with no
arguments asks for the node list.

The answer is the remote node's clock, which for CPPNOD and A29RT1 is
UTC - so don't expect it to agree with TIM on a box set to local time.
The date sums only cope with 2000-2099, which should do.

It connects by name, so RSX needs to know the node. If it doesn't:

```
>NCP SET NODE 29.150 NAME CPPNOD
```

(SET is gone after a reboot, use DEFINE if you want it to stick.)

If something fails you get the node, a step number (1 open, 2 connect,
3 send, 4 receive) and the status, and it moves on to the next node.

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

FTP in binary mode. Failing that, `DMP TI:=TSTIME.TSK/BL:1:10.` (however
many blocks DIR says it is) and turn the octal back into bytes, low
byte first. Note the dot - DMP assumes octal otherwise. I dumped it twice
and compared, a flipped digit on the serial line would be easy to miss.

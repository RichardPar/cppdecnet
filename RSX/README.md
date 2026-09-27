# RSX bits

Odds and ends for poking at cppdecnet from a real RSX-11M-PLUS box.
Built and tested on BAJI (RSX-11M-PLUS V4.6 BL87, DECnet-11M-PLUS).

## TSTIME

Asks the TIMESTAMP object on a node what time it is and prints it the
way TIM does, but with the full year. It can also set the clock from
it.

```
>TSTIME CPPNOD
17:23:20 27-SEP-2026
>TSTIME CPPNOD,A29RT1
17:11:13 27-SEP-2026
17:11:14 27-SEP-2026
>TSTIME CPPNOD/SET
17:11:20 27-SEP-2026
```

Under the hood it sends "UB" and gets back UTC as a VMS quadword (100ns
ticks since 17-NOV-1858). It adds SYS$UTC_OFFSET, the local offset in
minutes that LB:[1,2]TZ.CMD looks after, so what you see is local time
and matches TIM. The part of a second is worked out in real clock
ticks, using the rate GTIM$ reports.

More than one node gives one line each. Spaces or commas between
names, and "NODE::" works too. With /SET it sets the clock from the
first node that answers and stops there. The date sums only cope with
2000-2099, which should do.

It connects by name, so RSX needs to know the node. If it doesn't:

```
>NCP SET NODE 29.150 NAME CPPNOD
```

(SET is gone after a reboot, use DEFINE if you want it to stick.)

If something fails you get the node, a step number (1 open, 2 connect,
3 send, 4 receive, 5 setting the clock) and the status, and it moves
on to the next node.

## Keeping the clock right

Run with no command line at all it takes the nodes from the logical
TSTIME$NODE and sets the clock without printing anything unless it
fails. That's what you schedule. RUN won't touch a ...XXX task, so it
gets installed a second time as TSTSYN:

```
>INS TSTIME/TASK=TSTSYN
>DFL CPPNOD=TSTIME$NODE/GBL
>RUN TSTSYN 1M/RSI=10M
```

USERPROG.CMD in here does all of that at boot - STARTUP.CMD calls
LB:[1,2]USERPROG.CMD at the end if it exists. CLQ shows it queued.



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
TKB>TSTIME/-FP/PR:0=TSTIME
TKB>/
Enter Options:
TKB>ASG=NS0:1:2,TI0:5
TKB>//
```

LUN 1 is the network, LUN 2 the link, 5 the terminal. /PR:0 makes it
privileged, which STIM$ needs - without it /SET fails with status
177760 (IE.PRI).

Install it as ...TST so MCR hands it the command line:

```
>INS TSTIME/TASK=...TST
>TSTIME CPPNOD
```

REM ...TST (and TSTSYN) before installing a new build. It won't survive a reboot
either, so put the INS in your startup file if you want it permanent.

## Getting the .TSK back off the box

FTP in binary mode. Failing that, `DMP TI:=TSTIME.TSK/BL:1:9.` (however
many blocks DIR says it is) and turn the octal back into bytes, low
byte first. Note the dot - DMP assumes octal otherwise. I dumped it twice
and compared, a flipped digit on the serial line would be easy to miss.

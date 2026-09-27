.; USERPROG.CMD -- local additions, run by STARTUP.CMD once DECnet is up.
.;
.; Keep the clock in step with CPPNOD's TIMESTAMP object.  TSTIME is
.; installed twice: ...TST so TSTIME can be typed at MCR, and TSTSYN
.; because RUN won't schedule a ...XXX task.  Set it straight away, since
.; nothing else has - the boot time prompt just times out - then keep it
.; right.
	INS LB:[1,1]TSTIME/TASK=...TST
	INS LB:[1,1]TSTIME/TASK=TSTSYN
	NCP SET NODE 29.150 NAME CPPNOD
	DFL CPPNOD=TSTIME$NODE/GBL
	TSTIME CPPNOD/SET
	RUN TSTSYN 10M/RSI=10M

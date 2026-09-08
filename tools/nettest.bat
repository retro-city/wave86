@echo off
rem the network chain of the real machine: packet driver, mTCP DHCP, a list from waveserve.
rem (a redirect on an IF line runs even when the test is false, hence the goto)
NE2000 0x60 3 0x300 > C:\RESULTS\NE2000.TXT
set MTCPCFG=C:\WAVE86\MTCP.CFG
DHCP > C:\RESULTS\DHCP.TXT
WAVEGET LIST 10.0.2.2:8086 C:\RESULTS\LIST.TXT > C:\RESULTS\WAVEGET.TXT
if exist C:\RESULTS\LIST.TXT goto ok
echo no list from the server > C:\RESULTS\FAIL.TXT
:ok

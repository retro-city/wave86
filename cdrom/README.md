# CD image drivers for real DOS

Jason Hood's SHSUCD suite (https://github.com/adoxa/shsucd, commit 5ea0787,
zlib-style licence in LICENSE.txt), assembled here from its NASM sources:

    SHCDHD86.EXE   SHSUCDHD 3.01: presents an image file as a CD-ROM device
    SHCDX86.COM    SHSUCDX 3.09: MSCDEX replacement that gives it a letter

These are the 8086 builds, which run on anything. If SHSUCDX.COM and
SHSUCDHD.EXE (the 386 builds) sit next to the launcher it prefers them on
a 386 or better. The launcher puts, around a game that has a CD image,

    SHSUCDHD /F:C:\GAMES\SYNDICAT\CD\SYNDICAT.ISO /Q
    SHSUCDX /D:SHSU-CDH,D /Q
    ... the game ...
    SHSUCDX /U /Q
    SHSUCDHD /U /Q

To rebuild: `nasm -O9 -Di8086 -i ./ -o SHCDX86.COM shsucdx.nsm` and
`nasm -O9 -Di8086 -i ./ -fobj shsucdhd.nsm` followed by
`wlink system dos file shsucdhd.obj name SHCDHD86.EXE` (Open Watcom's
linker stands in for ALINK). Leave out -Di8086 for the 386 builds; those
need NASM 0.98.39, since NASM 3 rejects the size-less operands of the
suite's `movzx.` macro.

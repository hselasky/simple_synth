# Simple synth

## Features

- different waveforms
- ressonators
- different scales

## How to build
<PRE>
make PREFIX=/usr/local all install
</PRE>

## How to run
<PRE>
mkfifo /midi

simple_synth -d /midi -f /dev/dsp -r 48000 -S 27.5,12,0.92 -w 0.7
</PRE>

## Supported platforms
- FreeBSD

## Privacy policy
Simple synth does not collect any information from its users.

## Software support
If you like this software, want particular improvements or have it ported
to your platform please contact <A HREF="mailto:hps&#x40;selasky.org">hps&#x40;selasky.org</A>.

<PRE>
--HPS
</PRE>

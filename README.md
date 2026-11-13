# ManateeHost

A manatee.drv 'host' that takes MIDI input and lets you hear it with loaded manatee banks in real-time. Made to help with editing or creating MIDIs for Rez (and it should work for other Dreamcast games that use Manatee v2). AICA/ARM emulation provided by [Highly Theoretical](https://gitlab.com/kode54/highly_theoretical/) and using WinMM MIDI with [libremidi](https://github.com/celtera/libremidi).

## Build
* Make sure to clone with submodules or run ``git submodule update --init --recursive`` in the root.
* Build with `build.cmd` and VS 2022 x64 build tools.

## Usage
* Start loopMIDI and create a port called 'ManateeHost' (or, if you want to use a custom name, you can override using ``--port``).
* Set your DAW to send MIDI output to this port.
* Open ManateeHost and load manatee.drv and banks. You can also launch ManateeHost with ``--port "name"`` to use a different input. Or launch with ``--list-ports`` to see all the MIDI ports available as input.
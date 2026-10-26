# Hamlib in Eye of ZorCom

Eye of ZorCom ships **Hamlib** (https://hamlib.github.io/, by the Hamlib group and contributors) for CAT control of amateur and commercial transceivers (frequency, mode, PTT) with IQ or audio from a sound card.

- **Licence:** GNU Lesser General Public License, version 2.0 or later (the licence text is in `COPYING` in the same folder when the package is built).
- **Linked library:** Hamlib is linked into Eye of ZorCom (`core/src/source/source_hamlib.cpp`). Linux packages place `libhamlib` in `lib/zorcom` next to the other radio libraries; macOS packages place it in `ZorCom.app/Contents/Frameworks`.
- **Windows:** official Windows packages do not yet bundle Hamlib; build from source with Hamlib installed or use Linux/macOS packages for CAT rig support.

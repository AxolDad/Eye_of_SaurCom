# SDDC_FX3.img (RX888 MkII USB firmware)

Eye of SaurCom copies this file into its `codecs` folder in official packages. The RX888 MkII Cypress FX3 has no
persistent program: the host loads this image into **RAM** when the radio enumerates as `04b4:00f3`. Nothing is
written to the radio's flash; unplugging clears it (same as HDSDR / SDDC tools).

Source: [SDDC_Driver](https://github.com/renardspark/SDDC_Driver) release **v2.0.0** (`SDDC_FX3.img`, 145868 bytes):
<https://github.com/renardspark/SDDC_Driver/releases/tag/v2.0.0>. The firmware source is in
<https://github.com/ik1xpv/ExtIO_sddc> (folder `SDDC_FX3`). The SDDC projects are MIT-licensed (`LICENSE-SDDC_Driver.txt`); the image
contains Cypress/Infineon EZ-USB FX3 SDK object code, which that SDK's licence allows to be redistributed in firmware for FX3 devices.

**Get it yourself instead:** if you do not want Eye of SaurCom's copy, download `SDDC_FX3.img` from the release link above and put it in Eye of SaurCom's
`codecs` folder (replacing ours), or delete our copy; Eye of SaurCom uses whichever file is there.

Do not replace this file with a random download unless it is the official SDDC_FX3.img from that project.

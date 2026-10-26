# RF camera

The RF camera lays the direction finder's bearing spectrum over a picture as a heat map, so you see roughly where in a scene a signal comes from. It is a section of the Direction finder window (View > Direction finder > RF camera, tick "Enable RF camera").

Azimuth only. The array is planar, so there is no elevation estimate: the heat map is the same down every column of the picture, and the picture is a backdrop, not a measurement. Nothing here has been tried with real hardware; the checks below are with the built-in simulator and synthetic signals.

## How it works

- The direction finder's capture (4096 snapshots per update, about 8 updates a second) is split into sub-bands (default 16) with a Hann-windowed DFT of 256 points. Each sub-band gets its own covariance and its own estimate (the Estimator chosen in the window: MUSIC, Capon, Bartlett, interferometer), so two transmitters on different frequencies each keep their bearing and their frequency.
- A sub-band counts as occupied when its dominant eigenvalue is more than 3 dB above the median of the others. Its spectrum is weighted by how far above (0 at 3 dB, 1 at 23 dB) and the heat map is the maximum over the bands.
- Peaks carry the centre frequency of their sub-band and are shown as markers with a frequency and bearing label; close peaks (3 degrees) are merged, labels are stacked in rows so they do not overlap, weak peaks are hidden ("Weakest shown").
- Band filter: "Band from / to (kHz)" relative to the tuned frequency limits the analysis to part of the sample rate (both 0 = all of it).

## Pictures

| Source | Notes |
| --- | --- |
| Demo skyline | A drawn 360 degree panorama (no camera needed). Works with the simulator. |
| Picture or panorama file | PNG everywhere, other formats where the system decoder reads them (macOS: JPEG, HEIC ...). The file is checked about once a second and reloaded when it changes, so a program that keeps overwriting a still (a webcam grabber, ffmpeg writing a frame) gives a live view. |
| Camera snapshot URL | A camera's JPEG snapshot address, fetched with curl about once a second. Streaming MJPEG and video files are not decoded: use the camera's snapshot address or a grabber that rewrites a still. |

Eye of SaurCom has no webcam capture of its own (no AVFoundation or Media Foundation camera code). Wide pictures are halved to 1280 pixels or less.

## Angle to pixel mapping and calibration

- Normal camera lens: pinhole model, column = w/2 + f tan(azimuth - centre), f from the horizontal field of view.
- Panorama (equirectangular): columns are linear in azimuth; 360 degrees covers the whole width and wraps.
- Set the field of view and the azimuth of the centre column by hand, or use "Calibrate from two landmarks": give the picture column and the true azimuth of two things you can see (click the picture to fill the column), and the field of view and centre are solved. Compass offset in the Direction finder window applies to the bearings before they are drawn.

## Snapshot

"Save snapshot" writes `rfcamera_<date>_<time>.png` (picture with heat map and marker ticks) and a `.txt` with azimuth, frequency and strength per peak into the Eye of SaurCom data folder.

## Tests

`tests/df/test_df_rfcam.cpp`: angle to pixel mapping and round trips, wrap across north, panorama, two-landmark calibration (lens and panorama), two narrowband simulated transmitters at different bearings and frequencies recovered with their frequencies, the band filter, the compass offset, noise only gives no strong peaks, heat map blending, marker merging/ordering/label rows. The simulator got a `bandwidthHz` option for narrowband messages (default 0 = as before).

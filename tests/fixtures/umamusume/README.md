# Umamusume scrollbar regression images

Nine original PNG screenshots supplied by the user for detector validation. The duplicate 15.39.45 image was retained only once. Source filenames preserve the capture timestamps; `image.png` is the wide layout. These are one screen family at two portrait resolutions and one wide resolution, not a representative benchmark for every game screen or application.

Pixel inspection located the visible track cores:

| Frame size | Track core x | Track core y (inclusive) |
| --- | --- | --- |
| 569 × 1023 | 546–550 | 189–707, extending to 710 in the bottom-thumb sample |
| 711 × 1279 | 685–689 | 235–885 |
| 1917 × 1115 | 841–845 | 209–772 |

Rounded/antialiased edges extend beyond these core pixels. The test allows four pixels of uncertainty for strip location and vertical endpoints, bounds the detected strip width, checks the first-ranked candidate, and checks that discovery and tracking agree. Individual thumb annotations are in `tests/scrollbar_corpus_tests.cpp`; they were checked against original image pixels rather than generated from detector outputs.

Run through CTest as `stitch_scrollbar_corpus_tests`. No network is required. The images are not installed into the application package.

The wide frame previously ranked a window-edge artifact first. Its actual scrollbar was second, had a track extending through the white header/footer, and its long thumb inverted into a track gap during tracking. Synthetic tests separately cover 75%, 86%, and 95% thumb occupancy at top, middle, and bottom positions.

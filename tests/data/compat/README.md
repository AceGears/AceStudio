# Projects written by older AceStudio versions

Each `eraN.acestudio` was saved by an older AceStudio core, and each
`eraN_frameK.png` is that same version's render of frame K. They were made by
compiling `tests/compat/make_fixture.cpp` against these commits:

| File  | Version | Commit  | Schema | Contents |
|-------|---------|---------|--------|----------|
| era1  | 0.1.0   | 8c5936c | 1 | groups, all blend modes, opacity, hidden layer, cel offsets, frame durations, a ping-pong tag |
| era2  | 0.5.0   | 0a0f270 | 2 | era1 + layer mask, clipping, Hue/Saturation and Posterize adjustment layers, tile mode |
| era3  | 0.8.1   | c23aa9b | 3 | era2 + Levels and Curves adjustments, linked cels, palette |

`test_regression.cpp` opens each with the current build and requires identical
pixels. One documented exception: 0.2.0 changed group compositing on purpose
(groups composite in isolation, W3C semantics), so in era1 pixels under group
content may differ; everything else must match.

To add a fixture for a new release, build the generator against that release
(`ERA` selects the content) and commit the outputs; never regenerate an
existing one with a newer version.

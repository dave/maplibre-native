# Contour fixtures

Real terrarium DEM tiles (512 px WebP) for the contour tests in
`test/tile/contour_fixtures.test.cpp`:

- `11_608..610_1340..1342.webp`: the 3×3 block around 11/609/1341, the
  Southern Patagonian Ice Field.
- `11_1067_728.webp`: the Matterhorn.

They come from the Viatrail elevation archive
`https://tiles.viatrail.app/2026-10-13/elevation.pmtiles` (the tiles as served,
unchanged).

`expected-z11.png` and `expected-z11.4.png` are the contour lines of the
3×3 block at the corner of 11/609/1341, drawn by fork 52b261a0e374 (before
the contour work was made cheaper). `MLN_CONTOUR_UPDATE=1` writes them again.

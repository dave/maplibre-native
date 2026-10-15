# Label lane: Viatrail GPT bodies

The placement and names bodies Viatrail sets on its two label sources, for the
headless check in `test/api/label_lane_real_data.test.cpp`.

Exported 2026-10-15 from the Viatrail app repo at commit `1b45c326aa`
(`feature/map`): `go/pkg/core/placement`'s GPT fixture
(`testdata/gpt_candidates.json.gz`, sha-256 `e07fb2fd…2d662`), solved with the
package's test sizer at the Pixel 7 view (1080 × 2400 px, 2.625 px per dp)
centred on -48.65, -72.85, and encoded with `Encode` and `EncodeNames`, as the
apps set them. The device runs of the drag study used the same camera.

| File | Zoom | Features |
|---|---|---|
| `gpt-placement-z10.json` | 10 | 190 placed, and the solve's marker |
| `gpt-names-z10.json` | 10 | 41 names |

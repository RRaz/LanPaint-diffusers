# Affinity easy inpaint vs LanPaint

Three frames kept from the quick comparison. They are qualitative, not a
controlled benchmark: the source frame is the same, but the selections and
settings were not instrumented.

| File | What it is |
|---|---|
| `Affinityinpaint.jpg` | Affinity Photo's quick selection and easy inpaint. |
| `inpaint.jpg` | The LanPaint result, including the added bush. |
| `inpaint with mask.jpg` | The selection/mask overlay used to show the region. |

Notes for anyone reading the images:

- The yellow border and the yellow painted area are **Affinity's selection/mask
  display**, not part of either render. They are there so the region is visible
  in the record.
- The yellow overlay was produced from a quick selection, not a hand-painted
  mask. The LanPaint plugin itself uses the layer's alpha channel; the overlay
  is only the visualisation.
- The photograph is a scan of a 47-year-old Kodachrome slide. The people in it
  are from the slide and are cleared for this public archive.
- The bush arrived with a potted base — the model's “something stands here”
  prior. It is a prompting example, not a mask or compositing fault. See the
  post-mortem.

The comparison is the basis of the verdict in
[`../POST-MORTEM.md`](../POST-MORTEM.md): for a small local edit, Affinity was
comparable and far faster.

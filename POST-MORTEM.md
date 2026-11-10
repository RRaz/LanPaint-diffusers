# Post-mortem: LanPaint for Affinity

**Status: archived, November 2026.** The filter was built, tested and retired.
This repository is kept for the record; it is not maintained.

## What was attempted

A Photoshop-type `.8bf` filter for Affinity Photo that sent a 16-bit layer to
LanPaint with SD3 Medium and used the layer's alpha channel as the inpainting
mask. The mask was grown, the result was pasted back through a distance ramp,
and the whole layer was worked at its own scale — never silently reduced.

The goal was a practical retouching tool for real photographs: remove objects,
replace surfaces and add material inside a selection while keeping colour and
exposure.

## What worked

- **Masked inpainting at strength 1.0** did the job on small, well-chosen edits.
  A lamp post was removed and a bush was placed in the right area in one pass.
- **Grey alpha** (anything below fully opaque) counted as selected; a soft
  selection was picked up rather than half-ignored at a 50% cutoff.
- **The grown mask and inward ramp** removed the old object's rim and hid the
  model's seam. An irregular mask on foliage was a good demonstration: the
  model drew a little beyond the painted shape, and the grown mask kept it.
- **Whole-layer context** was more robust than a tight crop. The last rows and
  columns needed padding and a margin so the model's edge blend fell outside the
  picture; once added, the edge problem disappeared.
- **The dialog** measured the selection before the prompt, showed the working
  size, offered cancel, guarded the experimental unmasked mode, and kept a
  history of prompt/negative pairs.

Measured on an RTX 3060 12 GB, SD3 Medium, 20 steps, `offload=1`:

| Image | Working size | Total | Sampling | Peak VRAM |
|---|---|---|---|---|
| 512 x 512 | 512 x 512 | 252 s | 76 s (1.3 s/step) | 5.9 GB |
| 1280 x 1024 | 1280 x 1024 | 539 s | 185 s (6.7 s/step) | 11.9 GB |
| 2048 x 1536, 320 x 220 selection | 1136 x 928 | 446 s | 148 s (5.1 s/step) | 6.9 GB |

Model load alone cost 80–110 s per run. Nothing shortens that except a resident
service, which was not built.

## Why it was canned

1. **Too slow and complicated for removal.** Five to eight minutes per run
   against seconds in Affinity, with similar quality for small objects.
   The model-load cost dominates and rules it out as a quick retouch tool.
2. **Large objects are a poor fit.** Diffusion inpainting has no structural
   model of what it is replacing; Affinity struggles too, and compositing is the
   better tool for both.
3. **Text is too vague for a specific silhouette.** The bush example worked, but
   the result was hit-and-miss: a prompt sets a direction, not a specification.
   Compositing gives the control the model cannot.
4. **The model's weak areas are not fixable in the plugin.** A figure test
   exposed a strong anatomical prior; neither low-strength img2img (which kept
   the shape) nor text-to-image (which repeated the same prior) corrected it.
   That is a model/workflow limitation, not a filter bug.

The small potted plant that appeared at a removed lamp base was a prompting
example rather than a mask failure: the model completed “something stands on
paving” with the most plausible object. Naming the replacement, or adding
`plant pot, planter, bucket, bin, post base` to the negative, is the right fix.
Compositing avoids the question entirely.

## What is worth keeping

The reusable part is the mask and composite logic, not the model bridge:

- mask bounding box and crop planning;
- grey-to-binary mask handling;
- mask growth and the distance-based inward ramp;
- padding to the VAE grid with an edge margin;
- paste-back that leaves everything outside the grown mask byte-identical.

That fragment lives in [RRaz/mask-blend](https://github.com/RRaz/mask-blend),
with synthetic tests for soft edges, irregular and multiple regions, and masks
that touch the frame.

## Comparison images

`docs/comparison/` holds the three frames from the quick comparison:

| File | Label |
|---|---|
| `Affinityinpaint.jpg` | Affinity Photo, quick selection and easy inpaint |
| `inpaint.jpg` | LanPaint result, with the added bush |
| `inpaint with mask.jpg` | the selection/mask overlay used to show the region |

The yellow frame is Affinity's selection/mask display, not part of the render.
The photograph is a scan of a 47-year-old Kodachrome slide; the people in it
are from the slide and are cleared for this public archive.

The comparison supports the verdict: for a small local edit, Affinity was
comparable and far faster. The bush shows where text-driven generation is
useful for material and light, and where it is too vague for an exact shape.

## Lessons

- Do not build a per-run model bridge for interactive retouching: load time
  alone decides the experience.
- Compare against the host application's built-in tools first. If the result is
  comparable and the plugin is slower, stop before building the UX.
- Use text for material, colour and light; use compositing for an exact shape.
- Test the model on its weak subjects before investing in the integration.
- Keep prompt and negative history from the first run; recoverability matters
  more than it seems at the time.

## Licence

GPL-3.0, as upstream. The upstream project is
[charrywhite/LanPaint-diffusers](https://github.com/charrywhite/LanPaint-diffusers)
and [scraed/LanPaint](https://github.com/scraed/LanPaint).

# LanPaint for Affinity

> **Archived (November 2026).** This project is retired; see
> [POST-MORTEM.md](POST-MORTEM.md) for the verdict and the reasons. The reusable
> mask/composite fragment is in
> [RRaz/mask-blend](https://github.com/RRaz/mask-blend).

A working project built on
[charrywhite/LanPaint-diffusers](https://github.com/charrywhite/LanPaint-diffusers):
a Photoshop-type filter that runs LanPaint on a 16-bit layer inside Affinity
Photo, using the layer's **alpha channel as the inpainting mask**.

The upstream repository is GPL-3.0. Everything added here is under the same
licence.

## What is here

| Path | What it is |
|---|---|
| `lanpaint_pipeline/` | upstream library, unchanged |
| `run_lanpaint.py` | upstream CLI, unchanged |
| `affinity_runner.py` | bridge: raw RGBA16 in, alpha as mask, raw RGB16 out |
| `affinity/` | the filter: `lanpaint.c`, `lanpaint.rc`, build script, ini |
| `test/` | a 512x512 case built from the upstream example image |

## Environment

Python 3.12 in `.venv`, PyTorch 2.8.0 with CUDA 12.8. The upstream
`requirements.txt` cannot be installed as written: it pins `torch==2.8.0` with
`torchvision==0.25.0`, which conflict, and its `huggingface-hub` and
`transformers` pins conflict with current diffusers. The environment therefore
uses:

- `torch==2.8.0+cu128`, no torchvision (nothing imports it)
- `diffusers 0.42.0.dev0` from git, `transformers 5.22.2`, `huggingface-hub 2.4.0`
- `LanPaint 2.1.0` from git

Rebuild it with:

```
uv venv --python 3.12 .venv
.venv\Scripts\python.exe -m pip install torch==2.8.0 --index-url https://download.pytorch.org/whl/cu128
.venv\Scripts\python.exe -m pip install "git+https://github.com/scraed/LanPaint.git" ^
    "git+https://github.com/huggingface/diffusers.git" transformers accelerate ^
    safetensors numpy Pillow tqdm requests huggingface-hub scipy sentencepiece protobuf
```

## Models

| Key | Model | Works on a 12 GB card |
|---|---|---|
| `sd3` | Stable Diffusion 3 Medium | yes, with offloading |
| `z-image` | Z-Image Turbo | with offloading, slower |
| `flux-klein` | Flux2 Klein 9B | heavy offloading |
| `krea2` | Krea 2 Turbo | heavy offloading |
| `qwen` | Qwen Image Edit 2509 | heavy offloading |

SD3 is gated on Hugging Face: the licence has to be accepted by the account and
a read token placed in `%USERPROFILE%\.cache\huggingface\token`. Fetch the fp16
weights and the configuration only — the full repository is 31 GB, half of it
fp32 files that are never used:

```
hf download stabilityai/stable-diffusion-3-medium-diffusers ^
    --include "*fp16*" --include "*.json" --include "*.txt" --include "*.model"
```

The `*fp16*` pattern matters: the T5 encoder ships as `model.fp16-00001-of-00002`
and the like, which a plain `*.fp16.safetensors` pattern misses.

Once the files are on disk, `models\sd3` holds a plain copy of them and
`model_id` in `lanpaint.ini` points at that folder. Nothing is fetched at run
time then, and the token is only needed for the download.

`affinity_runner.py` loads the pipeline with `enable_model_cpu_offload()`, which
keeps one sub-model on the card at a time. That is what makes SD3 Medium fit in
12 GB, at the cost of moving tensors between memory and card during a run.

## Measured on this machine

RTX 3060 12 GB, SD3 Medium, 20 steps, `offload=1`:

| Image | Working size | Total | Sampling | Peak VRAM |
|---|---|---|---|---|
| 512 x 512 | 512 x 512 | 252 s | 76 s (1.3 s/step) | 5.9 GB |
| 1280 x 1024 | 1280 x 1024 | 539 s | 185 s (6.7 s/step) | 11.9 GB |
| 2048 x 1536, smart crop, 320 x 220 selection | 1136 x 928 crop | 446 s | 148 s (5.1 s/step) | 6.9 GB |

The 1.3 MP case sits at the card's edge. Anything above `max_area` is refused
rather than reduced: crop the layer in Affinity first, then merge the result back.

## Public release

- **Code** stays GPL-3.0, as upstream, with the source published.
- **Weights are not shipped.** SD3 Medium is under Stability's non-commercial
  licence: each user accepts it on Hugging Face, creates a read token, and the
  first run downloads about 15.5 GB. Bundling the weights would mix a
  non-commercial model with GPL code.
- The filter works with any diffusers model through `model` and `model_id`;
  Z-Image Turbo and Qwen Image Edit are Apache-2.0 if a permissive default is
  ever wanted.

## The working area

The whole layer goes to the model at its own size. Nothing is scaled: the layer
must fit `max_area` (2 MP), otherwise it is refused and should be cropped in
Affinity first. That is the normal workflow, since Affinity's crop is
non-destructive and the result merges back as an ordinary layer.

Sides are padded to the VAE's 16-pixel grid, with a small extra margin on a
whole-layer run, then cropped back. The model's own mask boundary therefore falls
outside the picture, so the last rows and columns are not caught in its edge
blend when a selection runs to the frame.

Sending the whole layer also gives the model the full frame as context, so an
edit near an edge is not judged from a narrow window around the selection.

**Smart crop** is the optional alternative (`smart_crop=1`), for a layer too large
to pre-crop conveniently. It sends only the area around the selection, still at
its own scale:

1. Find the smallest rectangle holding every painted pixel.
2. If that rectangle is larger than **0.6 MP** (`selection_limit`), refuse with a
   message asking for a smaller selection.
3. Grow it: half the short side on every side, at least 32 px, then out to the
   **1 MP** target (`crop_area`). A long, thin selection gains most of its extra
   room across the short side, which is where it lacks context.
4. Place the crop so it always contains the selection and always stays inside the
   picture — a selection on an edge or in a corner is clamped, not extended past
   the frame. Run the model on that crop at its own scale, then paste the result
   back through the mask.

Only painted pixels change; everything outside the selection keeps its original
pixels exactly.

Inside the working area the edit area is grown by `mask_grow` pixels (8 by
default). The model then draws a little past the selection, so the soft seam the
VAE leaves at the edge of its own mask falls outside the area that is pasted
back. That seam, not the compositing, is what produced a dark line along the mask
edge; widening the growth thins it further at the cost of regenerating a little
more area.

The pasted area is not cut at the selection. The blend works from the **grown**
mask: everything inside it is replaced outright, because the model drew that
area, and the weight then fades inward over `blend_ramp` pixels (4 by default)
from zero at the grown edge to full strength. Because the weight is measured as
distance inside the grown mask, it follows any shape, not just a rectangle.

That ordering matters. Replacing the whole grown area is what removes a thin dark
rim of the old object that sat just outside the selection; fading at the outer
edge is what hides the seam the model leaves at its own mask edge. A ramp that
starts at the selection instead leaves that rim visible, which looks like a
one-pixel black outline in the result while the patch itself is clean.

An irregular mask on an organic subject showed the intended trade. A coarse
brush mask around a bush, grown by 8 px, let the model draw a little foliage
beyond the painted selection, and the grown mask kept it. On foliage that reads
naturally: the mask localizes the edit, and the silhouette is the model's. On a
hard-edged subject the same growth reads as an overshoot; reduce `mask_grow` to
3–4, or paint the mask a few pixels wider. Adjust growth before the ramp — the
ramp is what hides the seam.

Two files in `%TEMP%\rraz-lanpaint\` help when a seam appears: `<pid>.png` is the
finished picture and `<pid>-crop.png` is the patch before it was pasted. If the
line is in the patch, it is the model or the VAE; if only the finished picture
shows it, the compositing is at fault.

With smart crop off, the patch is the whole layer; the mask still limits what is
pasted back, so a seam is easier to attribute to the model when it appears
somewhere the mask did not cover.

## Unmasked runs (experimental)

With nothing painted the whole layer is processed: that is img2img, not
inpainting. Results are only good enough for experiment, so this is kept thin.

- **Image to image** is the tick that enables Strength. It is available only
  when nothing is painted; with a mask the run is inpainting, the tick is off,
  and the field is fixed at 1.00.
- **Strength** applies only to that ticked, unmasked run. 0.20–0.30 rewrites
  texture and keeps the picture; 0.50 and above replaces everything but the
  composition.
- **Prompt the whole frame**, not the change: "garden corner with dense
  fine-leaved bushes, smooth sandstone paving, warm afternoon light".
- Put what must go in the **negative**: `lamp post, pole, plant pot, colour cast`.
- **Text to image** ignores the layer entirely and overrides both ticks. It is a
  size-limited prompt generator, nothing more.

A masked pass at strength 1.0 is the reliable route for real work: pre-crop the
layer in Affinity, mask what should change, merge the result back. Its crops are
non-destructive, so the original frame is never lost.

An unmasked run asks for confirmation before it starts, with **No** as the
default. The dialog says how many pixels will be re-rendered and at what
strength. A successful run replaces the layer, so duplicate it first if the
result matters; cancelling or failing leaves it untouched.


## The blend, and what could change

The composite is a blend of two images — the generated patch and the original —
each dominant over its own range: the generated content inside the selection and
just beyond it, the original further out, and a band between them where the two
are mixed. What the code chooses is only the shape of the mixing across that
band; today it is linear in distance from the grown edge.

Two ranges matter more than the shape: where the generated content is trusted
outright, and where the original resumes. `mask_grow` sets how far the first
reaches, which is what moves the model's seam away from the selection, and
`blend_ramp` sets the width of the transition between the two.

The mixing shape could take any monotone form — linear, smoothstep, logistic, or
a function fitted to a seam measured from real runs — without changing that
structure. The linear version stays as the baseline the others would be compared
against.

This is a distinction worth keeping in view when comparing with mask editing:
ordinary mask operations can grow, shrink or blur an edge, but none of them
produces a true linear ramp. A gradient mask can, and gradient tools use the same
principle of two ranges with a controlled transition between them — but the
gradient has to be drawn by hand. Here the ramp is derived from the selection
itself, so the transition is controlled without painting a second mask, and it
follows the selection's shape rather than a straight line across it.

## Prompts

The masked pixels are replaced, never edited: the model never sees what was
inside the hole. So a prompt describes **what should appear there**, and removal
is done by describing the replacement.

| Goal | Prompt | Negative |
|---|---|---|
| remove a lamp post | `empty wet pavement, the road continuing under the kerb` | `lamp post, pole` |
| remove a person from a street | `pavement and shopfront behind, midday light` | `person, people` |
| remove a sign from a wall | `weathered brick wall, matching the brick and mortar` | `sign, lettering` |

The model completes the scene, not only the hole. In one lamp-post removal the
base still read as somewhere for something to stand, and the fill produced a
planter. That is a prompt matter: name the replacement ("bare paving where the
post stood") or add `plant pot, planter, bucket, bin, post base` to the negative.

Naming the object in the negative prompt is what stops it creeping back. A
generic "low quality" adds little on SD3 — Stability's own reference example uses
an empty negative with 28 steps at guidance 7.0.

Keep prompts to 10–30 words, and name the material, colour and light that make
the patch belong: "warm evening light from the left", "1970s Kodachrome grain".
The model sees the whole layer by default, so the context around the selection is
already there; a mask that leaves the pavement, wall or sky understandable
produces a better fill than a tight one.

## The mask

The layer's alpha channel is the mask: **0 replaces, maximum keeps**. Grey counts
as selected — anything short of fully opaque is inside the edit area, so a
feathered selection is fully replaced rather than half-ignored at a 50% cutoff.
That matches the model's binary mask, which has to be one or the other, and the
transition is then handled by the ramp.

A masked run is inpainting and always uses **strength 1.0**: the dialog disables
the field, and the runner enforces the same rule. Strength belongs to an unmasked
**Image to image** run, which is why the tick is only available when nothing is
painted.

With `keep_mask=0` (the default) every partly transparent pixel becomes opaque
after the run, not only the solidly masked ones. Without that, the soft rim of the
selection stayed semi-transparent and read as a faint outline of the selection in
the result. A layer with intentional transparency elsewhere should use
`keep_mask=1`.

## The filter

`affinity\out\LanPaint.8bf` is built by `affinity\build_lanpaint.bat`. It lists
as **Filters > Plugins > RRAZ > LanPaint** once
`%LOCALAPPDATA%\RRAZ\Affinity` is in Affinity's Photoshop Plugins list.

`lanpaint.ini` beside the `.8bf` sets `home` (this folder), `python`, `model`,
`steps`, `guidance`, `timeout`, and remembers the last `prompt`, `negative` and
(with Image to image ticked) `strength`.

A run shows a small window while the model works: an indeterminate bar, the
seconds elapsed, and **Cancel**. Cancelling stops the Python process, leaves the
layer exactly as it was, and reports nothing as a failure. The log is
`lanpaint.log` beside the filter.

## Prompt history

The dialog has a **History** list above the prompt. Each Run adds the prompt and
negative pair to `lanpaint-history.ini` beside the filter, newest first, keeping
the last 20. Selecting an entry fills both fields.

This is separate from the last prompt and negative in `lanpaint.ini`, which only
keep the most recent pair. History is what lets a prompt that worked be found
again after several other runs. The log records both prompt and negative for
every run as a timeline; the history file is the same pairs as a quick list.

## Limits

- 16-bit layers only, whole layer in one pass, three channels plus alpha.
- Nothing is scaled down. A layer larger than `max_area` (2,097,152 pixels, about
  2 MP) is refused with a message asking for a crop, because a reduced image loses
  the detail the run exists to add. Under 1 MP a run is quick; between 1 and 2 MP
  it is slower. Both work at the layer's own size. Cropping in Affinity is
  non-destructive, and the cropped result is merged back as a layer.
- Several separate painted areas are handled in one pass, each with its own
  ramp. With smart crop, the selection limit applies to the rectangle that
  contains them all, not to the painted area, so two subjects far apart may need
  separate passes even when each one is small. With the whole layer (the
  default), that limit does not apply; `max_area` does.
- The result comes back at 8 bits per channel, because the diffusion model
  produces 8-bit images; it is placed into the 16-bit layer.
- `keep_mask=1` leaves the mask in place, so the painted area stays transparent,
  for blending the result with the layers below or running the same selection
  again. `keep_mask=0` (the current setting) makes the fill show by itself.
- The model loads on each run, which costs time before the first sample.

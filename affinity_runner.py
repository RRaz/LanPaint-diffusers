"""LanPaint runner for the Affinity filter.

The filter dumps the layer as 16-bit RGBA and calls this script. The alpha
channel is the mask: 0 edits, maximum keeps. The result is written back as raw
16-bit RGB, which the filter folds into the layer.

    python affinity_runner.py --input in.rgba16 --width 800 --height 600 \
        --planes 4 --prompt "..." --output out.rgb16
"""

from __future__ import annotations

import argparse
import os
import sys

import numpy as np
from PIL import Image, ImageFilter, ImageOps

HOST_MAX = 32767  # Affinity 16-bit samples run 0..32767


def parse_args():
    p = argparse.ArgumentParser(description="LanPaint runner for the Affinity filter")
    p.add_argument("--input", required=True, help="raw RGBA16 dump from the filter")
    p.add_argument("--width", type=int, required=True)
    p.add_argument("--height", type=int, required=True)
    p.add_argument("--planes", type=int, required=True)
    p.add_argument("--prompt", default="")
    p.add_argument("--prompt-file", default=None, help="read the prompt from a file (avoids quoting)")
    p.add_argument("--negative-prompt", default="")
    p.add_argument("--negative-prompt-file", default=None)
    p.add_argument("--output", required=True, help="raw RGB16 result for the filter")
    p.add_argument("--preview", default=None, help="optional PNG preview")
    p.add_argument("--model", default="sd3")
    p.add_argument("--model-id", default=None)
    p.add_argument("--steps", type=int, default=28)
    p.add_argument("--guidance", type=float, default=7.0)
    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--device", default="cuda")
    p.add_argument("--dtype", default="", help="float32, bfloat16 or float16; empty uses the model default")
    p.add_argument("--offload", type=int, default=1, help="1 per sub-model, 2 per layer for models bigger than the card, 0 none")
    p.add_argument("--variant", default="", help="weight variant, e.g. fp16")
    p.add_argument("--max-area", type=int, default=2097152,
                   help="images up to this area keep their own size")
    p.add_argument("--smart-crop", type=int, default=0,
                   help="1 sends only the area around the selection to the model")
    p.add_argument("--selection-limit", type=int, default=629145,
                   help="largest selection accepted, in pixels (0.6 MP)")
    p.add_argument("--crop-area", type=int, default=1048576,
                   help="area the crop is grown to before the run (1 MP)")
    p.add_argument("--blend-ramp", type=int, default=12,
                   help="pixels the blend fades out over, outside the grown mask")
    p.add_argument("--strength", type=float, default=1.0,
                   help="img2img strength; 1 regenerates, lower keeps more of the source")
    p.add_argument("--t2i", type=int, default=0,
                   help="1 ignores the layer and generates from the prompt alone")
    p.add_argument("--no-mask", type=int, default=0,
                   help="1 processes the whole layer when nothing is painted")
    p.add_argument("--mask-grow", type=int, default=24,
                   help="pixels the edit area is grown by inside the crop, so the VAE seam falls outside the mask")
    p.add_argument("--lp-n-steps", type=int, default=5)
    p.add_argument("--lp-friction", type=float, default=15.0)
    p.add_argument("--lp-lambda", type=float, default=8.0)
    p.add_argument("--lp-beta", type=float, default=1.0)
    p.add_argument("--lp-step-size", type=float, default=0.2)
    return p.parse_args()


def load_layer(path: str, width: int, height: int, planes: int):
    """Returns the image as RGB and the alpha channel as a mask (0 = edit)."""
    count = width * height * planes
    raw = np.fromfile(path, dtype="<u2", count=count)
    if raw.size != count:
        raise SystemExit(f"short read: {raw.size} of {count} samples")

    layer = raw.reshape(height, width, planes)

    # 0..32767 to 0..255, the range the pipeline works in
    rgb = (layer[:, :, :3] >> 7).astype(np.uint8) if planes >= 3 else None
    if planes == 1:
        grey = (layer[:, :, 0] >> 7).astype(np.uint8)
        rgb = np.repeat(grey[:, :, None], 3, axis=2)
    elif planes == 2:
        grey = (layer[:, :, 0] >> 7).astype(np.uint8)
        rgb = np.repeat(grey[:, :, None], 3, axis=2)

    if planes == 4:
        mask = (layer[:, :, 3] >> 7).astype(np.uint8)
    elif planes == 2:
        mask = (layer[:, :, 1] >> 7).astype(np.uint8)
    else:
        raise SystemExit("the layer has no alpha channel to use as a mask")

    return Image.fromarray(rgb, "RGB"), Image.fromarray(mask, "L")


def mask_box(mask: Image.Image):
    """Smallest rectangle holding every painted pixel: (left, top, right, bottom)."""
    arr = np.asarray(mask, dtype=np.uint8)
    painted = np.argwhere(arr < 250)
    if painted.size == 0:
        return None
    top, left = painted.min(axis=0)
    bottom, right = painted.max(axis=0)
    return int(left), int(top), int(right), int(bottom)


def round16(value: float) -> int:
    return max(16, int(round(value / 16.0)) * 16)


def plan_crop(box, size, selection_limit: int, target_area: int):
    """
    Grows the selection's rectangle into the area that goes to the model.

    The result always contains the whole selection and always stays inside the
    picture, which matters when the selection touches an edge: filling white
    space along a border is a normal use. The rectangle takes its shape from the
    selection, gains most of its context across the short side, and stops at the
    target area.
    """
    left, top, right, bottom = box
    image_w, image_h = size
    width, height = right - left + 1, bottom - top + 1

    if width * height > selection_limit:
        raise SystemExit(
            f"the selection covers {width * height / 1e6:.2f} MP, over the "
            f"{selection_limit / 1e6:.2f} MP limit; select a smaller area"
        )
    if image_w < 32 or image_h < 32:
        raise SystemExit(
            f"the picture is {image_w}x{image_h}; LanPaint needs at least 32x32"
        )

    # Context first: half the short side, at least 32 px. A thin bar gains most
    # of its room across the thin direction, which is where it lacks context.
    pad = max(32, int(round(0.5 * min(width, height))))
    grow_left, grow_top = left - pad, top - pad
    grow_right, grow_bottom = right + pad, bottom + pad

    crop_w = min(image_w, grow_right - grow_left + 1)
    crop_h = min(image_h, grow_bottom - grow_top + 1)

    # Then out towards the target area, never beyond the picture.
    area = crop_w * crop_h
    if area < target_area:
        scale = (target_area / float(area)) ** 0.5
        crop_w = min(image_w, int(round(crop_w * scale)))
        crop_h = min(image_h, int(round(crop_h * scale)))

    # It has to hold the selection, whatever the arithmetic above produced.
    crop_w = max(crop_w, width)
    crop_h = max(crop_h, height)
    crop_w = min(crop_w, image_w)
    crop_h = min(crop_h, image_h)

    # The pipeline crops again internally when a side is not a multiple of its
    # VAE factor. That shifts the generated patch by a few pixels, which shows up
    # as a rim of old pixels along the mask edge. Keep both sides on the grid,
    # provided the selection still fits inside that slightly smaller window.
    grid_w = (crop_w // 16) * 16
    grid_h = (crop_h // 16) * 16
    if grid_w >= width:
        crop_w = grid_w
    if grid_h >= height:
        crop_h = grid_h

    def place(box_low, box_high, crop_len, limit):
        # Any start that keeps the crop over the selection and inside the frame.
        lowest = max(0, box_high - crop_len + 1)
        highest = min(box_low, limit - crop_len)
        centred = int(round((box_low + box_high) / 2.0 - crop_len / 2.0))
        return max(lowest, min(centred, highest))

    crop_left = place(left, right, crop_w, image_w)
    crop_top = place(top, bottom, crop_h, image_h)
    return crop_left, crop_top, crop_left + crop_w, crop_top + crop_h

def paste_back(original: Image.Image, generated: Image.Image, box, mask: Image.Image,
               grow: int = 0, ramp: int = 12):
    """Puts the generated crop into the picture.

    Everything inside the *grown* mask is replaced outright: the model has drawn
    that area, so a dark rim of the original object sitting just outside the
    selection goes with it. The seam the model leaves at its own mask edge is then
    faded out over `ramp` pixels, measured outward from the grown mask. Being
    distance-based, it follows any mask shape.
    """
    left, top, right, bottom = box
    keep = mask.crop(box)

    from scipy import ndimage

    # Grey counts as selected here too, matching the mask the model was given.
    edit = np.asarray(keep, dtype=np.uint8) < 250
    if grow > 0:
        # Grow the edit region the same way the model mask was grown. The array is
        # True where pixels are edited, so growing it means taking the maximum.
        edit = ndimage.maximum_filter(edit, size=2 * grow + 1)

    if ramp > 0:
        # Distance inward from the grown mask's edge: zero exactly at the edge,
        # full `ramp` pixels inside it. That fades the model's seam away while
        # still replacing everything deeper in, including any rim of the old
        # object that sat just outside the selection.
        inside = ndimage.distance_transform_edt(edit)
        weight = np.clip(inside / float(ramp), 0.0, 1.0)
    else:
        weight = edit.astype(np.float32)

    alpha = Image.fromarray((weight * 255.0).astype(np.uint8))
    alpha = alpha.filter(ImageFilter.GaussianBlur(0.6))

    out = original.copy()
    out.paste(generated, (left, top), alpha)
    return out



def main() -> int:
    args = parse_args()

    if args.prompt_file:
        with open(args.prompt_file, "r", encoding="utf-8") as handle:
            args.prompt = handle.read().strip()
    if args.negative_prompt_file:
        try:
            with open(args.negative_prompt_file, "r", encoding="utf-8") as handle:
                args.negative_prompt = handle.read().strip()
        except OSError:
            args.negative_prompt = ""

    # "low quality" is always in the negative prompt: it is the one term that
    # helps every fill, and Stability's own examples carry it.
    if "low quality" not in args.negative_prompt.lower():
        args.negative_prompt = (args.negative_prompt + ", low quality").strip(", ")

    if not args.prompt:
        raise SystemExit("no prompt")

    image, mask = load_layer(args.input, args.width, args.height, args.planes)

    spread = int(mask.getextrema()[1]) - int(mask.getextrema()[0])
    if spread < 8 and not args.no_mask:
        print("warning: the alpha channel is uniform; the whole layer would be edited",
              file=sys.stderr)

    # Settle the working area before the model loads, so an image that cannot be
    # run at its own size is refused in a second rather than after a long load.
    # Nothing is ever scaled down: a reduced image loses the detail a run is for.
    box = None
    work_image, work_mask = image, mask

    if args.no_mask or args.t2i:
        # Nothing painted: the whole layer is the subject, so every pixel is
        # editable. Only if it fits the working area, which is checked here as
        # well as in the filter.
        if image.size[0] * image.size[1] > args.max_area:
            raise SystemExit(
                f"the whole layer is {image.size[0]} x {image.size[1]}, over the "
                f"{args.max_area / 1e6:.2f} MP limit for unmasked work. Crop the "
                f"layer in Affinity first; its crops are non-destructive."
            )
        work_mask = Image.new("L", image.size, 0)
        work_image = image
        print(f"no mask: whole layer {image.size[0]} x {image.size[1]}", flush=True)

    elif args.smart_crop:
        box = mask_box(mask)
        if box is None:
            raise SystemExit("nothing is painted in the alpha channel")
        print(f"selection: {box[2] - box[0] + 1}x{box[3] - box[1] + 1} at "
              f"{box[0]},{box[1]}", flush=True)
        box = plan_crop(box, image.size, args.selection_limit, args.crop_area)
        print(f"crop: {box[2] - box[0]}x{box[3] - box[1]} at {box[0]},{box[1]}",
              flush=True)
        work_image = image.crop(box)
        work_mask = mask.crop(box)
        # Grey in the alpha channel means "partly selected". The model takes a
        # binary mask, so anything short of fully kept counts as editable; the
        # ramp then handles the transition at the outer edge.
        work_mask = work_mask.point(lambda v: 0 if v < 250 else 255)
        if args.mask_grow > 0:
            # The model regenerates a little beyond the selection; the VAE seam
            # then lands outside it, and the paste still uses the original mask.
            work_mask = work_mask.filter(ImageFilter.MinFilter(2 * args.mask_grow + 1))

    else:
        # Whole layer with a mask: the model sees the full frame, but the mask
        # still decides what is replaced when the result comes back.
        if image.size[0] * image.size[1] > args.max_area:
            raise SystemExit(
                f"the whole layer is {image.size[0]} x {image.size[1]} = "
                f"{image.size[0] * image.size[1] / 1e6:.2f} MP, over the "
                f"{args.max_area / 1e6:.2f} MP limit. Crop the layer in Affinity "
                f"first; its crops are non-destructive."
            )
        box = (0, 0, image.size[0], image.size[1])
        work_image = image
        work_mask = mask.point(lambda v: 0 if v < 250 else 255)
        if args.mask_grow > 0:
            work_mask = work_mask.filter(ImageFilter.MinFilter(2 * args.mask_grow + 1))
        print(f"whole layer: {image.size[0]} x {image.size[1]}", flush=True)

    width, height = work_image.size
    if width * height > args.max_area:
        raise SystemExit(
            f"the working area is {width} x {height} = "
            f"{width * height / 1e6:.2f} MP, over the {args.max_area / 1e6:.2f} MP "
            f"limit. Crop the layer in Affinity first; its crops are non-destructive."
        )
    work_size = (width, height)

    # The VAE needs both sides on a 16-pixel grid. Pad the right and bottom
    # rather than rounding down, so no pixel and no part of the selection is
    # lost; the result is cropped back before it is pasted.
    pad_w = (-width) % 16
    pad_h = (-height) % 16
    # A whole-layer run has no surrounding context beyond the frame, so give the
    # model an extra margin. Its own mask boundary then falls outside the area
    # that is cropped and pasted, which keeps the last rows and columns of the
    # picture out of the boundary blend.
    if args.no_mask or args.t2i or not args.smart_crop:
        pad_w += 16
        pad_h += 16
    if pad_w or pad_h:
        pixels = np.pad(np.asarray(work_image),
                        ((0, pad_h), (0, pad_w), (0, 0)), mode="edge")
        work_image = Image.fromarray(pixels, "RGB")
        # Extend the mask by its own edge, not with "keep": when the selection
        # runs to the frame, the padding stays editable, so the model does not
        # preserve a band of replicated edge pixels that showed as a blurred
        # strip along the right and bottom after cropping.
        kept = np.pad(np.asarray(work_mask), ((0, pad_h), (0, pad_w)),
                      mode="edge")
        work_mask = Image.fromarray(kept, "L")
        width += pad_w
        height += pad_h

    print(f"working size: {width}x{height}", flush=True)
    print(f"strength: {1.0 if (args.t2i or not args.no_mask) else args.strength:.2f} "
          f"({'img2img' if args.no_mask and not args.t2i else 'full'})", flush=True)

    os.environ.setdefault("HF_HUB_DISABLE_TELEMETRY", "1")

    from lanpaint_pipeline import LanPaintConfig, LanPaintInpaintPipeline
    from lanpaint_pipeline.registry import _import_class, get_model_spec

    spec = get_model_spec(args.model)
    model_id = args.model_id or spec.default_model_id
    PipelineClass = _import_class(spec.pipeline_cls_path)
    AdapterClass = _import_class(spec.adapter_cls_path)

    import torch as _torch
    if args.dtype:
        dtype = {"float32": _torch.float32, "bfloat16": _torch.bfloat16,
                 "float16": _torch.float16}.get(args.dtype, spec.default_dtype)
    elif args.device == "cpu":
        dtype = _torch.bfloat16      # float16 is not usable on a CPU
    else:
        dtype = spec.default_dtype
    variant = args.variant or ("fp16" if args.model == "sd3" else "")
    load_kwargs = {"torch_dtype": dtype}
    if variant:
        load_kwargs["variant"] = variant
    try:
        pipe = PipelineClass.from_pretrained(model_id, **load_kwargs)
    except (OSError, ValueError) as error:
        if not variant:
            raise
        print(f"note: no {variant} files for this model, loading the default", flush=True)
        load_kwargs.pop("variant", None)
        pipe = PipelineClass.from_pretrained(model_id, **load_kwargs)
    if args.offload == 2:
        # Layer by layer on and off the card: for a model whose single largest
        # sub-model is bigger than the card, which Z-Image's 24.6 GB transformer is.
        pipe.enable_sequential_cpu_offload()
    elif args.offload == 1:
        # Keeps one sub-model on the card at a time; enough for SD3 at 12 GB.
        pipe.enable_model_cpu_offload()
    else:
        pipe.to(args.device)
    adapter = AdapterClass(pipe)
    config = LanPaintConfig(
        n_steps=args.lp_n_steps,
        friction=args.lp_friction,
        chara_lambda=args.lp_lambda,
        beta=args.lp_beta,
        step_size=args.lp_step_size,
    )
    pipe = LanPaintInpaintPipeline(adapter, config=config)
    print(f"model: {spec.name}", flush=True)

    result = pipe(
        prompt=args.prompt,
        negative_prompt=args.negative_prompt,
        image=work_image,
        mask_image=work_mask,
        height=height,
        width=width,
        guidance_scale=args.guidance,
        num_inference_steps=args.steps,
        strength=1.0 if (args.t2i or not args.no_mask) else args.strength,
        seed=args.seed,
    )

    out = result.images[0].convert("RGB")
    if out.size != (width, height):
        out = out.resize((width, height), Image.LANCZOS)
    if (width, height) != work_size:
        # Drop the padding that put the layer on the VAE's 16-pixel grid.
        out = out.crop((0, 0, work_size[0], work_size[1]))

    if args.preview and box is not None:
        # The patch before it is put back, for telling a model seam from a
        # compositing one.
        out.save(args.preview.replace(".png", "-crop.png"))

    if box is not None:
        out = paste_back(image, out, box, mask,
                         grow=args.mask_grow, ramp=args.blend_ramp)

    if out.size != (args.width, args.height):
        out = out.resize((args.width, args.height), Image.LANCZOS)

    if args.preview:
        out.save(args.preview)

    # Back to 16-bit RGB for the filter: 0..255 to 0..65535
    arr = np.asarray(out, dtype=np.uint16) * 257
    arr.tofile(args.output)
    print(f"written: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

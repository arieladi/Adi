"""Model adapters. No modification of the approved renderer or model weights."""
from __future__ import annotations

import importlib
import logging
import sys
from pathlib import Path
from typing import Any, Callable

import cv2
import numpy as np
import torch
import torch.nn.functional as F
from numpy.typing import NDArray
from spandrel import ImageModelDescriptor, ModelLoader

LOG = logging.getLogger(__name__)
Image = NDArray[np.uint8]


class OptionalCompile:
    """Catch lazy compilation failures as well as torch.compile construction failures."""
    def __init__(self, eager: Callable[..., Any], enabled: bool) -> None:
        self.eager = eager
        self.compiled: Callable[..., Any] | None = None
        if enabled:
            try:
                self.compiled = torch.compile(eager, mode="max-autotune")
            except Exception:
                LOG.exception("Compiler unavailable; using eager inference")

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        if self.compiled is not None:
            try:
                return self.compiled(*args, **kwargs)
            except Exception:
                LOG.exception("Compiled execution failed; reverting to eager inference")
                self.compiled = None
        return self.eager(*args, **kwargs)


class Upscaler:
    def __init__(self, path: Path, device: torch.device, fp16: bool,
                 compile_model: bool, tile: int, padding: int, minimum: int,
                 extra_arches: bool) -> None:
        if extra_arches:
            import spandrel_extra_arches
            spandrel_extra_arches.install()
        model = ModelLoader().load_from_file(path)
        if not isinstance(model, ImageModelDescriptor):
            raise TypeError("Expected a single-image super-resolution model")
        if model.input_channels != 3 or model.output_channels != 3:
            raise ValueError("This renderer requires RGB input/output models")
        self.dtype = torch.float16 if fp16 and model.supports_half else torch.float32
        if fp16 and self.dtype != torch.float16:
            LOG.warning("%s does not advertise FP16 support; using FP32", model.architecture.id)
        self.device, self.scale = device, model.scale
        self.model = model.to(device=device, dtype=self.dtype).eval()
        self.call = OptionalCompile(self.model, compile_model)
        self.tile, self.padding, self.minimum = tile, padding, minimum
        self.description = str(model.architecture.id)
        LOG.info("Upscaler %s scale=%s dtype=%s device=%s", self.description, self.scale, self.dtype, device)

    @torch.inference_mode()
    def _render(self, source: torch.Tensor, tile: int) -> Image:
        h, w, _ = source.shape
        s, pad = self.scale, self.padding
        # Full native output lives in ordinary RAM, not VRAM or locked RAM.
        output = np.empty((h*s, w*s, 3), dtype=np.uint8)
        for top in range(0, h, tile):
            for left in range(0, w, tile):
                bottom, right = min(top+tile, h), min(left+tile, w)
                y0, y1 = max(0, top-pad), min(h, bottom+pad)
                x0, x1 = max(0, left-pad), min(w, right+pad)
                x = source[y0:y1, x0:x1].to(self.device, non_blocking=True)
                x = x.permute(2, 0, 1).unsqueeze(0).to(self.dtype).div_(255)
                y = self.call(x)
                if not torch.isfinite(y).all().item():
                    raise FloatingPointError("Upscaler produced non-finite pixels")
                crop = y[0, :, s*(top-y0):s*(bottom-y0), s*(left-x0):s*(right-x0)]
                output[s*top:s*bottom, s*left:s*right] = (
                    crop.clamp(0, 1).mul(255).round().to(torch.uint8)
                    .permute(1, 2, 0).cpu().numpy())
                del x, y, crop
        return output

    def render(self, source: torch.Tensor, target: torch.Tensor) -> None:
        # The failing attempt unwinds before clearing the CUDA allocator.
        while True:
            try:
                output = self._render(source, self.tile)
                break
            except torch.cuda.OutOfMemoryError:
                torch.cuda.empty_cache()
                if self.tile <= self.minimum:
                    raise RuntimeError("Upscaler OOM at minimum tile size; reduce padding or use a smaller model") from None
                self.tile = max(self.minimum, self.tile // 2)
                LOG.warning("Upscaler OOM: retrying this frame with tile=%d", self.tile)
        h, w, _ = target.shape
        if output.shape[:2] != (h, w):
            output = cv2.resize(output, (w, h), interpolation=cv2.INTER_LANCZOS4)
        target.copy_(torch.from_numpy(output))


def fp16_safe_warp(image: torch.Tensor, flow: torch.Tensor) -> torch.Tensor:
    """Use FP32 coordinate math/grid sampling; keep network activations FP16.

    The original RIFE warp constructs FP32 grids even for FP16 inputs. Explicit
    FP32 sampling also avoids coarse half-precision grid coordinates at 4K.
    """
    _, _, h, w = image.shape
    x = torch.linspace(-1, 1, w, device=image.device, dtype=torch.float32)
    y = torch.linspace(-1, 1, h, device=image.device, dtype=torch.float32)
    yy, xx = torch.meshgrid(y, x, indexing="ij")
    base = torch.stack((xx, yy), -1).unsqueeze(0)
    delta = flow.float().permute(0, 2, 3, 1) * flow.new_tensor(
        [2 / max(w-1, 1), 2 / max(h-1, 1)], dtype=torch.float32)
    return F.grid_sample(image.float(), base + delta, mode="bilinear",
                         padding_mode="border", align_corners=True).to(image.dtype)


class Interpolator:
    def __init__(self, root: Path, device: torch.device, fp16: bool,
                 compile_model: bool, flow_scale: float) -> None:
        sys.path.insert(0, str(root))
        module = importlib.import_module("train_log.IFNet_HDv3")
        # Local adapter only; original source on disk is untouched.
        module.warp = fp16_safe_warp
        self.device = device
        self.dtype = torch.float16 if fp16 else torch.float32
        self.flow_scale = flow_scale
        self.model = module.IFNet()
        weights = torch.load(root / "train_log/flownet.pkl", map_location="cpu", weights_only=True)
        weights = {k.removeprefix("module."): v for k, v in weights.items()}
        # Released checkpoint includes training-only heads absent in IFNet inference.
        weights = {k: v for k, v in weights.items() if not k.startswith(("teacher.", "caltime."))}
        self.model.load_state_dict(weights, strict=True)
        self.model.to(device=device, dtype=self.dtype).eval()
        self.call = OptionalCompile(self.model, compile_model)
        LOG.info("RIFE 4.26 dtype=%s device=%s flow_scale=%s", self.dtype, device, flow_scale)

    def upload(self, frame: torch.Tensor) -> torch.Tensor:
        return frame.to(self.device, non_blocking=True).permute(2, 0, 1).unsqueeze(0).to(self.dtype).div_(255)

    @torch.inference_mode()
    def render(self, left: torch.Tensor, right: torch.Tensor, fraction: float,
               target: torch.Tensor) -> None:
        h, w = left.shape[-2:]
        if fraction <= 1e-9:
            out = left
        else:
            multiple = int(64 / self.flow_scale)
            x = F.pad(torch.cat((left, right), 1), (0, (-w) % multiple, 0, (-h) % multiple), mode="replicate")
            scales = [v / self.flow_scale for v in (16, 8, 4, 2, 1)]
            out = self.call(x, fraction, scales)[2][-1][:, :, :h, :w]
        if not torch.isfinite(out).all().item():
            raise FloatingPointError("RIFE produced non-finite pixels; retry with --no-fp16")
        pixels = out[0].clamp(0, 1).mul(255).round().to(torch.uint8).permute(1, 2, 0)
        target.copy_(pixels, non_blocking=True)
        torch.cuda.current_stream(self.device).synchronize()

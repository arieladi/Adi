"""Wan 2.2 TI2V 5B with a fixed first AND last frame (Adi 2026-11-11: define where an action starts and lands).

Same as ComfyUI's Wan22ImageToVideoLatent, plus an optional end_image encoded into the last latent frame and kept
out of denoising, so the clip has to travel from start_image to end_image. The 5B model was not trained for this;
it is an experiment judged by eye.
"""
import torch
import comfy.latent_formats
import comfy.model_management
import comfy.utils
import nodes


class Wan22FirstLastLatent:
    @classmethod
    def INPUT_TYPES(cls):
        return {"required": {"vae": ("VAE",),
                             "width": ("INT", {"default": 1280, "min": 32, "max": nodes.MAX_RESOLUTION, "step": 32}),
                             "height": ("INT", {"default": 736, "min": 32, "max": nodes.MAX_RESOLUTION, "step": 32}),
                             "length": ("INT", {"default": 81, "min": 5, "max": nodes.MAX_RESOLUTION, "step": 4}),
                             "start_image": ("IMAGE",)},
                "optional": {"end_image": ("IMAGE",)}}

    RETURN_TYPES = ("LATENT",)
    FUNCTION = "run"
    CATEGORY = "model/conditioning/wan"

    def run(self, vae, width, height, length, start_image, end_image=None):
        dev = comfy.model_management.intermediate_device()
        t = ((length - 1) // 4) + 1
        latent = torch.zeros([1, 48, t, height // 16, width // 16], device=dev)
        mask = torch.ones([1, 1, t, height // 16, width // 16], device=dev)

        def enc(img):
            img = comfy.utils.common_upscale(img[:1].movedim(-1, 1), width, height, "bilinear", "center").movedim(1, -1)
            return vae.encode(img[:, :, :, :3])

        a = enc(start_image)
        latent[:, :, :1] = a[:, :, :1]
        mask[:, :, :1] = 0.0
        if end_image is not None:
            b = enc(end_image)
            latent[:, :, -1:] = b[:, :, :1]
            mask[:, :, -1:] = 0.0
        # As the stock node: known frames keep their encoded latent, the rest get the format's empty value.
        latent = comfy.latent_formats.Wan22().process_out(latent) * mask + latent * (1.0 - mask)
        return ({"samples": latent, "noise_mask": mask},)


NODE_CLASS_MAPPINGS = {"Wan22FirstLastLatent": Wan22FirstLastLatent}
NODE_DISPLAY_NAME_MAPPINGS = {"Wan22FirstLastLatent": "Wan 2.2 first + last frame latent (Avastha)"}

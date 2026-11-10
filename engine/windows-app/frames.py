"""Validate runtime frames and map pointer coordinates for every panel size."""
from __future__ import annotations

import base64
import binascii
from dataclasses import dataclass
import math

MAX_PIXELS = 1024 * 1024
MAX_FRAME_BYTES = MAX_PIXELS * 4


@dataclass(frozen=True)
class Frame:
    sequence: int
    width: int
    height: int
    stride: int
    pixel_format: str
    pixels: bytes

    def image(self):
        from PIL import Image
        image = Image.frombytes("RGB", (self.width, self.height), self.pixels, "raw",
                                "BGR;16" if self.pixel_format == "rgb565le" else "RGB",
                                self.stride, 1)
        if self.pixel_format == "rgb565le":
            image = image.point([v & 0xf8 for v in range(256)] +
                                [v & 0xfc for v in range(256)] +
                                [v & 0xf8 for v in range(256)])
        return image


def decode_frame(value):
    if not isinstance(value, dict):
        raise ValueError("无效的显示帧")
    width, height, sequence = (value.get(name) for name in ("width", "height", "sequence"))
    if (any(type(v) is not int for v in (width, height, sequence)) or
            not 0 < width <= 1024 or not 0 < height <= 1024 or
            width * height > MAX_PIXELS or sequence < 0):
        raise ValueError("无效的显示帧尺寸或序号")
    pixel_format = value.get("pixelFormat")
    if pixel_format not in ("rgb565le", "rgb888"):
        raise ValueError("不支持的显示帧格式")
    minimum_stride = width * (2 if pixel_format == "rgb565le" else 3)
    stride = value.get("stride", minimum_stride)
    if (type(stride) is not int or not minimum_stride <= stride <= minimum_stride + 4096 or
            stride * height > MAX_FRAME_BYTES):
        raise ValueError("无效的显示帧行长度")
    encoded = value.get("pixelsBase64")
    if not isinstance(encoded, str) or len(encoded) > (MAX_FRAME_BYTES + 2) // 3 * 4:
        raise ValueError("显示帧数据超出限制")
    try:
        pixels = base64.b64decode(encoded, validate=True)
    except (ValueError, binascii.Error) as error:
        raise ValueError("无效的显示帧编码") from error
    if len(pixels) != stride * height:
        raise ValueError("显示帧数据长度不一致")
    return Frame(sequence, width, height, stride, pixel_format, pixels)


def fit_panel(panel_width, panel_height, available_width, available_height):
    if min(panel_width, panel_height, available_width, available_height) <= 0:
        raise ValueError("显示区域尺寸必须大于零")
    scale = min(available_width / panel_width, available_height / panel_height)
    width, height = max(1, int(panel_width * scale)), max(1, int(panel_height * scale))
    return ((available_width - width) // 2, (available_height - height) // 2, width, height)


def panel_point(x, y, rectangle, panel_width, panel_height, *, clamp=False):
    left, top, width, height = rectangle
    if not all(math.isfinite(v) for v in (x, y)) or min(width, height, panel_width, panel_height) <= 0:
        raise ValueError("无效的触控坐标")
    if not clamp and not (left <= x < left + width and top <= y < top + height):
        return None
    return (max(0, min(panel_width - 1, int((x - left) * panel_width / width))),
            max(0, min(panel_height - 1, int((y - top) * panel_height / height))))

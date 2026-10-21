# P67 solid-path destination-in rendering

The P67 music-player comparison exposed a renderer rejection for `A00=0x10000402`, `A10=0x103`. This change implements that exact solid-path `DST_IN` operation on a premultiplied BGRA8888 target. Other blend modes, image `DST_IN`, global alpha, separate masks, and unsafe target descriptions remain rejected.

## Command and pixel semantics

- The official [NXP VGLite driver](https://github.com/nxp-mcuxpresso/gpu-vglite/blob/master/VGLite/vg_lite.c) maps `VG_LITE_BLEND_DST_IN` to `0x400` in `convert_blend`. Its `set_render_target` sets bit `0x100` for a premultiplied target.
- The official [path renderer](https://github.com/nxp-mcuxpresso/gpu-vglite/blob/master/VGLite/vg_lite_path.c) emits `vg_lite_draw` with constant paint in `A02` and without image-mode bits. Thus the captured `A25=0x01000107` describes a previous image operation; this solid draw must not validate or sample that stale texture.
- [OpenVG 1.1](https://registry.khronos.org/OpenVG/specs/openvg-1.1.pdf), sections 2.8 and 13.5, defines destination-in on premultiplied color and applies coverage by interpolating the blended result with the original destination. [NXP's blend reference](https://mcuxpresso.nxp.com/mcuxsdk/latest/middleware/vglite/topics/blits_for_compositing_and_blending.html) also specifies the destination-in factors.

For paint alpha `a` and path coverage `c`, each stored destination RGBA channel is multiplied by `1 - c + c*a`. Paint RGB has no effect. Zero coverage preserves the original pixel; fully covered zero-alpha paint erases all four channels. The implementation retains the existing 4×4 coverage precision until the final rounded store.

## Verification and scope

Run from the repository root:

```sh
python3 -m unittest discover -s tests -p 'test_gpu_*.py'
```

`test_gpu_dst_in.py` compiles the production decoder with AddressSanitizer and UndefinedBehaviorSanitizer. Its 240 checks cover fixed BGRA pixel vectors, full/half/zero coverage, four alpha levels, opaque/translucent destinations, paint-RGB independence, clipping, stale/invalid texture state, rejection guards, and absence of guest-memory writes while preparing either valid or rejected submissions. The existing 57 alpha checks remain unchanged. A retained independent color fixture also passed its 216 checks.

These host checks establish the bounded decoder behavior. They do not constitute a new full-firmware execution result; that result is recorded separately by the simulator instance owner. No GPU rejection counter, runtime failure policy, or persistence-health guard is bypassed.

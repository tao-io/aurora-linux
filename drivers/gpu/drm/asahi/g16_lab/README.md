# J713 G16 dirtyroom compute laboratory

Imported from committed donor `8d31dd1170ac955d228477bb5141f028dd358399`
in `linux/m4_work_finished_kernel`. Captured dispatch resources, generated
shader binaries and the progress-marker include come from that preserved
working tree; `payloads.json` records their exact hashes and source paths.
This is a dirtyroom bring-up module containing reconstructed firmware/driver
interfaces and captured macOS resource layouts. It is specific to J713/T8132
and the recorded RTKit-3255.160.4 G16G firmware. It is not a DRM/Mesa driver.

Build externally against a matching 16 KiB arm64 kernel output directory:

```sh
make -C /path/to/linux-alpha O=/path/to/kernel-build ARCH=arm64 LLVM=1 \
  M=/path/to/linux-alpha/drivers/gpu/drm/asahi/g16_lab modules
```

The kernel needs the PMP bridge and the opt-in
`apple_j713_reserve_stage2=1` memory reservation. The stage2 device tree uses
`linux-enablement-mac,g16-lab` under `/chosen` to export GPU handoff metadata.
The default `stage=smoke` does not execute a GPU workload. Nothing autoloads
this module. Start each firmware experiment after a fresh boot: stopping the
ASC after an incomplete handshake is not a firmware reset.

Initial alpha validation: identity and powered register reads pass; the full
RTKit handshake and exact InitData reach firmware ready. The fenced no-op is
consumed, but the donor observation loop incorrectly reports timeout before
its final success check. Compute validation is pending.

Alpha adaptations retain the donor compute path but explicitly disable its
unused PMP tool-snapshot diagnostic. PMP power requests use the active kernel
PMP driver through a bridge serialized against driver removal.

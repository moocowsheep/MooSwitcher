# OMT (Open Media Transport) input

MooSwitcher can ingest OMT sources (vMix's open, MIT-licensed NDI
alternative — VMX intra codec over TCP, mDNS discovery). Support is
optional: CMake enables it when `third_party/omt/` is populated.

## Getting the libraries

`third_party/` is gitignored; build the two libraries once per machine:

```sh
cd third_party
git clone --depth 1 https://github.com/openmediatransport/libvmx
git clone --depth 1 https://github.com/openmediatransport/libomtnet
git clone --depth 1 https://github.com/openmediatransport/libomt

# 1. VMX codec: plain clang++ (no deps)
(cd libvmx/build && sh buildlinuxx64.sh)

# 2. .NET 8 SDK, unprivileged (skip if `dotnet` exists)
curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 8.0
export DOTNET_ROOT=$HOME/.dotnet PATH=$HOME/.dotnet:$PATH

# 3. libomtnet (C# implementation), then libomt (NativeAOT C ABI wrapper)
(cd libomtnet && dotnet build libomtnet.sln -c Release)
(cd libomt && dotnet publish libomt.sln -r linux-x64 -c Release)

# 4. Lay out the SDK the build expects
mkdir -p omt/include omt/lib
cp libomt/libomt.h omt/include/
cp libomt/bin/Release/net8.0/linux-x64/publish/libomt.so omt/lib/
cp libvmx/build/libvmx.so omt/lib/
```

Re-run cmake; it reports `OMT SDK found`. `libomt.so` dlopens `libvmx.so`
from the same directory (rpath is wired by CMake).

## Using OMT inputs

- Headless: `moo-headless --omt-input "HOSTNAME (Name)"` (discovery name)
  or `--omt-input omt://host:port` (direct, senders bind from 6400 up).
- GUI source picker: the discovery list shows NDI and OMT sources together
  (badged `NDI`/`OMT`; the row carries its type, so bare OMT names never
  get misread as NDI substrings — first open may need one Refresh while
  mDNS answers arrive). The manual field accepts `omt://host:port` and
  `srt://` URLs; bare manual text stays an NDI name substring. Show files
  store the true type (`type=omt`) for round-trip fidelity.
- Frame sync (`--framesync IDX[:N]`, per-input GUI checkbox) works on OMT
  inputs; sender timestamps are 100 ns units like NDI's and get the same
  cadence sanity check + synthesized-pts fallback.
- Test sender: `moo-testgen --omt [--noise] [--size WxH]` (VMX encode
  happens in-process; the 5 s stats line reports encoder ms/frame, Mbps,
  and transport-envelope drops).

## Limits measured on this box

See `docs/bench-omt.md` for the 1080p/8K measurements, CPU costs, and the
10 MB-per-compressed-frame transport envelope (`OMTConstants.VIDEO_MAX_SIZE`
— oversize frames are counted drops at the sender, not a stall; patchable
in our vendored build if 8K noise content ever matters).

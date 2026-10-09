# Third-party notices

wwmi-dx12 is licensed under GPL-3.0 (`LICENSE`). It contains or links the following.

| Component | License | Notice / text |
|---|---|---|
| 3Dmigoto, XXMI fork (github.com/SpectrumQT/3Dmigoto), compiled into the add-on | GPL-3.0 | `third_party/3Dmigoto/COPYING.txt`, `AUTHORS.txt`, `LICENSE.GPL.txt` |
| Nektra Deviare-InProcess (via 3Dmigoto) | GPL-3.0 | `third_party/3Dmigoto/COPYING.txt` |
| PCRE2 10.30 (via 3Dmigoto) | BSD (PCRE2 licence) | `third_party/3Dmigoto/pcre2-10.30/LICENCE` |
| DirectXTK, three source files (via 3Dmigoto) | MIT, (c) Microsoft Corporation | `third_party/3Dmigoto/DirectXTK/LICENSE` |
| crc32c-hw 1.0.5 (via 3Dmigoto) | zlib | header of `third_party/3Dmigoto/crc32c-hw-1.0.5/src/crc32c.cpp` |
| ReShade add-on SDK headers | BSD-3-Clause, (c) Patrick Mours | `reshade_addon/sdk/LICENSE.md` |
| Dear ImGui headers | MIT, (c) Omar Cornut | `reshade_addon/sdk/deps/imgui/LICENSE.txt` |
| bcdec | MIT or Unlicense | end of `third_party/bcdec/bcdec.h` |
| DirectX Shader Compiler 1.8.2502 (`dxcompiler.dll`, `dxil.dll` from a github.com/microsoft/DirectXShaderCompiler release, unmodified, in `third_party/dxc/bin/x64`; shipped in `wwmi_dx12\dxc\` of the binary package) | dxcompiler: University of Illinois/NCSA and MIT; dxil: Microsoft licence | `third_party/dxc/LICENSE-LLVM.txt`, `LICENSE-MIT.txt`, `LICENSE-MS.txt` |

The paths above are in the source tree. The binary package carries the same texts in `wwmi_dx12\licenses\`
(the crc32c and bcdec notices as the excerpts of their source files) and the DirectX Shader Compiler's in
`wwmi_dx12\dxc\`.

ReShade itself is not included; install it from reshade.me.

## Modified 3Dmigoto files

The following files are derived from 3Dmigoto (XXMI fork, commit e8ea2075) and were modified by
lekarsenten:

| File | Derived from | Modified | Change |
|---|---|---|---|
| `reshade_addon/migoto/frame_analysis_engine.cpp`, `.h` | `DirectX11/FrameAnalysis.cpp`, `.h` | 2026-08-11 | extracted; D3D11 staging and state reads replaced by D3D12 equivalents |
| `reshade_addon/migoto/frame_analysis_format.cpp`, `.h` | `DirectX11/FrameAnalysis.cpp` | 2026-08-11 | the dump format functions extracted |

All other 3Dmigoto sources are compiled unmodified from the submodule (`reshade_addon/migoto/PRISTINE.txt`).

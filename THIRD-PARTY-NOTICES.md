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

The following files are derived from 3Dmigoto (XXMI fork) and were modified by lekarsenten:

| File | Derived from | Modified | Change |
|---|---|---|---|
| `reshade_addon/migoto/frame_analysis_engine.cpp`, `.h` | `DirectX11/FrameAnalysis.cpp`, `.h` (commit e8ea2075) | 2026-08-11 | extracted; D3D11 staging and state reads replaced by D3D12 equivalents |
| `reshade_addon/migoto/frame_analysis_format.cpp`, `.h` | `DirectX11/FrameAnalysis.cpp` (commit e8ea2075) | 2026-08-11 | the dump format functions extracted |
| `reshade_addon/migoto/HackerContext.h`, `HackerDevice.h` | `DirectX11/HackerContext.h`, `HackerDevice.h` | 2026-10-09 | stand-ins for theirs: the class names, interface GUIDs and the members their engine reads are kept; the calls go to the D3D12 layer |
| `reshade_addon/migoto/hacker_context_shim.cpp` | `DirectX11/HackerContext.cpp` | 2026-10-05 | `Bind3DMigotoResources`: the IniParams bind on every shader stage |
| `reshade_addon/migoto/port_globals.cpp` | `DirectX11/D3D11Wrapper.cpp`, `DirectX11/Hunting.cpp` | 2026-10-09 | the globals their engine links against (`G`, the log state, the locks) and `ParseHuntingSection`, reproduced |
| `reshade_addon/migoto/port_dispatch.cpp`, `BoundShaderFilter` | `DirectX11/CommandList.cpp`, `process_shader_filter` | 2026-10-09 | the bound-shader lookup, re-done for the add-on's own draws |
| `reshade_addon/migoto/port_ini.cpp`, `port_override.cpp` | `DirectX11/IniHandler.cpp`, `Override.cpp` | 2026-10-10 | theirs compiled unmodified, included with macros that route `optimise_command_lists`, `RegisterKeyBinding` and `ParseRunExplicitCommandList` through wrappers |

All other 3Dmigoto sources are compiled unmodified from the submodule, commit a7847eae
(`reshade_addon/migoto/PRISTINE.txt`). `reshade_addon/migoto/nvapi.h` and `NktHookLib.h` are minimal stand-ins
that declare only the names those sources use, not the libraries' code.

#pragma once

#include <d3d11_1.h>
#include "globals.h"      // FrameAnalysisOptions, G -- BEFORE HackerContext.h,
#include "HackerContext.h"
#include <wrl/client.h>   // Microsoft::WRL::ComPtr, used by the deferred dump lists
#include <stdexcept>       // std::out_of_range, caught by the dedupe map lookups

class FrameAnalysisContext;
inline const char *type_name(const FrameAnalysisContext *) { return "FrameAnalysisContext"; }

// {2AEE5B3A-68ED-44E9-AA4D-9EAA6315D72B}
DEFINE_GUID(IID_FrameAnalysisContext,
0x2aee5b3a, 0x68ed, 0x44e9, 0xaa, 0x4d, 0x9e, 0xaa, 0x63, 0x15, 0xd7, 0x2b);

// {4EAF92BD-5552-44C7-804C-4CE1014C1B17}
DEFINE_GUID(InputLayoutDescGuid,
0x4eaf92bd, 0x5552, 0x44c7, 0x80, 0x4c, 0x4c, 0xe1, 0x1, 0x4c, 0x1b, 0x17);

struct FrameAnalysisDeferredDumpBufferArgs {
	FrameAnalysisOptions analyse_options;

	Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
	Microsoft::WRL::ComPtr<ID3DBlob> layout;
	Microsoft::WRL::ComPtr<ID3D11Buffer> staged_ib_for_vb;

	D3D11_BUFFER_DESC orig_desc;
	wstring filename;
	FrameAnalysisOptions buf_type_mask;
	int idx;
	DXGI_FORMAT ib_fmt;
	UINT stride;
	UINT offset;
	UINT first;
	UINT count;
	DrawCallInfo call_info;
	UINT ib_off_for_vb;
	D3D11_PRIMITIVE_TOPOLOGY topology;

	FrameAnalysisDeferredDumpBufferArgs(FrameAnalysisOptions analyse_options, ID3D11Buffer *staging,
			D3D11_BUFFER_DESC *orig_desc, wchar_t *filename, FrameAnalysisOptions buf_type_mask, int idx,
			DXGI_FORMAT ib_fmt, UINT stride, UINT offset, UINT first, UINT count, ID3DBlob *layout,
			D3D11_PRIMITIVE_TOPOLOGY topology, DrawCallInfo *call_info,
			ID3D11Buffer *staged_ib_for_vb, UINT ib_off_for_vb) :
		analyse_options(analyse_options), staging(staging),
		orig_desc(*orig_desc), filename(filename),
		buf_type_mask(buf_type_mask), idx(idx), ib_fmt(ib_fmt),
		stride(stride), offset(offset), first(first), count(count),
		layout(layout), topology(topology),
		call_info(call_info ? *call_info : DrawCallInfo()),
		staged_ib_for_vb(staged_ib_for_vb), ib_off_for_vb(ib_off_for_vb)
	{}
};
struct FrameAnalysisDeferredDumpTex2DArgs {
	FrameAnalysisOptions analyse_options;

	Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;

	wstring filename;
	bool stereo;
	D3D11_TEXTURE2D_DESC orig_desc;
	DXGI_FORMAT format;

	FrameAnalysisDeferredDumpTex2DArgs(FrameAnalysisOptions analyse_options,
			ID3D11Texture2D *staging, wchar_t *filename,
			bool stereo, D3D11_TEXTURE2D_DESC *orig_desc, DXGI_FORMAT format) :
		analyse_options(analyse_options), staging(staging),
		filename(filename), stereo(stereo), orig_desc(*orig_desc), format(format)
	{}
};

typedef vector<FrameAnalysisDeferredDumpBufferArgs> FrameAnalysisDeferredBuffers;
typedef std::unique_ptr<FrameAnalysisDeferredBuffers> FrameAnalysisDeferredBuffersPtr;
typedef vector<FrameAnalysisDeferredDumpTex2DArgs> FrameAnalysisDeferredTex2D;
typedef std::unique_ptr<FrameAnalysisDeferredTex2D> FrameAnalysisDeferredTex2DPtr;

class FrameAnalysisContext : public HackerContext
{
private:
	template <class ID3D11Shader>
	void FrameAnalysisLogShaderHash(ID3D11Shader *shader);
	void FrameAnalysisLogResourceHash(ID3D11Resource *resource);
	void FrameAnalysisLogResource(int slot, char *slot_name, ID3D11Resource *resource);
	void FrameAnalysisLogResourceArray(UINT start, UINT len, ID3D11Resource *const *ppResources);
	void FrameAnalysisLogView(int slot, char *slot_name, ID3D11View *view);
	void FrameAnalysisLogViewArray(UINT start, UINT len, ID3D11View *const *ppViews);
	void FrameAnalysisLogMiscArray(UINT start, UINT len, void *const *array);
	void FrameAnalysisLogAsyncQuery(ID3D11Asynchronous *async);
	void FrameAnalysisLogData(void *buf, UINT size);
	FILE *frame_analysis_log;
	unsigned draw_call;
	unsigned non_draw_call_dump_counter;

	FrameAnalysisDeferredBuffersPtr deferred_buffers;
	FrameAnalysisDeferredTex2DPtr deferred_tex2d;

	ID3D11DeviceContext* GetDumpingContext();

	void Dump2DResource(ID3D11Texture2D *resource, wchar_t *filename,
			bool stereo, D3D11_TEXTURE2D_DESC *orig_desc, DXGI_FORMAT format);
	bool DeferDump2DResource(ID3D11Texture2D *staging, wchar_t *filename,
			bool stereo, D3D11_TEXTURE2D_DESC *orig_desc, DXGI_FORMAT format);
	void Dump2DResourceImmediateCtx(ID3D11Texture2D *staging, wstring filename,
			bool stereo, D3D11_TEXTURE2D_DESC *orig_desc, DXGI_FORMAT format);

	HRESULT ResolveMSAA(ID3D11Texture2D *src, D3D11_TEXTURE2D_DESC *srcDesc,
			ID3D11Texture2D **resolved, DXGI_FORMAT format);
	HRESULT StageResource(ID3D11Texture2D *src,
			D3D11_TEXTURE2D_DESC *srcDesc, ID3D11Texture2D **dst, DXGI_FORMAT format);
	HRESULT CreateStagingResource(ID3D11Texture2D **resource,
		D3D11_TEXTURE2D_DESC desc, bool stereo, bool msaa, DXGI_FORMAT format);

	void DumpStereoResource(ID3D11Texture2D *resource, wchar_t *filename, DXGI_FORMAT format);
	void DumpBufferTxt(wchar_t *filename, D3D11_MAPPED_SUBRESOURCE *map,
			UINT size, char type, int idx, UINT stride, UINT offset);
	void DumpVBTxt(wchar_t *filename, D3D11_MAPPED_SUBRESOURCE *map,
			UINT size, int idx, UINT stride, UINT offset,
			UINT first, UINT count, ID3DBlob *layout,
			D3D11_PRIMITIVE_TOPOLOGY topology, DrawCallInfo *call_info);
	void DumpIBTxt(wchar_t *filename, D3D11_MAPPED_SUBRESOURCE *map,
			UINT size, DXGI_FORMAT ib_fmt, UINT offset,
			UINT first, UINT count, D3D11_PRIMITIVE_TOPOLOGY topology);

	void DumpBuffer(ID3D11Buffer *buffer, wchar_t *filename,
			FrameAnalysisOptions buf_type_mask, int idx, DXGI_FORMAT ib_fmt,
			UINT stride, UINT offset, UINT first, UINT count, ID3DBlob *layout,
			D3D11_PRIMITIVE_TOPOLOGY topology, DrawCallInfo *call_info,
			ID3D11Buffer **staged_ib_ret, ID3D11Buffer *staged_ib_for_vb, UINT ib_off_for_vb);
	bool DeferDumpBuffer(ID3D11Buffer *staging,
			D3D11_BUFFER_DESC *orig_desc, wchar_t *filename,
			FrameAnalysisOptions buf_type_mask, int idx, DXGI_FORMAT ib_fmt,
			UINT stride, UINT offset, UINT first, UINT count, ID3DBlob *layout,
			D3D11_PRIMITIVE_TOPOLOGY topology, DrawCallInfo *call_info,
			ID3D11Buffer *staged_ib_for_vb, UINT ib_off_for_vb);
	void DumpBufferImmediateCtx(ID3D11Buffer *staging, D3D11_BUFFER_DESC *orig_desc,
			wstring filename, FrameAnalysisOptions buf_type_mask,
			int idx, DXGI_FORMAT ib_fmt, UINT stride, UINT offset,
			UINT first, UINT count, ID3DBlob *layout,
			D3D11_PRIMITIVE_TOPOLOGY topology, DrawCallInfo *call_info,
			ID3D11Buffer *staged_ib_for_vb, UINT ib_off_for_vb);

	void DumpResource(ID3D11Resource *resource, wchar_t *filename,
			FrameAnalysisOptions buf_type_mask, int idx, DXGI_FORMAT format,
			UINT stride, UINT offset);
	void _DumpCBs(char shader_type, bool compute,
		ID3D11Buffer *buffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]);
	void _DumpTextures(char shader_type, bool compute,
		ID3D11ShaderResourceView *views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]);
	void DumpCBs(bool compute);
	void DumpVBs(DrawCallInfo *call_info, ID3D11Buffer *staged_ib, DXGI_FORMAT ib_fmt, UINT ib_off);
	void DumpIB(DrawCallInfo *call_info, ID3D11Buffer **staged_ib, DXGI_FORMAT *format, UINT *offset);
	void DumpMesh(DrawCallInfo *call_info);
	void DumpTextures(bool compute);
	void DumpRenderTargets();
	void DumpDepthStencilTargets();
	void DumpUAVs(bool compute);
	template <typename DescType>
	void DumpDesc(DescType *desc, const wchar_t *filename);

	void dump_deferred_resources(ID3D11CommandList *command_list);
	void finish_deferred_resources(ID3D11CommandList *command_list);

	HRESULT FrameAnalysisFilename(wchar_t *filename, size_t size, bool compute,
			wchar_t *reg, char shader_type, int idx, ID3D11Resource *handle);
	HRESULT FrameAnalysisFilenameResource(wchar_t *filename, size_t size, const wchar_t *type,
			ID3D11Resource *handle, bool force_filename_handle);
	const wchar_t* dedupe_tex2d_filename(ID3D11Texture2D *resource,
			D3D11_TEXTURE2D_DESC *desc, wchar_t *dedupe_filename,
			size_t size, const wchar_t *traditional_filename, DXGI_FORMAT format);
	void dedupe_buf_filename(ID3D11Buffer *resource,
			D3D11_BUFFER_DESC *orig_desc,
			D3D11_MAPPED_SUBRESOURCE *map,
			wchar_t *dedupe_filename, size_t size);
	void dedupe_buf_filename_txt(const wchar_t *bin_filename,
			wchar_t *txt_filename, size_t size, char type, int idx,
			UINT stride, UINT offset);
	void dedupe_buf_filename_vb_txt(const wchar_t *bin_filename,
			wchar_t *txt_filename, size_t size, int idx,
			UINT stride, UINT offset, UINT first, UINT count, ID3DBlob *layout,
			D3D11_PRIMITIVE_TOPOLOGY topology, DrawCallInfo *call_info);
	void dedupe_buf_filename_ib_txt(const wchar_t *bin_filename,
			wchar_t *txt_filename, size_t size, DXGI_FORMAT ib_fmt,
			UINT offset, UINT first, UINT count, D3D11_PRIMITIVE_TOPOLOGY topology);
	void link_deduplicated_files(const wchar_t *filename, const wchar_t *dedupe_filename);
	void rotate_when_nearing_hard_link_limit(const wchar_t *dedupe_filename);
	void rotate_deduped_file(const wchar_t *dedupe_filename);
	void get_deduped_dir(wchar_t *path, size_t size);

	void determine_vb_count(UINT *count, ID3D11Buffer *staged_ib_for_vb,
			DrawCallInfo *call_info, UINT ib_off_for_vb, DXGI_FORMAT ib_fmt);

	void FrameAnalysisClearRT(ID3D11RenderTargetView *target);
	void FrameAnalysisClearUAV(ID3D11UnorderedAccessView *uav);
	void update_per_draw_analyse_options();
	void FrameAnalysisAfterDraw(bool compute, DrawCallInfo *call_info);
	void _FrameAnalysisAfterUpdate(ID3D11Resource *pResource,
			FrameAnalysisOptions type_mask, wchar_t *type);
	void FrameAnalysisAfterUnmap(ID3D11Resource *pResource);
	void FrameAnalysisAfterUpdate(ID3D11Resource *pResource);
	void update_stereo_dumping_mode();
	void set_default_dump_formats(bool draw);

	FrameAnalysisOptions analyse_options;
	FrameAnalysisOptions oneshot_analyse_options;
	bool oneshot_valid;
public:

	FrameAnalysisContext(ID3D11Device1 *pDevice, ID3D11DeviceContext1 *pContext);
	~FrameAnalysisContext();

	// public to allow CommandList access
	void FrameAnalysisLog(char *fmt, ...);   // theirs: override. Our HackerContext shim has no virtuals.
	void vFrameAnalysisLog(char *fmt, va_list ap);
#define FrameAnalysisLogNoNL FrameAnalysisLog

	void FrameAnalysisTrigger(FrameAnalysisOptions new_options);   // theirs: override. Our HackerContext shim has no virtuals.
	void FrameAnalysisDump(ID3D11Resource *resource, FrameAnalysisOptions options,
		const wchar_t *target, DXGI_FORMAT format, UINT stride, UINT offset);   // theirs: override. Our HackerContext shim has no virtuals.

public:
	void PortEntryAfterDraw(bool compute, DrawCallInfo *call_info)
	{
		if (G->analyse_frame)
			FrameAnalysisAfterDraw(compute, call_info);
		oneshot_valid = false;
		non_draw_call_dump_counter = 0;
	}

	unsigned PortDrawIndex() const { return draw_call; }

	ID3D11Texture2D *mPreviewSource = nullptr;

	// Class closed here. Everything from their line 236 -- /*** IUnknown methods ***/ onward -- is
	// the D3D11 vtable that FrameAnalysisContext overrides only to log each call. We reach the
	// engine from the addon's own draw hooks instead, so none of it is needed, and its bodies are
	// past line 3020 which frame_analysis_engine.cpp deliberately stops before.
};


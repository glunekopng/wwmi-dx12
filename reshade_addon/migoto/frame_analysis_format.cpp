// ===========================================================================================
//  frame_analysis_format.cpp -- THEIRS, PORTED VERBATIM from 3Dmigoto/DirectX11/FrameAnalysis.cpp
//  lines 718-1527. Extracted mechanically (scratchpad/extract_fa.py), not retyped.
//
//  This is the layer that DEFINES the on-disk frame-dump format that wwmi-tools imports:
//  the .txt sidecars for buffers, vertex buffers and index buffers, plus the dedupe filename
//  rules. It contains no D3D11 API calls and uses no member state (only FALogErr), so the only
//  change from the reference is dropping the FrameAnalysisContext:: qualifier.
//
//  WHY PORTED RATHER THAN REBUILT: our own dump reimplemented this and diverged -- it captures one
//  draw per two frames, cannot see our injected draws at all, and produced a session with zero
//  claimed draws. Reimplementing a file format is how you get files that look right and import
//  wrong. The DX12 half (staging copy + fence) is ours; the format is theirs.
// ===========================================================================================
#include "frame_analysis_format.h"
// util.h belongs HERE, not in the header: it pulls in crc32c.h, whose include path exists only in
// the migoto-lib build -- and the header is also included by the addon TU (frame_dump.cpp). The
// format code itself does need it (crc32c_hw, TexFormatStr, dxgi_format_size).
#include "util.h"

namespace FrameAnalysisFormat {

static void copy_until_extension(wchar_t *txt_filename, const wchar_t *bin_filename, size_t size, wchar_t **pos, size_t *rem)
{
	size_t ext_pos;
	const wchar_t *ext;

	ext = wcsrchr(bin_filename, L'.');
	if (ext)
		ext_pos = ext - bin_filename;
	else
		ext_pos = wcslen(bin_filename);

	StringCchPrintfExW(txt_filename, size, pos, rem, NULL, L"%.*s", ext_pos, bin_filename);
}

void dedupe_buf_filename_txt(const wchar_t *bin_filename,
		wchar_t *txt_filename, size_t size, char type, int idx,
		UINT stride, UINT offset)
{
	wchar_t *pos;
	size_t rem;

	copy_until_extension(txt_filename, bin_filename, MAX_PATH, &pos, &rem);

	if (idx != -1)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-%cb%i", type, idx);

	if (offset)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-offset=%u", offset);

	if (stride)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-stride=%u", stride);

	if (FAILED(StringCchPrintfW(pos, rem, L".txt")))
		FALogErr("Failed to create buffer filename\n");
}

void DumpBufferTxt(wchar_t *filename, D3D11_MAPPED_SUBRESOURCE *map,
		UINT size, char type, int idx, UINT stride, UINT offset)
{
	FILE *fd = NULL;
	char *components = "xyzw";
	float *buf = (float*)map->pData;
	UINT i, c;
	errno_t err;

	err = wfopen_ensuring_access(&fd, filename, L"w");
	if (!fd) {
		FALogErr("Unable to create %S: %u\n", filename, err);
		return;
	}

	if (offset)
		fprintf(fd, "offset: %u\n", offset);
	if (stride)
		fprintf(fd, "stride: %u\n", stride);

	for (i = offset / 16; i < size / 16; i++) {
		for (c = offset % 4; c < 4; c++) {
			if (idx == -1)
				fprintf(fd, "buf[%d].%c: %.9g\n", i, components[c], buf[i*4+c]);
			else
				fprintf(fd, "%cb%i[%d].%c: %.9g\n", type, idx, i, components[c], buf[i*4+c]);
		}
	}

	fclose(fd);
}

static const char* TopologyStr(D3D11_PRIMITIVE_TOPOLOGY topology)
{
	switch(topology) {
		case D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED: return "undefined";
		case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST: return "pointlist";
		case D3D11_PRIMITIVE_TOPOLOGY_LINELIST: return "linelist";
		case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP: return "linestrip";
		case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST: return "trianglelist";
		case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return "trianglestrip";
		case D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ: return "linelist_adj";
		case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ: return "linestrip_adj";
		case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ: return "trianglelist_adj";
		case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ: return "trianglestrip_adj";
		case D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST: return "1_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST: return "2_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST: return "3_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST: return "4_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_5_CONTROL_POINT_PATCHLIST: return "5_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_6_CONTROL_POINT_PATCHLIST: return "6_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_7_CONTROL_POINT_PATCHLIST: return "7_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_8_CONTROL_POINT_PATCHLIST: return "8_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_9_CONTROL_POINT_PATCHLIST: return "9_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_10_CONTROL_POINT_PATCHLIST: return "10_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_11_CONTROL_POINT_PATCHLIST: return "11_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_12_CONTROL_POINT_PATCHLIST: return "12_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_13_CONTROL_POINT_PATCHLIST: return "13_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_14_CONTROL_POINT_PATCHLIST: return "14_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_15_CONTROL_POINT_PATCHLIST: return "15_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_16_CONTROL_POINT_PATCHLIST: return "16_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_17_CONTROL_POINT_PATCHLIST: return "17_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_18_CONTROL_POINT_PATCHLIST: return "18_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_19_CONTROL_POINT_PATCHLIST: return "19_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_20_CONTROL_POINT_PATCHLIST: return "20_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_21_CONTROL_POINT_PATCHLIST: return "21_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_22_CONTROL_POINT_PATCHLIST: return "22_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_23_CONTROL_POINT_PATCHLIST: return "23_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_24_CONTROL_POINT_PATCHLIST: return "24_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_25_CONTROL_POINT_PATCHLIST: return "25_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_26_CONTROL_POINT_PATCHLIST: return "26_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_27_CONTROL_POINT_PATCHLIST: return "27_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_28_CONTROL_POINT_PATCHLIST: return "28_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_29_CONTROL_POINT_PATCHLIST: return "29_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_30_CONTROL_POINT_PATCHLIST: return "30_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_31_CONTROL_POINT_PATCHLIST: return "31_control_point_patchlist";
		case D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST: return "32_control_point_patchlist";
	}
	return "invalid";
}

void dedupe_buf_filename_vb_txt(const wchar_t *bin_filename,
		wchar_t *txt_filename, size_t size, int idx, UINT stride,
		UINT offset, UINT first, UINT count, ID3DBlob *layout,
		D3D11_PRIMITIVE_TOPOLOGY topology, DrawCallInfo *call_info)
{
	wchar_t *pos;
	size_t rem;
	uint32_t layout_hash;

	copy_until_extension(txt_filename, bin_filename, MAX_PATH, &pos, &rem);

	StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-vb%i", idx);

	if (layout) {
		layout_hash = crc32c_hw(0, layout->GetBufferPointer(), layout->GetBufferSize());
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-layout=%08x", layout_hash);
	}

	if (topology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-topology=%S", TopologyStr(topology));

	if (offset)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-offset=%u", offset);

	if (stride)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-stride=%u", stride);

	if (first)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-first=%u", first);

	if (count)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-count=%u", count);

	if (call_info && call_info->FirstInstance)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-first_inst=%u", call_info->FirstInstance);

	if (call_info && call_info->InstanceCount)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-inst_count=%u", call_info->InstanceCount);

	if (FAILED(StringCchPrintfW(pos, rem, L".txt")))
		FALogErr("Failed to create vertex buffer filename\n");
}

static void dump_ia_layout(FILE *fd, D3D11_INPUT_ELEMENT_DESC *layout_desc, size_t layout_elements, int slot, bool *per_vert, bool *per_inst)
{
	UINT i;

	for (i = 0; i < layout_elements; i++) {
		fprintf(fd, "element[%i]:\n", i);
		fprintf(fd, "  SemanticName: %s\n", layout_desc[i].SemanticName);
		fprintf(fd, "  SemanticIndex: %u\n", layout_desc[i].SemanticIndex);
		fprintf(fd, "  Format: %s\n", TexFormatStr(layout_desc[i].Format));
		fprintf(fd, "  InputSlot: %u\n", layout_desc[i].InputSlot);
		if (layout_desc[i].AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT)
			fprintf(fd, "  AlignedByteOffset: append\n");
		else
			fprintf(fd, "  AlignedByteOffset: %u\n", layout_desc[i].AlignedByteOffset);
		switch(layout_desc[i].InputSlotClass) {
			case D3D11_INPUT_PER_VERTEX_DATA:
				fprintf(fd, "  InputSlotClass: per-vertex\n");
				if (layout_desc[i].InputSlot == slot)
					*per_vert = true;
				break;
			case D3D11_INPUT_PER_INSTANCE_DATA:
				fprintf(fd, "  InputSlotClass: per-instance\n");
				if (layout_desc[i].InputSlot == slot)
					*per_inst = true;
				break;
			default:
				fprintf(fd, "  InputSlotClass: %u\n", layout_desc[i].InputSlotClass);
				break;
		}
		fprintf(fd, "  InstanceDataStepRate: %u\n", layout_desc[i].InstanceDataStepRate);
	}
}

static void dump_vb_unknown_layout(FILE *fd, D3D11_MAPPED_SUBRESOURCE *map,
		UINT size, int slot, UINT offset, UINT first, UINT count, UINT stride)
{
	float *buff = (float*)map->pData;
	uint32_t *buf32 = (uint32_t*)map->pData;
	uint8_t *buf8 = (uint8_t*)map->pData;
	UINT vertex, j, start, end, buf_idx;

	start = offset / stride + first;
	end = size / stride;
	if (count)
		end = min(end, start + count);

	for (vertex = start; vertex < end; vertex++) {
		fprintf(fd, "\n");

		for (j = 0; j < stride / 4; j++) {
			buf_idx = vertex * stride / 4 + j;
			fprintf(fd, "vb%i[%u]+%03u: 0x%08x %.9g\n", slot, vertex - start, j*4, buf32[buf_idx], buff[buf_idx]);
		}

		// In case we find one that is not a 32bit multiple finish off one byte at a time:
		for (j = j * 4; j < stride; j++) {
			buf_idx = vertex * stride + j;
			fprintf(fd, "vb%i[%u]+%03u: 0x%02x\n", slot, vertex - start, j, buf8[buf_idx]);
		}
	}
}

static UINT dxgi_format_alignment(DXGI_FORMAT format)
{
	switch (format) {
		case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
		case DXGI_FORMAT_R32G32B32A32_UINT:
		case DXGI_FORMAT_R32G32B32A32_SINT:
		case DXGI_FORMAT_R32G32B32_TYPELESS:
		case DXGI_FORMAT_R32G32B32_FLOAT:
		case DXGI_FORMAT_R32G32B32_UINT:
		case DXGI_FORMAT_R32G32B32_SINT:
		case DXGI_FORMAT_R32G32_TYPELESS:
		case DXGI_FORMAT_R32G32_FLOAT:
		case DXGI_FORMAT_R32G32_UINT:
		case DXGI_FORMAT_R32G32_SINT:
		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R32_UINT:
		case DXGI_FORMAT_R32_SINT:
			return 4;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R16G16B16A16_UINT:
		case DXGI_FORMAT_R16G16B16A16_SNORM:
		case DXGI_FORMAT_R16G16B16A16_SINT:
		case DXGI_FORMAT_R16G16_TYPELESS:
		case DXGI_FORMAT_R16G16_FLOAT:
		case DXGI_FORMAT_R16G16_UNORM:
		case DXGI_FORMAT_R16G16_UINT:
		case DXGI_FORMAT_R16G16_SNORM:
		case DXGI_FORMAT_R16G16_SINT:
		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_R16_UINT:
		case DXGI_FORMAT_R16_SNORM:
		case DXGI_FORMAT_R16_SINT:
			return 2;
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_UINT:
		case DXGI_FORMAT_R8G8B8A8_SNORM:
		case DXGI_FORMAT_R8G8B8A8_SINT:
		case DXGI_FORMAT_R8G8_TYPELESS:
		case DXGI_FORMAT_R8G8_UNORM:
		case DXGI_FORMAT_R8G8_UINT:
		case DXGI_FORMAT_R8G8_SNORM:
		case DXGI_FORMAT_R8G8_SINT:
		case DXGI_FORMAT_R8_TYPELESS:
		case DXGI_FORMAT_R8_UNORM:
		case DXGI_FORMAT_R8_UINT:
		case DXGI_FORMAT_R8_SNORM:
		case DXGI_FORMAT_R8_SINT:
		case DXGI_FORMAT_A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			return 1;
	}
	return 0;
}

static float float16(uint16_t f16)
{
	// Shift sign and mantissa to new positions:
	uint32_t f32 = ((f16 & 0x8000) << 16) | ((f16 & 0x3ff) << 13);
	// Need to check special cases of the biased exponent:
	int biased_exponent = (f16 & 0x7c00) >> 10;

	if (biased_exponent == 0) {
		// Zero / subnormal: New biased exponent remains zero
	} else if (biased_exponent == 0x1f) {
		// Infinity / NaN: New biased exponent is filled with 1s
		f32 |= 0x7f800000;
	} else {
		// Normal number: Adjust the exponent bias:
		biased_exponent = biased_exponent - 15 + 127;
		f32 |= biased_exponent << 23;
	}

	return *(float*)&f32;
}

static float unorm24(uint32_t val)
{
	return (float)val / (float)0xffffff;
}

static float unorm16(uint16_t val)
{
	return (float)val / (float)0xffff;
}

static float snorm16(int16_t val)
{
	return (float)val / (float)0x7fff;
}

static float unorm8(uint8_t val)
{
	return (float)val / (float)0xff;
}

static float snorm8(int8_t val)
{
	return (float)val / (float)0x7f;
}

static int fprint_dxgi_format(FILE *fd, DXGI_FORMAT format, uint8_t *buf)
{
	float *f = (float*)buf;
	uint32_t *u32 = (uint32_t*)buf;
	int32_t *s32 = (int32_t*)buf;
	uint16_t *u16 = (uint16_t*)buf;
	int16_t *s16 = (int16_t*)buf;
	uint8_t *u8 = (uint8_t*)buf;
	int8_t *s8 = (int8_t*)buf;
	unsigned i;

	switch (format) {
		// --- 32-bit ---
		case DXGI_FORMAT_R32G32B32A32_TYPELESS:
			return fprintf(fd, "%08x, %08x, %08x, %08x", u32[0], u32[1], u32[2], u32[3]);
		case DXGI_FORMAT_R32G32B32_TYPELESS:
			return fprintf(fd, "%08x, %08x, %08x", u32[0], u32[1], u32[2]);
		case DXGI_FORMAT_R32G32_TYPELESS:
			return fprintf(fd, "%08x, %08x", u32[0], u32[1]);
		case DXGI_FORMAT_R32_TYPELESS:
			return fprintf(fd, "%08x", u32[0]);

		case DXGI_FORMAT_R32G32B32A32_FLOAT:
			return fprintf(fd, "%.9g, %.9g, %.9g, %.9g", f[0], f[1], f[2], f[3]);
		case DXGI_FORMAT_R32G32B32_FLOAT:
			return fprintf(fd, "%.9g, %.9g, %.9g", f[0], f[1], f[2]);
		case DXGI_FORMAT_R32G32_FLOAT:
			return fprintf(fd, "%.9g, %.9g", f[0], f[1]);
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
			return fprintf(fd, "%.9g", f[0]);

		case DXGI_FORMAT_R32G32B32A32_UINT:
			return fprintf(fd, "%u, %u, %u, %u", u32[0], u32[1], u32[2], u32[3]);
		case DXGI_FORMAT_R32G32B32_UINT:
			return fprintf(fd, "%u, %u, %u", u32[0], u32[1], u32[2]);
		case DXGI_FORMAT_R32G32_UINT:
			return fprintf(fd, "%u, %u", u32[0], u32[1]);
		case DXGI_FORMAT_R32_UINT:
			return fprintf(fd, "%u", u32[0]);

		case DXGI_FORMAT_R32G32B32A32_SINT:
			return fprintf(fd, "%d, %d, %d, %d", s32[0], s32[1], s32[2], s32[3]);
		case DXGI_FORMAT_R32G32B32_SINT:
			return fprintf(fd, "%d, %d, %d", s32[0], s32[1], s32[2]);
		case DXGI_FORMAT_R32G32_SINT:
			return fprintf(fd, "%d, %d", s32[0], s32[1]);
		case DXGI_FORMAT_R32_SINT:
			return fprintf(fd, "%d", s32[0]);

		// --- 16-bit ---
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			return fprintf(fd, "%04x, %04x, %04x, %04x", u16[0], u16[1], u16[2], u16[3]);
		case DXGI_FORMAT_R16G16_TYPELESS:
			return fprintf(fd, "%04x, %04x", u16[0], u16[1]);
		case DXGI_FORMAT_R16_TYPELESS:
			return fprintf(fd, "%04x", u16[0]);

		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			return fprintf(fd, "%.9g, %.9g, %.9g, %.9g", float16(u16[0]), float16(u16[1]), float16(u16[2]), float16(u16[3]));
		case DXGI_FORMAT_R16G16_FLOAT:
			return fprintf(fd, "%.9g, %.9g", float16(u16[0]), float16(u16[1]));
		case DXGI_FORMAT_R16_FLOAT:
			return fprintf(fd, "%.9g", float16(u16[0]));

		case DXGI_FORMAT_R16G16B16A16_UNORM:
			return fprintf(fd, "%.9g, %.9g, %.9g, %.9g", unorm16(u16[0]), unorm16(u16[1]), unorm16(u16[2]), unorm16(u16[3]));
		case DXGI_FORMAT_R16G16_UNORM:
			return fprintf(fd, "%.9g, %.9g", unorm16(u16[0]), unorm16(u16[1]));
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R16_UNORM:
			return fprintf(fd, "%.9g", unorm16(u16[0]));

		case DXGI_FORMAT_R16G16B16A16_SNORM:
			return fprintf(fd, "%.9g, %.9g, %.9g, %.9g", snorm16(s16[0]), snorm16(s16[1]), snorm16(s16[2]), snorm16(s16[3]));
		case DXGI_FORMAT_R16G16_SNORM:
			return fprintf(fd, "%.9g, %.9g", snorm16(s16[0]), snorm16(s16[1]));
		case DXGI_FORMAT_R16_SNORM:
			return fprintf(fd, "%.9g", snorm16(s16[0]));

		case DXGI_FORMAT_R16G16B16A16_UINT:
			return fprintf(fd, "%u, %u, %u, %u", u16[0], u16[1], u16[2], u16[3]);
		case DXGI_FORMAT_R16G16_UINT:
			return fprintf(fd, "%u, %u", u16[0], u16[1]);
		case DXGI_FORMAT_R16_UINT:
			return fprintf(fd, "%u", u16[0]);

		case DXGI_FORMAT_R16G16B16A16_SINT:
			return fprintf(fd, "%d, %d, %d, %d", s16[0], s16[1], s16[2], s16[3]);
		case DXGI_FORMAT_R16G16_SINT:
			return fprintf(fd, "%d, %d", s16[0], s16[1]);
		case DXGI_FORMAT_R16_SINT:
			return fprintf(fd, "%d", s16[0]);

		// --- 8-bit ---
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			return fprintf(fd, "%02x, %02x, %02x, %02x", u8[0], u8[1], u8[2], u8[3]);
		case DXGI_FORMAT_B8G8R8X8_TYPELESS:
			return fprintf(fd, "%02x, %02x, %02x", u8[0], u8[1], u8[2]);
		case DXGI_FORMAT_R8G8_TYPELESS:
			return fprintf(fd, "%02x, %02x", u8[0], u8[1]);
		case DXGI_FORMAT_R8_TYPELESS:
			return fprintf(fd, "%02x", u8[0]);

		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: // XXX: Should we apply the SRGB formula?
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: // XXX: Should we apply the SRGB formula?
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: // XXX: Should we apply the SRGB formula?
		case DXGI_FORMAT_R8G8_B8G8_UNORM:
		case DXGI_FORMAT_G8R8_G8B8_UNORM:
			return fprintf(fd, "%.9g, %.9g, %.9g, %.9g", unorm8(u8[0]), unorm8(u8[1]), unorm8(u8[2]), unorm8(u8[3]));
		case DXGI_FORMAT_R8G8_UNORM:
			return fprintf(fd, "%.9g, %.9g", unorm8(u8[0]), unorm8(u8[1]));
		case DXGI_FORMAT_R8_UNORM:
			return fprintf(fd, "%.9g", unorm8(u8[0]));

		case DXGI_FORMAT_R8G8B8A8_SNORM:
			return fprintf(fd, "%.9g, %.9g, %.9g, %.9g", snorm8(s8[0]), snorm8(s8[1]), snorm8(s8[2]), snorm8(s8[3]));
		case DXGI_FORMAT_R8G8_SNORM:
			return fprintf(fd, "%.9g, %.9g", snorm8(s8[0]), snorm8(s8[1]));
		case DXGI_FORMAT_R8_SNORM:
		case DXGI_FORMAT_A8_UNORM:
			return fprintf(fd, "%.9g", snorm8(s8[0]));

		case DXGI_FORMAT_R8G8B8A8_UINT:
			return fprintf(fd, "%u, %u, %u, %u", u8[0], u8[1], u8[2], u8[3]);
		case DXGI_FORMAT_R8G8_UINT:
			return fprintf(fd, "%u, %u", u8[0], u8[1]);
		case DXGI_FORMAT_R8_UINT:
			return fprintf(fd, "%u", u8[0]);

		case DXGI_FORMAT_R8G8B8A8_SINT:
			return fprintf(fd, "%d, %d, %d, %d", s8[0], s8[1], s8[2], s8[3]);
		case DXGI_FORMAT_R8G8_SINT:
			return fprintf(fd, "%d, %d", s8[0], s8[1]);
		case DXGI_FORMAT_R8_SINT:
			return fprintf(fd, "%d", s8[0]);

		case DXGI_FORMAT_R32G8X24_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
		case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
			return fprintf(fd, "%.9g, %d", f[0], u8[4]);

		case DXGI_FORMAT_R24G8_TYPELESS:
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
		case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
		case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
			return fprintf(fd, "%.9g, %d", unorm24(u32[0] & 0xffffff), u8[3]);

	}

	for (i = 0; i < dxgi_format_size(format); i++)
		fprintf(fd, "%02x", buf[i]);
	return i * 2;
}

static void dump_vb_elem(FILE *fd, uint8_t *buf, size_t avail,   // PORT-LEAF: avail = bytes from buf to the end of the map
		D3D11_INPUT_ELEMENT_DESC *layout_desc, size_t layout_elements,
		int slot, UINT vb_idx, UINT elem, UINT stride)
{
	UINT offset = 0, alignment, size;

	if (layout_desc[elem].InputSlot != slot)
		return;

	if (layout_desc[elem].AlignedByteOffset != D3D11_APPEND_ALIGNED_ELEMENT) {
		offset = layout_desc[elem].AlignedByteOffset;
	} else {
		alignment = dxgi_format_alignment(layout_desc[elem].Format);
		if (!alignment) {
			fprintf(fd, "# WARNING: Unknown format alignment, vertex buffer may be decoded incorrectly\n");
		} else if (offset % alignment) {
			fprintf(fd, "# WARNING: Untested alignment code in use, please report incorrectly decoded vertex buffers\n");
			// XXX: Also, what if the entire vertex is misaligned in the buffer?
			offset += alignment - (offset % alignment);
		}
	}

	fprintf(fd, "vb%i[%u]+%03u %s", slot, vb_idx, offset, layout_desc[elem].SemanticName);
	if (layout_desc[elem].SemanticIndex)
		fprintf(fd, "%u", layout_desc[elem].SemanticIndex);
	fprintf(fd, ": ");

	size = dxgi_format_size(layout_desc[elem].Format);
	if ((size_t)offset + (size ? size : 16) > avail) {
		fprintf(fd, "# beyond the end of the buffer, not read\n");
		return;
	}
	fprint_dxgi_format(fd, layout_desc[elem].Format, buf + offset);
	fprintf(fd, "\n");

	if (!size)
		fprintf(fd, "# WARNING: Unknown format size, vertex buffer may be decoded incorrectly\n");
	offset += size;
	if (offset > stride)
		fprintf(fd, "# WARNING: Offset exceeded stride, vertex buffer may be decoded incorrectly\n");
}

static void dump_vb_known_layout(FILE *fd, D3D11_MAPPED_SUBRESOURCE *map,
		D3D11_INPUT_ELEMENT_DESC *layout_desc, size_t layout_elements,
		UINT size, int slot, UINT offset, UINT first, UINT count, UINT stride)
{
	UINT vertex, elem, start, end;

	start = offset / stride + first;
	end = size / stride;
	if (count)
		end = min(end, start + count);

	for (vertex = start; vertex < end; vertex++) {
		fprintf(fd, "\n");
		for (elem = 0; elem < layout_elements; elem++) {
			if (layout_desc[elem].InputSlotClass != D3D11_INPUT_PER_VERTEX_DATA)
				continue;

			dump_vb_elem(fd, (uint8_t*)map->pData + stride*vertex, (size_t)size - (size_t)stride*vertex,   // PORT-LEAF
					layout_desc, layout_elements, slot,
					vertex - start, elem, stride);
		}
	}
}

static void dump_vb_instance_data(FILE *fd, D3D11_MAPPED_SUBRESOURCE *map,
		D3D11_INPUT_ELEMENT_DESC *layout_desc, size_t layout_elements,
		UINT size, int slot, UINT offset, UINT first, UINT count, UINT stride)
{
	UINT instance, idx, elem, start, end;

	start = offset / stride + first;
	end = size / stride;
	if (count)
		end = min(end, start + count);

	for (instance = start; instance < end; instance++) {
		fprintf(fd, "\n");
		for (elem = 0; elem < layout_elements; elem++) {
			if (layout_desc[elem].InputSlotClass != D3D11_INPUT_PER_INSTANCE_DATA)
				continue;

			if (layout_desc[elem].InstanceDataStepRate)
				idx = (instance-start) / layout_desc[elem].InstanceDataStepRate + start;
			else
				idx = instance;

			dump_vb_elem(fd, (uint8_t*)map->pData + stride*idx, (size_t)size - (size_t)stride*idx,   // PORT-LEAF
					layout_desc, layout_elements, slot,
					idx - start, elem, stride);
		}
	}
}

void DumpVBTxt(wchar_t *filename, D3D11_MAPPED_SUBRESOURCE *map,
		UINT size, int slot, UINT stride, UINT offset, UINT first, UINT count, ID3DBlob *layout,
		D3D11_PRIMITIVE_TOPOLOGY topology, DrawCallInfo *call_info)
{
	FILE *fd = NULL;
	errno_t err;
	D3D11_INPUT_ELEMENT_DESC *layout_desc = NULL;
	size_t layout_elements;
	bool per_vert = false, per_inst = false;

	err = wfopen_ensuring_access(&fd, filename, L"w");
	if (!fd) {
		FALogErr("Unable to create %S: %u\n", filename, err);
		return;
	}

	if (offset)
		fprintf(fd, "byte offset: %u\n", offset);
	fprintf(fd, "stride: %u\n", stride);
	if (first || count) {
		fprintf(fd, "first vertex: %u\n", first);
		fprintf(fd, "vertex count: %u\n", count);
	}
	if (call_info && call_info->FirstInstance || call_info->InstanceCount) {
		fprintf(fd, "first instance: %u\n", call_info->FirstInstance);
		fprintf(fd, "instance count: %u\n", call_info->InstanceCount);
	}
	if (topology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED)
		fprintf(fd, "topology: %s\n", TopologyStr(topology));
	if (layout) {
		layout_desc = (D3D11_INPUT_ELEMENT_DESC*)layout->GetBufferPointer();
		layout_elements = layout->GetBufferSize() / sizeof(D3D11_INPUT_ELEMENT_DESC);
		dump_ia_layout(fd, layout_desc, layout_elements, slot, &per_vert, &per_inst);
	}
	if (!stride) {
		FALogErr("Cannot dump vertex buffer with stride=0\n");
		goto out_close;
	}

	if (layout_desc) {
		if (per_vert) {
			fprintf(fd, "\nvertex-data:\n");
			dump_vb_known_layout(fd, map, layout_desc, layout_elements,
					size, slot, offset, first, count, stride);
		}

		if (per_inst && call_info) {
			fprintf(fd, "\ninstance-data:\n");
			dump_vb_instance_data(fd, map, layout_desc,
					layout_elements, size, slot, offset,
					call_info->FirstInstance,
					call_info->InstanceCount, stride);
		}
	} else {
		dump_vb_unknown_layout(fd, map, size, slot, offset, first, count, stride);
	}

out_close:
	fclose(fd);
}

void dedupe_buf_filename_ib_txt(const wchar_t *bin_filename,
		wchar_t *txt_filename, size_t size, DXGI_FORMAT ib_fmt,
		UINT offset, UINT first, UINT count, D3D11_PRIMITIVE_TOPOLOGY topology)
{
	wchar_t *pos;
	size_t rem;

	copy_until_extension(txt_filename, bin_filename, MAX_PATH, &pos, &rem);

	StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-ib");

	if (ib_fmt != DXGI_FORMAT_UNKNOWN)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-format=%S", TexFormatStr(ib_fmt));

	if (topology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-topology=%S", TopologyStr(topology));

	if (offset)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-offset=%u", offset);

	if (first)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-first=%u", first);

	if (count)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-count=%u", count);

	if (FAILED(StringCchPrintfW(pos, rem, L".txt")))
		FALogErr("Failed to create index buffer filename\n");
}

void DumpIBTxt(wchar_t *filename, D3D11_MAPPED_SUBRESOURCE *map,
		UINT size, DXGI_FORMAT format, UINT offset, UINT first, UINT count,
		D3D11_PRIMITIVE_TOPOLOGY topology)
{
	FILE *fd = NULL;
	uint16_t *buf16 = (uint16_t*)map->pData;
	uint32_t *buf32 = (uint32_t*)map->pData;
	UINT start, end, i;
	errno_t err;
	int grouping = 1;

	err = wfopen_ensuring_access(&fd, filename, L"w");
	if (!fd) {
		FALogErr("Unable to create %S: %u\n", filename, err);
		return;
	}

	fprintf(fd, "byte offset: %u\n", offset);
	if (first || count) {
		fprintf(fd, "first index: %u\n", first);
		fprintf(fd, "index count: %u\n", count);
	}

	if (topology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED)
		fprintf(fd, "topology: %s\n", TopologyStr(topology));
	switch(topology) {
		case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:
			grouping = 2;
			break;
		case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
			grouping = 3;
			break;
		// TODO: Appropriate grouping for other input topologies
	}

	switch(format) {
	case DXGI_FORMAT_R16_UINT:
		fprintf(fd, "format: DXGI_FORMAT_R16_UINT\n");

		start = offset / 2 + first;
		end = size / 2;
		if (count)
			end = min(end, start + count);

		for (i = start; i < end; i++) {
			if ((i-start) % grouping == 0)
				fprintf(fd, "\n");
			else
				fprintf(fd, " ");
			fprintf(fd, "%u", buf16[i]);
		}
		fprintf(fd, "\n");
		break;
	case DXGI_FORMAT_R32_UINT:
		fprintf(fd, "format: DXGI_FORMAT_R32_UINT\n");

		start = offset / 4 + first;
		end = size / 4;
		if (count)
			end = min(end, start + count);

		for (i = start; i < end; i++) {
			if ((i-start) % grouping == 0)
				fprintf(fd, "\n");
			else
				fprintf(fd, " ");
			fprintf(fd, "%u", buf32[i]);
		}
		fprintf(fd, "\n");
		break;
	default:
		// Illegal format for an index buffer
		fprintf(fd, "format %u is illegal\n", format);
		break;
	}

	fclose(fd);
}

template <typename DescType>
void DumpDesc(DescType *desc, const wchar_t *filename)
{
	FILE *fd = NULL;
	char buf[256];
	errno_t err;

	StrResourceDesc(buf, 256, desc);

	err = wfopen_ensuring_access(&fd, filename, L"w");
	if (!fd) {
		FALogErr("Unable to create %S: %u\n", filename, err);
		return;
	}
	fwrite(buf, 1, strlen(buf), fd);
	putc('\n', fd);
	fclose(fd);
}

} // namespace FrameAnalysisFormat


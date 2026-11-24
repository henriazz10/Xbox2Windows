// Desktop xg_xs.dll trap stubs.
// Every export traps; see trap.h. Scaffolding, not an ABI claim.
#define X2W_TRAP_MODULE "xg_xs.dll"
#define X2W_TRAP_MODULE_ID 5
#include "startup_stubs/trap.h"
#include "startup_stubs/local_log.h"

#include "startup_stubs/slice_footprints.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

// Experimental (docs/startup-runtime.md). With X2W_D3D12_PERMISSIVE=1 the exports
// stand in for the console texture-layout library; otherwise they trap.
//
// The title describes a texture with a D3D12_RESOURCE_DESC-shaped block (0x38 bytes; the Layout
// field holds a console swizzle mode) and asks a "texture computer" for its memory layout. The
// console layouts are swizzled; this stand-in answers with the desktop's linear layout instead, so
// the sizes and offsets the title uses are consistent with each other, but texel data it copies or
// reads through them is not in the console arrangement.
namespace {
using u64 = std::uint64_t;

bool permissive() noexcept { return x2w_local::env_on("X2W_D3D12_PERMISSIVE"); }

ID3D12Device* layout_device()
{
    static ID3D12Device* device = [] {
        ID3D12Device* created = nullptr;
        const HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&created));
        x2w_local::logf("x2w-xg layout device -> 0x%08lx", static_cast<unsigned long>(hr));
        return created;
    }();
    return device;
}

DXGI_FORMAT desktop_format(DXGI_FORMAT format)
{
    switch (static_cast<UINT>(format)) {
    case 116: case 117: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 118: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case 119: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case 120: return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    default: return format;
    }
}

D3D12_RESOURCE_DESC desktop_desc(const D3D12_RESOURCE_DESC& desc)
{
    D3D12_RESOURCE_DESC copy = desc;
    copy.Alignment = 0;
    copy.Format = desktop_format(desc.Format);
    copy.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    copy.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<UINT>(desc.Flags) & 0x3f);
    if (copy.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
        copy.Flags &= ~(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if (!copy.SampleDesc.Count) copy.SampleDesc.Count = 1;
    return copy;
}

struct Computer {
    void* const* vtable;
    D3D12_RESOURCE_DESC desc;
    LONG refs;
};

// Slot 2: (layout*) -> HRESULT. Layout as the title reads it: a 0x18-byte header (taken as total
// size, base alignment, mip count, plane count), then planes of 0x6c8 bytes. In a plane: +0x10 is
// multiplied by the array slice index (a per-slice stride), and mip records of 0x70 bytes start at
// +0x38 with size at +0x00, offset at +0x08, slice size at +0x10 and row pitch in bytes at +0x1c.
// The remaining mip fields follow the usual order (pitch in pixels, alignment, padded and plain
// dimensions, sample count) as a guess. NV12 additionally describes its interleaved chroma plane.
constexpr std::size_t layout_header = 0x18, plane_bytes = 0x6c8, mip_base = 0x38, mip_bytes = 0x70;
constexpr int max_mips = 15;

long __stdcall computer_layout(Computer* self, unsigned char* layout)
{
    if (!layout) return static_cast<long>(0x80004003);
    std::memset(layout, 0, layout_header + 4 * plane_bytes + 8);
    ID3D12Device* device = layout_device();
    if (!device) return static_cast<long>(0x80004005);
    const D3D12_RESOURCE_DESC desc = desktop_desc(self->desc);
    const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    const unsigned slices = volume ? 1 : desc.DepthOrArraySize ? desc.DepthOrArraySize : 1;
    unsigned mips = desc.MipLevels ? desc.MipLevels : 1;
    if (mips > max_mips) mips = max_mips;
    const x2w_startup::SliceFootprints plan(device, desc, mips);
    const auto& footprints = plan.placed;
    const auto& rows = plan.rows;
    UINT64 slice_total = plan.size;
    const bool valid = slice_total != ~0ull && slice_total != 0;
    if (!valid) slice_total = 0x10000;
    const u64 stride = (slice_total + 0xFFFF) & ~u64{0xFFFF};
    const u64 total = stride * slices;

    auto put64 = [](unsigned char* at, u64 value) { std::memcpy(at, &value, 8); };
    auto put32 = [](unsigned char* at, std::uint32_t value) { std::memcpy(at, &value, 4); };
    put64(layout + 0x00, total);
    put64(layout + 0x08, 0x10000);
    put32(layout + 0x10, mips);
    put32(layout + 0x14, plan.planes);
    for (unsigned p = 0; p < plan.planes; ++p) {
        unsigned char* plane = layout + layout_header + p * plane_bytes;
        put64(plane + 0x08, total);
        put64(plane + 0x10, slices > 1 ? stride : 0);
        put64(plane + 0x18, 0x10000);
        for (unsigned m = 0; valid && m < mips; ++m) {
            const unsigned i = p * mips + m;
            unsigned char* mip = plane + mip_base + m * mip_bytes;
            const auto& fp = footprints[i].Footprint;
            const u64 slice_size = u64{fp.RowPitch} * rows[i];
            put64(mip + 0x00, slice_size * fp.Depth);
            put64(mip + 0x08, footprints[i].Offset);
            put64(mip + 0x10, slice_size);
            put32(mip + 0x18, fp.Width);
            put32(mip + 0x1c, fp.RowPitch);
            put32(mip + 0x20, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
            put32(mip + 0x24, fp.Width);
            put32(mip + 0x28, fp.Height);
            put32(mip + 0x2c, volume ? fp.Depth : slices);
            put32(mip + 0x30, fp.Width);
            put32(mip + 0x34, fp.Height);
            put32(mip + 0x38, volume ? fp.Depth : slices);
            put32(mip + 0x3c, desc.SampleDesc.Count);
        }
    }
    static LONG calls, failures;
    static const bool verbose = [] { char v[8] = {}; return GetEnvironmentVariableA("X2W_XG_VERBOSE", v, sizeof v) > 0 && v[0] == '1'; }();
    if (x2w_local::first_calls(calls, verbose ? 4000 : 6) || (!valid && x2w_local::first_calls(failures, 8))) {
        char sizes[256] = {};
        int used = 0;
        for (unsigned i = 0; valid && i < mips && used < 200; ++i)
            used += std::snprintf(sizes + used, sizeof sizes - used, " %llx@%llx",
                                  u64{footprints[i].Footprint.RowPitch} * rows[i] * footprints[i].Footprint.Depth, footprints[i].Offset);
        x2w_local::logf("x2w-xg TextureComputer::slot2(layout) dim=%d %llux%ux%u mips=%u fmt=%d(->%d) layout=0x%x -> %s "
                        "slice=0x%llx total=0x%llx mip0 pitch=%u mips:%s", self->desc.Dimension, self->desc.Width, self->desc.Height,
                        self->desc.DepthOrArraySize, self->desc.MipLevels, self->desc.Format, desc.Format, self->desc.Layout,
                        valid ? "ok" : "DESKTOP REJECTED THE DESC", stride, total, valid ? footprints[0].Footprint.RowPitch : 0, sizes);
    }
    return 0;
}

// Slot 10: the desktop ID3D12Device::GetResourceTiling shape without the resource:
// (tile count*, packed mip info*, tile shape*, tiling count in/out*, first subresource, tilings*).
// Answered from a desktop reserved resource with the same description.
void __stdcall computer_tiling(Computer* self, UINT* tiles, D3D12_PACKED_MIP_INFO* packed, D3D12_TILE_SHAPE* shape,
                               UINT* count, UINT first, D3D12_SUBRESOURCE_TILING* tilings)
{
    if (tiles) *tiles = 0;
    if (packed) *packed = {};
    if (shape) *shape = {};
    const UINT capacity = count ? *count : 0;
    if (count) *count = 0;
    ID3D12Device* device = layout_device();
    HRESULT hr = E_FAIL;
    if (device) {
        D3D12_RESOURCE_DESC desc = desktop_desc(self->desc);
        desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
        ID3D12Resource* reserved = nullptr;
        hr = device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&reserved));
        if (SUCCEEDED(hr)) {
            UINT wanted = capacity;
            device->GetResourceTiling(reserved, tiles, packed, shape, count ? &wanted : nullptr, first, tilings);
            if (count) *count = wanted;
            reserved->Release();
        }
    }
    static LONG calls, failures;
    if (x2w_local::first_calls(calls, 6) || (FAILED(hr) && x2w_local::first_calls(failures, 8)))
        x2w_local::logf("x2w-xg TextureComputer::slot10(tiling) dim=%d %llux%ux%u mips=%u fmt=%d flags=0x%x -> 0x%08lx tiles=%u "
                        "standard=%u packed=%u", self->desc.Dimension, self->desc.Width, self->desc.Height,
                        self->desc.DepthOrArraySize, self->desc.MipLevels, self->desc.Format, self->desc.Flags,
                        static_cast<unsigned long>(hr), tiles ? *tiles : 0, packed ? packed->NumStandardMips : 0,
                        packed ? packed->NumPackedMips : 0);
}

// Slot 1: called when the title is done with the computer.
// Slot 8, CopyIntoSubresource(tiled base, plane, subresource, linear data, row pitch, slice pitch)
// (argument order as in DirectXTex's use of the public XG header). The stand-in's layout is the
// desktop copyable-footprint layout reported by slot 2, so the "tiled" memory is filled row by row
// at those offsets; the D3D12 stand-in is told so that it can refresh its desktop texture.
long __stdcall computer_copy_into(Computer* self, unsigned char* base, unsigned plane, unsigned subresource,
                                  const unsigned char* linear, unsigned row_pitch, unsigned slice_pitch)
{
    static LONG calls, refused;
    ID3D12Device* device = layout_device();
    const D3D12_RESOURCE_DESC desc = desktop_desc(self->desc);
    const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    const unsigned slices = volume ? 1 : desc.DepthOrArraySize ? desc.DepthOrArraySize : 1;
    unsigned mips = desc.MipLevels ? desc.MipLevels : 1;
    if (mips > max_mips) mips = max_mips;
    const unsigned mip = subresource % mips, slice = subresource / mips;
    const x2w_startup::SliceFootprints plan(device, desc, mips);
    const UINT i = plane * mips + mip;
    const auto& footprints = plan.placed;
    const auto& rows = plan.rows;
    const auto& row_bytes = plan.row_bytes;
    const UINT64 slice_total = plan.size;
    if (!device || !base || !linear || plane >= plan.planes || slice >= slices || slice_total == ~0ull || !slice_total || !row_pitch ||
        (desc.Format == DXGI_FORMAT_NV12 && row_pitch < row_bytes[i])) {
        if (x2w_local::first_calls(refused, 8))
            x2w_local::logf("x2w-xg TextureComputer::slot8(copy into) refused: base=%p plane=%u subresource=%u linear=%p pitch=%u/%u",
                            static_cast<void*>(base), plane, subresource, static_cast<const void*>(linear), row_pitch, slice_pitch);
        return static_cast<long>(0x80070057);
    }
    const u64 stride = (slice_total + 0xFFFF) & ~u64{0xFFFF};
    const auto& fp = footprints[i].Footprint;
    const u64 used = row_bytes[i] < row_pitch ? row_bytes[i] : row_pitch;
    unsigned char* target = base + stride * slice + footprints[i].Offset;
    const u64 target_bytes = plan.bytes(i);
    const u64 source_slice = slice_pitch ? slice_pitch : u64{row_pitch} * rows[i];
    const bool ok = x2w_local::writable(target, target_bytes) &&
                    x2w_local::readable(linear, source_slice * (fp.Depth - 1) + u64{row_pitch} * (rows[i] - 1) + used);
    if (ok) {
        for (unsigned z = 0; z < fp.Depth; ++z)
            for (unsigned y = 0; y < rows[i]; ++y)
                std::memcpy(target + (u64{z} * rows[i] + y) * fp.RowPitch, linear + z * source_slice + u64{y} * row_pitch, used);
        using Notify = void (*)(void*, unsigned long long);
        static const Notify notify = [] {
            const HMODULE d3d = GetModuleHandleW(L"d3d12_xs.dll");
            return d3d ? reinterpret_cast<Notify>(GetProcAddress(d3d, "x2w_d3d12_texture_written")) : nullptr;
        }();
        if (notify) notify(base, stride * slices);
    }
    if (x2w_local::first_calls(calls, 12) || !ok)
        x2w_local::logf("x2w-xg TextureComputer::slot8(copy into) base=%p subresource=%u (mip %u slice %u) %ux%u fmt=%d rows=%u "
                        "pitch %u->%u -> %s", static_cast<void*>(base), subresource, mip, slice, fp.Width, fp.Height, desc.Format,
                        rows[i], row_pitch, fp.RowPitch, ok ? "copied" : "MEMORY NOT ACCESSIBLE");
    return ok ? 0 : static_cast<long>(0x80004005);
}

u64 __stdcall computer_release(Computer* self)
{
    if (InterlockedDecrement(&self->refs) == 0) delete self;
    return 0;
}

// Other slots (slot 8 copies linear texels into the console layout): logged once, nothing done.
template <int N>
__declspec(noinline) u64 computer_slot(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    static LONG calls;
    if (InterlockedIncrement(&calls) <= 1) {
        char name[48];
        std::snprintf(name, sizeof(name), "TextureComputer::slot%d(no-op)", N);
        x2w_trap::record("x2w-xg", X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, name, N, rcx, rdx, r8, r9, _ReturnAddress());
    }
    return 0;
}

template <std::size_t... I>
auto computer_table(std::index_sequence<I...>)
{
    return std::array<void*, sizeof...(I)>{reinterpret_cast<void*>(&computer_slot<static_cast<int>(I)>)...};
}
}  // namespace

// XGCreateTextureComputer(desc*, computer**) -> HRESULT.
extern "C" __declspec(noinline) long XGCreateTextureComputer(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "XGCreateTextureComputer", 0, rcx, rdx, r8, r9, _ReturnAddress());
    static auto table = computer_table(std::make_index_sequence<32>{});
    static bool ready = false;
    if (!ready) {
        table[1] = reinterpret_cast<void*>(&computer_release);
        table[2] = reinterpret_cast<void*>(&computer_layout);
        table[8] = reinterpret_cast<void*>(&computer_copy_into);
        table[10] = reinterpret_cast<void*>(&computer_tiling);
        ready = true;
    }
    if (!rcx || !rdx) return static_cast<long>(0x80004003);
    const auto& desc = *reinterpret_cast<const D3D12_RESOURCE_DESC*>(rcx);
    if (!x2w_startup::SliceFootprints::supports(desc)) {
        *reinterpret_cast<Computer**>(rdx) = nullptr;
        return E_NOTIMPL;
    }
    auto* computer = new Computer{table.data(), desc, 1};
    *reinterpret_cast<Computer**>(rdx) = computer;
    static LONG calls;
    if (InterlockedIncrement(&calls) <= 3)
        x2w_trap::record("x2w-xg", X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "XGCreateTextureComputer(stand-in)", 0, rcx, rdx, r8,
                         r9, _ReturnAddress());
    return 0;
}

// XGSuggestSwizzleMode(desc, 0, &swizzle_field, 1) -> HRESULT, after the title stored a default
// swizzle (0x100 or 0x102) in that field. Desktop resources do not use console swizzle modes, so the
// title's default is kept.
extern "C" __declspec(noinline) long XGSuggestSwizzleMode(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "XGSuggestSwizzleMode", 1, rcx, rdx, r8, r9, _ReturnAddress());
    static LONG calls;
    if (InterlockedIncrement(&calls) <= 3)
        x2w_trap::record("x2w-xg", X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "XGSuggestSwizzleMode(keep default, S_OK)", 1, rcx,
                         rdx, r8, r9, _ReturnAddress());
    return 0;
}

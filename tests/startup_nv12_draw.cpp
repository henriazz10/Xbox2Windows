#include "cpu/shim_gpu_device.h"
#include "desktop_gpu.h"
#include "startup_stubs/slice_footprints.h"

#include <d3dcompiler.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
extern "C" __declspec(dllimport) HRESULT XGCreateTextureComputer(const D3D12_RESOURCE_DESC*, void***, UINT64, UINT64);

namespace {
constexpr UINT width = 8, height = 4;

struct TextureComputer {
    void** object;
    void** table() const { return static_cast<void**>(object[0]); }
    ~TextureComputer() { reinterpret_cast<UINT64(*)(void*)>(table()[1])(object); }
};

UINT luma(UINT x, UINT y) { return 95 + x + 3 * y; }
UINT chroma_u(UINT x, UINT y) { return 91 + x + 2 * y; }
UINT chroma_v(UINT x, UINT y) { return 157 - 2 * x + y; }

UINT expected(UINT x, UINT y, bool swapped)
{
    const int yy = luma(x, y);
    const int u = chroma_u(x / 2, y / 2) - 128;
    const int v = chroma_v(x / 2, y / 2) - 128;
    const int cb = swapped ? v : u;
    const int cr = swapped ? u : v;
    // Integer approximation of a full-range YCbCr conversion. Chosen inputs
    // keep all values in range; preserve all three channels independently.
    return (yy + (359 * cr >> 8)) | ((yy - ((88 * cb + 183 * cr) >> 8)) << 8) |
           ((yy + (454 * cb >> 8)) << 16) | 0xff000000u;
}

void populate(const D3D12_RESOURCE_DESC& desc, const x2w_startup::SliceFootprints& plan, unsigned char* base, bool swapped)
{
    void** instance = nullptr;
    check(XGCreateTextureComputer(&desc, &instance, 0, 0), "XGCreateTextureComputer");
    TextureComputer computer{instance};
    using Query = HRESULT(*)(void*, unsigned char*);
    using Write = HRESULT(*)(void*, unsigned char*, UINT, UINT, const unsigned char*, UINT, UINT);
    std::array<unsigned char, 0x18 + 4 * 0x6c8 + 8> layout{};
    check(reinterpret_cast<Query>(computer.table()[2])(instance, layout.data()), "XG layout");
    UINT reported = 0;
    std::memcpy(&reported, layout.data() + 0x14, sizeof(reported));
    if (reported != 2) throw std::runtime_error("XG did not report both NV12 planes");
    auto write = reinterpret_cast<Write>(computer.table()[8]);
    std::array<unsigned char, width * height> y_values{};
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x)
            y_values[y * width + x] = static_cast<unsigned char>(luma(x, y));
    std::array<unsigned char, width * height / 2> uv{};
    for (UINT y = 0; y < height / 2; ++y)
        for (UINT x = 0; x < width / 2; ++x) {
            uv[y * width + 2 * x + swapped] = static_cast<unsigned char>(chroma_u(x, y));
            uv[y * width + 2 * x + 1 - swapped] = static_cast<unsigned char>(chroma_v(x, y));
        }
    check(write(instance, base, 0, 0, y_values.data(), width, 0), "Copy luma");
    check(write(instance, base, 1, 0, uv.data(), width, 0), "Copy chroma");
    if (write(instance, base, 2, 0, uv.data(), width, 0) != E_INVALIDARG ||
        write(instance, base, 1, 0, uv.data(), width - 1, 0) != E_INVALIDARG)
        throw std::runtime_error("Unexpected acceptance of invalid plane or short UV row");
    for (UINT p = 0; p < 2; ++p) {
        const auto* mip = layout.data() + 0x18 + p * 0x6c8 + 0x38;
        UINT64 offset = 0;
        UINT row_pitch = 0;
        std::memcpy(&offset, mip + 8, 8);
        std::memcpy(&row_pitch, mip + 0x1c, 4);
        if (offset != plan.placed[p].Offset || row_pitch != plan.placed[p].Footprint.RowPitch)
            throw std::runtime_error("Reported plane layout differs from the desktop footprints");
    }
}

constexpr char shader[] = R"(
Texture2D<float> tex_y : register(t0);
Texture2D<float2> tex_uv : register(t1);
float4 vs(uint i : SV_VertexID) : SV_Position {
    float2 p = float2((i << 1) & 2, i & 2);
    return float4(p * float2(2,-2) + float2(-1,1), 0, 1);
}
uint4 ps(float4 pos : SV_Position) : SV_Target {
    uint2 xy = uint2(pos.xy);
    int y = int(round(255 * tex_y.Load(int3(xy, 0))));
    int2 uv = int2(round(255 * tex_uv.Load(int3(xy / 2, 0)))) - 128;
    return uint4(y + ((359 * uv.y) >> 8), y - ((88 * uv.x + 183 * uv.y) >> 8), y + ((454 * uv.x) >> 8), 255);
})";

ComPtr<ID3DBlob> compile(const char* stage, const char* profile)
{
    ComPtr<ID3DBlob> result, errors;
    HRESULT hr = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, stage, profile, 0, 0, &result, &errors);
    if (errors) std::fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr);
    check(hr, "Compile plane sample shader");
    return result;
}
void run(bool swapped)
{
    auto device = shim_create_device();
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory");
    check(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)), "Identify draw adapter");
    DXGI_ADAPTER_DESC1 adapter_desc{};
    check(adapter->GetDesc1(&adapter_desc), "Read adapter name");
    std::printf("adapter=%ls\n", adapter_desc.Description);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_NV12;
    auto array_desc = desc;
    array_desc.DepthOrArraySize = 2;
    void** rejected = nullptr;
    if (XGCreateTextureComputer(&array_desc, &rejected, 0, 0) != E_NOTIMPL || rejected)
        throw std::runtime_error("Unexpected acceptance of NV12 array");
    auto texture = resource(device.Get(), desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    x2w_startup::SliceFootprints plan(device.Get(), desc, 1);
    if (plan.size == UINT64_MAX || !plan.size || plan.count != 2) throw std::runtime_error("Invalid NV12 footprint");
    const UINT64 stride = (plan.size + 65535) & ~UINT64{65535};
    auto upload = make_buffer(device.Get(), stride, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    D3D12_RANGE none{};
    check(upload->Map(0, &none, &mapped), "Map upload");
    populate(desc, plan, static_cast<unsigned char*>(mapped), swapped);
    upload->Unmap(0, nullptr);

    ComputeCommands commands(device.Get(), nullptr, D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto* list = commands.list.Get();
    for (UINT i = 0; i < plan.count; ++i)
        plan.upload(list, texture.Get(), upload.Get(), 1, 1, 0, i, plan.placed[i].Offset);
    transition(list, texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_DESCRIPTOR_RANGE srvs{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {1, &srvs};
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC sig{1, &parameter};
    ComPtr<ID3DBlob> root_data;
    check(D3D12SerializeRootSignature(&sig, D3D_ROOT_SIGNATURE_VERSION_1, &root_data, nullptr), "Serialize root");
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, root_data->GetBufferPointer(), root_data->GetBufferSize(), IID_PPV_ARGS(&root)), "Create root");
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ComPtr<ID3D12DescriptorHeap> heap;
    check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "Create plane descriptors");
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT p = 0; p < 2; ++p) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = p == 0 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.PlaneSlice = p;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(texture.Get(), &srv, cpu);
        cpu.ptr += device->GetDescriptorHandleIncrementSize(heap_desc.Type);
    }

    desc.Width = width;
    desc.DepthOrArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    auto target = resource(device.Get(), desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    ComPtr<ID3D12DescriptorHeap> rtv;
    check(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv)), "Create RTV");
    auto handle = rtv->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, handle);
    auto vs = compile("vs", "vs_5_0"), ps = compile("ps", "ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.SampleDesc.Count = 1;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = desc.Format;
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)), "Create YUV graphics pipeline");
    D3D12_VIEWPORT viewport{0, 0, width, height, 0, 1};
    D3D12_RECT rect{0, 0, width, height};
    list->SetPipelineState(pipeline.Get());
    list->SetDescriptorHeaps(1, heap.GetAddressOf());
    list->SetGraphicsRootSignature(root.Get());
    list->SetGraphicsRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &rect);
    list->OMSetRenderTargets(1, &handle, FALSE, nullptr);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list, target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 size = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &size);
    auto readback = make_buffer(device.Get(), size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    to.PlacedFootprint = fp;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    commands.submit_and_wait(device.Get());
    check(readback->Map(0, nullptr, &mapped), "Map colour readback");
    for (UINT y = 0; y < height; ++y) {
        auto* pixels = reinterpret_cast<const UINT*>(static_cast<const char*>(mapped) + y * fp.Footprint.RowPitch);
        for (UINT x = 0; x < width; ++x) {
            const UINT want = expected(x, y, swapped);
            if (pixels[x] != want) {
                std::fprintf(stderr, "pixel (%u,%u) actual=%08x expected=%08x\n", x, y, pixels[x], want);
                throw std::runtime_error("Colour mismatch");
            }
        }
    }
    readback->Unmap(0, &none);
    std::printf("NV12 pixels=32 chroma=%s mismatches=0\n", swapped ? "VU" : "UV");
}
}  // namespace

int main()
{
    SetEnvironmentVariableW(L"X2W_D3D12_PERMISSIVE", L"1");
    try { run(false); run(true); }
    catch (...) { return 1; }
    return 0;
}

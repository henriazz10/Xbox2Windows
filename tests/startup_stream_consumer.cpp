#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cstring>
#include <wrl/client.h>

#include <array>
#include <fstream>
#include <iterator>
#include <vector>

using Microsoft::WRL::ComPtr;
extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(IUnknown*, const void*, REFIID, void**);

int check_stream_draw(IDXGIAdapter* adapter, void* guest, void* pso, const char* pixels);

struct alignas(8) Stage {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE tag;
    D3D12_SHADER_BYTECODE shader;
};
struct alignas(8) Root {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE tag;
    ID3D12RootSignature* signature;
};
struct Stream {
    Stage ps;
    Root root;
    Stage vs;
};

int main(int argc, char** argv)
{
    if (argc != 4 && argc != 6) return 2;
    std::ifstream vs_file(argv[1], std::ios::binary), ps_file(argv[2], std::ios::binary);
    std::vector<char> vs{std::istreambuf_iterator<char>(vs_file), {}};
    std::vector<char> ps{std::istreambuf_iterator<char>(ps_file), {}};
    if (vs.empty() || ps.empty()) return 3;

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 4;
    if (argc == 6 && std::strcmp(argv[4], "hardware") == 0) {
        ComPtr<IDXGIFactory6> factory6;
        if (FAILED(factory.As(&factory6))) return 77;
        for (UINT n = 0;; ++n) {
            ComPtr<IDXGIAdapter1> candidate;
            HRESULT hr = factory6->EnumAdapterByGpuPreference(n, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
            if (hr == DXGI_ERROR_NOT_FOUND) return 77;
            if (FAILED(hr)) return 4;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(candidate->GetDesc1(&desc))) return 4;
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr))) {
                adapter = candidate;
                break;
            }
        }
    } else if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) return 4;
    DXGI_ADAPTER_DESC desc{};
    LARGE_INTEGER driver{};
    if (FAILED(adapter->GetDesc(&desc))) return 4;
    adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
    std::printf("adapter=%ls driver=%u.%u.%u.%u\n", desc.Description,
        HIWORD(driver.HighPart), LOWORD(driver.HighPart), HIWORD(driver.LowPart), LOWORD(driver.LowPart));
    std::array<unsigned char, 64> parameters{};
    void* device = nullptr;
    if (FAILED(D3D12XboxCreateDevice(adapter.Get(), parameters.data(), __uuidof(ID3D12Device), &device))) return 5;

    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    ComPtr<ID3DBlob> blob;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr))) return 6;
    std::ofstream(argv[3], std::ios::binary).write(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
    using CreateRoot = HRESULT (WINAPI*)(void*, UINT, const void*, SIZE_T, REFIID, void**);
    using CreateStream = HRESULT (WINAPI*)(void*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
    auto** slots = *static_cast<void***>(device);
    ID3D12RootSignature* root = nullptr;
    if (FAILED(reinterpret_cast<CreateRoot>(slots[16])(device, 0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                        __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&root)))) return 7;

    Stream stream{{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, {ps.data(), ps.size()}},
                  {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, root},
                  {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, {vs.data(), vs.size()}}};
    void* pso = nullptr;
    auto submit = [&](SIZE_T bytes) {
        D3D12_PIPELINE_STATE_STREAM_DESC desc{bytes, &stream};
        const auto hr = reinterpret_cast<CreateStream>(slots[86])(device, &desc, __uuidof(ID3D12PipelineState), &pso);
        return SUCCEEDED(hr) && pso;
    };
    // A truncated last shader must not publish a partial request, even with readable backing memory.
    if (!submit(sizeof(stream) - 8)) return 8;
    if (GetEnvironmentVariableW(L"X2W_TEST_TRUNCATED_ONLY", nullptr, 0)) return 0;
    if (!submit(sizeof(stream))) return 9;
    return argc == 6 ? check_stream_draw(adapter.Get(), device, pso, argv[5]) : 0;
}

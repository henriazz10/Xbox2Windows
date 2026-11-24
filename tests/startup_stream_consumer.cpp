#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <fstream>
#include <iterator>
#include <vector>

using Microsoft::WRL::ComPtr;
extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(IUnknown*, const void*, REFIID, void**);

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
    if (argc != 4) return 2;
    std::ifstream vs_file(argv[1], std::ios::binary), ps_file(argv[2], std::ios::binary);
    std::vector<char> vs{std::istreambuf_iterator<char>(vs_file), {}};
    std::vector<char> ps{std::istreambuf_iterator<char>(ps_file), {}};
    if (vs.empty() || ps.empty()) return 3;

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) return 4;
    std::array<unsigned char, 64> parameters{};
    void* device = nullptr;
    if (FAILED(D3D12XboxCreateDevice(warp.Get(), parameters.data(), __uuidof(ID3D12Device), &device))) return 5;

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
    auto submit = [&](SIZE_T bytes) {
        void* pso = nullptr;
        D3D12_PIPELINE_STATE_STREAM_DESC desc{bytes, &stream};
        const auto hr = reinterpret_cast<CreateStream>(slots[86])(device, &desc, __uuidof(ID3D12PipelineState), &pso);
        return SUCCEEDED(hr) && pso;
    };
    // A truncated last shader must not publish a partial request, even with readable backing memory.
    if (!submit(sizeof(stream) - 8)) return 8;
    if (GetEnvironmentVariableW(L"X2W_TEST_TRUNCATED_ONLY", nullptr, 0)) return 0;
    return submit(sizeof(stream)) ? 0 : 9;
}

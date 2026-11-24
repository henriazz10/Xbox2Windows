#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <fstream>

using Microsoft::WRL::ComPtr;

// The components under test receive the guest ABI's vtable slots. The observation
// queue and its readback buffer are desktop objects owned only by the fixture.
using Word = std::uint64_t;
template<class T> Word arg(T* value) { return reinterpret_cast<Word>(value); }
Word invoke(void* object, unsigned index, Word a = 0, Word b = 0, Word c = 0, Word d = 0,
            Word e = 0, Word f = 0, Word g = 0, Word h = 0)
{
    using Slot = Word (WINAPI*)(void*, Word, Word, Word, Word, Word, Word, Word, Word);
    return reinterpret_cast<Slot>((*static_cast<void***>(object))[index])(object, a, b, c, d, e, f, g, h);
}

int check_stream_draw(IDXGIAdapter* adapter, void* guest, void* pso, const char* pixels)
{
    constexpr UINT width = 8, height = 4;
    ComPtr<ID3D12Device> desktop;
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&desktop)))) return 20;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1,
        DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ComPtr<ID3D12Resource> target;
    if (FAILED(desktop->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&target)))) return 21;
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    if (FAILED(desktop->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)))) return 22;
    const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    invoke(guest, 20, arg(target.Get()), 0, rtv.ptr);

    void* list = nullptr;
    void* queue = nullptr;
    void* allocator = nullptr;
    void* fence = nullptr;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    const unsigned list_desc[] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 2, 1, 0};
    if (FAILED(static_cast<HRESULT>(invoke(guest, 8, arg(&queue_desc), arg(&__uuidof(ID3D12CommandQueue)), arg(&queue)))) ||
        FAILED(static_cast<HRESULT>(invoke(guest, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, arg(&__uuidof(ID3D12CommandAllocator)), arg(&allocator)))) ||
        FAILED(static_cast<HRESULT>(invoke(guest, 73, arg(list_desc), arg(allocator), 0, arg(&__uuidof(ID3D12GraphicsCommandList)), arg(&list)))) ||
        FAILED(static_cast<HRESULT>(invoke(guest, 36, 0, 0, arg(&__uuidof(ID3D12Fence)), arg(&fence))))) return 23;

    const float clear[] = {0, 0, 1, 1};
    const D3D12_VIEWPORT viewport{0, 0, width, height, 0, 1};
    const D3D12_RECT left{0, 0, width / 2, height};
    invoke(list, 139, arg(allocator));
    invoke(list, 52, rtv.ptr, arg(clear));
    invoke(list, 50, 1, arg(&rtv));
    invoke(list, 21, 1, arg(&viewport));
    invoke(list, 22, 1, arg(&left));
    invoke(list, 20, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    invoke(list, 25, arg(pso));
    invoke(list, 12, 3, 1);
    invoke(list, 9);
    invoke(queue, 10, 1, arg(&list));
    if (FAILED(static_cast<HRESULT>(invoke(queue, 14, arg(fence), 1)))) return 24;
    const auto deadline = GetTickCount64() + 10000;
    while (invoke(fence, 8) == 0 && GetTickCount64() < deadline) Sleep(1);
    if (invoke(fence, 8) != 1) return 25;

    ComPtr<ID3D12CommandQueue> observer;
    ComPtr<ID3D12CommandAllocator> copy_allocator;
    ComPtr<ID3D12GraphicsCommandList> copy;
    ComPtr<ID3D12Fence> complete;
    if (FAILED(desktop->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&observer))) ||
        FAILED(desktop->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copy_allocator))) ||
        FAILED(desktop->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copy_allocator.Get(), nullptr, IID_PPV_ARGS(&copy))) ||
        FAILED(desktop->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&complete)))) return 26;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    desktop->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1,
                               DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(desktop->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return 27;
    D3D12_TEXTURE_COPY_LOCATION destination{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    copy->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    if (FAILED(copy->Close())) return 28;
    ID3D12CommandList* submit[] = {copy.Get()};
    observer->ExecuteCommandLists(1, submit);
    if (FAILED(observer->Signal(complete.Get(), 1))) return 29;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return 30;
    const bool completed = SUCCEEDED(complete->SetEventOnCompletion(1, event)) &&
                           WaitForSingleObject(event, 10000) == WAIT_OBJECT_0 && complete->GetCompletedValue() == 1;
    CloseHandle(event);
    if (!completed) return 31;
    void* memory = nullptr;
    if (FAILED(readback->Map(0, nullptr, &memory))) return 32;
    std::ofstream out(pixels, std::ios::binary);
    for (UINT y = 0; y < height; ++y)
        out.write(static_cast<const char*>(memory) + y * footprint.Footprint.RowPitch, width * 4);
    readback->Unmap(0, nullptr);
    return out ? 0 : 33;
}

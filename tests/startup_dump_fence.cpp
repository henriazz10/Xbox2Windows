#include "cpu/shim_gpu_device.h"
#include "desktop_gpu.h"
#include "startup_stubs/draw_dump_tracking.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
using x2w_startup::DrawDump;
using x2w_startup::DrawReadbacks;

namespace {
void require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

void record_copy(DrawReadbacks& tracker, ID3D12Device* device, ID3D12GraphicsCommandList* list,
                 ID3D12Resource* upload, UINT word, UINT cookie)
{
    DrawDump dump{};
    dump.buffer = make_buffer(device, sizeof(UINT), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    dump.key = cookie;
    auto* destination = dump.buffer.Get();
    require(tracker.record(list, std::move(dump)), "Could not record capture");
    list->CopyBufferRegion(destination, 0, upload, sizeof(UINT) * word, sizeof(UINT));
}

void run()
{
    auto device = shim_create_device();
    auto upload = make_buffer(device.Get(), 8, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    constexpr UINT words[] = {0x1c205ea7, 0x9df81342};
    D3D12_RANGE none{};
    void* mapped = nullptr;
    check(upload->Map(0, &none, &mapped), "Map input");
    std::memcpy(mapped, words, sizeof(words));
    upload->Unmap(0, nullptr);

    DrawReadbacks tracker;
    ComputeCommands work(device.Get(), nullptr, D3D12_COMMAND_LIST_TYPE_DIRECT);
    ComputeCommands other(device.Get(), nullptr, D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto* list = work.list.Get();
    auto* queue = work.queue.Get();
    record_copy(tracker, device.Get(), list, upload.Get(), 0, 10);
    check(list->Close(), "Close first recording");
    require(tracker.take_completed().empty(), "Unsubmitted recording published a capture");

    // This reset replaces a recording that was closed but never submitted.
    check(list->Reset(work.allocator.Get(), nullptr), "Reset unsubmitted list");
    tracker.discard(list);
    record_copy(tracker, device.Get(), list, upload.Get(), 1, 20);
    check(list->Close(), "Close replacement recording");
    check(other.list->Close(), "Close unrelated list");
    ID3D12CommandList* batch[] = {other.list.Get()};
    check(tracker.execute(other.queue.Get(), 1, batch), "Execute unrelated list");
    require(tracker.take_completed().empty(), "Unrelated queue published the replacement recording");

    ComPtr<ID3D12Fence> gate, done;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "Create CPU gate");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "Create completion fence");
    check(queue->Wait(gate.Get(), 1), "Hold execution behind gate");
    batch[0] = list;
    check(tracker.execute(queue, 1, batch), "Submit capture list");
    const bool empty_before_signal = tracker.take_completed().empty();

    // Reset the command-list object with a fresh allocator while its previous
    // recording is in flight. The readback must outlive this recording reset.
    ComPtr<ID3D12CommandAllocator> next;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&next)), "Create different allocator");
    check(list->Reset(next.Get(), nullptr), "Reset submitted list");
    tracker.discard(list);
    record_copy(tracker, device.Get(), list, upload.Get(), 0, 30);
    check(list->Close(), "Close yet-unsubmitted list");
    check(gate->Signal(1), "Open gate");
    check(queue->Signal(done.Get(), 1), "Signal completion");
    check(done->SetEventOnCompletion(1, nullptr), "Wait for execution");
    check(device->GetDeviceRemovedReason(), "Device after submission");
    require(empty_before_signal, "Capture published before queue could execute");
    auto completed = tracker.take_completed();
    require(completed.size() == 1 && completed.front().key == 20, "Expected only the submitted recording");
    auto* readback = completed.front().buffer.Get();
    D3D12_RANGE range{0, sizeof(UINT)};
    check(readback->Map(0, &range, &mapped), "Map completed copy");
    UINT actual = 0;
    std::memcpy(&actual, mapped, sizeof(actual));
    readback->Unmap(0, &none);
    require(actual == words[1], "Mapped value differs from the executed copy");
    require(tracker.take_completed().empty(), "Capture published more than once");
    check(list->Reset(next.Get(), nullptr), "Discard final unsubmitted list");
    tracker.discard(list);
    check(list->Close(), "Close empty list");
    std::printf("submitted=1 cancelled=2 published=1 readback=%08x\n", actual);
}
}  // namespace

int main()
{
    try { run(); }
    catch (const std::exception& err) { std::fprintf(stderr, "%s\n", err.what()); return 1; }
    catch (...) { return 1; }
    return 0;
}

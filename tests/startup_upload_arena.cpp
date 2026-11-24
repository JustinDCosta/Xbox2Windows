#include "startup_stubs/list_upload_pages.h"

#include <d3d12sdklayers.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
int select_adapter(bool hardware, IDXGIFactory6* factory, ComPtr<IDXGIAdapter>& adapter)
{
    if (!hardware) return SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) ? 0 : 3;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        const HRESULT hr = factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                IID_PPV_ARGS(&candidate));
        if (hr == DXGI_ERROR_NOT_FOUND) return 77;
        if (FAILED(hr)) return 3;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(candidate->GetDesc1(&desc))) return 3;
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr))) {
            adapter = candidate;
            return 0;
        }
    }
}

const char* check_arena(ID3D12Device* device)
{
    x2w_upload::Arena arena;
    const auto first = arena.allocate(device, 640, 16);
    if (!first.cpu || !first.gpu || !first.resource || first.cpu % 16 || first.gpu % 16 || first.bytes != 640 ||
        first.gpu != first.resource->GetGPUVirtualAddress() + first.offset) return "640/16 allocation";
    const auto second = arena.allocate(device, 16, 16);
    if (!second.cpu || second.resource != first.resource || second.cpu < first.cpu + first.bytes) return "same-page allocation";
    const auto large = arena.allocate(device, 65536, 16);
    if (!large.cpu || large.resource == first.resource || large.bytes != 65536) return "page rollover";

    auto* payload = reinterpret_cast<unsigned char*>(first.cpu);
    std::memset(payload, 0x17, 640);
    std::memset(reinterpret_cast<void*>(second.cpu), 0x34, 16);
    std::memset(reinterpret_cast<void*>(large.cpu), 0x56, 65536);
    if (!std::all_of(payload, payload + 640, [](unsigned char value) { return value == 0x17; })) return "retained CPU payload";
    if (arena.translate(first.cpu, 640) != first.gpu || arena.translate(first.cpu + 16, 624) != first.gpu + 16 ||
        arena.translate(first.cpu + 639, 1) != first.gpu + 639 || arena.translate(second.cpu, 16) != second.gpu ||
        arena.translate(large.cpu, 65536) != large.gpu) return "live-range translation";
    if (arena.translate(first.cpu, 641) || arena.translate(first.cpu + 639, 2) || arena.translate(first.cpu, 0) ||
        arena.allocate(nullptr, 640, 16).cpu || arena.allocate(device, 0, 16).cpu ||
        arena.allocate(device, 640, 0).cpu || arena.allocate(device, 640, 3).cpu ||
        arena.allocate(device, UINT64_MAX, 16).cpu) return "invalid allocation or range accepted";

    // The fixture made no GPU submissions. Both pages can be retired now.
    arena.reset();
    if (arena.translate(first.cpu, 1) || arena.translate(second.cpu, 1) || arena.translate(large.cpu, 1)) return "retired range translated";
    const auto next = arena.allocate(device, 640, 16);
    if (!next.cpu || !next.gpu || arena.translate(next.cpu, 640) != next.gpu) return "allocation after idle reset";
    return nullptr;
}

const char* check_fenced_lifetime(ID3D12Device* device)
{
    constexpr UINT64 payload_bytes = 4096;
    constexpr unsigned char pattern_a = 0xA7;
    constexpr unsigned char pattern_b = 0x5C;

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) return "copy queue creation";

    ComPtr<ID3D12Fence> gate, complete;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&complete)))) return "fence creation";

    auto make_readback = [&](ComPtr<ID3D12Resource>& resource) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = payload_bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource));
    };

    ComPtr<ID3D12Resource> readback_a, readback_b;
    if (FAILED(make_readback(readback_a)) || FAILED(make_readback(readback_b))) return "readback creation";

    ComPtr<ID3D12CommandAllocator> allocator_a, allocator_b;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocator_a))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocator_b)))) return "allocator creation";
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator_a.Get(), nullptr,
                                         IID_PPV_ARGS(&list)))) return "command list creation";

    x2w_upload::Arena arena_a, arena_b;
    const auto a = arena_a.allocate(device, payload_bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    const auto b = arena_b.allocate(device, payload_bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (!a.cpu || !a.gpu || !b.cpu || !b.gpu || a.resource == b.resource ||
        a.gpu != a.resource->GetGPUVirtualAddress() + a.offset ||
        b.gpu != b.resource->GetGPUVirtualAddress() + b.offset) return "generation allocations";
    std::memset(reinterpret_cast<void*>(a.cpu), pattern_a, payload_bytes);
    std::memset(reinterpret_cast<void*>(b.cpu), pattern_b, payload_bytes);

    list->CopyBufferRegion(readback_a.Get(), 0, a.resource, a.offset, payload_bytes);
    if (FAILED(list->Close())) return "first command-list close";

    // Queue wait holds both copies until both arena generations and the reset are ready.
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return "completion event creation";
    if (FAILED(queue->Wait(gate.Get(), 1))) {
        CloseHandle(event);
        return "queue fence wait";
    }
    ID3D12CommandList* first_lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, first_lists);

    HRESULT reset_hr = list->Reset(allocator_b.Get(), nullptr);
    if (SUCCEEDED(reset_hr)) {
        list->CopyBufferRegion(readback_b.Get(), 0, b.resource, b.offset, payload_bytes);
        reset_hr = list->Close();
    }
    if (SUCCEEDED(reset_hr)) {
        ID3D12CommandList* second_lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, second_lists);
    }
    const HRESULT signal_hr = queue->Signal(complete.Get(), 1);
    const bool queue_held = complete->GetCompletedValue() == 0;
    const auto cpu_pattern_matches = [&](uintptr_t address, unsigned char expected) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(address);
        return std::all_of(bytes, bytes + payload_bytes,
                           [expected](unsigned char value) { return value == expected; });
    };
    const bool both_generations_retained = cpu_pattern_matches(a.cpu, pattern_a) &&
                                           cpu_pattern_matches(b.cpu, pattern_b);
    // Always unblock the queue before returning, including on reset or signal errors.
    const HRESULT release_hr = gate->Signal(1);
    const HRESULT event_hr = complete->SetEventOnCompletion(1, event);
    const DWORD wait_result = SUCCEEDED(event_hr) ? WaitForSingleObject(event, 30000) : WAIT_FAILED;
    CloseHandle(event);
    if (FAILED(event_hr) || wait_result != WAIT_OBJECT_0) return "GPU completion wait";
    if (!queue_held) return "queue fence failed to hold submissions";
    if (!both_generations_retained) return "upload payload changed before fence completion";
    if (FAILED(reset_hr)) return "command-list reset or second close";
    if (FAILED(signal_hr) || FAILED(release_hr)) return "fence signal";

    auto matches = [&](ID3D12Resource* resource, unsigned char expected) {
        D3D12_RANGE read_range{0, static_cast<SIZE_T>(payload_bytes)};
        void* mapped = nullptr;
        if (FAILED(resource->Map(0, &read_range, &mapped))) return false;
        const auto* bytes = static_cast<const unsigned char*>(mapped);
        const bool equal = std::all_of(bytes, bytes + payload_bytes,
                                       [expected](unsigned char value) { return value == expected; });
        D3D12_RANGE no_writes{};
        resource->Unmap(0, &no_writes);
        return equal;
    };
    if (!matches(readback_a.Get(), pattern_a) || !matches(readback_b.Get(), pattern_b)) return "fenced payload readback";
    if (arena_a.translate(a.cpu, payload_bytes) != a.gpu || arena_b.translate(b.cpu, payload_bytes) != b.gpu)
        return "generation translation before retirement";

    // Both submissions have completed, so these pages may now be retired.
    arena_a.reset();
    arena_b.reset();
    if (arena_a.translate(a.cpu, 1) || arena_b.translate(b.cpu, 1)) return "retired generation translated";
    return nullptr;
}
}

int main(int argc, char** argv)
{
    if (argc != 2 || (std::strcmp(argv[1], "warp") && std::strcmp(argv[1], "hardware"))) return 2;
    ComPtr<ID3D12Debug> debug;
    const bool debugging = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if (debugging) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 3;
    ComPtr<IDXGIAdapter> adapter;
    if (const int result = select_adapter(!std::strcmp(argv[1], "hardware"), factory.Get(), adapter)) return result;
    DXGI_ADAPTER_DESC adapter_desc{};
    LARGE_INTEGER driver{};
    if (FAILED(adapter->GetDesc(&adapter_desc))) return 3;
    adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
    std::printf("adapter=%ls driver=%u.%u.%u.%u debug_layer=%s\n", adapter_desc.Description,
                HIWORD(driver.HighPart), LOWORD(driver.HighPart), HIWORD(driver.LowPart), LOWORD(driver.LowPart),
                debugging ? "enabled" : "unavailable");
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) return 4;
    ComPtr<ID3D12InfoQueue> info;
    if (debugging && FAILED(device.As(&info))) return 8;
    const UINT64 first_debug_message = info ? info->GetNumStoredMessages() : 0;
    if (const char* mismatch = check_arena(device.Get())) {
        std::fprintf(stderr, "Upload arena contract failed: %s\n", mismatch);
        return 5;
    }
    if (const char* mismatch = check_fenced_lifetime(device.Get())) {
        std::fprintf(stderr, "Upload arena fence lifetime failed: %s\n", mismatch);
        return 7;
    }
    if (const HRESULT hr = device->GetDeviceRemovedReason(); FAILED(hr)) {
        std::fprintf(stderr, "Device removed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return 6;
    }
    if (info) {
        for (UINT64 i = first_debug_message, count = info->GetNumStoredMessages(); i < count; ++i) {
            SIZE_T bytes = 0;
            if (FAILED(info->GetMessage(i, nullptr, &bytes))) return 8;
            std::vector<char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(info->GetMessage(i, message, &bytes))) return 8;
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::fprintf(stderr, "D3D12 debug error %u: %s\n", static_cast<unsigned>(message->ID),
                             message->pDescription);
                return 9;
            }
        }
    }
    std::puts("allocation, page rollover, bounded translation, idle reset and fenced generation lifetime passed");
    return 0;
}

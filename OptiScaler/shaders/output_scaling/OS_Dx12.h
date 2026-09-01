#pragma once
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

#include <cstdint>
#include <d3d12.h>
#include <d3dx/d3dx12.h>

enum class Scaler : uint32_t;

#define OS_NUM_OF_HEAPS 2

class OS_Dx12 : public Shader_Dx12
{
  private:
    bool _upsample = false;
    Scaler _scaler;

    FrameDescriptorHeap _frameHeaps[OS_NUM_OF_HEAPS];

    ID3D12Resource* _buffer = nullptr;
    D3D12_RESOURCE_STATES _bufferState = D3D12_RESOURCE_STATE_COMMON;

    uint32_t InNumThreadsX = 16;
    uint32_t InNumThreadsY = 16;

  public:
    bool CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InSource, uint32_t InWidth, uint32_t InHeight,
                              D3D12_RESOURCE_STATES InState);
    void SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState);
    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, ID3D12Resource* OutResource);

    ID3D12Resource* Buffer() { return _buffer; }
    bool IsUpsampling() const { return _upsample; }
    bool CanRender() const { return _init && _buffer != nullptr; }

    OS_Dx12(std::string InName, ID3D12Device* InDevice, bool InUpsample);
    OS_Dx12(std::string InName, ID3D12Device* InDevice, bool InUpsample, Scaler kernel);

    ~OS_Dx12();
};

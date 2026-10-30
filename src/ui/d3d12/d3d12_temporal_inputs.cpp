/**
 * @file        src/ui/d3d12/d3d12_temporal_inputs.cpp
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/d3d12/d3d12_temporal_inputs.h>

#include <algorithm>
#include <utility>

#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>

namespace rex::ui::d3d12 {

namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/temporal_motion_cs.h"
}  // namespace shaders

namespace {

// Registers of shaders/temporal_motion_cs.hlsl, in descriptor table order.
enum Descriptor : uint32_t {
  kDescriptorCurrentColor,         // t0
  kDescriptorPreviousColor,        // t1
  kDescriptorCurrentLumaQuarter,   // u0
  kDescriptorPreviousLumaQuarter,  // u1
  kDescriptorCoarseMotion,         // u2
  kDescriptorMotion,               // u3
  kDescriptorDepth,                // u4
  kDescriptorCount,
};
constexpr uint32_t kSRVCount = 2;
constexpr uint32_t kUAVCount = kDescriptorCount - kSRVCount;

struct RootConstants {
  uint32_t width;
  uint32_t height;
  uint32_t pass_index;
  uint32_t has_history;
};

// Group size of the shader.
constexpr uint32_t kGroupSize = 8;
// Quarter-resolution pixels per coarse block, and full-resolution pixels per fine block.
constexpr uint32_t kCoarseBlock = 4;
constexpr uint32_t kFineBlock = 8;

uint32_t DivideRoundUp(uint32_t a, uint32_t b) {
  return (a + b - 1) / b;
}

D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  return barrier;
}

}  // namespace

std::unique_ptr<D3D12TemporalInputs> D3D12TemporalInputs::Create(const D3D12Provider& provider) {
  auto inputs = std::unique_ptr<D3D12TemporalInputs>(new D3D12TemporalInputs(provider));
  if (!inputs->Initialize()) {
    return nullptr;
  }
  return inputs;
}

D3D12TemporalInputs::~D3D12TemporalInputs() = default;

bool D3D12TemporalInputs::Initialize() {
  ID3D12Device* device = provider_.GetDevice();

  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = kSRVCount;
  ranges[0].BaseShaderRegister = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = kUAVCount;
  ranges[1].BaseShaderRegister = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = kSRVCount;
  D3D12_ROOT_PARAMETER parameters[2] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.ShaderRegister = 0;
  parameters[0].Constants.Num32BitValues = sizeof(RootConstants) / sizeof(uint32_t);
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable.NumDescriptorRanges = 2;
  parameters[1].DescriptorTable.pDescriptorRanges = ranges;
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC root_signature_desc = {};
  root_signature_desc.NumParameters = 2;
  root_signature_desc.pParameters = parameters;
  root_signature_.Attach(util::CreateRootSignature(provider_, root_signature_desc));
  if (!root_signature_) {
    REXLOG_ERROR("D3D12TemporalInputs: Failed to create the motion root signature");
    return false;
  }
  pipeline_.Attach(util::CreateComputePipeline(device, shaders::temporal_motion_cs,
                                               sizeof(shaders::temporal_motion_cs),
                                               root_signature_.Get()));
  if (!pipeline_) {
    REXLOG_ERROR("D3D12TemporalInputs: Failed to create the motion pipeline");
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = kDescriptorCount * 2;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&descriptor_heap_)))) {
    REXLOG_ERROR("D3D12TemporalInputs: Failed to create the descriptor heap");
    return false;
  }
  return true;
}

void D3D12TemporalInputs::DestroySizedResources() {
  for (auto& color : colors_) {
    color.Reset();
  }
  for (auto& luma : lumas_quarter_) {
    luma.Reset();
  }
  coarse_motion_.Reset();
  motion_.Reset();
  depth_.Reset();
  width_ = 0;
  height_ = 0;
}

bool D3D12TemporalInputs::CreateSizedResources(uint32_t width, uint32_t height) {
  DestroySizedResources();
  ID3D12Device* device = provider_.GetDevice();

  auto create = [&](Microsoft::WRL::ComPtr<ID3D12Resource>& resource, DXGI_FORMAT format,
                    uint32_t resource_width, uint32_t resource_height, D3D12_RESOURCE_FLAGS flags,
                    D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = resource_width;
    desc.Height = resource_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return SUCCEEDED(device->CreateCommittedResource(&util::kHeapPropertiesDefault,
                                                     provider_.GetHeapFlagCreateNotZeroed(), &desc,
                                                     state, nullptr, IID_PPV_ARGS(&resource)));
  };
  const uint32_t quarter_width = std::max(width / 4, 1u);
  const uint32_t quarter_height = std::max(height / 4, 1u);
  const uint32_t coarse_width = DivideRoundUp(quarter_width, kCoarseBlock);
  const uint32_t coarse_height = DivideRoundUp(quarter_height, kCoarseBlock);
  bool ok = true;
  for (auto& color : colors_) {
    ok = ok && create(color, D3D12Presenter::kGuestOutputFormat, width, height,
                      D3D12_RESOURCE_FLAG_NONE, kReadState);
  }
  for (auto& luma : lumas_quarter_) {
    ok = ok &&
         create(luma, DXGI_FORMAT_R32_FLOAT, quarter_width, quarter_height,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  ok = ok &&
       create(coarse_motion_, DXGI_FORMAT_R32_UINT, coarse_width, coarse_height,
              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ok = ok && create(motion_, DXGI_FORMAT_R16G16_FLOAT, width, height,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kReadState);
  ok = ok && create(depth_, DXGI_FORMAT_R32_FLOAT, width, height,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kReadState);
  if (!ok) {
    REXLOG_ERROR("D3D12TemporalInputs: Failed to create {}x{} resources", width, height);
    DestroySizedResources();
    return false;
  }

  // Table i reads colors_[i] / lumas_quarter_[i] as current and the other pair as previous.
  const D3D12_CPU_DESCRIPTOR_HANDLE heap_start =
      descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  for (uint32_t i = 0; i < 2; i++) {
    auto handle = [&](uint32_t descriptor) {
      return provider_.OffsetViewDescriptor(heap_start, i * kDescriptorCount + descriptor);
    };
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = D3D12Presenter::kGuestOutputFormat;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(colors_[i].Get(), &srv_desc, handle(kDescriptorCurrentColor));
    device->CreateShaderResourceView(colors_[i ^ 1].Get(), &srv_desc,
                                     handle(kDescriptorPreviousColor));
    auto uav = [&](ID3D12Resource* resource, DXGI_FORMAT format, uint32_t descriptor) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
      uav_desc.Format = format;
      uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      device->CreateUnorderedAccessView(resource, nullptr, &uav_desc, handle(descriptor));
    };
    uav(lumas_quarter_[i].Get(), DXGI_FORMAT_R32_FLOAT, kDescriptorCurrentLumaQuarter);
    uav(lumas_quarter_[i ^ 1].Get(), DXGI_FORMAT_R32_FLOAT, kDescriptorPreviousLumaQuarter);
    uav(coarse_motion_.Get(), DXGI_FORMAT_R32_UINT, kDescriptorCoarseMotion);
    uav(motion_.Get(), DXGI_FORMAT_R16G16_FLOAT, kDescriptorMotion);
    uav(depth_.Get(), DXGI_FORMAT_R32_FLOAT, kDescriptorDepth);
  }

  width_ = width;
  height_ = height;
  parity_ = 0;
  frames_recorded_ = 0;
  return true;
}

bool D3D12TemporalInputs::Record(ID3D12GraphicsCommandList* command_list, ID3D12Resource* color,
                                 D3D12_RESOURCE_STATES color_state, uint32_t width, uint32_t height,
                                 bool& has_history_out) {
  has_history_out = false;
  if (width != width_ || height != height_) {
    if (!CreateSizedResources(width, height)) {
      return false;
    }
  }
  ID3D12Resource* current = colors_[parity_].Get();

  // Copy the guest output into this frame's color, which the next frame reads as the previous.
  {
    D3D12_RESOURCE_BARRIER barriers[2] = {
        TransitionBarrier(color, color_state, D3D12_RESOURCE_STATE_COPY_SOURCE),
        TransitionBarrier(current, kReadState, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    command_list->ResourceBarrier(2, barriers);
    D3D12_TEXTURE_COPY_LOCATION dest = {};
    dest.pResource = current;
    dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = color;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = {0, 0, 0, width, height, 1};
    command_list->CopyTextureRegion(&dest, 0, 0, 0, &source, &box);
    D3D12_RESOURCE_BARRIER barriers_back[4] = {
        TransitionBarrier(color, D3D12_RESOURCE_STATE_COPY_SOURCE, color_state),
        TransitionBarrier(current, D3D12_RESOURCE_STATE_COPY_DEST, kReadState),
        TransitionBarrier(motion_.Get(), kReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        TransitionBarrier(depth_.Get(), kReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    command_list->ResourceBarrier(4, barriers_back);
  }

  ID3D12DescriptorHeap* heap = descriptor_heap_.Get();
  command_list->SetDescriptorHeaps(1, &heap);
  command_list->SetComputeRootSignature(root_signature_.Get());
  command_list->SetPipelineState(pipeline_.Get());
  command_list->SetComputeRootDescriptorTable(
      1, provider_.OffsetViewDescriptor(heap->GetGPUDescriptorHandleForHeapStart(),
                                        parity_ * kDescriptorCount));

  const uint32_t quarter_width = std::max(width / 4, 1u);
  const uint32_t quarter_height = std::max(height / 4, 1u);
  const std::array<std::pair<uint32_t, uint32_t>, 3> pass_sizes = {{
      {quarter_width, quarter_height},
      {DivideRoundUp(quarter_width, kCoarseBlock), DivideRoundUp(quarter_height, kCoarseBlock)},
      {DivideRoundUp(width, kFineBlock), DivideRoundUp(height, kFineBlock)},
  }};
  // Without history, only the luma pass runs, and it writes zero motion.
  const uint32_t pass_count = frames_recorded_ ? 3 : 1;
  for (uint32_t pass = 0; pass < pass_count; pass++) {
    if (pass) {
      D3D12_RESOURCE_BARRIER uav_barrier = {};
      uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      uav_barrier.UAV.pResource = nullptr;
      command_list->ResourceBarrier(1, &uav_barrier);
    }
    RootConstants constants = {width, height, pass, frames_recorded_ ? 1u : 0u};
    command_list->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(uint32_t), &constants,
                                               0);
    command_list->Dispatch(DivideRoundUp(pass_sizes[pass].first, kGroupSize),
                           DivideRoundUp(pass_sizes[pass].second, kGroupSize), 1);
  }

  D3D12_RESOURCE_BARRIER barriers_read[2] = {
      TransitionBarrier(motion_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadState),
      TransitionBarrier(depth_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadState),
  };
  command_list->ResourceBarrier(2, barriers_read);

  has_history_out = frames_recorded_ != 0;
  if (frames_recorded_ < UINT32_MAX) {
    frames_recorded_++;
  }
  parity_ ^= 1;
  return true;
}

}  // namespace rex::ui::d3d12

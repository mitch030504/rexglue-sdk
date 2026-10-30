#pragma once
/**
 * @file        rex/ui/d3d12/d3d12_temporal_inputs.h
 * @brief       Color history, motion vectors and depth for the D3D12 presenter's temporal upscaler.
 *
 * The D3D12 counterpart of rex/ui/vulkan/temporal_inputs.h: the guest gives the presenter only its
 * final image, so motion is estimated from consecutive images (shaders/temporal_motion_cs.hlsl)
 * and depth is a constant.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <cstdint>
#include <memory>

#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::ui::d3d12 {

class D3D12Provider;

class D3D12TemporalInputs {
 public:
  static std::unique_ptr<D3D12TemporalInputs> Create(const D3D12Provider& provider);
  ~D3D12TemporalInputs();

  D3D12TemporalInputs(const D3D12TemporalInputs&) = delete;
  D3D12TemporalInputs& operator=(const D3D12TemporalInputs&) = delete;

  // Records the inputs for a new frame. `color` must be a D3D12Presenter::kGuestOutputFormat
  // resource in `color_state`, and is left in it. Afterwards color(), motion() and depth() are in
  // kReadState. has_history_out is false when there is no previous frame to match against (first
  // frame, or the size changed), and the upscaler must reset. The caller must have awaited the GPU
  // before a size change, as the resources are recreated. Binds this object's descriptor heap, so
  // the caller must rebind its own afterwards.
  bool Record(ID3D12GraphicsCommandList* command_list, ID3D12Resource* color,
              D3D12_RESOURCE_STATES color_state, uint32_t width, uint32_t height,
              bool& has_history_out);

  // The state the upscaler reads color(), motion() and depth() in.
  static constexpr D3D12_RESOURCE_STATES kReadState =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  ID3D12Resource* color() const { return colors_[parity_ ^ 1].Get(); }  // the frame just recorded
  ID3D12Resource* motion() const { return motion_.Get(); }
  ID3D12Resource* depth() const { return depth_.Get(); }

 private:
  explicit D3D12TemporalInputs(const D3D12Provider& provider) : provider_(provider) {}
  bool Initialize();
  bool CreateSizedResources(uint32_t width, uint32_t height);
  void DestroySizedResources();

  const D3D12Provider& provider_;

  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
  // Two descriptor tables, one per parity (see descriptor_tables_).
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap_;

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  // Index of the color/luma resources the next frame is written to; the other pair is the previous.
  uint32_t parity_ = 0;
  uint32_t frames_recorded_ = 0;
  std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> colors_;
  std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> lumas_quarter_;
  Microsoft::WRL::ComPtr<ID3D12Resource> coarse_motion_;
  Microsoft::WRL::ComPtr<ID3D12Resource> motion_;
  Microsoft::WRL::ComPtr<ID3D12Resource> depth_;
};

}  // namespace rex::ui::d3d12

// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/metal/surface_mtl.h"

#include "flutter/fml/trace_event.h"
#include "flutter/impeller/renderer/command_buffer.h"
#include "impeller/base/validation.h"
#include "impeller/core/texture_descriptor.h"
#include "impeller/renderer/backend/metal/context_mtl.h"
#include "impeller/renderer/backend/metal/formats_mtl.h"
#include "impeller/renderer/backend/metal/texture_mtl.h"
#include "impeller/renderer/render_target.h"

namespace impeller {

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunguarded-availability-new"

id<CAMetalDrawable> SurfaceMTL::GetMetalDrawableAndValidate(
    const std::shared_ptr<Context>& context,
    CAMetalLayer* layer) {
  TRACE_EVENT0("impeller", "SurfaceMTL::WrapCurrentMetalLayerDrawable");

  if (context == nullptr || !context->IsValid() || layer == nil) {
    return nullptr;
  }

  id<CAMetalDrawable> current_drawable = nil;
  {
    TRACE_EVENT0("impeller", "WaitForNextDrawable");
    current_drawable = [layer nextDrawable];
  }

  if (!current_drawable) {
    VALIDATION_LOG << "Could not acquire current drawable.";
    return nullptr;
  }
  return current_drawable;
}

static std::optional<RenderTarget> WrapTextureWithRenderTarget(
    Allocator& allocator,
    id<MTLTexture> texture,
    bool requires_blit,
    std::optional<IRect> clip_rect) {
  TextureDescriptor resolve_tex_desc;
  resolve_tex_desc.format = FromMTLPixelFormat(texture.pixelFormat);
  resolve_tex_desc.size = {static_cast<ISize::Type>(texture.width),
                           static_cast<ISize::Type>(texture.height)};
  resolve_tex_desc.usage = static_cast<uint64_t>(TextureUsage::kRenderTarget) |
                           static_cast<uint64_t>(TextureUsage::kShaderRead);
  resolve_tex_desc.sample_count = SampleCount::kCount1;
  resolve_tex_desc.storage_mode = StorageMode::kDevicePrivate;

  if (resolve_tex_desc.format == PixelFormat::kUnknown) {
    VALIDATION_LOG << "Unknown drawable color format.";
    return std::nullopt;
  }

  // Create color resolve texture.
  std::shared_ptr<Texture> resolve_tex;
  if (requires_blit) {
    resolve_tex_desc.compression_type = CompressionType::kLossy;
    resolve_tex = allocator.CreateTexture(resolve_tex_desc);
  } else {
    resolve_tex = std::make_shared<TextureMTL>(resolve_tex_desc, texture);
  }

  if (!resolve_tex) {
    VALIDATION_LOG << "Could not wrap resolve texture.";
    return std::nullopt;
  }
  resolve_tex->SetLabel("ImpellerOnscreenResolve");

  TextureDescriptor msaa_tex_desc;
  msaa_tex_desc.storage_mode = StorageMode::kDeviceTransient;
  msaa_tex_desc.type = TextureType::kTexture2DMultisample;
  msaa_tex_desc.sample_count = SampleCount::kCount4;
  msaa_tex_desc.format = resolve_tex->GetTextureDescriptor().format;
  msaa_tex_desc.size = resolve_tex->GetSize();
  msaa_tex_desc.usage = static_cast<uint64_t>(TextureUsage::kRenderTarget);

  auto msaa_tex = allocator.CreateTexture(msaa_tex_desc);
  if (!msaa_tex) {
    VALIDATION_LOG << "Could not allocate MSAA color texture.";
    return std::nullopt;
  }
  msaa_tex->SetLabel("ImpellerOnscreenColorMSAA");

  ColorAttachment color0;
  color0.texture = msaa_tex;
  color0.clear_color = Color::DarkSlateGray();
  color0.load_action = LoadAction::kClear;
  color0.store_action = StoreAction::kMultisampleResolve;
  color0.resolve_texture = std::move(resolve_tex);

  auto render_target_desc = std::make_optional<RenderTarget>();
  render_target_desc->SetColorAttachment(color0, 0u);

  return render_target_desc;
}

std::unique_ptr<SurfaceMTL> SurfaceMTL::MakeFromMetalLayerDrawable(
    const std::shared_ptr<Context>& context,
    id<CAMetalDrawable> drawable,
    std::optional<IRect> clip_rect) {
  bool requires_blit = ShouldPerformPartialRepaint(clip_rect);

  auto render_target =
      WrapTextureWithRenderTarget(*context->GetResourceAllocator(),
                                  drawable.texture, requires_blit, clip_rect);
  if (!render_target) {
    return nullptr;
  }

  auto source_texture =
      requires_blit ? render_target->GetRenderTargetTexture() : nullptr;
  auto destination_texture = TextureMTL::Wrapper(
      render_target->GetRenderTargetTexture()->GetTextureDescriptor(),
      drawable.texture);

  return std::unique_ptr<SurfaceMTL>(new SurfaceMTL(
      context,                                  // context
      *render_target,                           // target
      render_target->GetRenderTargetTexture(),  // resolve_texture
      drawable,                                 // drawable
      source_texture,                           // source_texture
      destination_texture,                      // destination_texture
      requires_blit,                            // requires_blit
      clip_rect                                 // clip_rect
      ));
}

std::unique_ptr<SurfaceMTL> SurfaceMTL::MakeFromTexture(
    const std::shared_ptr<Context>& context,
    id<MTLTexture> texture,
    std::optional<IRect> clip_rect) {
  bool requires_blit = ShouldPerformPartialRepaint(clip_rect);

  auto render_target = WrapTextureWithRenderTarget(
      *context->GetResourceAllocator(), texture, requires_blit, clip_rect);
  if (!render_target) {
    return nullptr;
  }

  auto source_texture =
      requires_blit ? render_target->GetRenderTargetTexture() : nullptr;
  auto destination_texture = TextureMTL::Wrapper(
      render_target->GetRenderTargetTexture()->GetTextureDescriptor(), texture);

  return std::unique_ptr<SurfaceMTL>(new SurfaceMTL(
      context,                                  // context
      *render_target,                           // target
      render_target->GetRenderTargetTexture(),  // resolve_texture
      nil,                                      // drawable
      source_texture,                           // source_texture
      destination_texture,                      // destination_texture
      requires_blit,                            // requires_blit
      clip_rect                                 // clip_rect
      ));
}

SurfaceMTL::SurfaceMTL(const std::weak_ptr<Context>& context,
                       const RenderTarget& target,
                       std::shared_ptr<Texture> resolve_texture,
                       id<CAMetalDrawable> drawable,
                       std::shared_ptr<Texture> source_texture,
                       std::shared_ptr<Texture> destination_texture,
                       bool requires_blit,
                       std::optional<IRect> clip_rect)
    : Surface(target),
      context_(context),
      resolve_texture_(std::move(resolve_texture)),
      drawable_(drawable),
      source_texture_(std::move(source_texture)),
      destination_texture_(std::move(destination_texture)),
      requires_blit_(requires_blit),
      clip_rect_(clip_rect) {}

// |Surface|
SurfaceMTL::~SurfaceMTL() = default;

bool SurfaceMTL::ShouldPerformPartialRepaint(std::optional<IRect> damage_rect) {
  // compositor_context.cc will conditionally disable partial repaint if the
  // damage region is large. If that happened, then a nullopt damage rect
  // will be provided here.
  if (!damage_rect.has_value()) {
    return false;
  }
  // If the damage rect is 0 in at least one dimension, partial repaint isn't
  // performed as we skip right to present.
  if (damage_rect->size.width <= 0 || damage_rect->size.height <= 0) {
    return false;
  }
  return true;
}

// |Surface|
IRect SurfaceMTL::coverage() const {
  return IRect::MakeSize(resolve_texture_->GetSize());
}

static bool ShouldWaitForCommandBuffer() {
#if ((FML_OS_MACOSX && !FML_OS_IOS) || FML_OS_IOS_SIMULATOR)
  // If a transaction is present, `presentDrawable` will present too early. And
  // so we wait on an empty command buffer to get scheduled instead, which
  // forces us to also wait for all of the previous command buffers in the queue
  // to get scheduled.
  return true;
#else
  // On Physical iOS devices we still need to wait if we're taking a frame
  // capture.
  return [[MTLCaptureManager sharedCaptureManager] isCapturing];
#endif  // ((FML_OS_MACOSX && !FML_OS_IOS) || FML_OS_IOS_SIMULATOR)
}

// |Surface|
bool SurfaceMTL::Present() const {
  auto context = context_.lock();
  if (!context) {
    return false;
  }

  if (requires_blit_) {
    if (!(source_texture_ && destination_texture_)) {
      return false;
    }

    auto blit_command_buffer = context->CreateCommandBuffer();
    if (!blit_command_buffer) {
      return false;
    }
    auto blit_pass = blit_command_buffer->CreateBlitPass();
    if (!clip_rect_.has_value()) {
      VALIDATION_LOG << "Missing clip rectangle.";
      return false;
    }
    blit_pass->AddCopy(source_texture_, destination_texture_, std::nullopt,
                       clip_rect_->origin);
    blit_pass->EncodeCommands(context->GetResourceAllocator());
    if (!blit_command_buffer->SubmitCommands()) {
      return false;
    }
  }

  if (drawable_) {
    if (ShouldWaitForCommandBuffer()) {
      id<MTLCommandBuffer> command_buffer =
          ContextMTL::Cast(context.get())->CreateMTLCommandBuffer();
      [command_buffer commit];
      [command_buffer waitUntilScheduled];
    }
    [drawable_ present];
  }

  return true;
}
#pragma GCC diagnostic pop

}  // namespace impeller

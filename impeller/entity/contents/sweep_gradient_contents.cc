// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sweep_gradient_contents.h"

#include "flutter/fml/logging.h"
#include "impeller/entity/contents/clip_contents.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/gradient_generator.h"
#include "impeller/entity/entity.h"
#include "impeller/geometry/gradient.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/renderer/sampler_library.h"

namespace impeller {

SweepGradientContents::SweepGradientContents() = default;

SweepGradientContents::~SweepGradientContents() = default;

void SweepGradientContents::SetCenterAndAngles(Point center,
                                               Degrees start_angle,
                                               Degrees end_angle) {
  center_ = center;
  Scalar t0 = start_angle.degrees / 360;
  Scalar t1 = end_angle.degrees / 360;
  FML_DCHECK(t0 < t1);
  bias_ = -t0;
  scale_ = 1 / (t1 - t0);
}

void SweepGradientContents::SetColors(std::vector<Color> colors) {
  colors_ = std::move(colors);
}

void SweepGradientContents::SetStops(std::vector<Scalar> stops) {
  stops_ = std::move(stops);
}

void SweepGradientContents::SetTileMode(Entity::TileMode tile_mode) {
  tile_mode_ = tile_mode;
}

const std::vector<Color>& SweepGradientContents::GetColors() const {
  return colors_;
}

const std::vector<Scalar>& SweepGradientContents::GetStops() const {
  return stops_;
}

bool SweepGradientContents::Render(const ContentContext& renderer,
                                   const Entity& entity,
                                   RenderPass& pass) const {
  if (renderer.GetBackendFeatures().ssbo_support) {
    return RenderSSBO(renderer, entity, pass);
  }
  return RenderTexture(renderer, entity, pass);
}

bool SweepGradientContents::RenderSSBO(const ContentContext& renderer,
                                       const Entity& entity,
                                       RenderPass& pass) const {
  using VS = SweepGradientSSBOFillPipeline::VertexShader;
  using FS = SweepGradientSSBOFillPipeline::FragmentShader;

  FS::GradientInfo gradient_info;
  gradient_info.center = center_;
  gradient_info.bias = bias_;
  gradient_info.scale = scale_;
  gradient_info.tile_mode = static_cast<Scalar>(tile_mode_);
  gradient_info.alpha = GetAlpha();

  auto& host_buffer = pass.GetTransientsBuffer();
  auto colors = CreateGradientColors(colors_, stops_);

  gradient_info.colors_length = colors.size();
  auto color_buffer = host_buffer.Emplace(
      colors.data(), colors.size() * sizeof(StopData), alignof(StopData));

  VS::FrameInfo frame_info;
  frame_info.mvp = Matrix::MakeOrthographic(pass.GetRenderTargetSize()) *
                   entity.GetTransformation();
  frame_info.matrix = GetInverseMatrix();

  Command cmd;
  cmd.label = "SweepGradientSSBOFill";
  cmd.stencil_reference = entity.GetStencilDepth();
  auto geometry_result =
      GetGeometry()->GetPositionBuffer(renderer, entity, pass);

  auto options = OptionsFromPassAndEntity(pass, entity);
  if (geometry_result.prevent_overdraw) {
    options.stencil_compare = CompareFunction::kEqual;
    options.stencil_operation = StencilOperation::kIncrementClamp;
  }
  options.primitive_type = geometry_result.type;
  cmd.pipeline = renderer.GetSweepGradientSSBOFillPipeline(options);

  cmd.BindVertices(geometry_result.vertex_buffer);
  FS::BindGradientInfo(
      cmd, pass.GetTransientsBuffer().EmplaceUniform(gradient_info));
  FS::BindColorData(cmd, color_buffer);
  VS::BindFrameInfo(cmd, pass.GetTransientsBuffer().EmplaceUniform(frame_info));

  if (!pass.AddCommand(std::move(cmd))) {
    return false;
  }

  if (geometry_result.prevent_overdraw) {
    return ClipRestoreContents().Render(renderer, entity, pass);
  }
  return true;
}

bool SweepGradientContents::RenderTexture(const ContentContext& renderer,
                                          const Entity& entity,
                                          RenderPass& pass) const {
  using VS = SweepGradientFillPipeline::VertexShader;
  using FS = SweepGradientFillPipeline::FragmentShader;

  auto gradient_data = CreateGradientBuffer(colors_, stops_);
  auto gradient_texture =
      CreateGradientTexture(gradient_data, renderer.GetContext());
  if (gradient_texture == nullptr) {
    return false;
  }

  FS::GradientInfo gradient_info;
  gradient_info.center = center_;
  gradient_info.bias = bias_;
  gradient_info.scale = scale_;
  gradient_info.texture_sampler_y_coord_scale =
      gradient_texture->GetYCoordScale();
  gradient_info.tile_mode = static_cast<Scalar>(tile_mode_);
  gradient_info.alpha = GetAlpha();
  gradient_info.half_texel = Vector2(0.5 / gradient_texture->GetSize().width,
                                     0.5 / gradient_texture->GetSize().height);

  auto geometry_result =
      GetGeometry()->GetPositionBuffer(renderer, entity, pass);

  VS::FrameInfo frame_info;
  frame_info.mvp = geometry_result.transform;
  frame_info.matrix = GetInverseMatrix();

  Command cmd;
  cmd.label = "SweepGradientFill";
  cmd.stencil_reference = entity.GetStencilDepth();

  auto options = OptionsFromPassAndEntity(pass, entity);
  if (geometry_result.prevent_overdraw) {
    options.stencil_compare = CompareFunction::kEqual;
    options.stencil_operation = StencilOperation::kIncrementClamp;
  }
  options.primitive_type = geometry_result.type;
  cmd.pipeline = renderer.GetSweepGradientFillPipeline(options);

  cmd.BindVertices(geometry_result.vertex_buffer);
  FS::BindGradientInfo(
      cmd, pass.GetTransientsBuffer().EmplaceUniform(gradient_info));
  VS::BindFrameInfo(cmd, pass.GetTransientsBuffer().EmplaceUniform(frame_info));
  SamplerDescriptor sampler_desc;
  sampler_desc.min_filter = MinMagFilter::kLinear;
  sampler_desc.mag_filter = MinMagFilter::kLinear;
  FS::BindTextureSampler(
      cmd, gradient_texture,
      renderer.GetContext()->GetSamplerLibrary()->GetSampler(sampler_desc));

  if (!pass.AddCommand(std::move(cmd))) {
    return false;
  }

  if (geometry_result.prevent_overdraw) {
    return ClipRestoreContents().Render(renderer, entity, pass);
  }
  return true;
}

}  // namespace impeller

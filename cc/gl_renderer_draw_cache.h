// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_GL_RENDERER_DRAW_CACHE_H_
#define CC_GL_RENDERER_DRAW_CACHE_H_

#include <vector>


// Collects 4 floats at a time for easy upload to GL.
struct Float4 { float data[4]; };

// Collects 16 floats at a time for easy upload to GL.
struct Float16 { float data[16]; };

// A cache for storing textured quads to be drawn.  Stores the minimum required
// data to tell if two back to back draws only differ in their transform. Quads
// that only differ by transform may be coalesced into a single draw call.
struct TexturedQuadDrawCache {
  TexturedQuadDrawCache();
  ~TexturedQuadDrawCache();

  // Values tracked to determine if textured quads may be coalesced.
  int program_id;
  int resource_id;
  bool use_premultiplied_alpha;
  bool needs_blending;

  // Information about the program binding that is required to draw.
  int uv_xform_location;
  int vertex_opacity_location;
  int matrix_location;
  int sampler_location;

  // A cache for the coalesced quad data.
  std::vector<Float4> uv_xform_data;
  std::vector<float> vertex_opacity_data;
  std::vector<Float16> matrix_data;
};

#endif // CC_GL_RENDERER_DRAW_CACHE_H_


// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_BASE_AUDIO_RENDERER_SINK_H_
#define MEDIA_BASE_AUDIO_RENDERER_SINK_H_

#include <vector>
#include "base/basictypes.h"
#include "base/memory/ref_counted.h"
#include "media/audio/audio_parameters.h"
#include "media/base/audio_bus.h"
#include "media/base/media_export.h"

namespace media {

// AudioRendererSink is an interface representing the end-point for
// rendered audio.  An implementation is expected to
// periodically call Render() on a callback object.

class AudioRendererSink
    : public base::RefCountedThreadSafe<media::AudioRendererSink> {
 public:
  class RenderCallback {
   public:
    // Attempts to completely fill all channels of |dest|, returns actual
    // number of frames filled.
    virtual int Render(AudioBus* dest, int audio_delay_milliseconds) = 0;

    // Synchronized audio I/O - see InitializeIO() below.
    virtual void RenderIO(AudioBus* source,
                          AudioBus* dest,
                          int audio_delay_milliseconds) {}

    // Signals an error has occurred.
    virtual void OnRenderError() = 0;

   protected:
    virtual ~RenderCallback() {}
  };

  // Sets important information about the audio stream format.
  // It must be called before any of the other methods.
  // For clients wishing to have synchronized input and output,
  // |params| may specify |input_channels| > 0, representing a
  // number of input channels which will be at the same sample-rate
  // and buffer-size as the output as specified in |params|.
  // In this case, the callback's RenderIO() method will be called instead
  // of Render(), providing the synchronized input data at the same time as
  // when new output data is to be rendered.
  virtual void Initialize(const AudioParameters& params,
                          RenderCallback* callback) = 0;

  // Starts audio playback.
  virtual void Start() = 0;

  // Stops audio playback.
  virtual void Stop() = 0;

  // Pauses playback.
  virtual void Pause(bool flush) = 0;

  // Resumes playback after calling Pause().
  virtual void Play() = 0;

  // Sets the playback volume, with range [0.0, 1.0] inclusive.
  // Returns |true| on success.
  virtual bool SetVolume(double volume) = 0;

 protected:
  friend class base::RefCountedThreadSafe<AudioRendererSink>;
  virtual ~AudioRendererSink() {}
};

}  // namespace media

#endif  // MEDIA_BASE_AUDIO_RENDERER_SINK_H_

// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/mojo/services/mojo_demuxer_stream_adapter.h"

#include <stdint.h>

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/numerics/safe_conversions.h"
#include "media/base/decoder_buffer.h"
#include "media/mojo/common/media_type_converters.h"
#include "media/mojo/common/mojo_decoder_buffer_converter.h"
#include "mojo/public/cpp/system/data_pipe.h"

namespace media {

MojoDemuxerStreamAdapter::MojoDemuxerStreamAdapter(
    mojo::PendingRemote<mojom::DemuxerStream> demuxer_stream,
    base::OnceClosure stream_ready_cb)
    : demuxer_stream_(std::move(demuxer_stream)),
      stream_ready_cb_(std::move(stream_ready_cb)) {
  DVLOG(1) << __func__;
  demuxer_stream_->Initialize(base::BindOnce(
      &MojoDemuxerStreamAdapter::OnStreamReady, weak_factory_.GetWeakPtr()));
}

MojoDemuxerStreamAdapter::~MojoDemuxerStreamAdapter() {
  DVLOG(1) << __func__;
}

void MojoDemuxerStreamAdapter::Read(uint32_t count, ReadCB read_cb) {
  DVLOG(3) << __func__;
  // We shouldn't be holding on to a previous callback if a new Read() came in.
  DCHECK(!read_cb_);

  read_cb_ = std::move(read_cb);
  demuxer_stream_->Read(count,
                        base::BindOnce(&MojoDemuxerStreamAdapter::OnBufferReady,
                                       weak_factory_.GetWeakPtr()));
}

AudioDecoderConfig MojoDemuxerStreamAdapter::audio_decoder_config() {
  DCHECK_EQ(type_, AUDIO);
  return audio_config_;
}

VideoDecoderConfig MojoDemuxerStreamAdapter::video_decoder_config() {
  DCHECK_EQ(type_, VIDEO);
  return video_config_;
}

DemuxerStream::Type MojoDemuxerStreamAdapter::type() const {
  return type_;
}

void MojoDemuxerStreamAdapter::EnableBitstreamConverter() {
  demuxer_stream_->EnableBitstreamConverter();
}

bool MojoDemuxerStreamAdapter::SupportsConfigChanges() {
  return true;
}

#if BUILDFLAG(USE_STARBOARD_MEDIA)
std::string MojoDemuxerStreamAdapter::mime_type() const {
  return mime_type_;
}
#endif  // BUILDFLAG(USE_STARBOARD_MEDIA)

// TODO(xhwang): Pass liveness here.
void MojoDemuxerStreamAdapter::OnStreamReady(
    Type type,
    mojo::ScopedDataPipeConsumerHandle consumer_handle,
    const absl::optional<AudioDecoderConfig>& audio_config,
#if BUILDFLAG(USE_STARBOARD_MEDIA)
    const absl::optional<VideoDecoderConfig>& video_config,
    const std::string& mime_type) {
#else   // BUILDFLAG(USE_STARBOARD_MEDIA)
    const absl::optional<VideoDecoderConfig>& video_config) {
#endif  // BUILDFLAG(USE_STARBOARD_MEDIA)
  DVLOG(1) << __func__;
  DCHECK_EQ(UNKNOWN, type_);
  DCHECK(consumer_handle.is_valid());

  type_ = type;
#if BUILDFLAG(USE_STARBOARD_MEDIA)
  mime_type_ = mime_type;
#endif  // BUILDFLAG(USE_STARBOARD_MEDIA)

  mojo_decoder_buffer_reader_ =
      std::make_unique<MojoDecoderBufferReader>(std::move(consumer_handle));

  UpdateConfig(std::move(audio_config), std::move(video_config));

  std::move(stream_ready_cb_).Run();
}

void MojoDemuxerStreamAdapter::OnBufferReady(
    Status status,
    std::vector<mojom::DecoderBufferPtr> batch_buffers,
    const absl::optional<AudioDecoderConfig>& audio_config,
    const absl::optional<VideoDecoderConfig>& video_config) {
  DVLOG(3) << __func__ << ": status=" << status
           << ", batch_buffers.size=" << batch_buffers.size();
  DCHECK(read_cb_);
  DCHECK_NE(type_, UNKNOWN);

  if (status == kConfigChanged) {
    UpdateConfig(std::move(audio_config), std::move(video_config));
    std::move(read_cb_).Run(kConfigChanged, {});
    return;
  }

  if (status == kAborted) {
    std::move(read_cb_).Run(kAborted, {});
    return;
  }

  DCHECK_EQ(status, kOk);
  status_ = status;
  actual_read_count_ = batch_buffers.size();
  for (mojom::DecoderBufferPtr& buffer : batch_buffers) {
    mojo_decoder_buffer_reader_->ReadDecoderBuffer(
        std::move(buffer),
        base::BindOnce(&MojoDemuxerStreamAdapter::OnBufferRead,
                       weak_factory_.GetWeakPtr()));
  }
}

void MojoDemuxerStreamAdapter::OnBufferRead(
    scoped_refptr<DecoderBuffer> buffer) {
  if (!buffer) {
    std::move(read_cb_).Run(kAborted, {});
    buffer_queue_.clear();
    return;
  }

  buffer_queue_.push_back(buffer);
  if (buffer_queue_.size() == actual_read_count_) {
    std::move(read_cb_).Run(status_, buffer_queue_);
    actual_read_count_ = 0;
    buffer_queue_.clear();
  }
}

void MojoDemuxerStreamAdapter::UpdateConfig(
    const absl::optional<AudioDecoderConfig>& audio_config,
    const absl::optional<VideoDecoderConfig>& video_config) {
  DCHECK_NE(type_, Type::UNKNOWN);

  switch(type_) {
    case AUDIO:
      DCHECK(audio_config && !video_config);
      audio_config_ = audio_config.value();
      break;
    case VIDEO:
      DCHECK(video_config && !audio_config);
      video_config_ = video_config.value();
      break;
    default:
      NOTREACHED();
  }
}

}  // namespace media

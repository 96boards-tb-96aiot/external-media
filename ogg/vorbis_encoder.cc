/*
 * Copyright (C) 2017 Hertz Wang 1989wanghang@163.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see http://www.gnu.org/licenses
 *
 * Any non-GPL usage of this software or parts of this software is strictly
 * forbidden.
 *
 */

#include "encoder.h"

#include <assert.h>
#include <deque>

extern "C" {
#include <vorbis/vorbisenc.h>
}

#include "ogg_utils.h"

#include "buffer.h"

namespace easymedia {

// A encoder which call the libvorbisenc interface.
class VorbisEncoder : public AudioEncoder {
public:
  VorbisEncoder(const char *param);
  virtual ~VorbisEncoder();
  static const char *GetCodecName() { return "libvorbisenc"; }
  virtual bool Init() override { return true; }
  virtual bool InitConfig(const MediaConfig &cfg) override;
  virtual int
  Process(std::shared_ptr<MediaBuffer> input _UNUSED,
          std::shared_ptr<MediaBuffer> output _UNUSED,
          std::shared_ptr<MediaBuffer> extra_output _UNUSED) override {
    errno = ENOSYS;
    return -1;
  }
  virtual int SendInput(std::shared_ptr<MediaBuffer> input) override;
  virtual std::shared_ptr<MediaBuffer> FetchOutput() override;

private:
  vorbis_info vi;    /* struct that stores all the static vorbis bitstream
                        settings */
  vorbis_comment vc; /* struct that stores all the user comments */

  vorbis_dsp_state vd;
  vorbis_block vb;

  std::deque<std::shared_ptr<MediaBuffer>> cached_ogg_packets;
  static const int MAX_CACHED_SIZE = 8;
};

VorbisEncoder::VorbisEncoder(const char *param _UNUSED) {
  vorbis_info_init(&vi);
  vorbis_comment_init(&vc);
  memset(&vd, 0, sizeof(vd));
  memset(&vb, 0, sizeof(vb));
}

VorbisEncoder::~VorbisEncoder() {
  vorbis_block_clear(&vb);
  vorbis_dsp_clear(&vd);
  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);
}

static int __ogg_packet_free(void *p) {
  return ogg_packet_free((ogg_packet *)p);
}

bool VorbisEncoder::InitConfig(const MediaConfig &cfg) {
  if (!vi.codec_setup)
    return false;
  const AudioConfig &ac = cfg.aud_cfg;
  const SampleInfo &si = ac.sample_info;
  // set vbr
  int ret =
      vorbis_encode_init_vbr(&vi, si.channels, si.sample_rate, ac.quality);
  if (ret) {
    LOG("vorbis_encode_init_vbr failed, ret = %d\n", ret);
    return false;
  }
  vorbis_comment_add_tag(&vc, "Encoder", GetCodecName());
  ret = vorbis_analysis_init(&vd, &vi);
  if (ret) {
    LOG("vorbis_analysis_init failed, ret = %d\n", ret);
    return false;
  }
  ret = vorbis_block_init(&vd, &vb);
  if (ret) {
    LOG("vorbis_block_init failed, ret = %d\n", ret);
    return false;
  }
  ogg_packet header;
  ogg_packet header_comm;
  ogg_packet header_code;
  ret =
      vorbis_analysis_headerout(&vd, &vc, &header, &header_comm, &header_code);
  if (ret) {
    LOG("vorbis_analysis_headerout failed, ret = %d\n", ret);
    return false;
  }
  void *extradata = nullptr;
  size_t extradatasize = 0;
  std::list<ogg_packet> ogg_packets;
  ogg_packets.push_back(header);
  ogg_packets.push_back(header_comm);
  ogg_packets.push_back(header_code);
  if (PackOggPackets(ogg_packets, &extradata, &extradatasize)) {
    SetExtraData(extradata, extradatasize, false);
  } else {
    LOG("PackOggPackets failed\n");
    return false;
  }
  return AudioEncoder::InitConfig(cfg);
}

int VorbisEncoder::SendInput(std::shared_ptr<MediaBuffer> input) {
  assert(input->GetType() == Type::Audio);
  std::shared_ptr<easymedia::SampleBuffer> sample_buffer =
      std::static_pointer_cast<easymedia::SampleBuffer>(input);
  SampleInfo &info = sample_buffer->GetSampleInfo();
  if (info.channels != 2 || info.fmt != SAMPLE_FMT_S16) {
    LOG("libvorbisenc only support s16 with 2ch\n");
    return -1;
  }

  int ret;
  int frame_num = sample_buffer->GetFrames();
  if (frame_num == 0) {
    if ((ret = vorbis_analysis_wrote(&vd, 0)) < 0) {
      LOG("vorbis_analysis_wrote 0 failed, ret = %d\n", ret);
      return -1;
    }
  } else {
    if (cached_ogg_packets.size() > MAX_CACHED_SIZE) {
      LOG("cached packets must be page out first\n");
      return -1;
    }
    int i;
    signed char *readbuffer = (signed char *)sample_buffer->GetPtr();
    float **buffer = vorbis_analysis_buffer(&vd, frame_num);
    /* uninterleave samples */
    for (i = 0; i < frame_num; i++) {
      buffer[0][i] =
          ((readbuffer[i * 4 + 1] << 8) | (0x00ff & (int)readbuffer[i * 4])) /
          32768.f;
      buffer[1][i] = ((readbuffer[i * 4 + 3] << 8) |
                      (0x00ff & (int)readbuffer[i * 4 + 2])) /
                     32768.f;
    }

    /* tell the library how much we actually submitted */
    if ((ret = vorbis_analysis_wrote(&vd, i)) < 0) {
      LOG("vorbis_analysis_wrote %d failed, ret = %d\n", i, ret);
      return -1;
    }
  }
  ogg_packet op;
  while ((ret = vorbis_analysis_blockout(&vd, &vb) == 1)) {
    if ((ret = vorbis_analysis(&vb, NULL)) < 0)
      break;
    if ((ret = vorbis_bitrate_addblock(&vb)) < 0)
      break;
    while ((ret = vorbis_bitrate_flushpacket(&vd, &op)) == 1) {
      ogg_packet *new_packet = ogg_packet_clone(op);
      if (!new_packet) {
        errno = ENOMEM;
        return -1;
      }
      auto buffer =
          std::make_shared<MediaBuffer>(new_packet->packet, new_packet->bytes,
                                        -1, new_packet, __ogg_packet_free);
      if (!buffer) {
        errno = ENOMEM;
        return -1;
      }
      buffer->SetType(Type::Audio);
      buffer->SetValidSize(op.bytes);
      buffer->SetTimeStamp(op.granulepos);
      buffer->SetEOF(op.e_o_s);
      cached_ogg_packets.push_back(buffer);
    }
    if (ret < 0) {
      LOG("vorbis_bitrate_flushpacket failed\n");
      break;
    }
  }

  if (ret < 0) {
    LOG("error getting available ogg packets, ret = %d\n", ret);
    return -1;
  }

  return 0;
}

// output: userdata is the ogg_packet while ptr is the ogg_packet.packet.
std::shared_ptr<MediaBuffer> VorbisEncoder::FetchOutput() {
  if (cached_ogg_packets.size() == 0)
    return nullptr;
  auto buffer = cached_ogg_packets.front();
  cached_ogg_packets.pop_front();
  return buffer;
}

DEFINE_AUDIO_ENCODER_FACTORY(VorbisEncoder)
const char *FACTORY(VorbisEncoder)::ExpectedInputDataType() {
  return AUDIO_PCM_S16;
}
const char *FACTORY(VorbisEncoder)::OutPutDataType() { return AUDIO_VORBIS; }

} // namespace easymedia

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

#ifndef EASYMEDIA_MPP_ENCODER_H
#define EASYMEDIA_MPP_ENCODER_H

#include "encoder.h"
#include "mpp_inc.h"

namespace easymedia {

// A encoder which call the mpp interface directly.
// Mpp is always video process module.
class MPPEncoder : public VideoEncoder {
public:
  MPPEncoder();
  virtual ~MPPEncoder() = default;

  virtual bool Init() override;
  virtual bool InitConfig(const MediaConfig &cfg) override;

  // sync encode the raw input buffer to output buffer
  virtual int Process(std::shared_ptr<MediaBuffer> input,
                      std::shared_ptr<MediaBuffer> output,
                      std::shared_ptr<MediaBuffer> extra_output) override;

  virtual int SendInput(std::shared_ptr<MediaBuffer> input) override;
  virtual std::shared_ptr<MediaBuffer> FetchOutput() override;

protected:
  // call before Init()
  void SetMppCodeingType(MppCodingType type) { coding_type = type; }
  virtual bool
  CheckConfigChange(std::pair<uint32_t, std::shared_ptr<ParameterBuffer>>) {
    return true;
  }
  // Control before encoding.
  int EncodeControl(int cmd, void *param);

  virtual int PrepareMppFrame(std::shared_ptr<MediaBuffer> input,
                              MppFrame &frame);
  virtual int PrepareMppPacket(std::shared_ptr<MediaBuffer> output,
                               MppPacket &packet);
  virtual int PrepareMppExtraBuffer(std::shared_ptr<MediaBuffer> extra_output,
                                    MppBuffer &buffer);
  int Process(MppFrame frame, MppPacket &packet, MppBuffer &mv_buf);

private:
  MppCodingType coding_type;
  std::shared_ptr<MPPContext> mpp_ctx;
};

} // namespace easymedia

#endif // EASYMEDIA_MPP_ENCODER_H

/************************************************************************************************
Copyright (C) 2023 Hesai Technology Co., Ltd.
Copyright (C) 2023 Original Authors
All rights reserved.

All code in this repository is released under the terms of the following Modified BSD License. 
Redistribution and use in source and binary forms, with or without modification, are permitted 
provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this list of conditions and 
  the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this list of conditions and 
  the following disclaimer in the documentation and/or other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its contributors may be used to endorse or 
  promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED 
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A 
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR 
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR 
TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF 
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
************************************************************************************************/

/*
 * File:       udp_p40_parser.cc
 * Author:     Zhang Yu <zhangyu@hesaitech.com>
 * Description: Implemente UdpP40Parser class
*/

#include "udp_p40_parser.h"
#include "general_parser.h"
#include "udp_protocol_p40.h"
using namespace hesai::lidar;
#define DISTANCEUNIT 0.004
template<typename T_Point>
UdpP40Parser<T_Point>::UdpP40Parser() {
  this->motor_speed_ = 0;
  this->return_mode_ = 0;
}

template<typename T_Point>
UdpP40Parser<T_Point>::~UdpP40Parser() { LogInfo("release general parser"); }

template<typename T_Point>
int UdpP40Parser<T_Point>::ComputeXYZI(LidarDecodedFrame<T_Point> &frame, int packet_index) {
  for (int blockid = 0; blockid < frame.block_num; blockid++) {
    // T_Point point;
    int elevation = 0;
    int azimuth = 0;

    for (int i = 0; i < frame.laser_num; i++) {
      int point_index = packet_index * frame.per_points_num + blockid * frame.laser_num + i;
      float distance = frame.pointData[point_index].distances * DISTANCEUNIT;
      int Azimuth = frame.pointData[point_index].azimuth * kFineResolutionInt;
      if (this->get_correction_file_) {
        int azimuth_coll = (int(this->azimuth_collection_[i] * kAllFineResolutionFloat) + CIRCLE) % CIRCLE;
        int elevation_corr = (int(this->elevation_correction_[i] * kAllFineResolutionFloat) + CIRCLE) % CIRCLE;
        if (this->enable_distance_correction_) {
          GetDistanceCorrection(azimuth_coll, elevation_corr, distance, GeometricCenter);
        }
        elevation = elevation_corr;
        azimuth = Azimuth + azimuth_coll;
        azimuth = (CIRCLE + azimuth) % CIRCLE;
      } 
      if (frame.config.fov_start != -1 && frame.config.fov_end != -1)
      {
        int fov_transfer = azimuth / 256 / 100;
        if (fov_transfer < frame.config.fov_start || fov_transfer > frame.config.fov_end){//不在fov范围continue
          continue;
        }
      }     
      float xyDistance = distance * this->cos_all_angle_[(elevation)];
      float x = xyDistance * this->sin_all_angle_[(azimuth)];
      float y = xyDistance * this->cos_all_angle_[(azimuth)];
      float z = distance * this->sin_all_angle_[(elevation)];
      this->TransformPoint(x, y, z);
      setX(frame.points[point_index], x);
      setY(frame.points[point_index], y);
      setZ(frame.points[point_index], z);
      setIntensity(frame.points[point_index], frame.pointData[point_index].reflectivities);
      setTimestamp(frame.points[point_index], double(frame.sensor_timestamp[packet_index]) / kMicrosecondToSecond);
      setRing(frame.points[point_index], i);
    }
  }
  GeneralParser<T_Point>::FrameNumAdd();
  return 0;
}

template<typename T_Point>
bool UdpP40Parser<T_Point>::IsNeedFrameSplit(uint16_t azimuth) {
  // Determine frame_start_azimuth_ [0,360)
  if (this->frame_start_azimuth_ < 0.0f || this->frame_start_azimuth_ >= 360.0f) {
    this->frame_start_azimuth_ = 0.0f;
  }
  // The first two packet dont have the information of last_azimuth_  and last_last_azimuth, so do not need split frame
  // The initial value of last_azimuth_ is -1
  // Determine the rotation direction and division
  int8_t rotation_flag = 1;
  uint16_t division = 0;
  // If last_last_azimuth_ != -1，the packet is the third, so we can determine whether the current packet requires framing
  if (this->last_last_azimuth_ != -1) 
  {
    // Get the division
    uint16_t division1 = abs(this->last_azimuth_ - this->last_last_azimuth_);
    uint16_t division2 = abs(this->last_azimuth_ - azimuth);
    division = std::min(division1, division2);
    // Prevent two consecutive packets from having the same angle when causing an error in framing
    if ( division == 0) return false;
    // In the three consecutive angle values, if the angle values appear by the division of the decreasing situation,it must be reversed
    // The same is true for FOV
    if( this->last_last_azimuth_ - this->last_azimuth_ == division || this->last_azimuth_ -azimuth == division)
    {
      rotation_flag = 0;
    }
  } else {
    // The first  and second packet do not need split frame
    return false;
  }
  if (rotation_flag) {
    // When an angle jump occurs, it maybe 359.9-0 or 39.9-40-10.0(consired FOV)
    if (this->last_azimuth_- azimuth > division)
    {
      if (uint16_t(this->frame_start_azimuth_ * kResolutionInt) > this->last_azimuth_ || uint16_t(this->frame_start_azimuth_ * kResolutionInt <= azimuth)) {
        return true;
      } 
      return false;
    }
    // No angle jump occurs, the frame_start_azimuth must betwen last_azimuth and azimuth  
    if (this->last_azimuth_ < azimuth && this->last_azimuth_ < uint16_t(this->frame_start_azimuth_ * kResolutionInt) 
        && azimuth >= uint16_t(this->frame_start_azimuth_ * kResolutionInt)) {
      return true;
    }
    return false;
  } else {
    if (azimuth - this->last_azimuth_ > division)
    {
      if (uint16_t(this->frame_start_azimuth_ * kResolutionInt) <= this->last_azimuth_ || uint16_t(this->frame_start_azimuth_ * kResolutionInt > azimuth)) {
        return true;
      } 
      return false;
    }  
    if (this->last_azimuth_ > azimuth && this->last_azimuth_ > uint16_t(this->frame_start_azimuth_ * kResolutionInt) 
        && azimuth <= uint16_t(this->frame_start_azimuth_ * kResolutionInt)) {
      return true;
    }
    return false;
  }
}

template<typename T_Point>
int UdpP40Parser<T_Point>::DecodePacket(LidarDecodedFrame<T_Point> &frame, const UdpPacket& udpPacket)
{
  if (!this->get_correction_file_) {
    static bool printErrorBool = true;
    if (printErrorBool) {
      LogInfo("No available angle calibration files, prohibit parsing of point cloud packages");
      printErrorBool = false;
    }
    return -1;
  }
  if (udpPacket.buffer[0] != 0xFF || udpPacket.buffer[1] != 0xEE ) {
    return -1;
  }
  const HS_LIDAR_BODY_AZIMUTH_L40 *pAzimuth = 
      reinterpret_cast<const HS_LIDAR_BODY_AZIMUTH_L40 *> (&(udpPacket.buffer[0]));
  
  const HS_LIDAR_BODY_CHN_UNIT_L40 *pChnUnit = 
      reinterpret_cast<const HS_LIDAR_BODY_CHN_UNIT_L40 *> ((const unsigned char *)pAzimuth + 
        sizeof(HS_LIDAR_BODY_AZIMUTH_L40));

  const HS_LIDAR_TAIL_L40 *pTail = 
      reinterpret_cast<const HS_LIDAR_TAIL_L40 *> (
        &(udpPacket.buffer[0]) +(sizeof(HS_LIDAR_BODY_AZIMUTH_L40) +
          sizeof(HS_LIDAR_BODY_CHN_UNIT_L40) * LASERNUM) * BLOCKNUM);

  const HS_LIDAR_TAIL_SEQ_NUM_L40 *pSeqNum = 
      reinterpret_cast<const HS_LIDAR_TAIL_SEQ_NUM_L40 *> (
        (const unsigned char *)pTail + sizeof(HS_LIDAR_TAIL_L40));

  this->CalPktLoss(pSeqNum->GetSeqNum());
  this->CalPktTimeLoss(pTail->GetMicroLidarTimeU64());
  this->spin_speed_ = pTail->GetMotorSpeed();
  frame.spin_speed = pTail->GetMotorSpeed();
  frame.lidar_state = pTail->m_u8Shutdown;
  frame.return_mode = pTail->m_u8ReturnMode;

  frame.per_points_num = BLOCKNUM * LASERNUM;
  frame.scan_complete = false;
  frame.block_num = BLOCKNUM;
  frame.laser_num = LASERNUM;
  frame.distance_unit = DISTANCEUNIT;

  if (frame.use_timestamp_type == 0) {
    frame.sensor_timestamp[frame.packet_num] = pTail->GetMicroLidarTimeU64();
  } else {
    frame.sensor_timestamp[frame.packet_num] = udpPacket.recv_timestamp;
  }
  
  frame.host_timestamp = GetMicroTickCountU64();
  int index = frame.packet_num * BLOCKNUM * LASERNUM;
  uint16_t u16Azimuth = 0;
  for (int j = 0; j < BLOCKNUM; j++) {
    u16Azimuth = pAzimuth->GetAzimuth();
    pChnUnit = reinterpret_cast<const HS_LIDAR_BODY_CHN_UNIT_L40 *>((const unsigned char *)pAzimuth + sizeof(HS_LIDAR_BODY_AZIMUTH_L40));

    pAzimuth = reinterpret_cast<const HS_LIDAR_BODY_AZIMUTH_L40 *>(
        (const unsigned char *)pAzimuth + sizeof(HS_LIDAR_BODY_AZIMUTH_L40) + 
        sizeof(HS_LIDAR_BODY_CHN_UNIT_L40) * LASERNUM
    );
    for (int i = 0; i < LASERNUM; i++) {
      if (this->get_firetime_file_) {
        frame.pointData[index].azimuth = u16Azimuth + this->GetFiretimesCorrection(i, this->spin_speed_) * kResolutionFloat;
      } else {
        frame.pointData[index].azimuth = u16Azimuth;
      }
      frame.pointData[index].distances = pChnUnit->GetDistance();
      frame.pointData[index].reflectivities = pChnUnit->GetReflectivity();
      pChnUnit = pChnUnit + 1;
      index = index + 1;   
    }
  }
  if (IsNeedFrameSplit(u16Azimuth)) {
    frame.scan_complete = true;
  }
  if (u16Azimuth != this->last_azimuth_) {
    this->last_last_azimuth_ = this->last_azimuth_; 
    this->last_azimuth_ = u16Azimuth;  
  }
  frame.packet_num++;
  return 0;
}
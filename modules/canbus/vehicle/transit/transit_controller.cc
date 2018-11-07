/* Copyright 2017 The Apollo Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "modules/canbus/vehicle/transit/transit_controller.h"

#include "modules/common/proto/vehicle_signal.pb.h"

#include "modules/common/kv_db/kv_db.h"
#include "cyber/common/log.h"
#include "modules/canbus/vehicle/transit/transit_message_manager.h"
#include "modules/canbus/vehicle/vehicle_controller.h"
#include "modules/common/time/time.h"
#include "modules/drivers/canbus/can_comm/can_sender.h"
#include "modules/drivers/canbus/can_comm/protocol_data.h"

namespace apollo {
namespace canbus {
namespace transit {

using ::apollo::common::ErrorCode;
using ::apollo::control::ControlCommand;
using ::apollo::drivers::canbus::ProtocolData;

namespace {
const int32_t kMaxFailAttempt = 10;
const int32_t CHECK_RESPONSE_STEER_UNIT_FLAG = 1;
const int32_t CHECK_RESPONSE_SPEED_UNIT_FLAG = 2;
}  // namespace

ErrorCode TransitController::Init(
    const VehicleParameter& params,
    CanSender<::apollo::canbus::ChassisDetail>* const can_sender,
    MessageManager<::apollo::canbus::ChassisDetail>* const message_manager) {
  if (is_initialized_) {
    AINFO << "TransitController has already been initiated.";
    return ErrorCode::CANBUS_ERROR;
  }

  params_.CopyFrom(params);
  if (!params_.has_driving_mode()) {
    AERROR << "Vehicle conf pb not set driving_mode.";
    return ErrorCode::CANBUS_ERROR;
  }

  if (can_sender == nullptr) {
    return ErrorCode::CANBUS_ERROR;
  }
  can_sender_ = can_sender;

  if (message_manager == nullptr) {
    AERROR << "protocol manager is null.";
    return ErrorCode::CANBUS_ERROR;
  }
  message_manager_ = message_manager;

  // sender part
  adc_auxiliarycontrol_110_ = dynamic_cast<Adcauxiliarycontrol110*>(
      message_manager_->GetMutableProtocolDataById(Adcauxiliarycontrol110::ID));
  if (adc_auxiliarycontrol_110_ == nullptr) {
    AERROR << "Adcauxiliarycontrol110 does not exist in the "
              "TransitMessageManager!";
    return ErrorCode::CANBUS_ERROR;
  }

  adc_motioncontrol1_10_ = dynamic_cast<Adcmotioncontrol110*>(
      message_manager_->GetMutableProtocolDataById(Adcmotioncontrol110::ID));
  if (adc_motioncontrol1_10_ == nullptr) {
    AERROR
        << "Adcmotioncontrol110 does not exist in the TransitMessageManager!";
    return ErrorCode::CANBUS_ERROR;
  }

  adc_motioncontrollimits1_12_ = dynamic_cast<Adcmotioncontrollimits112*>(
      message_manager_->GetMutableProtocolDataById(
          Adcmotioncontrollimits112::ID));
  if (adc_motioncontrollimits1_12_ == nullptr) {
    AERROR << "Adcmotioncontrollimits112 does not exist in the "
              "TransitMessageManager!";
    return ErrorCode::CANBUS_ERROR;
  }

  llc_diag_brakecontrol_721_ = dynamic_cast<Llcdiagbrakecontrol721*>(
      message_manager_->GetMutableProtocolDataById(Llcdiagbrakecontrol721::ID));
  if (llc_diag_brakecontrol_721_ == nullptr) {
    AERROR << "Llcdiagbrakecontrol721 does not exist in the "
              "TransitMessageManager!";
    return ErrorCode::CANBUS_ERROR;
  }

  llc_diag_steeringcontrol_722_ = dynamic_cast<Llcdiagsteeringcontrol722*>(
      message_manager_->GetMutableProtocolDataById(
          Llcdiagsteeringcontrol722::ID));
  if (llc_diag_steeringcontrol_722_ == nullptr) {
    AERROR << "Llcdiagsteeringcontrol722 does not exist in the "
              "TransitMessageManager!";
    return ErrorCode::CANBUS_ERROR;
  }

  can_sender_->AddMessage(Adcauxiliarycontrol110::ID, adc_auxiliarycontrol_110_,
                          false);
  can_sender_->AddMessage(Adcmotioncontrol110::ID, adc_motioncontrol1_10_,
                          false);
  can_sender_->AddMessage(Adcmotioncontrollimits112::ID,
                          adc_motioncontrollimits1_12_, false);
  can_sender_->AddMessage(Llcdiagbrakecontrol721::ID,
                          llc_diag_brakecontrol_721_, false);
  can_sender_->AddMessage(Llcdiagsteeringcontrol722::ID,
                          llc_diag_steeringcontrol_722_, false);

  // need sleep to ensure all messages received
  AINFO << "TransitController is initialized.";

  is_initialized_ = true;
  return ErrorCode::OK;
}

TransitController::~TransitController() {}

bool TransitController::Start() {
  if (!is_initialized_) {
    AERROR << "TransitController has NOT been initiated.";
    return false;
  }
  const auto& update_func = [this] { SecurityDogThreadFunc(); };
  thread_.reset(new std::thread(update_func));

  return true;
}

void TransitController::Stop() {
  if (!is_initialized_) {
    AERROR << "TransitController stops or starts improperly!";
    return;
  }

  if (thread_ != nullptr && thread_->joinable()) {
    thread_->join();
    thread_.reset();
    AINFO << "TransitController stopped.";
  }
}

Chassis TransitController::chassis() {
  chassis_.Clear();

  ChassisDetail chassis_detail;
  message_manager_->GetSensorData(&chassis_detail);

  // 21, 22, previously 1, 2
  if (driving_mode() == Chassis::EMERGENCY_MODE) {
    set_chassis_error_code(Chassis::NO_ERROR);
  }

  chassis_.set_driving_mode(driving_mode());
  chassis_.set_error_code(chassis_error_code());

  // 3
  chassis_.set_engine_started(true);
  // 4
  auto& transit = chassis_detail.transit();
  // TODO(luoqi): revisit it later
  // can't find speed
  auto& motion = transit.llc_motionfeedback1_20();
  if (motion.has_llc_fbk_throttleposition()) {
    chassis_.set_throttle_percentage(motion.llc_fbk_throttleposition());
  }
  // can't find brake percentage
  // can't find steering percentage
  if (motion.has_llc_fbk_gear()) {
    switch (motion.llc_fbk_gear()) {
      case Llc_motionfeedback1_20::LLC_FBK_GEAR_P_PARK:
        chassis_.set_gear_location(Chassis::GEAR_PARKING);
        break;
      case Llc_motionfeedback1_20::LLC_FBK_GEAR_D_DRIVE:
        chassis_.set_gear_location(Chassis::GEAR_DRIVE);
        break;
      case Llc_motionfeedback1_20::LLC_FBK_GEAR_N_NEUTRAL:
        chassis_.set_gear_location(Chassis::GEAR_NEUTRAL);
        break;
      case Llc_motionfeedback1_20::LLC_FBK_GEAR_R_REVERSE:
        chassis_.set_gear_location(Chassis::GEAR_REVERSE);
        break;
      default:
        break;
    }
  }
  auto& aux = transit.llc_auxiliaryfeedback_120();
  if (aux.has_llc_fbk_turnsignal()) {
    switch (aux.llc_fbk_turnsignal()) {
      case Adc_auxiliarycontrol_110::ADC_CMD_TURNSIGNAL_LEFT:
        chassis_.mutable_signal()
          ->set_turn_signal(common::VehicleSignal::TURN_LEFT);
      case Adc_auxiliarycontrol_110::ADC_CMD_TURNSIGNAL_RIGHT:
        chassis_.mutable_signal()
          ->set_turn_signal(common::VehicleSignal::TURN_RIGHT);
      case Adc_auxiliarycontrol_110::ADC_CMD_TURNSIGNAL_NONE:
        chassis_.mutable_signal()
          ->set_turn_signal(common::VehicleSignal::TURN_NONE);
      default:
        break;
    }
  }

  return chassis_;
}

void TransitController::Emergency() {
  set_driving_mode(Chassis::EMERGENCY_MODE);
  ResetProtocol();
}

ErrorCode TransitController::EnableAutoMode() {
  if (driving_mode() == Chassis::COMPLETE_AUTO_DRIVE) {
    AINFO << "already in COMPLETE_AUTO_DRIVE mode";
    return ErrorCode::OK;
  }
  adc_motioncontrol1_10_->
    set_adc_cmd_autonomyrequest(
    Adc_motioncontrol1_10::ADC_CMD_AUTONOMYREQUEST_AUTONOMY_REQUESTED);
  adc_motioncontrol1_10_->
    set_adc_cmd_steeringcontrolmode(
    Adc_motioncontrol1_10::ADC_CMD_STEERINGCONTROLMODE_ANGLE);
  adc_motioncontrol1_10_->
    set_adc_cmd_longitudinalcontrolmode(
    Adc_motioncontrol1_10::
    ADC_CMD_LONGITUDINALCONTROLMODE_DIRECT_THROTTLE_BRAKE);
  can_sender_->Update();
  const int32_t flag =
      CHECK_RESPONSE_STEER_UNIT_FLAG | CHECK_RESPONSE_SPEED_UNIT_FLAG;
  if (!CheckResponse(flag, true)) {
    AERROR << "Failed to switch to COMPLETE_AUTO_DRIVE mode.";
    Emergency();
    set_chassis_error_code(Chassis::CHASSIS_ERROR);
    return ErrorCode::CANBUS_ERROR;
  } else {
    set_driving_mode(Chassis::COMPLETE_AUTO_DRIVE);
    AINFO << "Switch to COMPLETE_AUTO_DRIVE mode ok.";
    return ErrorCode::OK;
  }
}

ErrorCode TransitController::DisableAutoMode() {
  ResetProtocol();
  can_sender_->Update();
  set_driving_mode(Chassis::COMPLETE_MANUAL);
  set_chassis_error_code(Chassis::NO_ERROR);
  AINFO << "Switch to COMPLETE_MANUAL ok.";
  return ErrorCode::OK;
}

ErrorCode TransitController::EnableSteeringOnlyMode() {
  if (driving_mode() == Chassis::COMPLETE_AUTO_DRIVE ||
      driving_mode() == Chassis::AUTO_STEER_ONLY) {
    set_driving_mode(Chassis::AUTO_STEER_ONLY);
    AINFO << "Already in AUTO_STEER_ONLY mode";
    return ErrorCode::OK;
  }
  adc_motioncontrol1_10_->
    set_adc_cmd_autonomyrequest(
    Adc_motioncontrol1_10::ADC_CMD_AUTONOMYREQUEST_AUTONOMY_REQUESTED);
  adc_motioncontrol1_10_->
    set_adc_cmd_steeringcontrolmode(
    Adc_motioncontrol1_10::ADC_CMD_STEERINGCONTROLMODE_ANGLE);
  adc_motioncontrol1_10_->
    set_adc_cmd_longitudinalcontrolmode(
    Adc_motioncontrol1_10::ADC_CMD_LONGITUDINALCONTROLMODE_NONE);
  can_sender_->Update();
  if (CheckResponse(CHECK_RESPONSE_STEER_UNIT_FLAG, true) == false) {
    AERROR << "Failed to switch to AUTO_STEER_ONLY mode.";
    Emergency();
    set_chassis_error_code(Chassis::CHASSIS_ERROR);
    return ErrorCode::CANBUS_ERROR;
  } else {
    set_driving_mode(Chassis::AUTO_STEER_ONLY);
    AINFO << "Switch to AUTO_STEER_ONLY mode ok.";
    return ErrorCode::OK;
  }
}

ErrorCode TransitController::EnableSpeedOnlyMode() {
  if (driving_mode() == Chassis::COMPLETE_AUTO_DRIVE ||
      driving_mode() == Chassis::AUTO_SPEED_ONLY) {
    set_driving_mode(Chassis::AUTO_SPEED_ONLY);
    AINFO << "Already in AUTO_SPEED_ONLY mode";
    return ErrorCode::OK;
  }
  adc_motioncontrol1_10_->
    set_adc_cmd_autonomyrequest(
    Adc_motioncontrol1_10::ADC_CMD_AUTONOMYREQUEST_AUTONOMY_REQUESTED);
  adc_motioncontrol1_10_->
    set_adc_cmd_steeringcontrolmode(
    Adc_motioncontrol1_10::ADC_CMD_STEERINGCONTROLMODE_NONE);
  adc_motioncontrol1_10_->
    set_adc_cmd_longitudinalcontrolmode(
    Adc_motioncontrol1_10::
    ADC_CMD_LONGITUDINALCONTROLMODE_DIRECT_THROTTLE_BRAKE);
  can_sender_->Update();
  if (CheckResponse(CHECK_RESPONSE_SPEED_UNIT_FLAG, true) == false) {
    AERROR << "Failed to switch to AUTO_STEER_ONLY mode.";
    Emergency();
    set_chassis_error_code(Chassis::CHASSIS_ERROR);
    return ErrorCode::CANBUS_ERROR;
  } else {
    set_driving_mode(Chassis::AUTO_SPEED_ONLY);
    AINFO << "Switch to AUTO_SPEED_ONLY mode ok.";
    return ErrorCode::OK;
  }
}

// NEUTRAL, REVERSE, DRIVE
void TransitController::Gear(Chassis::GearPosition gear_position) {
  if (!(driving_mode() == Chassis::COMPLETE_AUTO_DRIVE ||
        driving_mode() == Chassis::AUTO_SPEED_ONLY)) {
    AINFO << "this drive mode no need to set gear.";
    return;
  }
  switch (gear_position) {
    case Chassis::GEAR_NEUTRAL: {
      adc_motioncontrol1_10_->set_adc_cmd_gear(
        Adc_motioncontrol1_10::ADC_CMD_GEAR_N_NEUTRAL);
      break;
    }
    case Chassis::GEAR_REVERSE: {
      adc_motioncontrol1_10_->set_adc_cmd_gear(
        Adc_motioncontrol1_10::ADC_CMD_GEAR_R_REVERSE);
      break;
    }
    case Chassis::GEAR_DRIVE: {
      adc_motioncontrol1_10_->set_adc_cmd_gear(
        Adc_motioncontrol1_10::ADC_CMD_GEAR_D_DRIVE);
      break;
    }
    case Chassis::GEAR_PARKING: {
      adc_motioncontrol1_10_->set_adc_cmd_gear(
        Adc_motioncontrol1_10::ADC_CMD_GEAR_P_PARK);
      break;
    }
    case Chassis::GEAR_LOW: {
      adc_motioncontrol1_10_->set_adc_cmd_gear(
        Adc_motioncontrol1_10::ADC_CMD_GEAR_D_DRIVE);
      break;
    }
    case Chassis::GEAR_NONE: {
      adc_motioncontrol1_10_->set_adc_cmd_gear(
        Adc_motioncontrol1_10::ADC_CMD_GEAR_N_NEUTRAL);
      break;
    }
    case Chassis::GEAR_INVALID: {
      AERROR << "Gear command is invalid!";
      adc_motioncontrol1_10_->set_adc_cmd_gear(
        Adc_motioncontrol1_10::ADC_CMD_GEAR_N_NEUTRAL);
      break;
    }
    default: {
      adc_motioncontrol1_10_->set_adc_cmd_gear(
        Adc_motioncontrol1_10::ADC_CMD_GEAR_N_NEUTRAL);
      break;
    }
  }
  return;
}

// brake with new acceleration
// acceleration:0.00~99.99, unit:
// acceleration:0.0 ~ 7.0, unit:m/s^2
// acceleration_spd:60 ~ 100, suggest: 90
// -> pedal
void TransitController::Brake(double pedal) {
  // double real_value = params_.max_acc() * acceleration / 100;
  // TODO(QiL) :  Update brake value based on mode
  if (!(driving_mode() == Chassis::COMPLETE_AUTO_DRIVE ||
        driving_mode() == Chassis::AUTO_SPEED_ONLY)) {
    AINFO << "The current drive mode does not need to set acceleration.";
    return;
  }
  adc_motioncontrol1_10_->set_adc_cmd_brakepressure(pedal);
}

// drive with old acceleration
// gas:0.00~99.99 unit:
void TransitController::Throttle(double pedal) {
  if (!(driving_mode() == Chassis::COMPLETE_AUTO_DRIVE ||
        driving_mode() == Chassis::AUTO_SPEED_ONLY)) {
    AINFO << "The current drive mode does not need to set acceleration.";
    return;
  }
  adc_motioncontrol1_10_->set_adc_cmd_throttleposition(pedal);
}

// transit default, -470 ~ 470, left:+, right:-
// need to be compatible with control module, so reverse
// steering with old angle speed
// angle:-99.99~0.00~99.99, unit:, left:-, right:+
void TransitController::Steer(double angle) {
  if (!(driving_mode() == Chassis::COMPLETE_AUTO_DRIVE ||
        driving_mode() == Chassis::AUTO_STEER_ONLY)) {
    AINFO << "The current driving mode does not need to set steer.";
    return;
  }
  const double real_angle = vehicle_params_.max_steer_angle() * angle / 100.0;
  adc_motioncontrol1_10_->set_adc_cmd_steerwheelangle(real_angle);
  adc_motioncontrollimits1_12_->set_adc_cmd_steeringrate(200);
}

// steering with new angle speed
// angle:-99.99~0.00~99.99, unit:, left:-, right:+
// angle_spd:0.00~99.99, unit:deg/s
void TransitController::Steer(double angle, double angle_spd) {
  if (!(driving_mode() == Chassis::COMPLETE_AUTO_DRIVE ||
        driving_mode() == Chassis::AUTO_STEER_ONLY)) {
    AINFO << "The current driving mode does not need to set steer.";
    return;
  }
  const double real_angle = vehicle_params_.max_steer_angle() * angle / 100.0;
  const double real_angle_spd =
      ProtocolData<::apollo::canbus::ChassisDetail>::BoundedValue(
          vehicle_params_.min_steer_angle_rate() / M_PI * 180,
          vehicle_params_.max_steer_angle_rate() / M_PI * 180,
          vehicle_params_.max_steer_angle_rate() / M_PI * 180 * angle_spd /
              100.0);
  adc_motioncontrol1_10_->set_adc_cmd_steerwheelangle(real_angle);
  adc_motioncontrollimits1_12_->set_adc_cmd_steeringrate(real_angle_spd);
}

void TransitController::SetEpbBreak(const ControlCommand& command) {
  if (command.parking_brake()) {
    adc_motioncontrol1_10_->set_adc_cmd_parkingbrake(true);
  } else {
    adc_motioncontrol1_10_->set_adc_cmd_parkingbrake(false);
  }
}

void TransitController::SetBeam(const ControlCommand& command) {
  if (command.signal().high_beam()) {
    adc_auxiliarycontrol_110_->set_adc_cmd_highbeam(true);
  } else if (command.signal().low_beam()) {
    adc_auxiliarycontrol_110_->set_adc_cmd_lowbeam(true);
  } else {
    adc_auxiliarycontrol_110_->set_adc_cmd_highbeam(false);
    adc_auxiliarycontrol_110_->set_adc_cmd_lowbeam(false);
  }
}

void TransitController::SetHorn(const ControlCommand& command) {
  if (command.signal().horn()) {
    adc_auxiliarycontrol_110_->set_adc_cmd_horn(true);
  } else {
    adc_auxiliarycontrol_110_->set_adc_cmd_horn(false);
  }
}

void TransitController::SetTurningSignal(const ControlCommand& command) {
  // Set Turn Signal
  auto signal = command.signal().turn_signal();
  if (signal == common::VehicleSignal::TURN_LEFT) {
    adc_auxiliarycontrol_110_->set_adc_cmd_turnsignal(
      Adc_auxiliarycontrol_110::ADC_CMD_TURNSIGNAL_LEFT);
  } else if (signal == common::VehicleSignal::TURN_RIGHT) {
    adc_auxiliarycontrol_110_->set_adc_cmd_turnsignal(
      Adc_auxiliarycontrol_110::ADC_CMD_TURNSIGNAL_RIGHT);
  } else {
    adc_auxiliarycontrol_110_->set_adc_cmd_turnsignal(
      Adc_auxiliarycontrol_110::ADC_CMD_TURNSIGNAL_NONE);
  }
}

void TransitController::ResetProtocol() {
  message_manager_->ResetSendMessages();
}

bool TransitController::CheckChassisError() {
  // steer fault
  ChassisDetail chassis_detail;
  message_manager_->GetSensorData(&chassis_detail);

  int32_t error_cnt = 0;
  int32_t chassis_error_mask = 0;
  if (!chassis_detail.has_eps()) {
    AERROR_EVERY(100) << "ChassisDetail has NO eps."
                      << chassis_detail.DebugString();
    return false;
  }
  bool steer_fault = chassis_detail.eps().watchdog_fault() |
                     chassis_detail.eps().channel_1_fault() |
                     chassis_detail.eps().channel_2_fault() |
                     chassis_detail.eps().calibration_fault() |
                     chassis_detail.eps().connector_fault();

  chassis_error_mask |=
      ((chassis_detail.eps().watchdog_fault()) << (error_cnt++));
  chassis_error_mask |=
      ((chassis_detail.eps().channel_1_fault()) << (error_cnt++));
  chassis_error_mask |=
      ((chassis_detail.eps().channel_2_fault()) << (error_cnt++));
  chassis_error_mask |=
      ((chassis_detail.eps().calibration_fault()) << (error_cnt++));
  chassis_error_mask |=
      ((chassis_detail.eps().connector_fault()) << (error_cnt++));

  if (!chassis_detail.has_brake()) {
    AERROR_EVERY(100) << "ChassisDetail has NO brake."
                      << chassis_detail.DebugString();
    return false;
  }
  // brake fault
  bool brake_fault = chassis_detail.brake().watchdog_fault() |
                     chassis_detail.brake().channel_1_fault() |
                     chassis_detail.brake().channel_2_fault() |
                     chassis_detail.brake().boo_fault() |
                     chassis_detail.brake().connector_fault();

  chassis_error_mask |=
      ((chassis_detail.brake().watchdog_fault()) << (error_cnt++));
  chassis_error_mask |=
      ((chassis_detail.brake().channel_1_fault()) << (error_cnt++));
  chassis_error_mask |=
      ((chassis_detail.brake().channel_2_fault()) << (error_cnt++));
  chassis_error_mask |= ((chassis_detail.brake().boo_fault()) << (error_cnt++));
  chassis_error_mask |=
      ((chassis_detail.brake().connector_fault()) << (error_cnt++));

  if (!chassis_detail.has_gas()) {
    AERROR_EVERY(100) << "ChassisDetail has NO gas."
                      << chassis_detail.DebugString();
    return false;
  }
  // throttle fault
  bool throttle_fault = chassis_detail.gas().watchdog_fault() |
                        chassis_detail.gas().channel_1_fault() |
                        chassis_detail.gas().channel_2_fault() |
                        chassis_detail.gas().connector_fault();

  chassis_error_mask |=
      ((chassis_detail.gas().watchdog_fault()) << (error_cnt++));
  chassis_error_mask |=
      ((chassis_detail.gas().channel_1_fault()) << (error_cnt++));
  chassis_error_mask |=
      ((chassis_detail.gas().channel_2_fault()) << (error_cnt++));
  chassis_error_mask |=
      ((chassis_detail.gas().connector_fault()) << (error_cnt++));

  if (!chassis_detail.has_gear()) {
    AERROR_EVERY(100) << "ChassisDetail has NO gear."
                      << chassis_detail.DebugString();
    return false;
  }
  // gear fault
  bool gear_fault = chassis_detail.gear().canbus_fault();

  chassis_error_mask |=
      ((chassis_detail.gear().canbus_fault()) << (error_cnt++));

  set_chassis_error_mask(chassis_error_mask);

  if (steer_fault) {
    AERROR_EVERY(100) << "Steering fault detected: "
                      << chassis_detail.eps().watchdog_fault() << ", "
                      << chassis_detail.eps().channel_1_fault() << ", "
                      << chassis_detail.eps().channel_2_fault() << ", "
                      << chassis_detail.eps().calibration_fault() << ", "
                      << chassis_detail.eps().connector_fault();
  }

  if (brake_fault) {
    AERROR_EVERY(100) << "Brake fault detected: "
                      << chassis_detail.brake().watchdog_fault() << ", "
                      << chassis_detail.brake().channel_1_fault() << ", "
                      << chassis_detail.brake().channel_2_fault() << ", "
                      << chassis_detail.brake().boo_fault() << ", "
                      << chassis_detail.brake().connector_fault();
  }

  if (throttle_fault) {
    AERROR_EVERY(100) << "Throttle fault detected: "
                      << chassis_detail.gas().watchdog_fault() << ", "
                      << chassis_detail.gas().channel_1_fault() << ", "
                      << chassis_detail.gas().channel_2_fault() << ", "
                      << chassis_detail.gas().connector_fault();
  }

  if (gear_fault) {
    AERROR_EVERY(100) << "Gear fault detected: "
                      << chassis_detail.gear().canbus_fault();
  }

  if (steer_fault || brake_fault || throttle_fault) {
    return true;
  }

  return false;
}

void TransitController::SecurityDogThreadFunc() {
  int32_t vertical_ctrl_fail = 0;
  int32_t horizontal_ctrl_fail = 0;

  if (can_sender_ == nullptr) {
    AERROR << "Fail to run SecurityDogThreadFunc() because can_sender_ is "
              "nullptr.";
    return;
  }
  while (!can_sender_->IsRunning()) {
    std::this_thread::yield();
  }

  std::chrono::duration<double, std::micro> default_period{50000};
  int64_t start = 0;
  int64_t end = 0;
  while (can_sender_->IsRunning()) {
    start = ::apollo::common::time::AsInt64<::apollo::common::time::micros>(
        ::apollo::common::time::Clock::Now());
    const Chassis::DrivingMode mode = driving_mode();
    bool emergency_mode = false;

    // 1. horizontal control check
    if ((mode == Chassis::COMPLETE_AUTO_DRIVE ||
         mode == Chassis::AUTO_STEER_ONLY) &&
        CheckResponse(CHECK_RESPONSE_STEER_UNIT_FLAG, false) == false) {
      ++horizontal_ctrl_fail;
      if (horizontal_ctrl_fail >= kMaxFailAttempt) {
        emergency_mode = true;
        set_chassis_error_code(Chassis::MANUAL_INTERVENTION);
      }
    } else {
      horizontal_ctrl_fail = 0;
    }

    // 2. vertical control check
    if ((mode == Chassis::COMPLETE_AUTO_DRIVE ||
         mode == Chassis::AUTO_SPEED_ONLY) &&
        CheckResponse(CHECK_RESPONSE_SPEED_UNIT_FLAG, false) == false) {
      ++vertical_ctrl_fail;
      if (vertical_ctrl_fail >= kMaxFailAttempt) {
        emergency_mode = true;
        set_chassis_error_code(Chassis::MANUAL_INTERVENTION);
      }
    } else {
      vertical_ctrl_fail = 0;
    }
    if (CheckChassisError()) {
      set_chassis_error_code(Chassis::CHASSIS_ERROR);
      emergency_mode = true;
    }

    if (emergency_mode && mode != Chassis::EMERGENCY_MODE) {
      set_driving_mode(Chassis::EMERGENCY_MODE);
      message_manager_->ResetSendMessages();
    }
    end = ::apollo::common::time::AsInt64<::apollo::common::time::micros>(
        ::apollo::common::time::Clock::Now());
    std::chrono::duration<double, std::micro> elapsed{end - start};
    if (elapsed < default_period) {
      std::this_thread::sleep_for(default_period - elapsed);
    } else {
      AERROR
          << "Too much time consumption in TransitController looping process:"
          << elapsed.count();
    }
  }
}

bool TransitController::CheckResponse(const int32_t flags, bool need_wait) {
  return true;
}

void TransitController::set_chassis_error_mask(const int32_t mask) {
  std::lock_guard<std::mutex> lock(chassis_mask_mutex_);
  chassis_error_mask_ = mask;
}

int32_t TransitController::chassis_error_mask() {
  std::lock_guard<std::mutex> lock(chassis_mask_mutex_);
  return chassis_error_mask_;
}

Chassis::ErrorCode TransitController::chassis_error_code() {
  std::lock_guard<std::mutex> lock(chassis_error_code_mutex_);
  return chassis_error_code_;
}

void TransitController::set_chassis_error_code(
    const Chassis::ErrorCode& error_code) {
  std::lock_guard<std::mutex> lock(chassis_error_code_mutex_);
  chassis_error_code_ = error_code;
}

bool TransitController::CheckSafetyError(
    const ::apollo::canbus::ChassisDetail &chassis_detail) {
  return true;
}

}  // namespace transit
}  // namespace canbus
}  // namespace apollo
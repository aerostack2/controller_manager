/*!*******************************************************************************************
 *  \file       controller_manager.cpp
 *  \brief      controller_manager main file
 *  \authors    Miguel Fernández Cortizas
 *              Pedro Arias Pérez
 *              David Pérez Saura
 *              Rafael Pérez Seguí
 *
 *  \copyright  Copyright (c) 2022 Universidad Politécnica de Madrid
 *              All Rights Reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ********************************************************************************/

#include "controller_manager/controller_handler.hpp"
#include <as2_core/utils/tf_utils.hpp>

static inline bool checkMatchWithMask(const uint8_t mode1,
                                      const uint8_t mode2,
                                      const uint8_t mask) {
  return (mode1 & mask) == (mode2 & mask);
}

static uint8_t findBestMatchWithMask(const uint8_t mode,
                                     const std::vector<uint8_t> &mode_list,
                                     const uint8_t mask) {
  uint8_t best_match = 0;
  for (const auto &candidate : mode_list) {
    if (checkMatchWithMask(mode, candidate, mask)) {
      best_match = candidate;
      if (candidate == mode) {
        return candidate;
      }
    }
  }
  return best_match;
}

ControllerHandler::ControllerHandler(
    std::shared_ptr<controller_plugin_base::ControllerBase> controller,
    as2::Node *node)
    : controller_ptr_(controller), node_ptr_(node), tf_handler_(node) {
  /* node_ptr_->declare_parameter<bool>("use_bypass", true);
  node_ptr_->declare_parameter<std::string>("odom_frame_id", "odom");
  node_ptr_->declare_parameter<std::string>("base_frame_id", "base_link"); */

  node_ptr_->get_parameter("use_bypass", use_bypass_);
  node_ptr_->get_parameter("odom_frame_id", enu_frame_id_);
  node_ptr_->get_parameter("base_frame_id", flu_frame_id_);

  enu_frame_id_ = as2::tf::generateTfName(node_ptr_, enu_frame_id_);
  flu_frame_id_ = as2::tf::generateTfName(node_ptr_, flu_frame_id_);

  input_pose_frame_id_  = as2::tf::generateTfName(node_ptr_, input_pose_frame_id_);
  input_twist_frame_id_ = as2::tf::generateTfName(node_ptr_, input_twist_frame_id_);

  output_pose_frame_id_  = as2::tf::generateTfName(node_ptr_, output_pose_frame_id_);
  output_twist_frame_id_ = as2::tf::generateTfName(node_ptr_, output_twist_frame_id_);

  ref_pose_sub_ = node_ptr_->create_subscription<geometry_msgs::msg::PoseStamped>(
      as2_names::topics::motion_reference::pose, as2_names::topics::motion_reference::qos,
      std::bind(&ControllerHandler::ref_pose_callback, this, std::placeholders::_1));
  ref_twist_sub_ = node_ptr_->create_subscription<geometry_msgs::msg::TwistStamped>(
      as2_names::topics::motion_reference::twist, as2_names::topics::motion_reference::qos,
      std::bind(&ControllerHandler::ref_twist_callback, this, std::placeholders::_1));
  ref_traj_sub_ = node_ptr_->create_subscription<trajectory_msgs::msg::JointTrajectoryPoint>(
      as2_names::topics::motion_reference::trajectory, as2_names::topics::motion_reference::qos,
      std::bind(&ControllerHandler::ref_traj_callback, this, std::placeholders::_1));
  platform_info_sub_ = node_ptr_->create_subscription<as2_msgs::msg::PlatformInfo>(
      as2_names::topics::platform::info, as2_names::topics::platform::qos,
      std::bind(&ControllerHandler::platform_info_callback, this, std::placeholders::_1));

  twist_sub_ = node_ptr_->create_subscription<geometry_msgs::msg::TwistStamped>(
      as2_names::topics::self_localization::twist, as2_names::topics::self_localization::qos,
      std::bind(&ControllerHandler::state_callback, this, std::placeholders::_1));

  set_control_mode_client_ =
      std::make_shared<as2::SynchronousServiceClient<as2_msgs::srv::SetControlMode>>(
          as2_names::services::platform::set_platform_control_mode, node_ptr_);

  list_control_modes_client_ =
      std::make_shared<as2::SynchronousServiceClient<as2_msgs::srv::ListControlModes>>(
          as2_names::services::platform::list_control_modes, node_ptr_);

  pose_pub_ = node_ptr_->create_publisher<geometry_msgs::msg::PoseStamped>(
      as2_names::topics::actuator_command::pose, as2_names::topics::actuator_command::qos);
  twist_pub_ = node_ptr_->create_publisher<geometry_msgs::msg::TwistStamped>(
      as2_names::topics::actuator_command::twist, as2_names::topics::actuator_command::qos);
  thrust_pub_ = node_ptr_->create_publisher<as2_msgs::msg::Thrust>(
      as2_names::topics::actuator_command::thrust, as2_names::topics::actuator_command::qos);

  static auto parameters_callback_handle_ = node_ptr_->add_on_set_parameters_callback(
      std::bind(&ControllerHandler::parametersCallback, this, std::placeholders::_1));

  using namespace std::chrono_literals;
  // FIXME: Hardcoded timer period
  control_timer_ =
      node_ptr_->create_timer(10ms, std::bind(&ControllerHandler::control_timer_callback, this));

  set_control_mode_srv_ = node_ptr_->create_service<as2_msgs::srv::SetControlMode>(
      as2_names::services::controller::set_control_mode,
      std::bind(&ControllerHandler::setControlModeSrvCall, this,
                std::placeholders::_1,  // Corresponds to the 'request'  input
                std::placeholders::_2   // Corresponds to the 'response' input
                ));

  control_mode_in_.control_mode  = as2_msgs::msg::ControlMode::UNSET;
  control_mode_out_.control_mode = as2_msgs::msg::ControlMode::UNSET;
}

rcl_interfaces::msg::SetParametersResult ControllerHandler::parametersCallback(
    const std::vector<rclcpp::Parameter> &parameters) {
  rcl_interfaces::msg::SetParametersResult result;
  result.successful = true;
  result.reason     = "success";
  std::vector<std::string> changed_parameters(parameters.size());
  for (auto &param : parameters) {
    changed_parameters.push_back(param.get_name());
  }
  if (!controller_ptr_->updateParams(changed_parameters)) {
    result.successful = false;
    result.reason     = "Failed to update controller parameters";
  }
  return result;
}

void ControllerHandler::reset() {
  controller_ptr_->reset();
  last_time_                 = node_ptr_->now();
  state_adquired_            = false;
  motion_reference_adquired_ = false;
}

bool ControllerHandler::convertPoseStamped(const std::string &frame_id,
                                           geometry_msgs::msg::PoseStamped &pose) {
  if (frame_id == "" || pose.header.frame_id == "") {
    RCLCPP_ERROR(node_ptr_->get_logger(), "Could not convert from frame %s to frame %s",
                 pose.header.frame_id.c_str(), frame_id.c_str());
    return false;
  } else if (frame_id == pose.header.frame_id) {
    return true;
  }

  try {
    pose = tf_handler_.convert(pose, frame_id);
    return true;
  } catch (tf2::TransformException &ex) {
    RCLCPP_WARN(node_ptr_->get_logger(), "Could not get transform: %s", ex.what());
    return false;
  }
  return false;
}

bool ControllerHandler::convertTwistStamped(const std::string &frame_id,
                                            geometry_msgs::msg::TwistStamped &twist) {
  if (frame_id == "" || twist.header.frame_id == "") {
    RCLCPP_ERROR(node_ptr_->get_logger(), "Could not convert from frame %s to frame %s",
                 twist.header.frame_id.c_str(), frame_id.c_str());
    return false;
  } else if (frame_id == twist.header.frame_id) {
    return true;
  }

  try {
    twist = tf_handler_.convert(twist, frame_id);
    return true;
  } catch (tf2::TransformException &ex) {
    RCLCPP_WARN(node_ptr_->get_logger(), "Could not get transform: %s", ex.what());
    return false;
  }
  return false;
}

void ControllerHandler::state_callback(const geometry_msgs::msg::TwistStamped::SharedPtr msg) {
  if (!control_mode_established_) {
    return;
  }

  geometry_msgs::msg::PoseStamped pose_msg;
  geometry_msgs::msg::TwistStamped twist_msg = *msg;
  if (!convertTwistStamped(input_twist_frame_id_, twist_msg)) {
    return;
  }
  try {
    pose_msg = tf_handler_.getPoseStamped(input_pose_frame_id_, flu_frame_id_,
                                          tf2_ros::fromMsg(twist_msg.header.stamp));
  } catch (tf2::TransformException &ex) {
    RCLCPP_WARN(node_ptr_->get_logger(), "Could not get state pose transform: %s", ex.what());
    return;
  }
  state_adquired_ = true;
  state_pose_     = pose_msg;
  state_twist_    = twist_msg;
  if (!bypass_controller_) controller_ptr_->updateState(state_pose_, state_twist_);
}

void ControllerHandler::ref_pose_callback(const geometry_msgs::msg::PoseStamped::SharedPtr msg) {
  if (!control_mode_established_ ||
      control_mode_in_.control_mode == as2_msgs::msg::ControlMode::HOVER) {
    return;
  }

  geometry_msgs::msg::PoseStamped pose_msg = *msg;
  if (!convertPoseStamped(input_pose_frame_id_, pose_msg)) {
    return;
  }
  ref_pose_                  = pose_msg;
  motion_reference_adquired_ = true;

  if (!bypass_controller_) controller_ptr_->updateReference(ref_pose_);
}

void ControllerHandler::ref_twist_callback(const geometry_msgs::msg::TwistStamped::SharedPtr msg) {
  if (!control_mode_established_ ||
      control_mode_in_.control_mode == as2_msgs::msg::ControlMode::HOVER) {
    return;
  }

  geometry_msgs::msg::TwistStamped twist_msg = *msg;
  if (!convertTwistStamped(input_twist_frame_id_, twist_msg)) {
    return;
  }
  ref_twist_                 = twist_msg;
  motion_reference_adquired_ = true;

  if (!bypass_controller_) controller_ptr_->updateReference(ref_twist_);
}

void ControllerHandler::ref_traj_callback(
    const trajectory_msgs::msg::JointTrajectoryPoint::SharedPtr msg) {
  if (!control_mode_established_) {
    return;
  }

  // TODO: FORCE TRAJECTORY TO BE IN ODOM FRAME
  motion_reference_adquired_ = true;
  ref_traj_                  = *msg;
  if (!bypass_controller_) controller_ptr_->updateReference(ref_traj_);
}

void ControllerHandler::platform_info_callback(const as2_msgs::msg::PlatformInfo::SharedPtr msg) {
  platform_info_ = *msg;
}

void ControllerHandler::control_timer_callback() {
  if (!platform_info_.offboard || !platform_info_.armed) {
    return;
  }

  if (!control_mode_established_) {
    auto &clock = *node_ptr_->get_clock();
    RCLCPP_INFO_THROTTLE(node_ptr_->get_logger(), clock, 10000, "Control mode not established");
    return;
  }

  if (!state_adquired_) {
    auto &clock = *node_ptr_->get_clock();
    RCLCPP_INFO_THROTTLE(node_ptr_->get_logger(), clock, 1000, "Waiting for odometry ");

    return;
  }

  sendCommand();
};

bool ControllerHandler::setPlatformControlMode(const as2_msgs::msg::ControlMode &mode) {
  as2_msgs::srv::SetControlMode::Request set_control_mode_req;
  as2_msgs::srv::SetControlMode::Response set_control_mode_resp;
  set_control_mode_req.control_mode = mode;
  auto out = set_control_mode_client_->sendRequest(set_control_mode_req, set_control_mode_resp);
  if (out && set_control_mode_resp.success) return true;
  return false;
};

bool ControllerHandler::listPlatformAvailableControlModes() {
  if (platform_available_modes_in_.empty()) {
    RCLCPP_DEBUG(node_ptr_->get_logger(), "LISTING AVAILABLE MODES");
    // if the list is empty, send a request to the platform to get the list of
    // available modes
    as2_msgs::srv::ListControlModes::Request list_control_modes_req;
    as2_msgs::srv::ListControlModes::Response list_control_modes_resp;

    bool out =
        list_control_modes_client_->sendRequest(list_control_modes_req, list_control_modes_resp);
    if (!out) {
      RCLCPP_ERROR(node_ptr_->get_logger(), "Error listing control_modes");
      return false;
    }
    if (list_control_modes_resp.control_modes.empty()) {
      RCLCPP_ERROR(node_ptr_->get_logger(), "No available control modes");
      return false;
    }

    // log the available modes
    for (auto &mode : list_control_modes_resp.control_modes) {
      RCLCPP_DEBUG(node_ptr_->get_logger(), "Available mode: %s",
                   as2::control_mode::controlModeToString(mode).c_str());
    }

    platform_available_modes_in_ = list_control_modes_resp.control_modes;
  }
  return true;
}

bool ControllerHandler::tryToBypassController(const uint8_t input_mode, uint8_t &output_mode) {
  // check if platform available modes are set
  if ((input_mode & MATCH_MODE) == UNSET_MODE_MASK ||
      (input_mode & MATCH_MODE) == HOVER_MODE_MASK) {
    return false;
  }

  uint8_t candidate_mode =
      findBestMatchWithMask(input_mode, platform_available_modes_in_, MATCH_MODE_AND_YAW);
  if (candidate_mode) {
    output_mode = candidate_mode;
    return true;
  }
  return false;
}

bool ControllerHandler::checkSuitabilityInputMode(uint8_t &input_mode, const uint8_t output_mode) {
  // check if input_conversion is in the list of available modes
  bool mode_found = false;
  for (auto &mode : controller_available_modes_in_) {
    if ((input_mode & MATCH_MODE) == HOVER_MODE_MASK && (input_mode & MATCH_MODE) == mode) {
      mode_found = true;
      return true;
    } else if (mode == input_mode) {
      input_mode = mode;
      mode_found = true;
      break;
    }
  }

  // if not match, try to match only control mode and yaw mode
  if (!mode_found) {
    for (auto &mode : controller_available_modes_in_) {
      if (checkMatchWithMask(mode, input_mode, MATCH_MODE_AND_YAW)) {
        input_mode = mode;
        mode_found = true;
        break;
      }
    }
  }

  // check if the input mode is compatible with the output mode
  if ((input_mode & MATCH_MODE) < (output_mode & 0b1111000)) {
    RCLCPP_ERROR(node_ptr_->get_logger(),
                 "Input control mode has lower level than output control mode");
    return false;
  }

  return mode_found;
}

bool ControllerHandler::findSuitableOutputControlModeForPlatformInputMode(
    uint8_t &output_mode,
    const uint8_t input_mode) {
  //  check if the prefered mode is available
  if (prefered_output_mode_) {
    auto match = findBestMatchWithMask(prefered_output_mode_, platform_available_modes_in_,
                                       MATCH_MODE_AND_YAW);
    if (match) {
      output_mode = match;
      return true;
    }
  }

  // if the prefered mode is not available, search for the first common mode

  uint8_t common_mode = 0;
  bool same_yaw       = false;

  for (auto &mode_out : controller_available_modes_out_) {
    // skip unset modes and hover
    if ((mode_out & MATCH_MODE) == UNSET_MODE_MASK || (mode_out & MATCH_MODE) == HOVER_MODE_MASK) {
      continue;
    }
    common_mode = findBestMatchWithMask(mode_out, platform_available_modes_in_, MATCH_MODE_AND_YAW);
    if (common_mode) {
      break;
    }
  }

  // check if the common mode exist
  if (common_mode == 0) {
    return false;
  }
  output_mode = common_mode;
  return true;
}

bool ControllerHandler::findSuitableControlModes(uint8_t &input_mode, uint8_t &output_mode) {
  // check if the input mode is available. Get the best output mode
  bool success = findSuitableOutputControlModeForPlatformInputMode(output_mode, input_mode);
  if (!success) {
    RCLCPP_WARN(node_ptr_->get_logger(), "No suitable output control mode found");
    return false;
  }

  // Get the best input mode for the output mode
  success = checkSuitabilityInputMode(input_mode, output_mode);
  if (!success) {
    RCLCPP_ERROR(node_ptr_->get_logger(), "Input control mode is not suitable for this controller");
    return false;
  }
  return success;
}

std::string ControllerHandler::getFrameIdByReferenceFrame(uint8_t reference_frame) {
  switch (reference_frame) {
    case as2_msgs::msg::ControlMode::LOCAL_ENU_FRAME:
      return enu_frame_id_;
    case as2_msgs::msg::ControlMode::BODY_FLU_FRAME:
      return flu_frame_id_;
    case as2_msgs::msg::ControlMode::GLOBAL_LAT_LONG_ASML:
      return "not_implemented";
    case as2_msgs::msg::ControlMode::UNDEFINED_FRAME:
    default:
      return "undefined";
  }
}

void ControllerHandler::setControlModeSrvCall(
    const as2_msgs::srv::SetControlMode::Request::SharedPtr request,
    as2_msgs::srv::SetControlMode::Response::SharedPtr response) {
  uint8_t _control_mode_plugin_in  = 0;
  uint8_t _control_mode_plugin_out = 0;

  as2_msgs::msg::ControlMode _control_mode_msg_plugin_in;
  as2_msgs::msg::ControlMode _control_mode_msg_plugin_out;

  control_mode_established_ = false;

  // check if platform_available_modes is set
  if (!listPlatformAvailableControlModes()) {
    response->success = false;
    return;
  }

  // If the input mode is Hover, set desired control mode in to Hover,
  // else, set desired control mode in to the request one
  if (request->control_mode.control_mode == as2_msgs::msg::ControlMode::HOVER) {
    _control_mode_plugin_in = HOVER_MODE_MASK;
  } else {
    _control_mode_plugin_in =
        as2::control_mode::convertAS2ControlModeToUint8t(request->control_mode);
  }

  // Check if a bypass is possible for the input_control_mode_desired ( DISCARDING REFERENCE
  // COMPONENT)
  bypass_controller_ = false;
  if (use_bypass_) {
    bypass_controller_ = tryToBypassController(_control_mode_plugin_in, _control_mode_plugin_out);
  }

  if (bypass_controller_) {
    _control_mode_plugin_in = _control_mode_plugin_out;

    RCLCPP_INFO(node_ptr_->get_logger(), "Bypassing controller");
    _control_mode_plugin_in = UNSET_MODE_MASK;
  } else {
    bool success = findSuitableControlModes(_control_mode_plugin_in, _control_mode_plugin_out);

    if (!success) {
      RCLCPP_ERROR(node_ptr_->get_logger(), "No suitable control mode found");
      response->success = false;
      return;
    }
  }

  // request the out mode to the platform
  _control_mode_msg_plugin_out =
      as2::control_mode::convertUint8tToAS2ControlMode(_control_mode_plugin_out);
  if (!setPlatformControlMode(_control_mode_msg_plugin_out)) {
    RCLCPP_ERROR(node_ptr_->get_logger(), "Failed to set platform control mode");
    response->success = false;
    return;
  }

  // request the input and output modes to the platform
  _control_mode_msg_plugin_in =
      as2::control_mode::convertUint8tToAS2ControlMode(_control_mode_plugin_in);
  if (!controller_ptr_->setMode(_control_mode_msg_plugin_in, _control_mode_msg_plugin_out)) {
    RCLCPP_ERROR(node_ptr_->get_logger(), "Failed to set plugin control mode");
    response->success = false;
    return;
  }
  control_mode_established_ = true;

  control_mode_out_ = _control_mode_msg_plugin_out;
  if (bypass_controller_) {
    control_mode_in_ = _control_mode_msg_plugin_out;
  } else {
    control_mode_in_ = _control_mode_msg_plugin_in;
  }

  // set frames id
  output_pose_frame_id_  = getFrameIdByReferenceFrame(control_mode_out_.reference_frame);
  output_twist_frame_id_ = getFrameIdByReferenceFrame(control_mode_out_.reference_frame);
  if (bypass_controller_) {
    input_pose_frame_id_  = output_pose_frame_id_;
    input_twist_frame_id_ = output_twist_frame_id_;
  } else {
    input_pose_frame_id_ =
        as2::tf::generateTfName(node_ptr_, controller_ptr_->getDesiredPoseFrameId());
    input_twist_frame_id_ =
        as2::tf::generateTfName(node_ptr_, controller_ptr_->getDesiredTwistFrameId());
  }

  RCLCPP_INFO(node_ptr_->get_logger(), "input_mode:[%s]",
              as2::control_mode::controlModeToString(control_mode_in_).c_str());
  RCLCPP_INFO(node_ptr_->get_logger(), "output_mode:[%s]",
              as2::control_mode::controlModeToString(control_mode_out_).c_str());

  RCLCPP_INFO(node_ptr_->get_logger(), "input_pose_frame_id:[%s]", input_pose_frame_id_.c_str());
  RCLCPP_INFO(node_ptr_->get_logger(), "input_twist_frame_id:[%s]", input_twist_frame_id_.c_str());

  RCLCPP_INFO(node_ptr_->get_logger(), "output_pose_frame_id:[%s]", output_pose_frame_id_.c_str());
  RCLCPP_INFO(node_ptr_->get_logger(), "output_twist_frame_id:[%s]",
              output_twist_frame_id_.c_str());

  reset();
  response->success = true;
  return;
}

void ControllerHandler::sendCommand() {
  if (bypass_controller_) {
    if (!motion_reference_adquired_) {
      auto &clock = *node_ptr_->get_clock();
      RCLCPP_INFO_THROTTLE(node_ptr_->get_logger(), clock, 1000, "Waiting for motion reference");
      return;
    }
    command_pose_  = ref_pose_;
    command_twist_ = ref_twist_;
  } else {
    rclcpp::Time current_time = node_ptr_->now();
    double dt                 = (current_time - last_time_).nanoseconds() / 1.0e9;
    if (dt <= 0) {
      auto &clk = *node_ptr_->get_clock();
      RCLCPP_WARN_THROTTLE(node_ptr_->get_logger(), clk, 1000,
                           "Loop delta time is zero or below. Check your clock");
      return;
    }

    last_time_ = current_time;
    if (!controller_ptr_->computeOutput(dt, command_pose_, command_twist_, command_thrust_)) {
      return;
    }
  }
  publishCommand();
  return;
}

void ControllerHandler::publishCommand() {
  command_pose_.header.stamp  = node_ptr_->now();
  command_twist_.header.stamp = command_pose_.header.stamp;

  if (control_mode_out_.control_mode == as2_msgs::msg::ControlMode::POSITION ||
      control_mode_out_.control_mode == as2_msgs::msg::ControlMode::SPEED_IN_A_PLANE ||
      control_mode_out_.control_mode == as2_msgs::msg::ControlMode::TRAJECTORY ||
      control_mode_out_.control_mode == as2_msgs::msg::ControlMode::ATTITUDE) {
    if (!convertPoseStamped(output_pose_frame_id_, command_pose_)) {
      RCLCPP_ERROR(node_ptr_->get_logger(),
                   "Failed to convert command pose to output frame, from %s to %s",
                   command_pose_.header.frame_id.c_str(), output_pose_frame_id_.c_str());
      return;
    }
  }

  if (control_mode_out_.control_mode == as2_msgs::msg::ControlMode::SPEED ||
      control_mode_out_.control_mode == as2_msgs::msg::ControlMode::SPEED_IN_A_PLANE ||
      control_mode_out_.control_mode == as2_msgs::msg::ControlMode::TRAJECTORY ||
      control_mode_out_.control_mode == as2_msgs::msg::ControlMode::ACRO) {
    if (!convertTwistStamped(output_twist_frame_id_, command_twist_)) {
      RCLCPP_ERROR(node_ptr_->get_logger(),
                   "Failed to convert command twist to output frame, from %s to %s",
                   command_twist_.header.frame_id.c_str(), output_twist_frame_id_.c_str());
      return;
    }
  }

  switch (control_mode_out_.control_mode) {
    case as2_msgs::msg::ControlMode::POSITION:
      pose_pub_->publish(command_pose_);
      break;
    case as2_msgs::msg::ControlMode::SPEED:
      twist_pub_->publish(command_twist_);
      break;
    case as2_msgs::msg::ControlMode::SPEED_IN_A_PLANE:
    case as2_msgs::msg::ControlMode::TRAJECTORY:
      pose_pub_->publish(command_pose_);
      twist_pub_->publish(command_twist_);
      break;
    case as2_msgs::msg::ControlMode::ATTITUDE:
      command_thrust_.header = command_pose_.header;
      pose_pub_->publish(command_pose_);
      thrust_pub_->publish(command_thrust_);
      break;
    case as2_msgs::msg::ControlMode::ACRO:
      command_thrust_.header = command_pose_.header;
      twist_pub_->publish(command_twist_);
      thrust_pub_->publish(command_thrust_);
      break;
  }
}
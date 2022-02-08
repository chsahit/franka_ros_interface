/***************************************************************************

*
* @package: franka_ros_controllers
* @metapackage: franka_ros_interface
* @author: Saif Sidhik <sxs1412@bham.ac.uk>
*

**************************************************************************/

/***************************************************************************
* Copyright (c) 2019-2020, Saif Sidhik.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
**************************************************************************/
#include <franka_ros_controllers/velocity_cartesian_damping_controller.h>

#include <cmath>

#include <controller_interface/controller_base.h>
#include <hardware_interface/hardware_interface.h>
#include <hardware_interface/joint_command_interface.h>
#include <pluginlib/class_list_macros.h>
#include <ros/ros.h>

#include "pseudo_inversion.h"

namespace franka_ros_controllers {

bool VelocityCartesianDampingController::init(hardware_interface::RobotHW* robot_hardware,
                                          ros::NodeHandle& node_handle) {

  std::string arm_id;
  if (!node_handle.getParam("/robot_config/arm_id", arm_id)) {
    ROS_ERROR("VelocityCartesianDampingController: Could not read parameter arm_id");
  }
  desired_joints_subscriber_ = node_handle.subscribe(
      "/franka_ros_interface/motion_controller/arm/joint_commands", 20, &VelocityCartesianDampingController::jointVelCmdCallback, this,
      ros::TransportHints().reliable().tcpNoDelay());

  velocity_joint_interface_ = robot_hardware->get<hardware_interface::VelocityJointInterface>();
  if (velocity_joint_interface_ == nullptr) {
    ROS_ERROR(
        "VelocityCartesianDampingController: Error getting velocity joint interface from hardware!");
    return false;
  }

  franka_state_interface_ = robot_hardware->get<franka_hw::FrankaStateInterface>();
  if (franka_state_interface_  == nullptr) {
    ROS_ERROR(
        "VelocityCartesianDampingContrller: Could not get Franka State Interface from hardware");
    return false;
  }

  if (!node_handle.getParam("/robot_config/joint_names", joint_limits_.joint_names)) {
    ROS_ERROR("VelocityCartesianDampingController: Could not parse joint names");
  }
  if (joint_limits_.joint_names.size() != 7) {
    ROS_ERROR_STREAM("VelocityCartesianDampingController: Wrong number of joint names, got "
                     << joint_limits_.joint_names.size() << " instead of 7 names!");
    return false;
  }
  std::map<std::string, double> vel_limit_map;
  if (!node_handle.getParam("/robot_config/joint_config/joint_velocity_limit", vel_limit_map) ) {
  ROS_ERROR(
      "VelocityCartesianDampingController: Joint limits parameters not provided, aborting "
      "controller init!");
  return false;
      }
  

  for (size_t i = 0; i < joint_limits_.joint_names.size(); ++i){
    if (vel_limit_map.find(joint_limits_.joint_names[i]) != vel_limit_map.end())
      {
        joint_limits_.velocity.push_back(vel_limit_map[joint_limits_.joint_names[i]]);
      }
      else
      {
        ROS_ERROR("VelocityCartesianDampingController: Unable to find lower velocity limit values for joint %s...",
                       joint_limits_.joint_names[i].c_str());
      }
  }  

  velocity_joint_handles_.resize(7);
  for (size_t i = 0; i < 7; ++i) {
    try {
      velocity_joint_handles_[i] = velocity_joint_interface_->getHandle(joint_limits_.joint_names[i]);
    } catch (const hardware_interface::HardwareInterfaceException& e) {
      ROS_ERROR_STREAM(
          "VelocityCartesianDampingController: Exception getting joint handles: " << e.what());
      return false;
    }
  }

  try {
    franka_state_handle_ = std::make_unique<franka_hw::FrankaStateHandle>(
        franka_state_interface_->getHandle(arm_id + "_robot"));
  } catch (const hardware_interface::HardwareInterfaceException& ex) {
    ROS_ERROR_STREAM("VelocityCartesianDampingController: Exception getting Franka state handle: " << ex.what());
    return false;
  }

  auto* model_interface = robot_hardware->get<franka_hw::FrankaModelInterface>();
  if (model_interface == nullptr) {
    ROS_ERROR_STREAM(
        "VelocityCartesianDampingController: Error getting model interface from hardware");
    return false;
  }
  try {
    model_handle_ = std::make_unique<franka_hw::FrankaModelHandle>(
        model_interface->getHandle(arm_id + "_model"));
  } catch (hardware_interface::HardwareInterfaceException &ex) {
    ROS_ERROR_STREAM(
        "VelocityCartesianDampingController: Exception getting model handle from interface: "
        << ex.what());
    return false;
  }


  double controller_state_publish_rate(30.0);
  if (!node_handle.getParam("controller_state_publish_rate", controller_state_publish_rate)) {
    ROS_INFO_STREAM("VelocityCartesianDampingController: Did not find controller_state_publish_rate. Using default "
                    << controller_state_publish_rate << " [Hz].");
  }
  trigger_publish_ = franka_hw::TriggerRate(controller_state_publish_rate);

  dynamic_reconfigure_joint_controller_params_node_ =
      ros::NodeHandle("/franka_ros_interface/velocity_cartesian_damping_controller/arm/controller_parameters_config");

  dynamic_server_joint_controller_params_ = std::make_unique<
      dynamic_reconfigure::Server<franka_ros_controllers::joint_controller_paramsConfig>>(
      dynamic_reconfigure_joint_controller_params_node_);

  dynamic_server_joint_controller_params_->setCallback(
      boost::bind(&VelocityCartesianDampingController::jointControllerParamCallback, this, _1, _2));

  publisher_controller_states_.init(node_handle, "/franka_ros_interface/motion_controller/arm/joint_controller_states", 1);

  {
    std::lock_guard<realtime_tools::RealtimePublisher<franka_core_msgs::JointControllerStates> > lock(
        publisher_controller_states_);
    publisher_controller_states_.msg_.controller_name = "velocity_cartesian_damping_controller";
    publisher_controller_states_.msg_.names.resize(joint_limits_.joint_names.size());
    publisher_controller_states_.msg_.joint_controller_states.resize(joint_limits_.joint_names.size());

  }

  return true;
}

void VelocityCartesianDampingController::starting(const ros::Time& /* time */) {
  for (size_t i = 0; i < 7; ++i) {
    initial_vel_[i] = velocity_joint_handles_[i].getVelocity();
  }
  vel_d_ = initial_vel_;
  prev_d_ = vel_d_;
}

void VelocityCartesianDampingController::update(const ros::Time& time,
                                            const ros::Duration& period) {
 
  franka::RobotState initial_state = franka_state_handle_->getRobotState();
  auto forces = initial_state.O_F_ext_hat_K;
  Eigen::Map<Eigen::Matrix<double, 6, 1>> forces_eigen(forces.data());
  forces_eigen *= -1;
  std::array<double, 42> jacobian_array = model_handle_->getZeroJacobian(franka::Frame::kEndEffector);
  Eigen::Map<Eigen::Matrix<double, 6, 7>> jacobian(jacobian_array.data());
  // this should only be done one time in the callback
  Eigen::Map<Eigen::Matrix<double, 6, 1>> cartesian_desired(cartesian_target_.data());
  Eigen::Array<double, 6, 1> compliances;
  compliances << 600.0, 600.0, 100.0, 600.0, 600.0, 600.0;
  auto compliant_cartesian_desired = cartesian_desired + (compliances.cwiseInverse() * forces_eigen.array()).matrix();
  /*if (cartesian_desired[2] > 0.03 || cartesian_desired[2] < -0.03) { 
	  ROS_INFO_STREAM("proposed cartesian_vel " << compliant_cartesian_desired[0] << " " << compliant_cartesian_desired[1] << " " << compliant_cartesian_desired[2]);
	  ROS_INFO_STREAM("z force: " << forces[2]);
  }*/
  Eigen::MatrixXd jacobian_pinv;
  pseudoInverse(jacobian, jacobian_pinv);
  auto vel_d_target_eigen_ = jacobian_pinv * compliant_cartesian_desired;
  for (size_t i = 0; i < 7; ++i) {
    vel_d_target_[i] = vel_d_target_eigen_[i];
  }

  for (size_t i = 0; i < 7; ++i) {
    velocity_joint_handles_[i].setCommand(vel_d_[i]);
  }
  double filter_val = filter_joint_vel_ * filter_factor_;
  for (size_t i = 0; i < 7; ++i) {
    prev_d_[i] = velocity_joint_handles_[i].getVelocity();
    vel_d_[i] = filter_val * vel_d_target_[i] + (1.0 - filter_val) * vel_d_[i];
  }

  if (trigger_publish_() && publisher_controller_states_.trylock()) {
    for (size_t i = 0; i < 7; ++i){

      publisher_controller_states_.msg_.joint_controller_states[i].set_point = vel_d_target_[i];
      publisher_controller_states_.msg_.joint_controller_states[i].process_value = vel_d_[i];
      publisher_controller_states_.msg_.joint_controller_states[i].time_step = period.toSec();

      publisher_controller_states_.msg_.joint_controller_states[i].header.stamp = time;

    }

    publisher_controller_states_.unlockAndPublish();        
  }

  // update parameters changed online either through dynamic reconfigure or through the interactive
  // target by filtering
  filter_joint_vel_ = param_change_filter_ * target_filter_joint_vel_ + (1.0 - param_change_filter_) * filter_joint_vel_;

}

bool VelocityCartesianDampingController::checkVelocityLimits(std::vector<double> velocities)
{
  // bool retval = true;
  for (size_t i = 0;  i < 7; ++i){
    if (!(abs(velocities[i]) <= joint_limits_.velocity[i])){
      return true;
    }
  }

  return false;
}

void VelocityCartesianDampingController::jointVelCmdCallback(const franka_core_msgs::JointCommandConstPtr& msg) {

    if (msg->mode == franka_core_msgs::JointCommand::VELOCITY_MODE){
      if (msg->velocity.size() != 6) {
        ROS_ERROR_STREAM(
            "VelocityCartesianDampingController: Published Commands are not of size 6");
        vel_d_ = prev_d_;
        vel_d_target_ = prev_d_;
      }
      /**
      else if (checkVelocityLimits(msg->velocity)) {
         ROS_ERROR_STREAM(
            "VelocityCartesianDampingController: Commanded velocities are beyond allowed velocity limits.");
        vel_d_ = prev_d_;
        vel_d_target_ = prev_d_;

      }**/
      else
      {
        std::copy_n(msg->velocity.begin(), 6, cartesian_target_.begin());
	ROS_INFO_STREAM("desired cartesian velocity x: " << cartesian_target_[0]);
      }
      
    }
}

void VelocityCartesianDampingController::jointControllerParamCallback(franka_ros_controllers::joint_controller_paramsConfig& config,
                               uint32_t level){
  target_filter_joint_vel_ = config.velocity_joint_delta_filter;
}

void VelocityCartesianDampingController::stopping(const ros::Time& /*time*/) {
  // WARNING: DO NOT SEND ZERO VELOCITIES HERE AS IN CASE OF ABORTING DURING MOTION
  // A JUMP TO ZERO WILL BE COMMANDED PUTTING HIGH LOADS ON THE ROBOT. LET THE DEFAULT
  // BUILT-IN STOPPING BEHAVIOR SLOW DOWN THE ROBOT.
}

}  // namespace franka_ros_controllers

PLUGINLIB_EXPORT_CLASS(franka_ros_controllers::VelocityCartesianDampingController,
                       controller_interface::ControllerBase)

/**
 * @file tool_goal_pose.cpp
 * @brief This defines a cost function for tool goal pose.
 *
 * @author Jorge Nicho
 * @date June 2, 2016
 * @version TODO
 * @bug No known bugs
 *
 * @copyright Copyright (c) 2016, Southwest Research Institute
 *
 * @par License
 * Software License Agreement (Apache License)
 * @par
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * @par
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <math.h>
#include <stomp_plugins/cost_functions/tool_goal_pose.h>
#include <XmlRpcException.h>
#include <pluginlib/class_list_macros.h>
#include <ros/console.h>

PLUGINLIB_EXPORT_CLASS(stomp_moveit::cost_functions::ToolGoalPose,stomp_moveit::cost_functions::StompCostFunction);

static const int CARTESIAN_DOF_SIZE = 6;
static const double DEFAULT_POS_TOLERANCE = 0.001;
static const double DEFAULT_ROT_TOLERANCE = 0.01;
static const double POS_MAX_ERROR_RATIO = 10.0;
static const double ROT_MAX_ERROR_RATIO = 10.0;

namespace stomp_moveit
{
namespace cost_functions
{

ToolGoalPose::ToolGoalPose():
    name_("ToolGoalPose")
{
  // TODO Auto-generated constructor stub

}

ToolGoalPose::~ToolGoalPose()
{
  // TODO Auto-generated destructor stub
}

bool ToolGoalPose::initialize(moveit::core::RobotModelConstPtr robot_model_ptr,
                        const std::string& group_name,XmlRpc::XmlRpcValue& config)
{
  group_name_ = group_name;
  robot_model_ = robot_model_ptr;
  ik_solver_.reset(new utils::kinematics::IKSolver(robot_model_ptr,group_name));

  return configure(config);
}

bool ToolGoalPose::configure(const XmlRpc::XmlRpcValue& config)
{
  using namespace XmlRpc;

  try
  {
    XmlRpcValue params = config;

    position_cost_weight_ = static_cast<double>(params["position_cost_weight"]);
    orientation_cost_weight_ = static_cast<double>(params["orientation_cost_weight"]);

    // total weight
    cost_weight_ = position_cost_weight_ + orientation_cost_weight_;

  }
  catch(XmlRpc::XmlRpcException& e)
  {
    ROS_ERROR("%s failed to load parameters, %s",getName().c_str(),e.getMessage().c_str());
    return false;
  }

  return true;
}

bool ToolGoalPose::setMotionPlanRequest(const planning_scene::PlanningSceneConstPtr& planning_scene,
                 const moveit_msgs::MotionPlanRequest &req,
                 const stomp_core::StompConfiguration &config,
                 moveit_msgs::MoveItErrorCodes& error_code)
{
  using namespace Eigen;
  using namespace moveit::core;

  const JointModelGroup* joint_group = robot_model_->getJointModelGroup(group_name_);
  int num_joints = joint_group->getActiveJointModels().size();
  tool_link_ = joint_group->getLinkModelNames().back();
  state_.reset(new RobotState(robot_model_));
  robotStateMsgToRobotState(req.start_state,*state_);

  const std::vector<moveit_msgs::Constraints>& goals = req.goal_constraints;
  if(goals.empty())
  {
    ROS_ERROR("A goal constraint was not provided");
    error_code.val = error_code.INVALID_GOAL_CONSTRAINTS;
    return false;
  }

  // storing tool goal pose
  bool found_goal = false;
  for(const auto& g: goals)
  {

    if(utils::kinematics::validateCartesianConstraints(g))
    {
      // tool cartesian goal data
      state_->updateLinkTransforms();
      Eigen::Affine3d start_tool_pose = state_->getGlobalLinkTransform(tool_link_);
      moveit_msgs::Constraints cartesian_constraints = utils::kinematics::constructCartesianConstraints(g,start_tool_pose);
      std::vector<double> tolerance;
      found_goal = utils::kinematics::decodeCartesianConstraint(cartesian_constraints,tool_goal_pose_,tolerance);
      break;
    }


    if(!found_goal)
    {
      ROS_WARN("%s a cartesian goal pose in MotionPlanRequest was not provided,calculating it from FK",getName().c_str());

      // check joint constraints
      if(g.joint_constraints.empty())
      {
        ROS_ERROR_STREAM("No joint values for the goal were found");
        error_code.val = error_code.INVALID_GOAL_CONSTRAINTS;
        return false;
      }

      // compute FK to obtain tool pose
      const std::vector<moveit_msgs::JointConstraint>& joint_constraints = g.joint_constraints;

      // copying goal values into state
      for(auto& jc: joint_constraints)
      {
        state_->setVariablePosition(jc.joint_name,jc.position);
      }

      // storing tool goal pose and tolerance
      state_->update(true);
      tool_goal_pose_ = state_->getGlobalLinkTransform(tool_link_);
      tool_goal_tolerance_.resize(CARTESIAN_DOF_SIZE);
      double ptol = DEFAULT_POS_TOLERANCE;
      double rtol = DEFAULT_ROT_TOLERANCE;
      tool_goal_tolerance_ << ptol, ptol, ptol, rtol, rtol, rtol;
      found_goal = true;
      break;
    }
  }

  // setting cartesian error range
  min_twist_error_ = tool_goal_tolerance_;
  max_twist_error_.head(3) = min_twist_error_.head(3)*POS_MAX_ERROR_RATIO;
  max_twist_error_.tail(3) = min_twist_error_.tail(3)*ROT_MAX_ERROR_RATIO;
  max_twist_error_.tail(3) = (max_twist_error_.tail(3).array() > M_PI).select(M_PI,max_twist_error_.tail(3));

  return true;
}

bool ToolGoalPose::computeCosts(const Eigen::MatrixXd& parameters,
                          std::size_t start_timestep,
                          std::size_t num_timesteps,
                          int iteration_number,
                          int rollout_number,
                          Eigen::VectorXd& costs,
                          bool& validity)
{

  using namespace Eigen;
  using namespace utils::kinematics;
  validity = true;

  auto compute_scaled_error = [](VectorXd& val,VectorXd& min,VectorXd& max) -> VectorXd
  {
    VectorXd capped_val;
    capped_val = (val.array() > max.array()).select(max,val);
    capped_val = (val.array() < min.array()).select(min,val);
    auto range = max - min;
    VectorXd scaled = (capped_val - min).array()/(range.array());
    return scaled;
  };


  last_joint_pose_ = parameters.rightCols(1);
  state_->setJointGroupPositions(group_name_,last_joint_pose_);
  last_tool_pose_ = state_->getGlobalLinkTransform(tool_link_);

  // computing twist error
  Eigen::ArrayXi constrained_dof= Eigen::ArrayXi::Ones(6);
  computeTwist(last_tool_pose_,tool_goal_pose_,constrained_dof,tool_twist_error_);


  // computing relative error values
  VectorXd scaled_twist_error = compute_scaled_error(tool_twist_error_,min_twist_error_,max_twist_error_);
  double pos_error = scaled_twist_error.head(3).cwiseAbs().maxCoeff();
  double orientation_error = scaled_twist_error.tail(3).cwiseAbs().maxCoeff();

  // computing cost of last point
  costs.resize(parameters.cols());
  costs.setConstant(0.0);
  costs(costs.size()-1) = pos_error*position_cost_weight_ + orientation_error * orientation_cost_weight_;

  // check if valid when twist errors are below the allowed tolerance.
  validity = (tool_twist_error_.array() <= tool_goal_tolerance_.array()).all();

  return true;
}

} /* namespace cost_functions */
} /* namespace stomp_moveit */

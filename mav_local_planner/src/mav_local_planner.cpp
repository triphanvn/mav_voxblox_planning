#include <mav_msgs/default_topics.h>
#include <mav_trajectory_generation/trajectory_sampling.h>

#include "mav_local_planner/mav_local_planner.h"

namespace mav_planning {

MavLocalPlanner::MavLocalPlanner(const ros::NodeHandle& nh,
                                 const ros::NodeHandle& nh_private)
    : nh_(nh),
      nh_private_(nh_private),
      command_publishing_spinner_(1, &command_publishing_queue_),
      planning_spinner_(1, &planning_queue_),
      verbose_(false),
      global_frame_id_("map"),
      local_frame_id_("odom"),
      mpc_prediction_horizon_(300),
      command_publishing_dt_(1.0),
      replan_dt_(1.0),
      replan_lookahead_sec_(0.1),
      avoid_collisions_(true),
      autostart_(true),
      plan_to_start_(true),
      smoother_name_("loco"),
      current_waypoint_(-1),
      path_index_(0),
      max_failures_(5),
      num_failures_(0),
      esdf_server_(nh_, nh_private_),
      loco_planner_(nh_, nh_private_) {
  // Set up some settings.
  constraints_.setParametersFromRos(nh_private_);
  esdf_server_.setTraversabilityRadius(constraints_.robot_radius);
  loco_planner_.setEsdfMap(esdf_server_.getEsdfMapPtr());
  goal_selector_.setParametersFromRos(nh_private_);
  goal_selector_.setTsdfMap(esdf_server_.getTsdfMapPtr());

  nh_private_.param("verbose", verbose_, verbose_);
  nh_private_.param("global_frame_id", global_frame_id_, global_frame_id_);
  nh_private_.param("local_frame_id", local_frame_id_, local_frame_id_);
  nh_private_.param("mpc_prediction_horizon", mpc_prediction_horizon_,
                    mpc_prediction_horizon_);
  nh_private_.param("replan_dt", replan_dt_, replan_dt_);
  nh_private_.param("replan_lookahead_sec", replan_lookahead_sec_,
                    replan_lookahead_sec_);
  nh_private_.param("command_publishing_dt", command_publishing_dt_,
                    command_publishing_dt_);
  nh_private_.param("avoid_collisions", avoid_collisions_, avoid_collisions_);
  nh_private_.param("autostart", autostart_, autostart_);
  nh_private_.param("plan_to_start", plan_to_start_, plan_to_start_);
  nh_private_.param("smoother_name", smoother_name_, smoother_name_);

  max_failures_ = std::ceil(10.0 / replan_dt_);
}

  // Publishers and subscribers.
  odometry_sub_ = nh_.subscribe(mav_msgs::default_topics::ODOMETRY, 1,
                                &MavLocalPlanner::odometryCallback, this);
  waypoint_sub_ =
      nh_.subscribe("waypoint", 1, &MavLocalPlanner::waypointCallback, this);
  waypoint_list_sub_ = nh_.subscribe(
      "waypoint_list", 1, &MavLocalPlanner::waypointListCallback, this);

  command_pub_ = nh_.advertise<trajectory_msgs::MultiDOFJointTrajectory>(
      mav_msgs::default_topics::COMMAND_TRAJECTORY, 1);

  path_marker_pub_ = nh_private_.advertise<visualization_msgs::MarkerArray>(
      "local_path", 1, true);
  full_trajectory_pub_ =
      nh_private_.advertise<trajectory_msgs::MultiDOFJointTrajectory>(
          "full_trajectory", 1, true);

  // Services.
  start_srv_ = nh_private_.advertiseService(
      "start", &MavLocalPlanner::startCallback, this);
  pause_srv_ = nh_private_.advertiseService(
      "pause", &MavLocalPlanner::pauseCallback, this);
  stop_srv_ = nh_private_.advertiseService(
      "stop", &MavLocalPlanner::stopCallback, this);

  position_hold_client_ =
      nh_.serviceClient<std_srvs::Empty>("back_to_position_hold");

  // Start the planning timer. Will no-op most cycles.
  ros::TimerOptions timer_options(
      ros::Duration(replan_dt_),
      boost::bind(&MavLocalPlanner::planningTimerCallback, this, _1),
      &planning_queue_);

  planning_timer_ = nh_.createTimer(timer_options);

  // Start the command publishing spinner.
  command_publishing_spinner_.start();
  planning_spinner_.start();

  // Set up yaw policy.
  yaw_policy_.setPhysicalConstraints(constraints_);
  yaw_policy_.setYawPolicy(YawPolicy::PolicyType::kVelocityVector);

  // Set up smoothers.
  const double voxel_size = esdf_server_.getEsdfMapPtr()->voxel_size();

  // Straight-line smoother.
  ramp_smoother_.setParametersFromRos(nh_private_);

  // Poly smoother.
  poly_smoother_.setParametersFromRos(nh_private_);
  poly_smoother_.setMinCollisionCheckResolution(voxel_size);
  poly_smoother_.setMapDistanceCallback(
      std::bind(&MavLocalPlanner::getMapDistance, this, std::placeholders::_1));
  poly_smoother_.setOptimizeTime(true);
  poly_smoother_.setSplitAtCollisions(avoid_collisions_);

  // Loco smoother!
  loco_smoother_.setParametersFromRos(nh_private_);
  loco_smoother_.setMinCollisionCheckResolution(voxel_size);
  loco_smoother_.setDistanceAndGradientFunction(
      std::bind(&MavLocalPlanner::getMapDistanceAndGradient, this,
                std::placeholders::_1, std::placeholders::_2));
  loco_smoother_.setOptimizeTime(true);
  loco_smoother_.setResampleTrajectory(true);
  loco_smoother_.setResampleVisibility(true);
  loco_smoother_.setNumSegments(5);
}

void MavLocalPlanner::odometryCallback(const nav_msgs::Odometry& msg) {
  mav_msgs::eigenOdometryFromMsg(msg, &odometry_);
}

void MavLocalPlanner::waypointCallback(const geometry_msgs::PoseStamped& msg) {
  // Plan a path from the current position to the target pose stamped.
  ROS_INFO("[Mav Local Planner] Got a waypoint!");
  // Cancel any previous trajectory on getting a new one.
  clearTrajectory();

  mav_msgs::EigenTrajectoryPoint waypoint;
  eigenTrajectoryPointFromPoseMsg(msg, &waypoint);

  waypoints_.clear();
  waypoints_.push_back(waypoint);
  current_waypoint_ = 0;
  num_failures_ = 0;

  // Execute one planning step on main thread.
  planningStep();
  startPublishingCommands();
}

void MavLocalPlanner::waypointListCallback(
    const geometry_msgs::PoseArray& msg) {
  // Plan a path from the current position to the target pose stamped.
  ROS_INFO("[Mav Local Planner] Got a list of waypoints, %zu long!",
           msg.poses.size());
  // Cancel any previous trajectory on getting a new one.
  clearTrajectory();

  waypoints_.clear();

  for (const geometry_msgs::Pose& pose : msg.poses) {
    mav_msgs::EigenTrajectoryPoint waypoint;
    eigenTrajectoryPointFromPoseMsg(pose, &waypoint);
    waypoints_.push_back(waypoint);
  }
  current_waypoint_ = 0;
  num_failures_ = 0;

  // Execute one planning step on main thread.
  planningStep();
  startPublishingCommands();
}

void MavLocalPlanner::planningTimerCallback(const ros::TimerEvent& event) {
  // Wait on the condition variable from the publishing...
  if (should_replan_.wait_for(replan_dt_)) {
    if (verbose_) {
      ROS_WARN(
          "[Mav Planning Timer] Difference between real and expected: %f Real: "
          "%f Expected: %f Now: %f",
          (event.current_real - event.current_expected).toSec(),
          event.current_real.toSec(), event.current_expected.toSec(),
          ros::Time::now().toSec());
    }

    planningStep();
  }
}

void MavLocalPlanner::planningStep() {
  ROS_INFO(
      "[Mav Local Planner][Plan Step] Waypoint index: %zd Total waypoints: %zu",
      current_waypoint_, waypoints_.size());
  if (current_waypoint_ < 0 ||
      static_cast<int>(waypoints_.size()) <= current_waypoint_) {
    // This means that we probably planned to the end of the waypoints!

    // If we're done with sending waypoints, alllll good. Just quit.
    if (path_index_ >= path_queue_.size() || path_queue_.empty()) {
      return;
    }

    // If we're done with all waypoints, all good. Just quit.
    if (current_waypoint_ >= static_cast<int>(waypoints_.size())) {
      return;
    }

    // If we're not though, we should probably double check the trajectory!
  }

  mav_trajectory_generation::timing::MiniTimer timer;

  // First, easiest case: if we're not avoiding collisions, just use the
  // favorite path smoother. We only do this on the first planning call then
  // ignore all the rest.
  if (!avoid_collisions_) {
    mav_msgs::EigenTrajectoryPointVector waypoints;
    mav_msgs::EigenTrajectoryPoint current_point;
    current_point.position_W = odometry_.position_W;
    current_point.orientation_W_B = odometry_.orientation_W_B;

    if (plan_to_start_) {
      waypoints.push_back(current_point);
    }
    waypoints.insert(waypoints.end(), waypoints_.begin(), waypoints_.end());

    mav_msgs::EigenTrajectoryPointVector path;

    if (planPathThroughWaypoints(waypoints, &path)) {
      replacePath(path);
      current_waypoint_ = waypoints_.size();
    } else {
      ROS_ERROR("[Mav Local Planner] Waypoint planning failed!");
    }
  } else if (path_queue_.empty()) {
    const double kCloseEnough = 0.05;
//    ROS_INFO_STREAM("[Mav Local Planner][Plan Step] current position: "
//        << odometry_.position_W.transpose() << ", next waypoint ["
//        << current_waypoint_ << "]: "
//        << waypoints_[current_waypoint_].position_W.transpose());
    if ((waypoints_[current_waypoint_].position_W - odometry_.position_W).norm() < kCloseEnough) {
      if (!nextWaypoint()) {
        finishWaypoints();
        return;
      }
    }

    mav_msgs::EigenTrajectoryPoint current_odometry;
    current_odometry.position_W = odometry_.position_W;
    current_odometry.orientation_W_B = odometry_.orientation_W_B;
    mav_msgs::EigenTrajectoryPointVector path;
    size_t waypoint_index;
    const mav_msgs::EigenTrajectoryPointVector waypoint_list(
        waypoints_.begin() + current_waypoint_, waypoints_.end());

    ROS_INFO_STREAM("[Mav Local Planner][Plan Step] Planning at ("
                        << current_odometry.position_W.transpose() << ") with velocity "
                        << current_odometry.velocity_W.norm() << " m/s");
    bool success = findPathThroughCurrentWaypointList(
        current_odometry, waypoint_list, 0, &path, &waypoint_index);
    ROS_INFO("[Mav Local Planner][Plan Step] "
             "Planning of waypoint list successful? %d", success);
    if (success) {
      current_waypoint_ = std::min(current_waypoint_ + waypoint_index - 1,
                                   waypoints_.size() - 1);
//      ROS_INFO("current waypoint: %zu, index %zu, max %zu",
//               current_waypoint_, waypoint_index, waypoints_.size());
      replacePath(path);
      startPublishingCommands();
    } else {
      // Give up!
      avoidCollisionsTowardWaypoint();
    }
  } else {
    const double kCloseEnough = 0.05;

    // find path index from when on to replan
    size_t replan_start_index;
    mav_msgs::EigenTrajectoryPointVector path_chunk;
    {
      std::lock_guard<std::recursive_mutex> guard(path_mutex_);

      replan_start_index = std::max(std::min(path_index_ + static_cast<size_t>((replan_lookahead_sec_) /
                                                                               constraints_.sampling_dt),
          path_queue_.size() - 1), path_index_);

      std::copy(path_queue_.begin() + replan_start_index, path_queue_.end(),
                std::back_inserter(path_chunk));
    }

    // check if we already reached the next waypoint, increase accordingly
    mav_msgs::EigenTrajectoryPoint position;
    if (path_chunk.empty()) {
      position = path_queue_.back();
    } else {
      position = path_chunk.front();
    }
    if ((waypoints_[current_waypoint_].position_W
         - position.position_W).norm() < kCloseEnough) {
      if (!nextWaypoint()) {
        finishWaypoints();
        return;
      }
    }
    ROS_INFO_STREAM("[Mav Local Planner][Plan Step] Replanning at "
        << replan_start_index << " ("
        << position.position_W.transpose() << ") with velocity "
        << position.velocity_W.norm() << " m/s");
//        << " and next waypoint at ("
//        << waypoints_[current_waypoint_].position_W.transpose() << "), "
//        << (waypoints_[current_waypoint_].position_W - position.position_W).norm()
//        << "/" << kCloseEnough << " m away");

    // replan the remaining list of waypoints
    bool planning_success;
    mav_msgs::EigenTrajectoryPointVector path;
    size_t waypoint_index;
    int waypoints_added = 0;
    mav_msgs::EigenTrajectoryPointVector waypoint_list(waypoints_.begin() + current_waypoint_, waypoints_.end());

    planning_success = findPathThroughCurrentWaypointList(
        position, waypoint_list, waypoints_added, &path, &waypoint_index);
    ROS_INFO("[Mav Local Planner][Plan Step] "
             "Replanning of waypoint list successful? %d", planning_success);

    if (!planning_success) {
      // sample chunk to add as temporary waypoints for initial guesses
      waypoint_list.clear();
      // sample path chunk
      const double interval = 2;
      double distance = 0;
      waypoint_list.emplace_back(position);
      for (size_t idx = 0; idx < path_chunk.size(); ++idx) {
        if (idx > 0) {
          distance +=
              (path_chunk[idx].position_W - path_chunk[idx - 1].position_W)
                  .norm();
        }
        if (distance > interval) {
          waypoint_list.emplace_back(path_chunk[idx]);
          distance = 0;
        }
      }
      waypoint_list.erase(waypoint_list.begin());
      if (waypoint_list.size() > 0 and
          (waypoint_list.back().position_W -
           waypoints_[current_waypoint_].position_W)
                  .norm() <= interval) {
        waypoint_list.erase(waypoint_list.end());
      }
      //    waypoint_list.emplace_back(waypoints_[current_waypoint_]);
      //    for (auto it = path_chunk.rbegin(); it != path_chunk.rend(); ++it) {
      //      if ((it->position_W - waypoint_list.front().position_W).norm () > interval) {
      //        if ((it->position_W - position.position_W).norm () > interval) {
      //          waypoint_list.insert(waypoint_list.begin(), *it);
      //        }
      //      }
      //    }
      //    waypoint_list.erase(waypoint_list.end());
      waypoints_added = waypoint_list.size();
//      ROS_INFO("[Mav Local Planner][Plan Step] added %d temporary waypoints",
//               waypoints_added);
      // append waypoints_
      waypoint_list.insert(waypoint_list.end(),
                           waypoints_.begin() + current_waypoint_,
                           waypoints_.end());

      planning_success = findPathThroughCurrentWaypointList(
          position, waypoint_list, waypoints_added, &path, &waypoint_index);
      ROS_INFO("[Mav Local Planner][Plan Step] "
               "replanning with existing path successful? %d",
               planning_success);
    }

    // replace path
    if (planning_success) {
      num_failures_ = 0;
      current_waypoint_ = std::min(current_waypoint_ + waypoint_index - waypoints_added - 1,
                                   waypoints_.size() - 1);
      ROS_INFO("current waypoint: %zu, index %zu, added %d, max %zu",
          current_waypoint_, waypoint_index, waypoints_added, waypoints_.size());

      // prepare new trajectory
      const int64_t kDtNs =
          mav_msgs::secondsToNanoseconds(constraints_.sampling_dt);
      retimeTrajectoryWithStartTimeAndDt(
          position.time_from_start_ns, kDtNs, &path);

      path.front().orientation_W_B = position.orientation_W_B;
      yaw_policy_.applyPolicyInPlace(&path);

      // Remove what was in the trajectory before.
      if (replan_start_index < path_queue_.size()) {
        path_queue_.erase(path_queue_.begin() + replan_start_index,
                          path_queue_.end());
      }
      // Stick the new one in.
      path_queue_.insert(path_queue_.end(), path.begin(), path.end());
    } else {
      bool path_chunk_collision_free =
          isPathCollisionFree(path_chunk) && !path_chunk.empty();
      if (path_chunk_collision_free and
          path_index_ < replan_start_index /*and path_chunk.size() > 1*/) {
        ROS_INFO(
            "[Mav Local Planner][Plan Step] Path chunk (%zd) collision free, "
            "continuing existing solution.", path_chunk.size());
      } else {
        // Give up!
        if (path_chunk_collision_free) {
          if (path_chunk.size() <= 1) {
            ROS_WARN("[Mav Local Planner][Plan Step] Path chunk (%zd) collision free, "
                     "but too short. Tracking next waypoint.",
                     path_chunk.size());
          }
          if (path_index_ < replan_start_index) {
            ROS_WARN("[Mav Local Planner][Plan Step] Reached the end of the existing path, "
                     "tracking next waypoint.");
          }
        } else {
          ROS_WARN(
              "[Mav Local Planner][Plan Step] Path chunk (%zd) in collision, "
              "tracking next waypoint.",
              path_chunk.size());
        }
        avoidCollisionsTowardWaypoint();
      }
    }
  }

  ROS_INFO("[Mav Local Planner][Plan Step] Planning finished. Time taken: %f",
           timer.stop());
  visualizePath();
}

bool MavLocalPlanner::findPathThroughCurrentWaypointList(
    const mav_msgs::EigenTrajectoryPoint& start,
    const mav_msgs::EigenTrajectoryPointVector& waypoint_list,
    const size_t& temporary_waypoints,
    mav_msgs::EigenTrajectoryPointVector* path,
    size_t* waypoint_index) {

  if (waypoint_list.empty()) {
    return false;
  }

  // First check how many waypoints we haven't covered yet are in free space.
  mav_msgs::EigenTrajectoryPointVector free_waypoints;

  // If the path doesn't ALREADY start near the start, the first waypoint
  // should be the current pose.
  int waypoints_added = 0;
  *waypoint_index = 0;
  constexpr double kCloseToOdometry = 0.1;
  if (plan_to_start_ &&
      (start.position_W - waypoint_list.front().position_W).norm() >
      kCloseToOdometry) {
    free_waypoints.push_back(start);
    waypoints_added = 1;
//    ROS_INFO_STREAM("[Mav Local Planner][Plan List] added a waypoint at ("
//        << start.position_W.transpose() << ")");
  }

  // todo(gasserl): start loop at current waypoint
//  for (ulong waypoint_idx = current_waypoint_; waypoint_idx < waypoints_.size(); waypoint_idx++) {
//  for (const mav_msgs::EigenTrajectoryPoint& waypoint : waypoint_list) {
  Eigen::Vector3d occupied_point;
  bool occupied_point_exists = false;
  int ignored_temporary_waypoints = 0;
  for (ulong idx = 0; idx < waypoint_list.size(); ++idx) {
    const mav_msgs::EigenTrajectoryPoint& waypoint = waypoint_list[idx];
    Eigen::Vector3d pos = waypoint.position_W;
    double dist = getMapDistance(pos);
    double weight = 0;
    esdf_server_ptr_->getTsdfMapPtr()->getWeightAtPosition(pos, &weight);
    if (getMapDistance(waypoint.position_W) < constraints_.robot_radius) {
      if (waypoints_added == 1 and idx == 0) {
        if (weight < 1e-6) {
          ROS_WARN("[Mav Local Planner][Plan List] Start position unknown, "
                   "assumed as free");
          ROS_WARN("[Mav Local Planner][Plan List] (%.2f %.2f %.2f) - "
                   "distance %.3f / %.2f, weight %.3f",
                   pos.x(), pos.y(), pos.z(), dist, constraints_.robot_radius,
                   weight);
        } else {
          ROS_WARN("[Mav Local Planner][Plan List] Start position occupied");
          ROS_WARN("[Mav Local Planner][Plan List] (%.2f %.2f %.2f) - "
                   "distance %.3f / %.2f, weight %.3f",
                   pos.x(), pos.y(), pos.z(), dist, constraints_.robot_radius,
                   weight);
          ROS_ERROR("START <-> ODOMETRY: %.3f/%.3f",
              (odometry_.position_W - waypoint.position_W).norm(),
              kCloseToOdometry);
          if ((odometry_.position_W - waypoint.position_W).norm() < kCloseToOdometry) {
            ROS_WARN("[Mav Local Planner][Plan List] Odometry occupied, ignored");
          } else {
            return false;
          }
        }
      } else if (idx < temporary_waypoints) {
        ++ignored_temporary_waypoints;
        continue;
      } else {
        occupied_point = waypoint.position_W;
        occupied_point_exists = true;
        break;
      }
    }
    free_waypoints.push_back(waypoint);
  }
//  ROS_INFO("waypoint %zu/%zu, list of %zu, ignored %d, added %d, free %zu",
//    current_waypoint_, waypoints_.size() - 1, waypoint_list.size(),
//    ignored_temporary_waypoints, waypoints_added, free_waypoints.size());

  // visualize free waypoints
  visualization_msgs::MarkerArray marker_array;
  visualization_msgs::Marker path_marker;
  path_marker.header.frame_id = local_frame_id_;
  path_marker.header.stamp = ros::Time::now();
  path_marker.type = visualization_msgs::Marker::SPHERE_LIST;
  path_marker.ns = "waypoint_list";
  path_marker.scale.x = constraints_.robot_radius*2.5;
  path_marker.scale.y = constraints_.robot_radius*2.5;
  path_marker.scale.z = constraints_.robot_radius*2.5;
  mav_visualization::Color color_msg = mav_visualization::Color::Green();
  color_msg.a = 0.5;
  path_marker.color = color_msg;
  geometry_msgs::Point point_msg;
  for (const mav_msgs::EigenTrajectoryPoint& waypoint : free_waypoints) {
    mav_msgs::pointEigenToMsg(waypoint.position_W, &point_msg);
    path_marker.points.push_back(point_msg);
    path_marker.colors.push_back(color_msg);
  }
  marker_array.markers.push_back(path_marker);
  path_marker_pub_.publish(marker_array);

  if (occupied_point_exists) {
    marker_array.markers.clear();
    visualization_msgs::Marker path_marker2;
    path_marker2.header.frame_id = local_frame_id_;
    path_marker2.header.stamp = ros::Time::now();
    path_marker2.type = visualization_msgs::Marker::SPHERE_LIST;
    path_marker2.ns = "waypoint_list";
    path_marker2.scale.x = constraints_.robot_radius * 2.5;
    path_marker2.scale.y = constraints_.robot_radius * 2.5;
    path_marker2.scale.z = constraints_.robot_radius * 2.5;
    mav_visualization::Color color_msg2 = mav_visualization::Color::Red();
    color_msg2.a = 0.5;
    path_marker2.color = color_msg2;
    path_marker2.ns = "collision_waypoint";
    path_marker2.color = color_msg;
    mav_msgs::pointEigenToMsg(occupied_point, &point_msg);
    path_marker2.points.push_back(point_msg);
    path_marker2.colors.push_back(color_msg2);
    marker_array.markers.push_back(path_marker2);
  }
  path_marker_pub_.publish(marker_array);

//  ROS_INFO("[Mav Local Planner][Plan List] Of next %zu waypoints, %zu are free (%d were added).",
//           waypoint_list.size(), free_waypoints.size(), waypoints_added);
  bool success = false;
//  if (free_waypoints.size() <= static_cast<size_t>(waypoints_added) ||
//      free_waypoints.size() == 2) {
  if (free_waypoints.size() <= 1) {
    // Okay whatever just search for the first waypoint.
    ROS_WARN("[Mav Local Planner][Plan List] Too few free waypoints (%zu).",
        free_waypoints.size());
    success = false;
    return success;
  } else {
    // There is some hope! Maybe we can do path smoothing on these guys.
    success = planPathThroughWaypoints(free_waypoints, path);
    if (success) {
      *waypoint_index = std::min(free_waypoints.size() - waypoints_added + ignored_temporary_waypoints,
                                   waypoint_list.size());
//      ROS_INFO("waypoint index: %zu with %zu list, %d added, %d ignored, %zu max",
//          *waypoint_index, free_waypoints.size(), waypoints_added,
//               ignored_temporary_waypoints, waypoint_list.size());
//      ROS_INFO(
//          "[Mav Local Planner][Plan List] Successfully planned path through %zu free "
//          "waypoints.",
//          free_waypoints.size());
      success = isPathCollisionFree(*path);
      if (success) {
//        ROS_INFO(
//            "[Mav Local Planner][Plan List] Used smoothing through %zu waypoints! Total "
//            "waypoint size: %zu, added? %d",
//            free_waypoints.size(), waypoint_list.size(),
//            waypoints_added);
      } else {
        ROS_WARN("[Mav Local Planner][Plan List] Path was not collision free. :(");
      }
    }
  }
  return success;
}

void MavLocalPlanner::avoidCollisionsTowardWaypoint() {
  if (current_waypoint_ >= static_cast<int64_t>(waypoints_.size())) {
    return;
  }
  mav_msgs::EigenTrajectoryPoint waypoint = waypoints_[current_waypoint_];
  const double kCloseEnough = 0.05;

  const int64_t kDtNs =
      mav_msgs::secondsToNanoseconds(constraints_.sampling_dt);

//  ROS_INFO_STREAM("[Mav Local Planner][Plan Step] Current odometry: "
//                  << odometry_.position_W.transpose() << " Tracking waypoint ["
//                  << current_waypoint_
//                  << "]: " << waypoint.position_W.transpose());

  // Save success and Trajectory.
  mav_trajectory_generation::Trajectory trajectory;
  bool success = false;

  if (!path_queue_.empty()) {
    std::lock_guard<std::recursive_mutex> guard(path_mutex_);

    ROS_INFO(
        "[Mav Local Planner][Next Waypoint] Trying to avoid collisions on existing path.");
    mav_msgs::EigenTrajectoryPointVector path_chunk;
    size_t replan_start_index;
    {
      replan_start_index =
          std::max(std::min(path_index_ + static_cast<size_t>((replan_lookahead_sec_) /
                                                     constraints_.sampling_dt),
                   path_queue_.size()), path_index_);
//      ROS_INFO(
//          "[Mav Local Planner][Plan Step] Current path index: %zu Replan start "
//          "index: %zu Queue length: %zu",
//          path_index_, replan_start_index, path_queue_.size());
      // Cut out the remaining snippet of the trajectory so we can do
      // something with it.
      std::copy(path_queue_.begin() + replan_start_index, path_queue_.end(),
                std::back_inserter(path_chunk));
//      if (path_chunk.empty()) {
//        path_chunk.push_back(path_queue_.back());
//        // Check if the current waypoint is basically the replanning position.
//        if ((path_queue_[replan_start_index].position_W - waypoint.position_W).norm() <
//            kCloseEnough) {
//          if (!nextWaypoint()) {
//            finishWaypoints();
//          }
//        }
//      }
    }

    mav_msgs::EigenTrajectoryPoint current_position;
    mav_msgs::EigenTrajectoryPoint existing_goal;
    if (path_chunk.empty()) {
      current_position = path_queue_.back();
      existing_goal = path_queue_.back();
    } else {
      current_position = path_chunk.front();
      existing_goal = path_chunk.back();
    }

    bool path_chunk_collision_free = isPathCollisionFree(path_chunk) && !path_chunk.empty();
    ROS_INFO(
        "[Mav Local Planner][Next Waypoint] Existing chunk (%zd) is collision free? %d",
        path_chunk.size(), path_chunk_collision_free);
    // Check if the current path queue goes to the goal and is collision free.
    if ((existing_goal.position_W - waypoint.position_W).norm() <
        kCloseEnough) {
      // Collision check the remaining chunk of the trajectory.
      if (path_chunk_collision_free) {
        ROS_INFO(
            "[Mav Local Planner][Next Waypoint] Current plan is valid, just "
            "rollin' with it.");
//        nextWaypoint();
        return;
      }
    }
    // Otherwise we gotta replan this thing anyway.
    ROS_INFO_STREAM("[Mav Local Planner][Next Waypoint] Tracking waypoint at ("
                        << waypoint.position_W.transpose() << ") from position ("
                        << current_position.position_W.transpose() << ")");
    success = loco_planner_.getTrajectoryTowardGoal(current_position,
                                                    waypoint, &trajectory);
    ROS_INFO(
        "[Mav Local Planner][Next Waypoint] Local path to next waypoint successful? %d",
        success);
    if (!success) {
      if (path_chunk_collision_free and path_index_ < path_queue_.size() - 1) {
        ROS_WARN(
            "[Mav Local Planner][Next Waypoint] Couldn't find a solution :( "
            "Continuing existing solution.");
      } else {
        ROS_WARN(
            "[Mav Local Planner][Next Waypoint] ABORTING! No local solution "
            "found.");
        // TODO(helenol): which order to abort in?
        abort();
        dealWithFailure();
      }
      return;
    } else {
      if (trajectory.getMaxTime() <= 1e-6) {
        mav_msgs::EigenTrajectoryPointVector new_path_chunk;
        mav_trajectory_generation::sampleWholeTrajectory(
            trajectory, constraints_.sampling_dt, &new_path_chunk);

        if ((current_position.position_W - waypoint.position_W).norm() <
            kCloseEnough) {
          nextWaypoint();
        } else if (!new_path_chunk.empty() and
            (new_path_chunk.back().position_W - waypoint.position_W).norm() <
                   kCloseEnough) {
          nextWaypoint();
        } else {
          if (path_chunk_collision_free and path_index_ < path_queue_.size()) {
            ROS_WARN(
                "[Mav Local Planner][Next Waypoint] Too short solution found. "
                "Continuing existing solution.");
          } else {
            ROS_WARN(
                "[Mav Local Planner][Next Waypoint] ABORTING! Too short solution "
                "found.");
            abort();
            dealWithFailure();
          }
        }
      } else {
        num_failures_ = 0;
        ROS_INFO("[Mav Local Planner][Next Waypoint] Appending new path chunk (%zu).",
                 path_chunk.size());

        mav_msgs::EigenTrajectoryPointVector new_path_chunk;
        mav_trajectory_generation::sampleWholeTrajectory(
            trajectory, constraints_.sampling_dt, &new_path_chunk);

        retimeTrajectoryWithStartTimeAndDt(
            current_position.time_from_start_ns, kDtNs, &new_path_chunk);

        new_path_chunk.front().orientation_W_B =
            current_position.orientation_W_B;
        yaw_policy_.applyPolicyInPlace(&new_path_chunk);

        // Remove what was in the trajectory before.
        if (replan_start_index < path_queue_.size()) {
          path_queue_.erase(path_queue_.begin() + replan_start_index,
                            path_queue_.end());
        }
        // Stick the new one in.
        path_queue_.insert(path_queue_.end(), new_path_chunk.begin(),
                           new_path_chunk.end());
      }
    }
  } else {
    ROS_INFO("[Mav Local Planner][Next Waypoint] Trying to plan from scratch.");

    // There's nothing planned so far! So we plan from the current odometry.
    mav_msgs::EigenTrajectoryPoint current_point;
    current_point.position_W = odometry_.position_W;
    current_point.orientation_W_B = odometry_.orientation_W_B;

    success = loco_planner_.getTrajectoryTowardGoal(current_point, waypoint,
                                                    &trajectory);
    ROS_INFO_STREAM("[Mav Local Planner][Next Waypoint] Planning from "
        << current_point.position_W.transpose() << " to "
        << waypoint.position_W.transpose());
    ROS_INFO("[Mav Local Planner][Next Waypoint] Planning success? %d", success);

    if (success) {
      if (trajectory.getMaxTime() <= 0.1) {
        if ((waypoint.position_W - odometry_.position_W).norm() < kCloseEnough) {
          nextWaypoint();
        } else {
          ROS_WARN(
              "[Mav Local Planner][Next Waypoint] ABORTING! Too short solution "
              "found.");
          dealWithFailure();
        }
      } else {
        mav_msgs::EigenTrajectoryPointVector path;
        mav_trajectory_generation::sampleWholeTrajectory(
            trajectory, constraints_.sampling_dt, &path);
        // Copy this straight into the queue.
        num_failures_ = 0;
        replacePath(path);
        startPublishingCommands();
      }
    } else {
      dealWithFailure();
    }
  }
}

bool MavLocalPlanner::planPathThroughWaypoints(
    const mav_msgs::EigenTrajectoryPointVector& waypoints,
    mav_msgs::EigenTrajectoryPointVector* path) {
  CHECK_NOTNULL(path);
  bool success;
  if (smoother_name_ == "loco") {
    if (waypoints.size() == 2) {
      success = loco_smoother_.getPathBetweenTwoPoints(waypoints[0],
                                                       waypoints[1], path);
    } else {
      success = loco_smoother_.getPathBetweenWaypoints(waypoints, path);
    }
  } else if (smoother_name_ == "polynomial") {
    success = poly_smoother_.getPathBetweenWaypoints(waypoints, path);

  } else if (smoother_name_ == "ramp") {
    success = ramp_smoother_.getPathBetweenWaypoints(waypoints, path);
  } else {
    // Default case is ramp!
    ROS_ERROR(
        "[Mav Local Planner] Unknown smoother type %s, using ramp instead.",
        smoother_name_.c_str());
    success = ramp_smoother_.getPathBetweenWaypoints(waypoints, path);
  }
  return success;
}

bool MavLocalPlanner::nextWaypoint() {
  if (current_waypoint_ >= static_cast<int64_t>(waypoints_.size()) - 1) {
    current_waypoint_ = waypoints_.size() - 1;
    Eigen::Vector3d pos = waypoints_[current_waypoint_].position_W;
//    ROS_WARN("[Mav Local Planner] set waypoint index to %zu at (%.2f %.2f %.2f)", current_waypoint_, pos.x(), pos.y(), pos.z());
    return false;
  } else {
    current_waypoint_++;
    Eigen::Vector3d pos = waypoints_[current_waypoint_].position_W;
//    ROS_INFO("[Mav Local Planner] increasing waypoint index to "
//             "%zu at (%.2f %.2f %.2f)",
//             current_waypoint_, pos.x(), pos.y(), pos.z());
    return true;
  }
}

void MavLocalPlanner::finishWaypoints() {
  current_waypoint_ = waypoints_.size();
  ROS_INFO("[Mav Local Planner] reached the end of the waypoint list.");
}

void MavLocalPlanner::replacePath(
    const mav_msgs::EigenTrajectoryPointVector& path) {
  std::lock_guard<std::recursive_mutex> guard(path_mutex_);
  path_queue_.clear();
  path_queue_ = path;
  path_queue_.front().orientation_W_B = odometry_.orientation_W_B;
  yaw_policy_.applyPolicyInPlace(&path_queue_);
  path_index_ = 0;
}

void MavLocalPlanner::startPublishingCommands() {
  // Call the service call to takeover publishing commands.
  if (position_hold_client_.exists()) {
    std_srvs::Empty empty_call;
    position_hold_client_.call(empty_call);
  }

  // Publish the first set immediately, on this thread.
  commandPublishTimerCallback(ros::TimerEvent());

  // Need advanced timer options to assign callback queue to this timer.
  ros::TimerOptions timer_options(
      ros::Duration(command_publishing_dt_),
      boost::bind(&MavLocalPlanner::commandPublishTimerCallback, this, _1),
      &command_publishing_queue_);

  command_publishing_timer_ = nh_.createTimer(timer_options);
}

void MavLocalPlanner::commandPublishTimerCallback(
    const ros::TimerEvent& event) {
  constexpr size_t kQueueBuffer = 0;
  if (path_index_ < path_queue_.size()) {
    std::lock_guard<std::recursive_mutex> guard(path_mutex_);
    size_t number_to_publish = std::min<size_t>(
        std::floor(command_publishing_dt_ / constraints_.sampling_dt),
        path_queue_.size() - path_index_);

    size_t starting_index = 0;
    if (path_index_ != 0) {
      starting_index = path_index_ + kQueueBuffer;
      if (starting_index >= path_queue_.size()) {
        starting_index = path_index_;
      }
    }

    size_t number_to_publish_with_buffer = std::min<size_t>(
        number_to_publish + mpc_prediction_horizon_ - kQueueBuffer,
        path_queue_.size() - starting_index);

    // TODO(helenol): do this without copy! Use iterators properly!
    mav_msgs::EigenTrajectoryPointVector::const_iterator first_sample =
        path_queue_.begin() + starting_index;
    auto last_sample = first_sample + number_to_publish_with_buffer;
    mav_msgs::EigenTrajectoryPointVector trajectory_to_publish(first_sample,
                                                               last_sample);

    trajectory_msgs::MultiDOFJointTrajectory msg;
    msg.header.frame_id = local_frame_id_;
    msg.header.stamp = ros::Time::now();

    ROS_INFO(
        "[Mav Local Planner][Command Publish] Publishing %zu samples of %zu. "
        "Start index: %zu Time: %f Start position: %f Start velocity: %f End "
        "time: %f End position: %f",
        trajectory_to_publish.size(), path_queue_.size(), starting_index,
        trajectory_to_publish.front().time_from_start_ns * 1.0e-9,
        trajectory_to_publish.front().position_W.x(),
        trajectory_to_publish.front().velocity_W.x(),
        trajectory_to_publish.back().time_from_start_ns * 1.0e-9,
        trajectory_to_publish.back().position_W.x());
    mav_msgs::msgMultiDofJointTrajectoryFromEigen(trajectory_to_publish, &msg);

    command_pub_.publish(msg);
    path_index_ += number_to_publish;
    should_replan_.notify();
  }
  // Does there need to be an else????
}

void MavLocalPlanner::abort() {
  // No need to check anything on stop, just clear all the paths.
  clearTrajectory();
  // Make sure to clear the queue in the controller as well (we send about a
  // second of trajectories ahead).
  sendCurrentPose();
}

void MavLocalPlanner::clearTrajectory() {
  std::lock_guard<std::recursive_mutex> guard(path_mutex_);
  command_publishing_timer_.stop();
  path_queue_.clear();
  path_index_ = 0;
}

void MavLocalPlanner::sendCurrentPose() {
  // Sends the current pose with velocity 0 to the controller to clear the
  // controller's trajectory queue.
  // More or less an abort operation.
  mav_msgs::EigenTrajectoryPoint current_point;
  current_point.position_W = odometry_.position_W;
  current_point.orientation_W_B = odometry_.orientation_W_B;

  trajectory_msgs::MultiDOFJointTrajectory msg;
  msgMultiDofJointTrajectoryFromEigen(current_point, &msg);
  command_pub_.publish(msg);
}

bool MavLocalPlanner::startCallback(std_srvs::Empty::Request& request,
                                    std_srvs::Empty::Response& response) {
  if (path_queue_.size() <= path_index_) {
    ROS_WARN("Trying to start an empty or finished trajectory queue!");
    return false;
  }
  ROS_INFO("[Mav Local Planner] Starting to publish commands.");
  startPublishingCommands();
  return true;
}

bool MavLocalPlanner::pauseCallback(std_srvs::Empty::Request& request,
                                    std_srvs::Empty::Response& response) {
  if (path_queue_.size() <= path_index_) {
    ROS_WARN("Trying to pause an empty or finished trajectory queue!");
    return false;
  }
  command_publishing_timer_.stop();
  ROS_INFO("[Mav Local Planner] Pausing the publishing of commands.");
  return true;
}

bool MavLocalPlanner::stopCallback(std_srvs::Empty::Request& request,
                                   std_srvs::Empty::Response& response) {
  abort();
  ROS_INFO("[Mav Local Planner] Aborting.");
  return true;
}

void MavLocalPlanner::visualizePath() {
  // TODO: Split trajectory into two chunks: before and after.
  visualization_msgs::MarkerArray marker_array;
  visualization_msgs::Marker path_marker;
  {
    std::lock_guard<std::recursive_mutex> guard(path_mutex_);

    path_marker = createMarkerForPath(path_queue_, local_frame_id_,
                                      mav_visualization::Color::Black(),
                                      "local_path", 0.05);
  }
  marker_array.markers.push_back(path_marker);
  path_marker_pub_.publish(marker_array);
}

double MavLocalPlanner::getMapDistance(const Eigen::Vector3d& position) const {
  double distance = 0.0;
  const bool kInterpolate = false;
  if (!esdf_server_.getEsdfMapPtr()->getDistanceAtPosition(
          position, kInterpolate, &distance)) {
    return 0.0;
  }
  return distance;
}

double MavLocalPlanner::getMapDistanceAndGradient(
    const Eigen::Vector3d& position, Eigen::Vector3d* gradient) const {
  double distance = 0.0;
  const bool kInterpolate = false;
  if (!esdf_server_.getEsdfMapPtr()->getDistanceAndGradientAtPosition(
          position, kInterpolate, &distance, gradient)) {
    return 0.0;
  }
  return distance;
}

bool MavLocalPlanner::isPathCollisionFree(
    const mav_msgs::EigenTrajectoryPointVector& path) const {
  for (const mav_msgs::EigenTrajectoryPoint& point : path) {
    if (getMapDistance(point.position_W) < constraints_.robot_radius - 0.1) {
      double weight = 0;
      esdf_server_ptr_->getTsdfMapPtr()->getWeightAtPosition(point.position_W, &weight);
      ROS_WARN_STREAM("[Mav Local Planner][Collision Check]"
          << point.position_W.transpose() << " with dist "
          << getMapDistance(point.position_W) << "/"
          << constraints_.robot_radius << " and weight " << weight);
      return false;
    }
  }
  return true;
}

bool MavLocalPlanner::isPathFeasible(
    const mav_msgs::EigenTrajectoryPointVector& path) const {
  // This is easier to check in the trajectory but then we are limited in how
  // we do the smoothing.
  for (const mav_msgs::EigenTrajectoryPoint& point : path) {
    if (point.acceleration_W.norm() > constraints_.a_max + 1e-2) {
      return false;
    }
    if (point.velocity_W.norm() > constraints_.v_max + 1e-2) {
      return false;
    }
  }
  return true;
}

bool MavLocalPlanner::dealWithFailure() {
  ROS_INFO("[Mav Local Planning][Failed] Failure %d/%d",
      num_failures_, max_failures_);
  if (current_waypoint_ < 0) {
    ROS_WARN("[Mav Local Planning][Failed] No waypoint list being tracked.");
    return false;
  }

  constexpr double kCloseEnough = 0.05;
  mav_msgs::EigenTrajectoryPoint waypoint = waypoints_[current_waypoint_];
  mav_msgs::EigenTrajectoryPoint goal = waypoint;
  if (temporary_goal_ &&
      static_cast<int64_t>(waypoints_.size()) > current_waypoint_ + 1) {
    goal = waypoints_[current_waypoint_ + 1];
  }
  mav_msgs::EigenTrajectoryPoint current_point;
  current_point.position_W = odometry_.position_W;
  current_point.orientation_W_B = odometry_.orientation_W_B;

  mav_msgs::EigenTrajectoryPoint current_goal;
  if (!goal_selector_.selectNextGoal(goal, waypoint, current_point,
                                     &current_goal)) {
    num_failures_++;
    if (num_failures_ > max_failures_) {
      current_waypoint_ = -1;
      ROS_WARN("[Mav Local Planning][Failed] Max number of failures reached, "
               "waypoint list abandoned.");
    }
    ROS_INFO("[Mav Local Planning][Failed] Waypoint %ld/%zu, "
             "path queue of size %zu at %ld",
        current_waypoint_, waypoints_.size() -1, path_index_, path_queue_.size() -1);
    return false;
  } else {
    if ((current_goal.position_W - waypoint.position_W).norm() < kCloseEnough) {
      // Goal is unchanged. :(
      temporary_goal_ = false;
      ROS_INFO("[Mav Local Planning][Failed] goal is unchanged");
      ROS_INFO("[Mav Local Planning][Failed] waypoint %zu/%zu, path queue %zu/%zu",
               current_waypoint_, waypoints_.size() -1, path_index_, path_queue_.size() -1);
      return false;
    } else if ((current_goal.position_W - goal.position_W).norm() <
               kCloseEnough) {
      // This is just the next waypoint that we're trying to go to.
      current_waypoint_++;
      temporary_goal_ = false;
      ROS_INFO("[Mav Local Planning][Failed] we've reached a waypoint, all good");
      ROS_INFO("[Mav Local Planning][Failed] waypoint %zu/%zu, path queue %zu/%zu",
               current_waypoint_, waypoints_.size() -1, path_index_, path_queue_.size() -1);
      return true;
    } else {
      // Then this is something different!
      temporary_goal_ = true;
      waypoints_.insert(waypoints_.begin() + current_waypoint_, current_goal);
      ROS_INFO("[Mav Local Planning][Failed] inserting temporary waypoint");
      ROS_INFO("[Mav Local Planning][Failed] waypoint %zu/%zu, path queue %zu/%zu",
               current_waypoint_, waypoints_.size() -1, path_index_, path_queue_.size() -1);
      return true;
    }
  }
}

}  // namespace mav_planning

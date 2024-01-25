/*
 * multirobot_slam_toolbox
 * Copyright Work Modifications (c) 2023, Achala Athukorala
 *
 * THE WORK (AS DEFINED BELOW) IS PROVIDED UNDER THE TERMS OF THIS CREATIVE
 * COMMONS PUBLIC LICENSE ("CCPL" OR "LICENSE"). THE WORK IS PROTECTED BY
 * COPYRIGHT AND/OR OTHER APPLICABLE LAW. ANY USE OF THE WORK OTHER THAN AS
 * AUTHORIZED UNDER THIS LICENSE OR COPYRIGHT LAW IS PROHIBITED.
 *
 * BY EXERCISING ANY RIGHTS TO THE WORK PROVIDED HERE, YOU ACCEPT AND AGREE TO
 * BE BOUND BY THE TERMS OF THIS LICENSE. THE LICENSOR GRANTS YOU THE RIGHTS
 * CONTAINED HERE IN CONSIDERATION OF YOUR ACCEPTANCE OF SUCH TERMS AND
 * CONDITIONS.
 *
 */

#include "slam_toolbox/slam_toolbox_multirobot.hpp"

namespace slam_toolbox
{

/*****************************************************************************/
MultiRobotSlamToolbox::MultiRobotSlamToolbox(rclcpp::NodeOptions options)
: SlamToolbox(options), 
  localized_scan_topic_("/localized_scan"), 
  last_pose_(-1000, -1000, -1000)
/*****************************************************************************/
{
    current_ns_ = this->get_namespace() + 1;

    std::string collaboration_mode = this->declare_parameter("collaboration_mode", std::string("peer"));
    collaboration_mode = this->get_parameter("collaboration_mode").as_string();
    shared_min_travel_distance_ = this->declare_parameter("shared_minimum_travel_distance", 1.0);
    shared_min_travel_distance_ = this->get_parameter("shared_minimum_travel_distance").as_double();
    shared_min_travel_heading_ = this->declare_parameter("shared_minimum_travel_heading", 1.5);
    shared_min_travel_heading_ = this->get_parameter("shared_minimum_travel_heading").as_double();
    use_local_link_ = this->declare_parameter("use_local_link", true);
    use_local_link_ = this->get_parameter("use_local_link").as_bool();
    publish_hostbot_transform_ = this->declare_parameter("publish_hostbot_transform", true);
    publish_hostbot_transform_ = this->get_parameter("publish_hostbot_transform").as_bool();

    if (collaboration_mode == std::string("peer")) {
      RCLCPP_INFO(get_logger(), "Collaboration Mode: peer [Local mapping + Map merging]");
      collaboration_mode_ = CollaborationMode::PEER;
    } else if (collaboration_mode == std::string("publisher")) {
      RCLCPP_INFO(get_logger(), "Collaboration Mode: publisher [Local mapping only]");
      collaboration_mode_ = CollaborationMode::PUBLISHER;
    } else if (collaboration_mode == std::string("subscriber")) {
      RCLCPP_INFO(get_logger(), "Collaboration Mode: subscriber [Map merging only]");
      collaboration_mode_ = CollaborationMode::SUBSCRIBER;
    } else
    {
      RCLCPP_INFO(get_logger(), "Invalid Collaboration Mode : %s. Falling back to publisher mode [Local only]", 
        collaboration_mode.c_str());
      collaboration_mode_ = CollaborationMode::PUBLISHER;
    }

    if (use_local_link_) RCLCPP_INFO(get_logger(), "Using Local link");
    if (collaboration_mode_ == CollaborationMode::SUBSCRIBER && publish_hostbot_transform_) {
      RCLCPP_INFO(get_logger(), "Publishing hostbot transform!"
      " Make sure no one else is publishing hostbot map to odom transform");
    }

    switch (collaboration_mode_)
    {
      case CollaborationMode::PEER:
        localized_scan_pub_ = this->create_publisher<slam_toolbox::msg::LocalizedLaserScan>(
          localized_scan_topic_, 1000);
        localized_scan_sub_ = this->create_subscription<slam_toolbox::msg::LocalizedLaserScan>(
          localized_scan_topic_, 1000, std::bind(&MultiRobotSlamToolbox::localizedScanCallback, 
          this, std::placeholders::_1));
        transform_publish_timer_ = this->create_wall_timer(std::chrono::duration<double>(0.05), 
          std::bind(&MultiRobotSlamToolbox::publishTransforms, this));
        break;
      
      case CollaborationMode::PUBLISHER:
        localized_scan_pub_ = this->create_publisher<slam_toolbox::msg::LocalizedLaserScan>(
          localized_scan_topic_, 1000);
          if (use_local_link_) {
            localized_scan_pub_local_link_ = this->create_publisher<slam_toolbox::msg::LocalizedLaserScan>(
          localized_scan_topic_.substr(1) + "_local_link", 1000);
          }
        break;

      case CollaborationMode::SUBSCRIBER:
        localized_scan_sub_ = this->create_subscription<slam_toolbox::msg::LocalizedLaserScan>(
          localized_scan_topic_, 1000, std::bind(&MultiRobotSlamToolbox::localizedScanCallback, 
          this, std::placeholders::_1));
        transform_publish_timer_ = this->create_wall_timer(std::chrono::duration<double>(0.05), 
          std::bind(&MultiRobotSlamToolbox::publishTransforms, this));
        if (use_local_link_) {
            localized_scan_sub_local_link_ = this->create_subscription<slam_toolbox::msg::LocalizedLaserScan>(
          localized_scan_topic_.substr(1) + "_local_link", 1000, std::bind(&MultiRobotSlamToolbox::localizedScanCallback, 
          this, std::placeholders::_1));
          }
      
      default:;
    }
}

/*****************************************************************************/
void MultiRobotSlamToolbox::laserCallback(
  sensor_msgs::msg::LaserScan::ConstSharedPtr scan)
/*****************************************************************************/
{
  // Ignore scan callbacks in Merge only mode
  if (collaboration_mode_ == CollaborationMode::SUBSCRIBER) {
    return;
  }

  // store scan header
  scan_header = scan->header;
  // no odom info
  Pose2 pose;
  if (!pose_helper_->getOdomPose(pose, scan->header.stamp)) {
    RCLCPP_WARN(get_logger(), "Failed to compute odom pose");
    return;
  }

  // ensure the laser can be used
  LaserRangeFinder * laser = getLaser(scan);

  if (!laser) {
    RCLCPP_WARN(get_logger(), "Failed to create laser device for"
      " %s; discarding scan", scan->header.frame_id.c_str());
    return;
  }

  LocalizedRangeScan * range_scan = addScan(laser, scan, pose);
  if (range_scan != nullptr)
  {
    Matrix3 covariance;
    covariance.SetToIdentity();
    publishLocalizedScan(scan, laser->GetOffsetPose(), 
        range_scan->GetCorrectedPose(), covariance, scan->header.stamp);
  }
}

/*****************************************************************************/
void MultiRobotSlamToolbox::localizedScanCallback(
  slam_toolbox::msg::LocalizedLaserScan::ConstSharedPtr localized_scan)
{
  std::string scan_ns = localized_scan->scan.header.frame_id.substr(0, 
                          localized_scan->scan.header.frame_id.find('/'));
  if (collaboration_mode_==CollaborationMode::PEER && scan_ns == current_ns_) return; // Ignore callbacks from ourself

  sensor_msgs::msg::LaserScan::ConstSharedPtr scan = 
    std::make_shared<sensor_msgs::msg::LaserScan>(localized_scan->scan);
  Pose2 pose;
  pose.SetX(localized_scan->pose.pose.pose.position.x);
  pose.SetY(localized_scan->pose.pose.pose.position.y);
  tf2::Quaternion quat_tf;
  tf2::convert(localized_scan->pose.pose.pose.orientation, quat_tf);
  pose.SetHeading(tf2::getYaw(quat_tf));

  LaserRangeFinder * laser = getLaser(localized_scan);
  if (!laser) {
    RCLCPP_WARN(get_logger(), "Failed to create device for received localizedScanner"
      " %s; discarding scan", scan->header.frame_id.c_str());
    return;
  }
  LocalizedRangeScan * range_scan = addExternalScan(laser, scan, pose);
  
  if (range_scan != nullptr)
  {
    std::unique_lock lock(transforms_mutex_);

    // Set hostbot transform
    if (publish_hostbot_transform_ && (scan_ns == current_ns_)) {
      setTransformFromPoses(range_scan->GetCorrectedPose(), pose,
        scan->header.stamp, false);
    }

    // Set transforms
    pose = range_scan->GetCorrectedPose();
    tf2::Quaternion q(0., 0., 0., 1.0);
    geometry_msgs::msg::TransformStamped tf_msg;
    q.setRPY(0., 0., pose.GetHeading());
    tf2::Transform transform(q, tf2::Vector3(pose.GetX(), pose.GetY(), 0.0));
    tf2::toMsg(transform, tf_msg.transform);
    tf_msg.header.frame_id = map_frame_;
    tf_msg.header.stamp = localized_scan->pose.header.stamp;
    tf_msg.child_frame_id = localized_scan->scanner_offset.header.frame_id;

    // Ignore host transform when running without a namespace
    if (tf_msg.child_frame_id != "/" + base_frame_)
      transforms_[tf_msg.child_frame_id] = tf_msg;
  }
}

/*****************************************************************************/
LocalizedRangeScan * MultiRobotSlamToolbox::addExternalScan(
  LaserRangeFinder * laser,
  const sensor_msgs::msg::LaserScan::ConstSharedPtr & scan,
  Pose2 & odom_pose)
/*****************************************************************************/
{
  // get our localized range scan
  LocalizedRangeScan * range_scan = getLocalizedRangeScan(
    laser, scan, odom_pose);

  // Add the localized range scan to the smapper
  boost::mutex::scoped_lock lock(smapper_mutex_);
  bool processed = false, update_reprocessing_transform = false;

  Matrix3 covariance;
  covariance.SetToIdentity();

  if (processor_type_ == PROCESS) {
    processed = smapper_->getMapper()->Process(range_scan, &covariance);
  } else if (processor_type_ == PROCESS_FIRST_NODE) {
    processed = smapper_->getMapper()->ProcessAtDock(range_scan, &covariance);
    processor_type_ = PROCESS;
    update_reprocessing_transform = true;
  } else if (processor_type_ == PROCESS_NEAR_REGION) {
    boost::mutex::scoped_lock l(pose_mutex_);
    if (!process_near_pose_) {
      RCLCPP_ERROR(get_logger(), "Process near region called without a "
        "valid region request. Ignoring scan.");
      return nullptr;
    }
    range_scan->SetOdometricPose(*process_near_pose_);
    range_scan->SetCorrectedPose(range_scan->GetOdometricPose());
    process_near_pose_.reset(nullptr);
    processed = smapper_->getMapper()->ProcessAgainstNodesNearBy(
      range_scan, false, &covariance);
    update_reprocessing_transform = true;
    processor_type_ = PROCESS;
  } else {
    RCLCPP_FATAL(get_logger(),
      "SlamToolbox: No valid processor type set! Exiting.");
    exit(-1);
  }

  // if successfully processed, create odom to map transformation
  // and add our scan to storage
  if (processed) {
    if (enable_interactive_mode_) {
      scan_holder_->addScan(*scan);
    }
  } else {
    delete range_scan;
    range_scan = nullptr;
  }

  return range_scan;
}

/*****************************************************************************/
LaserRangeFinder * MultiRobotSlamToolbox::getLaser(
  const slam_toolbox::msg::LocalizedLaserScan::ConstSharedPtr localized_scan)
/*****************************************************************************/
{
  const std::string & frame = localized_scan->scan.header.frame_id;
  if (lasers_.find(frame) == lasers_.end()) {
    try {
      lasers_[frame] = laser_assistant_->toLaserMetadata(localized_scan->scan, 
                                            localized_scan->scanner_offset);
      dataset_->Add(lasers_[frame].getLaser(), true);
    } catch (tf2::TransformException & e) {
      RCLCPP_ERROR(get_logger(), "Failed to compute laser pose[%s], "
        "aborting initialization (%s)", frame.c_str(), e.what());
      return nullptr;
    }
  }

  return lasers_[frame].getLaser();
}

/*****************************************************************************/
void MultiRobotSlamToolbox::publishLocalizedScan( 
  const sensor_msgs::msg::LaserScan::ConstSharedPtr & scan,
  const Pose2 & offset,
  const Pose2 & pose,
  const Matrix3 & cov,
  const rclcpp::Time & t)
/*****************************************************************************/
{
  slam_toolbox::msg::LocalizedLaserScan scan_msg; 
  scan_msg.scan = *scan;

  tf2::Quaternion q_offset(0., 0., 0., 1.0);
  q_offset.setRPY(0., 0., offset.GetHeading());
  tf2::Transform scanner_offset(q_offset, tf2::Vector3(offset.GetX(), offset.GetY(), 0.0));
  tf2::toMsg(scanner_offset, scan_msg.scanner_offset.transform);
  scan_msg.scanner_offset.header.stamp = t;

  tf2::Quaternion q(0., 0., 0., 1.0);
  q.setRPY(0., 0., pose.GetHeading());
  tf2::Transform transform(q, tf2::Vector3(pose.GetX(), pose.GetY(), 0.0));
  tf2::toMsg(transform, scan_msg.pose.pose.pose);

  scan_msg.pose.pose.covariance[0] = cov(0, 0) * position_covariance_scale_;  // x
  scan_msg.pose.pose.covariance[1] = cov(0, 1) * position_covariance_scale_;  // xy
  scan_msg.pose.pose.covariance[6] = cov(1, 0) * position_covariance_scale_;  // xy
  scan_msg.pose.pose.covariance[7] = cov(1, 1) * position_covariance_scale_;  // y
  scan_msg.pose.pose.covariance[35] = cov(2, 2) * yaw_covariance_scale_;      // yaw
  scan_msg.pose.header.stamp = t;

  // Set prefixed frame names
  scan_msg.scan.header.frame_id = (*(scan->header.frame_id.cbegin()) == '/') ? 
    current_ns_ + scan->header.frame_id :
    current_ns_ + "/" + scan->header.frame_id;

  scan_msg.pose.header.frame_id = (*(map_frame_.cbegin()) == '/') ? 
    current_ns_ + map_frame_ :
    current_ns_ + "/" + map_frame_;

  scan_msg.scanner_offset.child_frame_id = scan_msg.scan.header.frame_id;

  scan_msg.scanner_offset.header.frame_id = (*(base_frame_.cbegin()) == '/') ?
    current_ns_ + base_frame_ :
    current_ns_ + "/" + base_frame_;

  if ((std::hypot(pose.GetX() - last_pose_.GetX(), 
                  pose.GetY() - last_pose_.GetY()) < shared_min_travel_distance_) &&
      (abs(pose.GetHeading() - last_pose_.GetHeading()) < shared_min_travel_heading_))
  {
    if (use_local_link_) localized_scan_pub_local_link_->publish(scan_msg);
    // Not enough displacement from last shared LocalizedScan
    return;
  }
  last_pose_ = pose;

  localized_scan_pub_->publish(scan_msg);
}

/*****************************************************************************/
void MultiRobotSlamToolbox::publishTransforms()
/*****************************************************************************/
{
  std::unique_lock lock(transforms_mutex_);
  for (auto& [frame, transform]: transforms_)
  {
    tfB_->sendTransform(transform);
  }

  if (publish_hostbot_transform_) {
    boost::mutex::scoped_lock lock(map_to_odom_mutex_);
    geometry_msgs::msg::TransformStamped msg;
    msg.transform = tf2::toMsg(map_to_odom_);
    msg.child_frame_id = odom_frame_;
    msg.header.frame_id = map_frame_;
    msg.header.stamp = this->get_clock()->now();
    tfB_->sendTransform(msg);
  }
}

/*****************************************************************************/
bool MultiRobotSlamToolbox::deserializePoseGraphCallback(
  const std::shared_ptr<rmw_request_id_t> request_header,
  const std::shared_ptr<slam_toolbox::srv::DeserializePoseGraph::Request> req,
  std::shared_ptr<slam_toolbox::srv::DeserializePoseGraph::Response> resp)
/*****************************************************************************/
{
  if (req->match_type == procType::LOCALIZE_AT_POSE) {
    RCLCPP_WARN(get_logger(), "Requested a localization deserialization "
      "in non-localization mode.");
    return false;
  }

  return SlamToolbox::deserializePoseGraphCallback(request_header, req, resp);
}

}
# Single scan from a planar laser range-finder and the pose of the robot

uint32 seq

sensor_msgs/LaserScan scan
# Scanner pose in robot base_frame
geometry_msgs/TransformStamped scanner_offset
# Robot base_frame odometric pose in a global (map) frame
geometry_msgs/PoseWithCovarianceStamped odometric_pose
# Robot base_frame pose in local (map) frame (corrected / local slam pose)
geometry_msgs/PoseWithCovarianceStamped pose

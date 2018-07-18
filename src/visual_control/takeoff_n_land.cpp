/**
 * @file offb_node.cpp
 * @brief offboard example node, written with mavros version 0.14.2, px4 flight
 * stack and tested in Gazebo SITL
 */

#include <ros/ros.h>
#include <std_msgs/String.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/PoseWithCovarianceStamped.h>
#include <geometry_msgs/PoseArray.h>
#include <geometry_msgs/TransformStamped.h>
#include <mavros_msgs/CommandBool.h>
#include <mavros_msgs/CommandTOL.h>
#include <mavros_msgs/SetMode.h>
#include <mavros_msgs/State.h>
#include <mavros_msgs/PositionTarget.h>
#include <tf/tf.h>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2_ros/static_transform_broadcaster.h>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2_ros/transform_listener.h>
#include <math.h>

#define FLIGHT_ALTITUDE 0.5
#define ROS_RATE 20.0
#define MAX_ATTEMPTS 300
#define SAFETY_TIME_SEC 3
#define TURN_STEP_RAD 4/ROS_RATE
#define INIT_FLIGHT_DURATION 4.0 //In seconds per side
#define INIT_FLIGHT_LENGTH 1.0   //In meters
#define INIT_FLIGHT_REPEAT 5     //Times
#define TEST_FLIGHT_DURATION 3.0 //In seconds per side
#define TEST_FLIGHT_LENGTH 4.0   //In meters
#define TEST_FLIGHT_REPEAT 2     //Times
#define KEEP_ALIVE false

ros::Subscriber state_sub;
ros::Subscriber marker_pos_sub;
ros::Subscriber local_pos_sub;
ros::Subscriber svo_pos_sub;

ros::Publisher setpoint_pos_pub;
ros::Publisher vision_pos_pub;
ros::Publisher svo_cmd_pub;

ros::ServiceClient arming_client;
ros::ServiceClient land_client;
ros::ServiceClient set_mode_client;

void offboardMode();
void takeOff();
void testFlightHorizontal();
void testFlightVertical();
void initVIO();
void vioOff();
void vioOn();
void turnTowardsMarker();
void approachMarker(ros::NodeHandle &nh);
void hover(int seconds);
void land();
void disarm();
float currentYaw();

bool approaching = false;
bool send_vision_estimate = true;
bool svo_running = false;
unsigned char close_enough = 0;
geometry_msgs::PoseStamped gps_init_pos;
geometry_msgs::PoseStamped setpoint_pos_ENU;
geometry_msgs::PoseStamped svo_init_pos;
geometry_msgs::PoseStamped vision_pos_ENU;
ros::Time last_request;
ros::Time last_svo_estimate;
mavros_msgs::CommandBool arm_cmd;
tf2_ros::Buffer tfBuffer;
std_msgs::String svo_cmd;

mavros_msgs::State current_state;
void state_cb(const mavros_msgs::State::ConstPtr& msg){
    current_state = *msg;
}

geometry_msgs::PoseArray marker_position;
void marker_position_cb(const geometry_msgs::PoseArray::ConstPtr& msg){
    marker_position = *msg;

    static tf2_ros::TransformBroadcaster br;

    // Transformation from drone to visual marker
    geometry_msgs::TransformStamped transformStamped;
    transformStamped.header.stamp = ros::Time::now();
    transformStamped.header.frame_id = "drone";
    transformStamped.child_frame_id = "marker";
    transformStamped.transform.translation.x = marker_position.poses[0].position.z;
    transformStamped.transform.translation.y = -marker_position.poses[0].position.x;
    transformStamped.transform.translation.z = -marker_position.poses[0].position.y;
    tf2::Quaternion q;
    q.setRPY(0, 0, 0);
    transformStamped.transform.rotation.x = q.x();
    transformStamped.transform.rotation.y = q.y();
    transformStamped.transform.rotation.z = q.z();
    transformStamped.transform.rotation.w = q.w();
    br.sendTransform(transformStamped);

    float target_distance = marker_position.poses[0].position.z/4; // Target distance is proportional to horizontal distance
    if (target_distance < 1) target_distance = 1; // Minimum of 1 meter

    // Transformation from visual marker to target position
    transformStamped.header.stamp = ros::Time::now();
    transformStamped.header.frame_id = "marker";
    transformStamped.child_frame_id = "target_position";
    if (close_enough > (SAFETY_TIME_SEC * ROS_RATE))
        transformStamped.transform.translation.x = -0.6; //The target is 0.6 m in front of the marker if the drone is close enough
    else
        transformStamped.transform.translation.x = -target_distance;
    transformStamped.transform.translation.y = 0;
    transformStamped.transform.translation.z = 0;
    transformStamped.transform.rotation = marker_position.poses[0].orientation;
    br.sendTransform(transformStamped);

    if (approaching) {
      try {
        transformStamped = tfBuffer.lookupTransform("map", "target_position", ros::Time(0));
        setpoint_pos_ENU.pose.position.x = transformStamped.transform.translation.x;
        setpoint_pos_ENU.pose.position.y = transformStamped.transform.translation.y;
        setpoint_pos_ENU.pose.position.z = transformStamped.transform.translation.z;
        //setpoint_pos_ENU.pose.orientation = tf::createQuaternionMsgFromYaw(currentYaw());

        ROS_INFO("Setpoint position: E: %f, N: %f, U: %f", transformStamped.transform.translation.x,
                 transformStamped.transform.translation.y, transformStamped.transform.translation.z);
      }
      catch (tf2::TransformException &ex){
        ROS_ERROR("%s",ex.what());
      }
    }
}

geometry_msgs::PoseStamped local_position;
void local_position_cb(const geometry_msgs::PoseStamped::ConstPtr& msg){
    local_position = *msg;
    static int cnt = 0;

    static tf2_ros::TransformBroadcaster br;

    // Transformation from map to drone
    geometry_msgs::TransformStamped transformStamped;
    transformStamped.header.stamp = ros::Time::now();
    transformStamped.header.frame_id = "map";
    transformStamped.child_frame_id = "drone";
    transformStamped.transform.translation.x = local_position.pose.position.x;
    transformStamped.transform.translation.y = local_position.pose.position.y;
    transformStamped.transform.translation.z = local_position.pose.position.z;
    transformStamped.transform.rotation = local_position.pose.orientation;

    cnt++;
    if (cnt % 100 == 0) {
        double roll, pitch, yaw;
        tf::Quaternion q(vision_pos_ENU.pose.orientation.x,
                         vision_pos_ENU.pose.orientation.y,
                         vision_pos_ENU.pose.orientation.z,
                         vision_pos_ENU.pose.orientation.w);
        tf::Matrix3x3(q).getRPY(roll, pitch, yaw);
        ROS_INFO("Mavros local position: E: %f, N: %f, U: %f, yaw: %f", transformStamped.transform.translation.x,
                 transformStamped.transform.translation.y, transformStamped.transform.translation.z, yaw);
    }

    br.sendTransform(transformStamped);
}

geometry_msgs::PoseWithCovarianceStamped svo_position;
void svo_position_cb(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr& msg){
    svo_position = *msg;
    static int cnt = 0;

    static tf2_ros::TransformBroadcaster br;
    static tf2_ros::StaticTransformBroadcaster sbr;
    geometry_msgs::TransformStamped transformStamped;

    if (ros::Time::now() - last_svo_estimate > ros::Duration(1.0)) {
        // svo_position is the first pose message after initialization/recovery, need to set svo_init_pos
        if (ros::Time::now() - local_position.header.stamp < ros::Duration(1.0)) {
            ROS_INFO("svo_init_pos = local_position");
            svo_init_pos = local_position;
        } else {
            ROS_INFO("svo_init_pos = 0");
            svo_init_pos.pose.position.x = 0;
            svo_init_pos.pose.position.y = 0;
            svo_init_pos.pose.position.z = 0;
            svo_init_pos.pose.orientation.x = 0;
            svo_init_pos.pose.orientation.y = 0;
            svo_init_pos.pose.orientation.z = 0;
            svo_init_pos.pose.orientation.w = 1;
        }

        // Transformation from map to svo_init
        transformStamped.header.stamp = ros::Time::now();
        transformStamped.header.frame_id = "map";
        transformStamped.child_frame_id = "svo_init";
        transformStamped.transform.translation.x = svo_init_pos.pose.position.x;
        transformStamped.transform.translation.y = svo_init_pos.pose.position.y;
        transformStamped.transform.translation.z = svo_init_pos.pose.position.z;
        transformStamped.transform.rotation = svo_init_pos.pose.orientation;

        sbr.sendTransform(transformStamped);
    }

    // Transformation from svo_init to drone_vision
    transformStamped.header.stamp = last_svo_estimate = ros::Time::now();
    transformStamped.header.frame_id = "svo_init";
    transformStamped.child_frame_id = "drone_vision";
    transformStamped.transform.translation.x = svo_position.pose.pose.position.x;
    transformStamped.transform.translation.y = svo_position.pose.pose.position.y;
    transformStamped.transform.translation.z = svo_position.pose.pose.position.z;
    transformStamped.transform.rotation = svo_position.pose.pose.orientation;

    br.sendTransform(transformStamped);

    if (send_vision_estimate) {
        try {
            // Send vision position estimate to mavros
            transformStamped = tfBuffer.lookupTransform("map", "drone_vision", ros::Time(0));
            vision_pos_ENU.header.stamp = ros::Time::now();
            vision_pos_ENU.header.frame_id = "map";
            vision_pos_ENU.pose.position.x = transformStamped.transform.translation.x;
            vision_pos_ENU.pose.position.y = transformStamped.transform.translation.y;
            vision_pos_ENU.pose.position.z = transformStamped.transform.translation.z;
            vision_pos_ENU.pose.orientation = transformStamped.transform.rotation;

            vision_pos_pub.publish(vision_pos_ENU);
            ros::spinOnce();

            cnt++;
            if (cnt % 66 == 0) {
                double roll, pitch, yaw;
                tf::Quaternion q(vision_pos_ENU.pose.orientation.x,
                                 vision_pos_ENU.pose.orientation.y,
                                 vision_pos_ENU.pose.orientation.z,
                                 vision_pos_ENU.pose.orientation.w);
                tf::Matrix3x3(q).getRPY(roll, pitch, yaw);
                ROS_INFO("Vision position lookup: E: %f, N: %f, U: %f, yaw: %f", transformStamped.transform.translation.x,
                         transformStamped.transform.translation.y, transformStamped.transform.translation.z, yaw);
            }
        }
        catch (tf2::TransformException &ex){
        ROS_ERROR("%s",ex.what());
        }
    }
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "offb_node");

    ros::NodeHandle nh;

    // The setpoint publishing rate MUST be faster than 2Hz
    ros::Rate rate(ROS_RATE);

    static tf2_ros::TransformListener tfListener(tfBuffer);

    state_sub = nh.subscribe<mavros_msgs::State>("/mavros/state", 10, state_cb);
    marker_pos_sub = nh.subscribe<geometry_msgs::PoseArray>("/whycon/poses", 10, marker_position_cb);
    local_pos_sub = nh.subscribe<geometry_msgs::PoseStamped>("/mavros/local_position/pose", 10, local_position_cb);
    svo_pos_sub = nh.subscribe<geometry_msgs::PoseWithCovarianceStamped>("/svo/pose_imu", 10, svo_position_cb);

    setpoint_pos_pub = nh.advertise<geometry_msgs::PoseStamped>("/mavros/setpoint_position/local", 10);
    vision_pos_pub = nh.advertise<geometry_msgs::PoseStamped>("/mavros/vision_pose/pose", 10);
    svo_cmd_pub = nh.advertise<std_msgs::String>("/svo/remote_key", 10);

    arming_client = nh.serviceClient<mavros_msgs::CommandBool>("/mavros/cmd/arming");
    land_client = nh.serviceClient<mavros_msgs::CommandTOL>("/mavros/cmd/land");
    set_mode_client = nh.serviceClient<mavros_msgs::SetMode>("/mavros/set_mode");

    // Wait for FCU connection
    while(ros::ok() && current_state.connected){
        ros::spinOnce();
        rate.sleep();
        ROS_INFO("connecting to FCT...");
    }

    offboardMode();

    //vioOff();

    // The tf module of mavros does not work currently. See also approachMarker function
    //nh.setParam("/mavros/local_position/tf/frame_id", "map");
    //nh.setParam("/mavros/local_position/tf/child_frame_id", "drone");
    //nh.setParam("/mavros/local_position/tf/send", true);

    //takeOff();

    //initVIO();

    if (svo_running) {
        //testFlightHorizontal();
        //testFlightVertical();
    }

    hover(15);

    //turnTowardsMarker();

    //approachMarker(nh);

    land();
    disarm();

    while(ros::ok() && KEEP_ALIVE) {
        ros::spinOnce();
        rate.sleep();
    }

    return 0;
}

void offboardMode(){
    // The setpoint publishing rate MUST be faster than 2Hz
    ros::Rate rate(ROS_RATE);

    ROS_INFO("Switching to OFFBOARD mode");

    last_svo_estimate = ros::Time::now() - ros::Duration(1.0);

    if (ros::Time::now() - local_position.header.stamp < ros::Duration(1.0)) {
        ROS_INFO("Local_position available");
    } else {
        ROS_INFO("Local_position not available, initializing to 0");
        local_position.header.stamp = ros::Time::now();
        local_position.header.frame_id = "map";
        local_position.pose.position.x = 0;
        local_position.pose.position.y = 0;
        local_position.pose.position.z = 0;
        local_position.pose.orientation.x = 0;
        local_position.pose.orientation.y = 0;
        local_position.pose.orientation.z = 0;
        local_position.pose.orientation.w = 1;
    }

    setpoint_pos_ENU = gps_init_pos = local_position;

    // Send a few setpoints before starting, otherwise px4 will not switch to OFFBOARD mode
    for(int i = 20; ros::ok() && i > 0; --i){
        setpoint_pos_pub.publish(setpoint_pos_ENU);
        ros::spinOnce();
        rate.sleep();
    }

    mavros_msgs::SetMode offb_set_mode;
    offb_set_mode.request.custom_mode = "OFFBOARD";

    arm_cmd.request.value = true;

    last_request = ros::Time::now();

    // Change to offboard mode and arm
    while(ros::ok() && !current_state.armed){
        if( current_state.mode != "OFFBOARD" && (ros::Time::now() - last_request > ros::Duration(5.0))){
            ROS_INFO(current_state.mode.c_str());
            if( set_mode_client.call(offb_set_mode) && offb_set_mode.response.mode_sent){
                ROS_INFO("Offboard enabled");
            }
            last_request = ros::Time::now();
        } else {
            if( !current_state.armed && (ros::Time::now() - last_request > ros::Duration(5.0))){
                if( arming_client.call(arm_cmd) && arm_cmd.response.success){
                    ROS_INFO("Vehicle armed");
                }
                last_request = ros::Time::now();
            }
        }
        setpoint_pos_pub.publish(setpoint_pos_ENU);
        ros::spinOnce();
        rate.sleep();
    }
}

void vioOff(){
    // The setpoint publishing rate MUST be faster than 2Hz
    ros::Rate rate(ROS_RATE);

    ROS_INFO("Disabling SVO");

    svo_cmd.data = "r";
    for(int i = 0; ros::ok() && i < 1 * ROS_RATE; ++i){
        svo_cmd_pub.publish(svo_cmd);
        ros::spinOnce();
        rate.sleep();
    }

    return;
}

void vioOn(){
    // The setpoint publishing rate MUST be faster than 2Hz
    ros::Rate rate(ROS_RATE);

    ROS_INFO("Starting SVO");

    svo_cmd.data = "s";
    for(int i = 0; ros::ok() && i < 1 * ROS_RATE; ++i){
        svo_cmd_pub.publish(svo_cmd);
        ros::spinOnce();
        rate.sleep();
    }

    return;
}

void takeOff(){
    // The setpoint publishing rate MUST be faster than 2Hz
    ros::Rate rate(ROS_RATE);

    ROS_INFO("Taking off. Current position: N: %f, W: %f, U: %f", local_position.pose.position.x, local_position.pose.position.y, local_position.pose.position.z);

    // Take off
    setpoint_pos_ENU = gps_init_pos;
    setpoint_pos_ENU.pose.position.z += FLIGHT_ALTITUDE;

    ROS_INFO("Taking off");
    for(int i = 0; ros::ok() && i < 10 * ROS_RATE; ++i){
        setpoint_pos_pub.publish(setpoint_pos_ENU);
        ros::spinOnce();
        rate.sleep();
    }
    ROS_INFO("Takeoff finished! Looking for whycon marker");
    return;
}

void initVIO() {
    // The setpoint publishing rate MUST be faster than 2Hz
    ros::Rate rate(ROS_RATE);

    vioOn();

    //Translational movement to start odometry
    for(int j = 0; ros::ok() && j < INIT_FLIGHT_REPEAT && ros::Time::now() - last_svo_estimate > ros::Duration(1.0); ++j){
        ROS_INFO("Translational movement");
        for(int i = 0; ros::ok() && i < INIT_FLIGHT_DURATION * ROS_RATE
            && ros::Time::now() - last_svo_estimate > ros::Duration(1.0); ++i){

            setpoint_pos_ENU.pose.position.x += INIT_FLIGHT_LENGTH/INIT_FLIGHT_DURATION/ROS_RATE;

            setpoint_pos_pub.publish(setpoint_pos_ENU);
            ros::spinOnce();
            rate.sleep();
        }
        for(int i = 0; ros::ok() && i < INIT_FLIGHT_DURATION * ROS_RATE
            && ros::Time::now() - last_svo_estimate > ros::Duration(1.0); ++i){

            setpoint_pos_ENU.pose.position.x -= INIT_FLIGHT_LENGTH/INIT_FLIGHT_DURATION/ROS_RATE;

            setpoint_pos_pub.publish(setpoint_pos_ENU);
            ros::spinOnce();
            rate.sleep();
        }
    }

    if (ros::Time::now() - last_svo_estimate < ros::Duration(1.0)) {
        ROS_INFO("SVO initialized successfully");
        svo_running = true;
    }
    else
        ROS_INFO("SVO initialization failed");

    setpoint_pos_ENU = gps_init_pos;
    setpoint_pos_ENU.pose.position.z += FLIGHT_ALTITUDE;

    ROS_INFO("Back to takeoff position");
    for(int i = 0; ros::ok() && i < 10 * ROS_RATE; ++i){
        setpoint_pos_pub.publish(setpoint_pos_ENU);
        ros::spinOnce();
        rate.sleep();
    }

    return;
}

void testFlightHorizontal() {
    // The setpoint publishing rate MUST be faster than 2Hz
    ros::Rate rate(ROS_RATE);

    ROS_INFO("Horizontal test flight");

    for(int j = 0; ros::ok() && j < TEST_FLIGHT_REPEAT; ++j){
        for(int i = 0; ros::ok() && i < TEST_FLIGHT_DURATION * ROS_RATE; ++i){
            setpoint_pos_ENU.pose.position.x += TEST_FLIGHT_LENGTH/TEST_FLIGHT_DURATION/ROS_RATE;

            setpoint_pos_pub.publish(setpoint_pos_ENU);
            ros::spinOnce();
            rate.sleep();
        }
        for(int i = 0; ros::ok() && i < TEST_FLIGHT_DURATION * ROS_RATE; ++i){
            setpoint_pos_ENU.pose.position.y += TEST_FLIGHT_LENGTH/TEST_FLIGHT_DURATION/ROS_RATE;

            setpoint_pos_pub.publish(setpoint_pos_ENU);
            ros::spinOnce();
            rate.sleep();
        }
        for(int i = 0; ros::ok() && i < TEST_FLIGHT_DURATION * ROS_RATE; ++i){
            setpoint_pos_ENU.pose.position.x -= TEST_FLIGHT_LENGTH/TEST_FLIGHT_DURATION/ROS_RATE;

            setpoint_pos_pub.publish(setpoint_pos_ENU);
            ros::spinOnce();
            rate.sleep();
        }
        for(int i = 0; ros::ok() && i < TEST_FLIGHT_DURATION * ROS_RATE; ++i){
            setpoint_pos_ENU.pose.position.y -= TEST_FLIGHT_LENGTH/TEST_FLIGHT_DURATION/ROS_RATE;

            setpoint_pos_pub.publish(setpoint_pos_ENU);
            ros::spinOnce();
            rate.sleep();
        }
    }
}

void testFlightVertical() {
    // The setpoint publishing rate MUST be faster than 2Hz
    ros::Rate rate(ROS_RATE);

    ROS_INFO("Vertical test flight");

    for(int j = 0; ros::ok() && j < TEST_FLIGHT_REPEAT; ++j){
        for(int i = 0; ros::ok() && i < TEST_FLIGHT_DURATION * ROS_RATE; ++i){
            setpoint_pos_ENU.pose.position.x += TEST_FLIGHT_LENGTH/TEST_FLIGHT_DURATION/ROS_RATE;

            setpoint_pos_pub.publish(setpoint_pos_ENU);
            ros::spinOnce();
            rate.sleep();
        }
        for(int i = 0; ros::ok() && i < TEST_FLIGHT_DURATION * ROS_RATE; ++i){
            setpoint_pos_ENU.pose.position.z += TEST_FLIGHT_LENGTH/TEST_FLIGHT_DURATION/ROS_RATE;

            setpoint_pos_pub.publish(setpoint_pos_ENU);
            ros::spinOnce();
            rate.sleep();
        }
        for(int i = 0; ros::ok() && i < TEST_FLIGHT_DURATION * ROS_RATE; ++i){
            setpoint_pos_ENU.pose.position.x -= TEST_FLIGHT_LENGTH/TEST_FLIGHT_DURATION/ROS_RATE;

            setpoint_pos_pub.publish(setpoint_pos_ENU);
            ros::spinOnce();
            rate.sleep();
        }
        for(int i = 0; ros::ok() && i < TEST_FLIGHT_DURATION * ROS_RATE; ++i){
            setpoint_pos_ENU.pose.position.z -= TEST_FLIGHT_LENGTH/TEST_FLIGHT_DURATION/ROS_RATE;

            setpoint_pos_pub.publish(setpoint_pos_ENU);
            ros::spinOnce();
            rate.sleep();
        }
    }
}

void turnTowardsMarker(){
    // The setpoint publishing rate MUST be faster than 2Hz
    ros::Rate rate(ROS_RATE);
    float rad, current_yaw;

    // Turn towards the marker without change of position
    setpoint_pos_ENU.pose.position.x = local_position.pose.position.x;
    setpoint_pos_ENU.pose.position.y = local_position.pose.position.y;
    setpoint_pos_ENU.pose.position.z = local_position.pose.position.z;

    for(int j = 0; ros::ok() && j < 10 * ROS_RATE; ++j){
        current_yaw = currentYaw();

        if (ros::Time::now() - marker_position.header.stamp < ros::Duration(1.0)) {
            // Calculate yaw angle difference of marker in radians
            rad = -atan2f(marker_position.poses[0].position.x, marker_position.poses[0].position.z);
            if (fabs(rad) < 0.1) {
                ROS_INFO("Headed towards marker!");
                break;
            }

            ROS_INFO("Marker found, current yaw: %f, turning %f radians", current_yaw, rad);
            setpoint_pos_ENU.pose.orientation = tf::createQuaternionMsgFromYaw(current_yaw+rad);
        } else {
            ROS_INFO("No marker was found in the last second, turning around");
            setpoint_pos_ENU.pose.orientation = tf::createQuaternionMsgFromYaw(current_yaw+TURN_STEP_RAD);
        }
        setpoint_pos_pub.publish(setpoint_pos_ENU);
        ros::spinOnce();
        rate.sleep();
    }

    // Send setpoint for 2 seconds
    for(int i = 0; ros::ok() && i < 2 * ROS_RATE; ++i){
        setpoint_pos_pub.publish(setpoint_pos_ENU);
        ros::spinOnce();
        rate.sleep();
    }

    return;
}

void approachMarker(ros::NodeHandle & nh){
    // The setpoint publishing rate MUST be faster than 2Hz
    ros::Rate rate(ROS_RATE);

    //nh.setParam("/mavros/setpoint_position/tf/frame_id", "map");
    //nh.setParam("/mavros/setpoint_position/tf/child_frame_id", "target_position");
    //nh.setParam("/mavros/setpoint_position/tf/listen", true);
    approaching = true;

    // TODO: handle after MAX_ATTEMPTS
    for(int j = 0; ros::ok() && j < MAX_ATTEMPTS; ++j){
      if (ros::Time::now() - marker_position.header.stamp < ros::Duration(1.0)) {
        if (marker_position.poses[0].position.z < 1.5) {
          close_enough++;
          // TODO: Changing orientation. Calulate yaw from marker orientation

          if (close_enough > (SAFETY_TIME_SEC * ROS_RATE)) {
            ROS_INFO("Close enough");
            break; // Exit loop and fly to final target
          }
        } else {close_enough = 0;}

        setpoint_pos_pub.publish(setpoint_pos_ENU);

        //nh.setParam("/mavros/setpoint_position/tf/listen", true);
        approaching = true;

      } else {
        //nh.setParam("/mavros/setpoint_position/tf/listen", false);
        approaching = false;
        ROS_INFO("No marker was found in the last 1 second");
      }
      ros::spinOnce();
      rate.sleep();
    }

    // Publish final setpoint for 4 seconds before landing
    for(int i = 0; ros::ok() && i < 4 * ROS_RATE; ++i){
      setpoint_pos_pub.publish(setpoint_pos_ENU);
      ros::spinOnce();
      rate.sleep();
    }

    //nh.setParam("/mavros/setpoint_position/tf/listen", false);
    approaching = false;
    ROS_INFO("Marker approached!");

    return;
}

void hover(int seconds){
    // The setpoint publishing rate MUST be faster than 2Hz
    ros::Rate rate(ROS_RATE);

    ROS_INFO("Hovering for %i seconds in position: E: %f, N: %f, U: %f", seconds,
             setpoint_pos_ENU.pose.position.x,
             setpoint_pos_ENU.pose.position.y,
             setpoint_pos_ENU.pose.position.z);
    for(int i = 0; ros::ok() && i < 15 * ROS_RATE; ++i){
        setpoint_pos_pub.publish(setpoint_pos_ENU);
        ros::spinOnce();
        rate.sleep();
    }

    return;
}

void land(){
    // The setpoint publishing rate MUST be faster than 2Hz
    ros::Rate rate(ROS_RATE);

    mavros_msgs::CommandTOL land_cmd;
    land_cmd.request.yaw = 0;
    land_cmd.request.latitude = 0;
    land_cmd.request.longitude = 0;
    land_cmd.request.altitude = 0;

    ROS_INFO("Trying to land");
    while (!(land_client.call(land_cmd) && land_cmd.response.success)){
        setpoint_pos_pub.publish(setpoint_pos_ENU);
        ROS_INFO("Retrying to land");
        ros::spinOnce();
        rate.sleep();
    }
    ROS_INFO("Success");

    // Wait 5 seconds for proper landing
    for(int i = 0; ros::ok() && i < 5 * ROS_RATE; ++i){
      ros::spinOnce();
      rate.sleep();
    }

    return;
}

void disarm(){
    // The setpoint publishing rate MUST be faster than 2Hz
    ros::Rate rate(ROS_RATE);

    // Disarm
    arm_cmd.request.value = false;
    while(ros::ok() && current_state.armed){
        if( current_state.armed && (ros::Time::now() - last_request > ros::Duration(5.0))){
            if( arming_client.call(arm_cmd) && arm_cmd.response.success){
                ROS_INFO("Vehicle disarmed");
            }
            last_request = ros::Time::now();
        }
        ros::spinOnce();
        rate.sleep();
    }
    return;
}

float currentYaw(){
    //Calculate yaw current orientation
    double roll, pitch, yaw;
    tf::Quaternion q(local_position.pose.orientation.x,
                     local_position.pose.orientation.y,
                     local_position.pose.orientation.z,
                     local_position.pose.orientation.w);
    tf::Matrix3x3(q).getRPY(roll, pitch, yaw);

    return yaw;
}

//
// Created by Li on 23-9-9.
//

//ROS
#include <image_transport/image_transport.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/camera_info.hpp>
#include <visualization_msgs/msg/marker_array.hpp>
#include <tf2/LinearMath/Matrix3x3.h>
#include "std_msgs/msg/bool.hpp"

//STD
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>


#include <cv_bridge/cv_bridge.h>

//OpenCV
#include <opencv2/core.hpp>
#include <opencv2/opencv.hpp>
#include <opencv2/calib3d.hpp>

#include "../include/mix_detector_node.hpp"


namespace rmos_detector_r
{
    void MixDetectorNode::imageCallBack(const sensor_msgs::msg::Image::ConstSharedPtr &image_msg)
    {
        cv::Scalar colors[4] = {cv::Scalar(255, 255, 0),
                       cv::Scalar(0, 255, 0),
                       cv::Scalar(255, 255, 0),
                       cv::Scalar(0, 255, 255)};

        //发布相机到陀螺仪的静态tf
        cv::Mat cam2IMU_matrix;
        cam2IMU_matrix = (cv::Mat_<double>(3, 3) <<0,0,1,-1,0,0,0,-1,0);
        tf2::Matrix3x3 tf2_cam2IMU_matrix(
                cam2IMU_matrix.at<double>(0, 0), cam2IMU_matrix.at<double>(0, 1),
                cam2IMU_matrix.at<double>(0, 2), cam2IMU_matrix.at<double>(1, 0),
                cam2IMU_matrix.at<double>(1, 1), cam2IMU_matrix.at<double>(1, 2),
                cam2IMU_matrix.at<double>(2, 0), cam2IMU_matrix.at<double>(2, 1),
                cam2IMU_matrix.at<double>(2, 2));

        tf2::Quaternion tf2_cam2IMU_quaternion;
        tf2_cam2IMU_matrix.getRotation(tf2_cam2IMU_quaternion);

        geometry_msgs::msg::TransformStamped t;
        t.header.stamp =  image_msg->header.stamp;
        t.header.frame_id = "IMU_r";
        t.child_frame_id = "RCam_Link";
        t.transform.rotation.x = tf2_cam2IMU_quaternion.x();
        t.transform.rotation.y = tf2_cam2IMU_quaternion.y();
        t.transform.rotation.z = tf2_cam2IMU_quaternion.z();
        t.transform.rotation.w = tf2_cam2IMU_quaternion.w();

        // 相机到IMU存在位置的偏移，每辆车不同，请在参数文件自行更改
        t.transform.translation.x = 0.003;
        t.transform.translation.y = 0;
        t.transform.translation.z = 0;
        // this->tf_publisher_->sendTransform(t);



        rclcpp::Clock steady_clock_{RCL_STEADY_TIME};
        auto time1 = steady_clock_.now();
        auto image = cv_bridge::toCvShare(image_msg, "bgr8")->image;
        std::vector<base::Armor> armors;
        std::vector<cv::Mat> inputs;
        std::vector<DetectResultList> outputs;
        armors.clear();
        outputs.clear();
        inputs.clear();
        inputs.push_back(image);
  
        mix_detector_->rt.run(inputs, outputs);
        for (auto out_box : outputs.at(0))
        {
            // int id = out_box.id % 9;
            int id=1;
            int color = out_box.id ; //0---blue,  1--red

            if (mix_detector_->enemy_color_ == base::Color::RED && color == 0)
            {
                continue;
            }
            if (mix_detector_->enemy_color_ == base::Color::BLUE && color == 1)
            {
                continue;
            }
            cv::Rect2d target = out_box.bbox;
            target.x = int(target.x - target.width / 5);
            target.y = int(target.y - target.height / 5);
            target.width = int(target.width * 1.4);
            target.height = int(target.height * 1.4);
            target = target & cv::Rect2d(0, 0, image.cols, image.rows);

            MixDetect::Armor out = MixDetect::Armor(target, id, color, out_box.points);


            bool if_found=mix_detector_->detect(image, out);
            if (if_found)
            {
                MixDetect::Armor output;
                mix_detector_->getResult(output);

                base::Armor armor;
                armor.num_id = output._color;  //0---blue,  1--red
                armor.points = output._points;
                armor.left.up = armor.points[1];
                armor.left.down = armor.points[0];
                armor.right.up = armor.points[2];
                armor.right.down = armor.points[3];
                armor.rect = output._bbox;
                armor.center_point = (armor.points[0]+armor.points[1]+armor.points[2]+armor.points[3])/4.0;
                armors.push_back(armor);
            }
            

            // std::cout<< "points____" << armor.points << std::endl;

            // if(if_found){
            //     cv::putText(image, std::to_string(color), output._bbox.tl(), 2, 1, colors[2]);
            //     for (size_t i = 0; i < output._points.size(); i++)
            //     {
            //         line(image, output._points[i], output._points[(i + 1) % 4], colors[int(output._if_stable)], 2, 8, 0);
            //     }
            // }
        }

        onnx_classifier_->classifyArmors(image,armors);

        rmos_interfaces::msg::Armors armors_msg;
        rmos_interfaces::msg::Armor armor_msg;
        armors_msg.header = image_msg->header;


        for(auto &armor : armors)
        {

            std::string text1 = std::to_string(armor.num_id);
            // std::string text2 = std::to_string(int(armor.confidence*100));
            cv::putText(image, text1, armor.left.up, cv::FONT_HERSHEY_SIMPLEX, 0.5,
                        cv::Scalar(0, 255, 0), 1.8);
            // cv::putText(image, text2, armor.right.up, cv::FONT_HERSHEY_SIMPLEX, 0.5,
            //             cv::Scalar(0, 255, 0), 0.5);
            cv::line(image,armor.left.up,armor.right.down ,cv::Scalar(255, 0, 255),1);
            cv::line(image,armor.left.down,armor.right.up ,cv::Scalar(255, 0, 255),1);

            //pnp solve
            cv::Mat tVec;
            cv::Mat rVec;
            bool is_solve;
            is_solve = this->pnp_solver_->solveArmorPose(armor,this->camera_matrix_,this->dist_coeffs_,tVec,rVec);
            if(!is_solve)
            {
                RCLCPP_WARN(this->get_logger(), "camera param empty");
                exit(0);
            }
            // std::cout << "tVec_____" << tVec << std::endl;
            // std::cout << "rVec_____" << rVec << std::endl;
            armor_msg.pose.position.x = tVec.at<double>(0, 0)/1000;
            armor_msg.pose.position.y = tVec.at<double>(1, 0)/1000;
            armor_msg.pose.position.z = tVec.at<double>(2, 0)/1000;
            // rvec to 3x3 rotation matrix
            cv::Mat rotation_matrix;
            cv::Rodrigues(rVec, rotation_matrix);
            // rotation matrix to quaternion
            tf2::Matrix3x3 tf2_rotation_matrix(
                    rotation_matrix.at<double>(0, 0), rotation_matrix.at<double>(0, 1),
                    rotation_matrix.at<double>(0, 2), rotation_matrix.at<double>(1, 0),
                    rotation_matrix.at<double>(1, 1), rotation_matrix.at<double>(1, 2),
                    rotation_matrix.at<double>(2, 0), rotation_matrix.at<double>(2, 1),
                    rotation_matrix.at<double>(2, 2));
            tf2::Quaternion tf2_quaternion;
            tf2_rotation_matrix.getRotation(tf2_quaternion);
            armor_msg.pose.orientation.x = tf2_quaternion.x();
            armor_msg.pose.orientation.y = tf2_quaternion.y();
            armor_msg.pose.orientation.z = tf2_quaternion.z();
            armor_msg.pose.orientation.w = tf2_quaternion.w();
            cv::Point2f center(image.rows/2,image.cols/2);
            armor_msg.distance_to_image_center = sqrt((center.x-armor.rrect.center.x)*(center.x-armor.rrect.center.x)+
                                                      (center.y-armor.rrect.center.y)*(center.y-armor.rrect.center.y));
            armor_msg.num_id = armor.num_id;

            double distance = sqrt(armor_msg.pose.position.x*armor_msg.pose.position.x+
                                           armor_msg.pose.position.y*armor_msg.pose.position.y+
                                           armor_msg.pose.position.z*armor_msg.pose.position.z);
            std::string text3 = std::to_string(float(distance));
            cv::putText(image, text3, armor.right.down, cv::FONT_HERSHEY_SIMPLEX, 0.5,
                        cv::Scalar(0, 255, 0), 0.5);
            armors_msg.armors.push_back(armor_msg);

            // std::cout << "distance_________" << distance << std::endl;
        }
        auto time2 = steady_clock_.now();

        if(debug::get_debug_option(base::SHOW_DETECT_COST))RCLCPP_INFO(this->get_logger(), "Cost %.4f ms", (time2-time1).seconds() * 1000);



        if(debug::get_debug_option(base::SHOW_ARMOR))
        {
            debug_image_msg_ = cv_bridge::CvImage(std_msgs::msg::Header(), "bgr8", image).toImageMsg();
            debug_img_pub_.publish(*debug_image_msg_,camera_info_msg_);
        }
        if(debug::get_debug_option(base::SHOW_BIN))
        {
            debug_bin_image_msg_ = cv_bridge::CvImage(std_msgs::msg::Header(), "mono8", this->mix_detector_->debug_binary_).toImageMsg();
            debug_bin_img_pub_.publish(*debug_bin_image_msg_,camera_info_msg_);
        }

        armors_pub_->publish(armors_msg);

    }

}


#include <rclcpp_components/register_node_macro.hpp>

// Register the component with class_loader.
// This acts as a sort of entry point, allowing the component to be discoverable when its library
// is being loaded into a running process.
RCLCPP_COMPONENTS_REGISTER_NODE(rmos_detector_r::MixDetectorNode)

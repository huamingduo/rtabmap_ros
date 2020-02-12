/*
Copyright (c) 2010-2019, Mathieu Labbe - IntRoLab - Universite de Sherbrooke
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Universite de Sherbrooke nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <rtabmap_ros/point_cloud_aggregator.hpp>

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>

#include <rtabmap_ros/MsgConversion.h>
#include <rtabmap/utilite/UConversion.h>
#include <rtabmap/utilite/ULogger.h>

namespace pcl_ros {

void
transformPointCloud (
		const Eigen::Matrix4f &transform,
		const sensor_msgs::msg::PointCloud2 &in,
        sensor_msgs::msg::PointCloud2 &out)
{
  // Get X-Y-Z indices
  int x_idx = pcl::getFieldIndex (in, "x");
  int y_idx = pcl::getFieldIndex (in, "y");
  int z_idx = pcl::getFieldIndex (in, "z");

  if (x_idx == -1 || y_idx == -1 || z_idx == -1)
  {
    UERROR ("Input dataset has no X-Y-Z coordinates! Cannot convert to Eigen format.");
    return;
  }

  if (in.fields[x_idx].datatype != sensor_msgs::PointField::FLOAT32 ||
      in.fields[y_idx].datatype != sensor_msgs::PointField::FLOAT32 ||
      in.fields[z_idx].datatype != sensor_msgs::PointField::FLOAT32)
  {
	  UERROR ("X-Y-Z coordinates not floats. Currently only floats are supported.");
    return;
  }

  // Check if distance is available
  int dist_idx = pcl::getFieldIndex (in, "distance");

  // Copy the other data
  if (&in != &out)
  {
    out.header = in.header;
    out.height = in.height;
    out.width  = in.width;
    out.fields = in.fields;
    out.is_bigendian = in.is_bigendian;
    out.point_step   = in.point_step;
    out.row_step     = in.row_step;
    out.is_dense     = in.is_dense;
    out.data.resize (in.data.size ());
    // Copy everything as it's faster than copying individual elements
    memcpy (&out.data[0], &in.data[0], in.data.size ());
  }

  Eigen::Array4i xyz_offset (in.fields[x_idx].offset, in.fields[y_idx].offset, in.fields[z_idx].offset, 0);

  for (size_t i = 0; i < in.width * in.height; ++i)
  {
    Eigen::Vector4f pt (*(float*)&in.data[xyz_offset[0]], *(float*)&in.data[xyz_offset[1]], *(float*)&in.data[xyz_offset[2]], 1);
    Eigen::Vector4f pt_out;

    bool max_range_point = false;
    int distance_ptr_offset = i*in.point_step + in.fields[dist_idx].offset;
    float* distance_ptr = (dist_idx < 0 ? NULL : (float*)(&in.data[distance_ptr_offset]));
    if (!std::isfinite (pt[0]) || !std::isfinite (pt[1]) || !std::isfinite (pt[2]))
    {
      if (distance_ptr==NULL || !std::isfinite(*distance_ptr))  // Invalid point
      {
        pt_out = pt;
      }
      else  // max range point
      {
        pt[0] = *distance_ptr;  // Replace x with the x value saved in distance
        pt_out = transform * pt;
        max_range_point = true;
        //std::cout << pt[0]<<","<<pt[1]<<","<<pt[2]<<" => "<<pt_out[0]<<","<<pt_out[1]<<","<<pt_out[2]<<"\n";
      }
    }
    else
    {
      pt_out = transform * pt;
    }

    if (max_range_point)
    {
      // Save x value in distance again
      *(float*)(&out.data[distance_ptr_offset]) = pt_out[0];
      pt_out[0] = std::numeric_limits<float>::quiet_NaN();
    }

    memcpy (&out.data[xyz_offset[0]], &pt_out[0], sizeof (float));
    memcpy (&out.data[xyz_offset[1]], &pt_out[1], sizeof (float));
    memcpy (&out.data[xyz_offset[2]], &pt_out[2], sizeof (float));


    xyz_offset += in.point_step;
  }

  // Check if the viewpoint information is present
  int vp_idx = pcl::getFieldIndex (in, "vp_x");
  if (vp_idx != -1)
  {
    // Transform the viewpoint info too
    for (size_t i = 0; i < out.width * out.height; ++i)
    {
      float *pstep = (float*)&out.data[i * out.point_step + out.fields[vp_idx].offset];
      // Assume vp_x, vp_y, vp_z are consecutive
      Eigen::Vector4f vp_in (pstep[0], pstep[1], pstep[2], 1);
      Eigen::Vector4f vp_out = transform * vp_in;

      pstep[0] = vp_out[0];
      pstep[1] = vp_out[1];
      pstep[2] = vp_out[2];
    }
  }
}

}

namespace rtabmap_ros
{

PointCloudAggregator::PointCloudAggregator(const rclcpp::NodeOptions & options) :
	Node("pointcloud_to_depthimage", options),
	warningThread_(0),
	callbackCalled_(false),
	exactSync4_(0),
	approxSync4_(0),
	exactSync3_(0),
	approxSync3_(0),
	exactSync2_(0),
	approxSync2_(0)
{

	tfBuffer_ = std::make_shared<tf2_ros::Buffer>(this->get_clock());
	//auto timer_interface = std::make_shared<tf2_ros::CreateTimerROS>(
	//	this->get_node_base_interface(),
	//	this->get_node_timers_interface());
	//tfBuffer_->setCreateTimerInterface(timer_interface);
	tfListener_ = std::make_shared<tf2_ros::TransformListener>(*tfBuffer_);

	int queueSize = 5;
	int count = 2;
	bool approx=true;
	queueSize = this->declare_parameter("queue_size", queueSize);
	frameId_ = this->declare_parameter("frame_id", frameId_);
	fixedFrameId_ = this->declare_parameter("fixed_frame_id", fixedFrameId_);
	approx = this->declare_parameter("approx_sync", approx);
	count = this->declare_parameter("count", count);

	cloudPub_ = create_publisher<sensor_msgs::msg::PointCloud2>("combined_cloud", 1);

	cloudSub_1_.subscribe(this, "cloud1", rmw_qos_profile_sensor_data);
	cloudSub_2_.subscribe(this, "cloud2", rmw_qos_profile_sensor_data);

	std::string subscribedTopicsMsg;
	if(count == 4)
	{
		cloudSub_3_.subscribe(this, "cloud3", rmw_qos_profile_sensor_data);
		cloudSub_4_.subscribe(this, "cloud4", rmw_qos_profile_sensor_data);
		if(approx)
		{
			approxSync4_ = new message_filters::Synchronizer<ApproxSync4Policy>(ApproxSync4Policy(queueSize), cloudSub_1_, cloudSub_2_, cloudSub_3_, cloudSub_4_);
			approxSync4_->registerCallback(std::bind(&rtabmap_ros::PointCloudAggregator::clouds4_callback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
		}
		else
		{
			exactSync4_ = new message_filters::Synchronizer<ExactSync4Policy>(ExactSync4Policy(queueSize), cloudSub_1_, cloudSub_2_, cloudSub_3_, cloudSub_4_);
			exactSync4_->registerCallback(std::bind(&rtabmap_ros::PointCloudAggregator::clouds4_callback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
		}
		subscribedTopicsMsg = uFormat("\n%s subscribed to (%s sync):\n   %s,\n   %s,\n   %s,\n   %s",
				get_name(),
				approx?"approx":"exact",
				cloudSub_1_.getTopic().c_str(),
				cloudSub_2_.getTopic().c_str(),
				cloudSub_3_.getTopic().c_str(),
				cloudSub_4_.getTopic().c_str());
	}
	else if(count == 3)
	{
		cloudSub_3_.subscribe(this, "cloud3", rmw_qos_profile_sensor_data);
		if(approx)
		{
			approxSync3_ = new message_filters::Synchronizer<ApproxSync3Policy>(ApproxSync3Policy(queueSize), cloudSub_1_, cloudSub_2_, cloudSub_3_);
			approxSync3_->registerCallback(std::bind(&rtabmap_ros::PointCloudAggregator::clouds3_callback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
		}
		else
		{
			exactSync3_ = new message_filters::Synchronizer<ExactSync3Policy>(ExactSync3Policy(queueSize), cloudSub_1_, cloudSub_2_, cloudSub_3_);
			exactSync3_->registerCallback(std::bind(&rtabmap_ros::PointCloudAggregator::clouds3_callback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
		}
		subscribedTopicsMsg = uFormat("\n%s subscribed to (%s sync):\n   %s,\n   %s,\n   %s",
				this->get_name(),
				approx?"approx":"exact",
				cloudSub_1_.getTopic().c_str(),
				cloudSub_2_.getTopic().c_str(),
				cloudSub_3_.getTopic().c_str());
	}
	else
	{
		if(approx)
		{
			approxSync2_ = new message_filters::Synchronizer<ApproxSync2Policy>(ApproxSync2Policy(queueSize), cloudSub_1_, cloudSub_2_);
			approxSync2_->registerCallback(std::bind(&rtabmap_ros::PointCloudAggregator::clouds2_callback, this, std::placeholders::_1, std::placeholders::_2));
		}
		else
		{
			exactSync2_ = new message_filters::Synchronizer<ExactSync2Policy>(ExactSync2Policy(queueSize), cloudSub_1_, cloudSub_2_);
			exactSync2_->registerCallback(std::bind(&rtabmap_ros::PointCloudAggregator::clouds2_callback, this, std::placeholders::_1, std::placeholders::_2));
		}
		subscribedTopicsMsg = uFormat("\n%s subscribed to (%s sync):\n   %s,\n   %s",
				this->get_name(),
				approx?"approx":"exact",
				cloudSub_1_.getTopic().c_str(),
				cloudSub_2_.getTopic().c_str());
	}


	warningThread_ = new std::thread([&](){
		rclcpp::Rate r(1.0/5.0);
		while(!callbackCalled_)
		{
			r.sleep();
			if(!callbackCalled_)
			{
				RCLCPP_WARN(this->get_logger(), "%s: Did not receive data since 5 seconds! Make sure the input topics are "
						"published (\"$ rostopic hz my_topic\") and the timestamps in their "
						"header are set. %s%s",
						this->get_name(),
						approx?"":"Parameter \"approx_sync\" is false, which means that input "
							"topics should have all the exact timestamp for the callback to be called.",
							subscribedTopicsMsg.c_str());
			}
		}
	});
}

PointCloudAggregator::~PointCloudAggregator()
{
	delete exactSync4_;
	delete approxSync4_;
	delete exactSync3_;
	delete approxSync3_;
	delete exactSync2_;
	delete approxSync2_;

	if(warningThread_)
	{
		callbackCalled_=true;
		warningThread_->join();
		delete warningThread_;
	}
}

void PointCloudAggregator::clouds4_callback(const sensor_msgs::msg::PointCloud2::ConstSharedPtr cloudMsg_1,
					 const sensor_msgs::msg::PointCloud2::ConstSharedPtr cloudMsg_2,
					 const sensor_msgs::msg::PointCloud2::ConstSharedPtr cloudMsg_3,
					 const sensor_msgs::msg::PointCloud2::ConstSharedPtr cloudMsg_4)
{
	std::vector<sensor_msgs::msg::PointCloud2::ConstSharedPtr> clouds;
	clouds.push_back(cloudMsg_1);
	clouds.push_back(cloudMsg_2);
	clouds.push_back(cloudMsg_3);
	clouds.push_back(cloudMsg_4);

	combineClouds(clouds);
}
void PointCloudAggregator::clouds3_callback(const sensor_msgs::msg::PointCloud2::ConstSharedPtr cloudMsg_1,
					 const sensor_msgs::msg::PointCloud2::ConstSharedPtr cloudMsg_2,
					 const sensor_msgs::msg::PointCloud2::ConstSharedPtr cloudMsg_3)
{
	std::vector<sensor_msgs::msg::PointCloud2::ConstSharedPtr> clouds;
	clouds.push_back(cloudMsg_1);
	clouds.push_back(cloudMsg_2);
	clouds.push_back(cloudMsg_3);

	combineClouds(clouds);
}
void PointCloudAggregator::clouds2_callback(const sensor_msgs::msg::PointCloud2::ConstSharedPtr cloudMsg_1,
					 const sensor_msgs::msg::PointCloud2::ConstSharedPtr cloudMsg_2)
{
	std::vector<sensor_msgs::msg::PointCloud2::ConstSharedPtr> clouds;
	clouds.push_back(cloudMsg_1);
	clouds.push_back(cloudMsg_2);

	combineClouds(clouds);
}
void PointCloudAggregator::combineClouds(const std::vector<sensor_msgs::msg::PointCloud2::ConstSharedPtr> & cloudMsgs)
{
	callbackCalled_ = true;
	UASSERT(cloudMsgs.size() > 1);
	if(cloudPub_->get_subscription_count())
	{
		pcl::PCLPointCloud2 output;

		std::string frameId = frameId_;
		if(!frameId.empty() && frameId.compare(cloudMsgs[0]->header.frame_id) != 0)
		{
			sensor_msgs::msg::PointCloud2 tmp;
			rtabmap::Transform t = rtabmap_ros::getTransform(frameId, cloudMsgs[0]->header.frame_id, cloudMsgs[0]->header.stamp, *tfBuffer_, 0.1);
			if(t.isNull())
			{
				return;
			}
			pcl_ros::transformPointCloud(t.toEigen4f(), *cloudMsgs[0], tmp);
			pcl_conversions::toPCL(tmp, output);
		}
		else
		{
			pcl_conversions::toPCL(*cloudMsgs[0], output);
			frameId = cloudMsgs[0]->header.frame_id;
		}

		for(unsigned int i=1; i<cloudMsgs.size(); ++i)
		{
			rtabmap::Transform cloudDisplacement;
			bool notsync = false;
			if(!fixedFrameId_.empty() &&
			   cloudMsgs[0]->header.stamp != cloudMsgs[i]->header.stamp)
			{
				// approx sync
				cloudDisplacement = rtabmap_ros::getTransform(
						frameId, //sourceTargetFrame
						fixedFrameId_, //fixedFrame
						cloudMsgs[i]->header.stamp, //stampSource
						cloudMsgs[0]->header.stamp, //stampTarget
						*tfBuffer_,
						0.1);
				notsync = true;
			}

			pcl::PCLPointCloud2 cloud2;
			if(frameId.compare(cloudMsgs[i]->header.frame_id) != 0)
			{
				sensor_msgs::msg::PointCloud2 tmp;
				rtabmap::Transform t = rtabmap_ros::getTransform(frameId, cloudMsgs[i]->header.frame_id, cloudMsgs[i]->header.stamp, *tfBuffer_, 0.1);
				pcl_ros::transformPointCloud(t.toEigen4f(), *cloudMsgs[i], tmp);
				if(!cloudDisplacement.isNull())
				{
					sensor_msgs::msg::PointCloud2 tmp2;
					pcl_ros::transformPointCloud(cloudDisplacement.toEigen4f(), tmp, tmp2);
					pcl_conversions::toPCL(tmp2, cloud2);
				}
				else
				{
					pcl_conversions::toPCL(tmp, cloud2);
				}

			}
			else
			{
				if(!cloudDisplacement.isNull())
				{
					sensor_msgs::msg::PointCloud2 tmp;
					pcl_ros::transformPointCloud(cloudDisplacement.toEigen4f(), *cloudMsgs[i], tmp);
					pcl_conversions::toPCL(tmp, cloud2);
				}
				else
				{
					pcl_conversions::toPCL(*cloudMsgs[i], cloud2);
				}
			}

			pcl::PCLPointCloud2 tmp_output;
			pcl::concatenatePointCloud(output, cloud2, tmp_output);
			output = tmp_output;
		}

		sensor_msgs::msg::PointCloud2::UniquePtr rosCloud(new sensor_msgs::msg::PointCloud2);
		pcl_conversions::moveFromPCL(output, *rosCloud);
		rosCloud->header.stamp = cloudMsgs[0]->header.stamp;
		rosCloud->header.frame_id = frameId;
		cloudPub_->publish(std::move(rosCloud));
	}
}

}

#include "rclcpp_components/register_node_macro.hpp"

// Register the component with class_loader.
// This acts as a sort of entry point, allowing the component to be discoverable when its library
// is being loaded into a running process.
RCLCPP_COMPONENTS_REGISTER_NODE(rtabmap_ros::PointCloudAggregator)

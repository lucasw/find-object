/*
Copyright (c) 2011-2014, Mathieu Labbe - IntRoLab - Universite de Sherbrooke
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

#include "CameraROS.h"
#include "find_object/Settings.h"

#include <opencv2/imgproc/imgproc.hpp>
#include <sensor_msgs/image_encodings.h>

using namespace find_object;

CameraROS::CameraROS(bool subscribeDepth, QObject * parent) :
	Camera(parent),
	subscribeDepth_(subscribeDepth),
	approxSync_(0),
	exactSync_(0)
{
	ros::NodeHandle nh; // public
	ros::NodeHandle pnh("~"); // private

	qRegisterMetaType<ros::Time>("ros::Time");
	qRegisterMetaType<cv::Mat>("cv::Mat");

	if(!subscribeDepth_)
	{
		image_transport::ImageTransport it(nh);
		imageSub_ = it.subscribe(nh.resolveName("image"), 1, &CameraROS::imgReceivedCallback, this);
	}
	else
	{
		int queueSize = 10;
		bool approxSync = true;
		pnh.param("queue_size", queueSize, queueSize);
		pnh.param("approx_sync", approxSync, approxSync);
		ROS_INFO("find_object_ros: queue_size = %d", queueSize);
		ROS_INFO("find_object_ros: approx_sync = %s", approxSync?"true":"false");

		ros::NodeHandle rgb_nh(nh, "rgb");
		ros::NodeHandle rgb_pnh(pnh, "rgb");
		ros::NodeHandle depth_nh(nh, "depth_registered");
		ros::NodeHandle depth_pnh(pnh, "depth_registered");
		image_transport::ImageTransport rgb_it(rgb_nh);
		image_transport::ImageTransport depth_it(depth_nh);
		image_transport::TransportHints hintsRgb("raw", ros::TransportHints(), rgb_pnh);
		image_transport::TransportHints hintsDepth("raw", ros::TransportHints(), depth_pnh);

		rgbSub_.subscribe(rgb_it, rgb_nh.resolveName("image_rect_color"), 1, hintsRgb);
		depthSub_.subscribe(depth_it, depth_nh.resolveName("image_raw"), 1, hintsDepth);
		cameraInfoSub_.subscribe(depth_nh, "camera_info", 1);
		if(approxSync)
		{
			approxSync_ = new message_filters::Synchronizer<MyApproxSyncPolicy>(MyApproxSyncPolicy(queueSize), rgbSub_, depthSub_, cameraInfoSub_);
			approxSync_->registerCallback(boost::bind(&CameraROS::imgDepthReceivedCallback, this,
					boost::placeholders::_1, boost::placeholders::_2, boost::placeholders::_3));
		}
		else
		{
			exactSync_ = new message_filters::Synchronizer<MyExactSyncPolicy>(MyExactSyncPolicy(queueSize), rgbSub_, depthSub_, cameraInfoSub_);
			exactSync_->registerCallback(boost::bind(&CameraROS::imgDepthReceivedCallback, this,
					boost::placeholders::_1, boost::placeholders::_2, boost::placeholders::_3));
		}
	}
}
CameraROS::~CameraROS()
{
	delete approxSync_;
	delete exactSync_;
}

QStringList CameraROS::subscribedTopics() const
{
	QStringList topics;
	if(subscribeDepth_)
	{
		topics.append(rgbSub_.getTopic().c_str());
		topics.append(depthSub_.getTopic().c_str());
		topics.append(cameraInfoSub_.getTopic().c_str());
	}
	else
	{
		topics.append(imageSub_.getTopic().c_str());
	}
	return topics;
}

bool CameraROS::start()
{
	this->startTimer();
	return true;
}

void CameraROS::stop()
{
	this->stopTimer();
}

void CameraROS::takeImage()
{
	ros::spinOnce();
}

void CameraROS::imgReceivedCallback(const sensor_msgs::ImageConstPtr & msg)
{
	if(msg->data.size())
	{
		cv::Mat image;
		cv_bridge::CvImageConstPtr imgPtr = cv_bridge::toCvShare(msg);
		try
		{
			if(msg->encoding.compare(sensor_msgs::image_encodings::MONO8) == 0 ||
			   msg->encoding.compare(sensor_msgs::image_encodings::MONO16) == 0)
			{
				image = cv_bridge::cvtColor(imgPtr, "mono8")->image;
			}
			else
			{
				image = cv_bridge::cvtColor(imgPtr, "bgr8")->image;
			}

			Q_EMIT imageReceived(image, Header(msg->header.frame_id.c_str(), msg->header.stamp.sec, msg->header.stamp.nsec), cv::Mat(), 0.0f);
		}
		catch(const cv_bridge::Exception & e)
		{
			ROS_ERROR("find_object_ros: Could not convert input image to mono8 or bgr8 format, encoding detected is %s... cv_bridge exception: %s", msg->encoding.c_str(), e.what());
		}
	}
}

void CameraROS::imgDepthReceivedCallback(
		const sensor_msgs::ImageConstPtr& rgbMsg,
		const sensor_msgs::ImageConstPtr& depthMsg,
		const sensor_msgs::CameraInfoConstPtr& cameraInfoMsg)
{
	if(depthMsg->encoding.compare(sensor_msgs::image_encodings::TYPE_16UC1)!=0 &&
	   depthMsg->encoding.compare(sensor_msgs::image_encodings::TYPE_32FC1)!=0)
	{
			ROS_ERROR("find_object_ros: Depth image type must be 32FC1 or 16UC1");
			return;
	}

	if(rgbMsg->data.size())
	{
		cv_bridge::CvImageConstPtr ptr = cv_bridge::toCvShare(rgbMsg);
		cv_bridge::CvImageConstPtr ptrDepth = cv_bridge::toCvShare(depthMsg);
		float depthConstant = 1.0f/cameraInfoMsg->K[4];

		cv::Mat image;
		cv_bridge::CvImageConstPtr imgPtr = cv_bridge::toCvShare(rgbMsg);
		try
		{
			if(rgbMsg->encoding.compare(sensor_msgs::image_encodings::MONO8) == 0 ||
			   rgbMsg->encoding.compare(sensor_msgs::image_encodings::MONO16) == 0)
			{
				image = cv_bridge::cvtColor(imgPtr, "mono8")->image;
			}
			else
			{
				image = cv_bridge::cvtColor(imgPtr, "bgr8")->image;
			}

			Q_EMIT imageReceived(image, Header(rgbMsg->header.frame_id.c_str(), rgbMsg->header.stamp.sec, rgbMsg->header.stamp.nsec), ptrDepth->image, depthConstant);
		}
		catch(const cv_bridge::Exception & e)
		{
			ROS_ERROR("find_object_ros: Could not convert input image to mono8 or bgr8 format, encoding detected is %s... cv_bridge exception: %s", rgbMsg->encoding.c_str(), e.what());
		}
	}


}

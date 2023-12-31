#include "imageProjection.hpp"

namespace lio_sam
{
ImageProjection::ImageProjection() : deskewFlag( 0 )
{
  subImu        = nh.subscribe<sensor_msgs::Imu>( imuTopic, 2000, &ImageProjection::imuHandler, this, ros::TransportHints().tcpNoDelay() );
  subOdom       = nh.subscribe<nav_msgs::Odometry>( odomTopic + "_incremental", 2000, &ImageProjection::odometryHandler, this, ros::TransportHints().tcpNoDelay() );
  subLaserCloud = nh.subscribe<sensor_msgs::PointCloud2>( pointCloudTopic, 5, &ImageProjection::cloudHandler, this, ros::TransportHints().tcpNoDelay() );

  pubExtractedCloud = nh.advertise<sensor_msgs::PointCloud2>( "lio_sam/deskew/cloud_deskewed", 1 );
  pubLaserCloudInfo = nh.advertise<lio_sam::cloud_info>( "lio_sam/deskew/cloud_info", 1 );

  allocateMemory();
  resetParameters();

  pcl::console::setVerbosityLevel( pcl::console::L_ERROR );
}

void ImageProjection::allocateMemory()
{
  laserCloudIn.reset( new pcl::PointCloud<PointXYZIRT>() );
  tmpOusterCloudIn.reset( new pcl::PointCloud<OusterPointXYZIRT>() );
  fullCloud.reset( new pcl::PointCloud<PointType>() );
  extractedCloud.reset( new pcl::PointCloud<PointType>() );

  fullCloud->points.resize( N_SCAN * Horizon_SCAN );

  cloudInfo.startRingIndex.assign( N_SCAN, 0 );
  cloudInfo.endRingIndex.assign( N_SCAN, 0 );

  cloudInfo.pointColInd.assign( N_SCAN * Horizon_SCAN, 0 );
  cloudInfo.pointRange.assign( N_SCAN * Horizon_SCAN, 0 );

  resetParameters();
}

void ImageProjection::resetParameters()
{
  // firstFlag = true;

  laserCloudIn->clear();
  extractedCloud->clear();
  // reset range matrix for range image projection
  rangeMat = cv::Mat( N_SCAN, Horizon_SCAN, CV_32F, cv::Scalar::all( FLT_MAX ) );

  imuPointerCur  = 0;
  firstPointFlag = true;
  odomDeskewFlag = false;

  for ( int i = 0; i < queueLength; ++i )
  {
    imuTime[ i ] = 0;
    imuRotX[ i ] = 0;
    imuRotY[ i ] = 0;
    imuRotZ[ i ] = 0;
  }

  columnIdnCountVec.assign( N_SCAN, 0 );
}

ImageProjection::~ImageProjection() {}

void ImageProjection::imuHandler( const sensor_msgs::Imu::ConstPtr &imuMsg )
{
  sensor_msgs::Imu thisImu = imuConverter( *imuMsg );

  std::lock_guard<std::mutex> lock1( imuLock );
  imuQueue.push_back( thisImu );

  // debug IMU data
  // std::cout << std::setprecision(6);
  // std::cout << "IMU acc: " << std::endl;
  // std::cout << "x: " << thisImu.linear_acceleration.x <<
  //       ", y: " << thisImu.linear_acceleration.y <<
  //       ", z: " << thisImu.linear_acceleration.z << std::endl;
  // std::cout << "IMU gyro: " << std::endl;
  // std::cout << "x: " << thisImu.angular_velocity.x <<
  //       ", y: " << thisImu.angular_velocity.y <<
  //       ", z: " << thisImu.angular_velocity.z << std::endl;
  // double imuRoll, imuPitch, imuYaw;
  // tf::Quaternion orientation;
  // tf::quaternionMsgToTF(thisImu.orientation, orientation);
  // tf::Matrix3x3(orientation).getRPY(imuRoll, imuPitch, imuYaw);
  // std::cout << "IMU roll pitch yaw: " << std::endl;
  // std::cout << "roll: " << imuRoll << ", pitch: " << imuPitch << ", yaw: " << imuYaw << std::endl << std::endl;
}

void ImageProjection::odometryHandler( const nav_msgs::Odometry::ConstPtr &odometryMsg )
{
  std::lock_guard<std::mutex> lock2( odoLock );
  odomQueue.push_back( *odometryMsg );
}

void ImageProjection::cloudHandler( const sensor_msgs::PointCloud2ConstPtr &laserCloudMsg )
{
  if ( !cachePointCloud( laserCloudMsg ) )
  {
    return;
  }

  if ( !deskewInfo() )
  {
    return;
  }

  projectPointCloud();

  cloudExtraction();

  publishClouds();

  resetParameters();
}

bool ImageProjection::cachePointCloud( const sensor_msgs::PointCloud2ConstPtr &laserCloudMsg )
{
  // cache point cloud
  cloudQueue.push_back( *laserCloudMsg );
  if ( cloudQueue.size() <= 2 )
  {
    return false;
  }

  // convert cloud
  currentCloudMsg = std::move( cloudQueue.front() );
  cloudQueue.pop_front();
  if ( sensor == SensorType::VELODYNE || sensor == SensorType::LIVOX )
  {
    pcl::moveFromROSMsg( currentCloudMsg, *laserCloudIn );
  }
  else if ( sensor == SensorType::LEISHEN )
  {
    ROS_INFO_STREAM( BOLDGREEN << "LEISHEN point cloud received. Calculating Point Time." << RESET );
    if ( firstFlag )
    {
      firstFlag     = false;
      timePrev      = currentCloudMsg.header.stamp.toSec();
      timeIncrement = 0.1;
    }
    else
    {
      timeIncrement = currentCloudMsg.header.stamp.toSec() - timePrev;
      timePrev      = currentCloudMsg.header.stamp.toSec();
    }

    pcl::moveFromROSMsg( currentCloudMsg, *laserCloudIn );
    std::vector<double> pointAngle;
    for ( std::size_t i = 0; i < laserCloudIn->points.size(); ++i )
    {
      double angle = atan2( laserCloudIn->points[ i ].y, laserCloudIn->points[ i ].x );
      pointAngle.push_back( angle );
    }
    double startAngle = *std::min_element( pointAngle.begin(), pointAngle.end() );
    double endAngle   = *std::max_element( pointAngle.begin(), pointAngle.end() );
    double angleRange = endAngle - startAngle;

    if ( angleRange < 0 )
    {
      angleRange += 2 * M_PI;
    }

    for ( std::size_t i = 0; i < laserCloudIn->points.size(); ++i )
    {
      double angle                   = pointAngle[ i ];
      double time                    = ( angle - startAngle ) / angleRange * timeIncrement;
      laserCloudIn->points[ i ].time = time;
    }

    std::sort( laserCloudIn->begin(), laserCloudIn->end(), []( const PointXYZIRT &a, const PointXYZIRT &b ) { return a.time < b.time; } );
  }
  else if ( sensor == SensorType::OUSTER )
  {
    // Convert to Velodyne format
    pcl::moveFromROSMsg( currentCloudMsg, *tmpOusterCloudIn );
    laserCloudIn->points.resize( tmpOusterCloudIn->size() );
    laserCloudIn->is_dense = tmpOusterCloudIn->is_dense;
    for ( size_t i = 0; i < tmpOusterCloudIn->size(); i++ )
    {
      auto &src     = tmpOusterCloudIn->points[ i ];
      auto &dst     = laserCloudIn->points[ i ];
      dst.x         = src.x;
      dst.y         = src.y;
      dst.z         = src.z;
      dst.intensity = src.intensity;
      dst.ring      = src.ring;
      dst.time      = src.t * 1e-9f;
    }
  }
  else
  {
    ROS_ERROR_STREAM( "Unknown sensor type: " << int( sensor ) );
    ros::shutdown();
  }

  // get timestamp
  cloudHeader = currentCloudMsg.header;
  timeScanCur = cloudHeader.stamp.toSec();
  timeScanEnd = timeScanCur + laserCloudIn->points.back().time;

  // check dense flag
  if ( laserCloudIn->is_dense == false )
  {
    ROS_ERROR( "Point cloud is not in dense format, please remove NaN points first!" );
    ros::shutdown();
  }

  // check ring channel
  static int ringFlag = 0;
  if ( ringFlag == 0 )
  {
    ringFlag = -1;
    for ( int i = 0; i < (int)currentCloudMsg.fields.size(); ++i )
    {
      if ( currentCloudMsg.fields[ i ].name == "ring" )
      {
        ringFlag = 1;
        break;
      }
    }
    if ( ringFlag == -1 )
    {
      ROS_ERROR( "Point cloud ring channel not available, please configure your point cloud data!" );
      ros::shutdown();
    }
  }

  // check point time
  if ( deskewFlag == 0 )
  {
    deskewFlag = -1;
    for ( auto &field : currentCloudMsg.fields )
    {
      if ( field.name == "time" || field.name == "t" )
      {
        deskewFlag = 1;
        break;
      }
    }
    if ( deskewFlag == -1 )
    {
      ROS_WARN( "Point cloud timestamp not available, deskew function disabled, system will drift significantly!" );
    }
  }

  return true;
}

bool ImageProjection::deskewInfo()
{
  std::lock_guard<std::mutex> lock1( imuLock );
  std::lock_guard<std::mutex> lock2( odoLock );

  // make sure IMU data available for the scan
  if ( imuQueue.empty() || imuQueue.front().header.stamp.toSec() > timeScanCur || imuQueue.back().header.stamp.toSec() < timeScanEnd )
  {
    ROS_INFO_STREAM( BOLDYELLOW << "Watiting for IMU data ..." << RESET );
    return false;
  }

  imuDeskewInfo();

  odomDeskewInfo();

  return true;
}

void ImageProjection::imuDeskewInfo()
{
  cloudInfo.imuAvailable = false;

  while ( !imuQueue.empty() )
  {
    if ( imuQueue.front().header.stamp.toSec() < timeScanCur - 0.01 )
    {
      imuQueue.pop_front();
    }
    else
    {
      break;
    }
  }

  if ( imuQueue.empty() )
  {
    return;
  }

  imuPointerCur = 0;

  for ( int i = 0; i < (int)imuQueue.size(); ++i )
  {
    sensor_msgs::Imu thisImuMsg     = imuQueue[ i ];
    double           currentImuTime = thisImuMsg.header.stamp.toSec();

    // get roll, pitch, and yaw estimation for this scan
    if ( imuType && currentImuTime <= timeScanCur )
    {
      imuRPY2rosRPY( &thisImuMsg, &cloudInfo.imuRollInit, &cloudInfo.imuPitchInit, &cloudInfo.imuYawInit );
    }

    if ( currentImuTime > timeScanEnd + 0.01 )
    {
      break;
    }

    if ( imuPointerCur == 0 )
    {
      imuRotX[ 0 ] = 0;
      imuRotY[ 0 ] = 0;
      imuRotZ[ 0 ] = 0;
      imuTime[ 0 ] = currentImuTime;
      ++imuPointerCur;
      continue;
    }

    // get angular velocity
    double angular_x, angular_y, angular_z;
    imuAngular2rosAngular( &thisImuMsg, &angular_x, &angular_y, &angular_z );

    // integrate rotation
    double timeDiff          = currentImuTime - imuTime[ imuPointerCur - 1 ];
    imuRotX[ imuPointerCur ] = imuRotX[ imuPointerCur - 1 ] + angular_x * timeDiff;
    imuRotY[ imuPointerCur ] = imuRotY[ imuPointerCur - 1 ] + angular_y * timeDiff;
    imuRotZ[ imuPointerCur ] = imuRotZ[ imuPointerCur - 1 ] + angular_z * timeDiff;
    imuTime[ imuPointerCur ] = currentImuTime;
    ++imuPointerCur;
  }

  --imuPointerCur;

  if ( imuPointerCur <= 0 )
  {
    return;
  }

  cloudInfo.imuAvailable = true;
}

void ImageProjection::odomDeskewInfo()
{
  cloudInfo.odomAvailable = false;

  // lio-sam 默认是500hz，0.01s就是5帧数据
  // 那么其他IMU也应该给他们5帧数据
  // 0.01 = 5 / 500hz
  static float timeDiff = 5.0f / imuRate;
  while ( !odomQueue.empty() )
  {
    // if ( odomQueue.front().header.stamp.toSec() < timeScanCur - 0.01 )
    if ( odomQueue.front().header.stamp.toSec() < timeScanCur - timeDiff )
    {
      odomQueue.pop_front();
    }
    else
    {
      break;
    }
  }

  if ( odomQueue.empty() )
  {
    return;
  }

  if ( odomQueue.front().header.stamp.toSec() > timeScanCur )
  {
    return;
  }

  // get start odometry at the beinning of the scan
  nav_msgs::Odometry startOdomMsg;

  for ( int i = 0; i < (int)odomQueue.size(); ++i )
  {
    startOdomMsg = odomQueue[ i ];

    if ( ROS_TIME( &startOdomMsg ) < timeScanCur )
    {
      continue;
    }
    else
    {
      break;
    }
  }

  // tf::Quaternion orientation;
  // tf::quaternionMsgToTF( startOdomMsg.pose.pose.orientation, orientation );

  // double roll, pitch, yaw;
  // tf::Matrix3x3( orientation ).getRPY( roll, pitch, yaw );

  tf2::Quaternion orientation;
  tf2::convert( startOdomMsg.pose.pose.orientation, orientation );

  double roll, pitch, yaw;
  tf2::Matrix3x3( orientation ).getRPY( roll, pitch, yaw );

  // Initial guess used in mapOptimization
  cloudInfo.initialGuessX     = startOdomMsg.pose.pose.position.x;
  cloudInfo.initialGuessY     = startOdomMsg.pose.pose.position.y;
  cloudInfo.initialGuessZ     = startOdomMsg.pose.pose.position.z;
  cloudInfo.initialGuessRoll  = roll;
  cloudInfo.initialGuessPitch = pitch;
  cloudInfo.initialGuessYaw   = yaw;

  cloudInfo.odomAvailable = true;

  // get end odometry at the end of the scan
  odomDeskewFlag = false;

  if ( odomQueue.back().header.stamp.toSec() < timeScanEnd )
  {
    return;
  }
  nav_msgs::Odometry endOdomMsg;

  for ( int i = 0; i < (int)odomQueue.size(); ++i )
  {
    endOdomMsg = odomQueue[ i ];

    if ( ROS_TIME( &endOdomMsg ) < timeScanEnd )
    {
      continue;
    }
    else
    {
      break;
    }
  }

  if ( int( round( startOdomMsg.pose.covariance[ 0 ] ) ) != int( round( endOdomMsg.pose.covariance[ 0 ] ) ) )
  {
    return;
  }
  Eigen::Affine3f transBegin = pcl::getTransformation( startOdomMsg.pose.pose.position.x, startOdomMsg.pose.pose.position.y, startOdomMsg.pose.pose.position.z, roll, pitch, yaw );

  // tf::quaternionMsgToTF( endOdomMsg.pose.pose.orientation, orientation );
  // tf::Matrix3x3( orientation ).getRPY( roll, pitch, yaw );

  tf2::convert( endOdomMsg.pose.pose.orientation, orientation );
  tf2::Matrix3x3( orientation ).getRPY( roll, pitch, yaw );

  Eigen::Affine3f transEnd = pcl::getTransformation( endOdomMsg.pose.pose.position.x, endOdomMsg.pose.pose.position.y, endOdomMsg.pose.pose.position.z, roll, pitch, yaw );

  Eigen::Affine3f transBt = transBegin.inverse() * transEnd;

  float rollIncre, pitchIncre, yawIncre;
  pcl::getTranslationAndEulerAngles( transBt, odomIncreX, odomIncreY, odomIncreZ, rollIncre, pitchIncre, yawIncre );

  odomDeskewFlag = true;
}

void ImageProjection::findRotation( double pointTime, float *rotXCur, float *rotYCur, float *rotZCur )
{
  *rotXCur = 0;
  *rotYCur = 0;
  *rotZCur = 0;

  int imuPointerFront = 0;
  while ( imuPointerFront < imuPointerCur )
  {
    if ( pointTime < imuTime[ imuPointerFront ] )
    {
      break;
    }
    ++imuPointerFront;
  }

  if ( pointTime > imuTime[ imuPointerFront ] || imuPointerFront == 0 )
  {
    *rotXCur = imuRotX[ imuPointerFront ];
    *rotYCur = imuRotY[ imuPointerFront ];
    *rotZCur = imuRotZ[ imuPointerFront ];
  }
  else
  {
    int    imuPointerBack = imuPointerFront - 1;
    double ratioFront     = ( pointTime - imuTime[ imuPointerBack ] ) / ( imuTime[ imuPointerFront ] - imuTime[ imuPointerBack ] );
    double ratioBack      = ( imuTime[ imuPointerFront ] - pointTime ) / ( imuTime[ imuPointerFront ] - imuTime[ imuPointerBack ] );
    *rotXCur              = imuRotX[ imuPointerFront ] * ratioFront + imuRotX[ imuPointerBack ] * ratioBack;
    *rotYCur              = imuRotY[ imuPointerFront ] * ratioFront + imuRotY[ imuPointerBack ] * ratioBack;
    *rotZCur              = imuRotZ[ imuPointerFront ] * ratioFront + imuRotZ[ imuPointerBack ] * ratioBack;
  }
}

void ImageProjection::findPosition( double relTime, float *posXCur, float *posYCur, float *posZCur )
{
  *posXCur = 0;
  *posYCur = 0;
  *posZCur = 0;

  // If the sensor moves relatively slow, like walking speed, positional deskew seems to have little benefits. Thus code below is commented.

  if ( cloudInfo.odomAvailable == false || odomDeskewFlag == false )
  {
    return;
  }

  float ratio = relTime / ( timeScanEnd - timeScanCur );

  *posXCur = ratio * odomIncreX;
  *posYCur = ratio * odomIncreY;
  *posZCur = ratio * odomIncreZ;
}

PointType ImageProjection::deskewPoint( PointType *point, double relTime )
{
  if ( deskewFlag == -1 || cloudInfo.imuAvailable == false )
  {
    ROS_WARN_STREAM( "deskewPoint is not available! Cause: deskewFlag =" << deskewFlag << ", cloudInfo.imuAvailable = " << cloudInfo.imuAvailable );
    return *point;
  }

  double pointTime = timeScanCur + relTime;

  float rotXCur, rotYCur, rotZCur;
  findRotation( pointTime, &rotXCur, &rotYCur, &rotZCur );

  float posXCur, posYCur, posZCur;
  findPosition( relTime, &posXCur, &posYCur, &posZCur );

  if ( firstPointFlag == true )
  {
    transStartInverse = ( pcl::getTransformation( posXCur, posYCur, posZCur, rotXCur, rotYCur, rotZCur ) ).inverse();
    firstPointFlag    = false;
  }

  // transform points to start
  Eigen::Affine3f transFinal = pcl::getTransformation( posXCur, posYCur, posZCur, rotXCur, rotYCur, rotZCur );
  Eigen::Affine3f transBt    = transStartInverse * transFinal;

  PointType newPoint;
  newPoint.x         = transBt( 0, 0 ) * point->x + transBt( 0, 1 ) * point->y + transBt( 0, 2 ) * point->z + transBt( 0, 3 );
  newPoint.y         = transBt( 1, 0 ) * point->x + transBt( 1, 1 ) * point->y + transBt( 1, 2 ) * point->z + transBt( 1, 3 );
  newPoint.z         = transBt( 2, 0 ) * point->x + transBt( 2, 1 ) * point->y + transBt( 2, 2 ) * point->z + transBt( 2, 3 );
  newPoint.intensity = point->intensity;

  return newPoint;
}

void ImageProjection::projectPointCloud()
{
  int cloudSize = laserCloudIn->points.size();
  // range image projection
  for ( int i = 0; i < cloudSize; ++i )
  {
    PointType thisPoint;
    thisPoint.x         = laserCloudIn->points[ i ].x;
    thisPoint.y         = laserCloudIn->points[ i ].y;
    thisPoint.z         = laserCloudIn->points[ i ].z;
    thisPoint.intensity = laserCloudIn->points[ i ].intensity;

    float range = pointDistance( thisPoint );
    if ( range < lidarMinRange || range > lidarMaxRange )
    {
      continue;
    }

    int rowIdn = laserCloudIn->points[ i ].ring;
    if ( rowIdn < 0 || rowIdn >= N_SCAN )
    {
      continue;
    }

    if ( rowIdn % downsampleRate != 0 )
    {
      continue;
    }

    int columnIdn = -1;
    if ( sensor == SensorType::VELODYNE || sensor == SensorType::LEISHEN || sensor == SensorType::OUSTER )
    {
      float        horizonAngle = atan2( thisPoint.x, thisPoint.y ) * 180 / M_PI;
      static float ang_res_x    = 360.0 / float( Horizon_SCAN );
      columnIdn                 = -round( ( horizonAngle - 90.0 ) / ang_res_x ) + Horizon_SCAN / 2;
      if ( columnIdn >= Horizon_SCAN )
      {
        columnIdn -= Horizon_SCAN;
      }
    }
    else if ( sensor == SensorType::LIVOX )
    {
      columnIdn = columnIdnCountVec[ rowIdn ];
      columnIdnCountVec[ rowIdn ] += 1;
    }

    if ( columnIdn < 0 || columnIdn >= Horizon_SCAN )
    {
      continue;
    }

    if ( rangeMat.at<float>( rowIdn, columnIdn ) != FLT_MAX )
    {
      continue;
    }

    thisPoint = deskewPoint( &thisPoint, laserCloudIn->points[ i ].time );

    // bug fixed: previously, the range is not properly compensated
    // rangeMat.at<float>( rowIdn, columnIdn ) = range;
    rangeMat.at<float>( rowIdn, columnIdn ) = pointDistance( thisPoint );

    int index                  = columnIdn + rowIdn * Horizon_SCAN;
    fullCloud->points[ index ] = thisPoint;
  }
}

void ImageProjection::cloudExtraction()
{
  int count = 0;
  // extract segmented cloud for lidar odometry
  for ( int i = 0; i < N_SCAN; ++i )
  {
    cloudInfo.startRingIndex[ i ] = count - 1 + 5;

    for ( int j = 0; j < Horizon_SCAN; ++j )
    {
      if ( rangeMat.at<float>( i, j ) != FLT_MAX )
      {
        // mark the points' column index for marking occlusion later
        cloudInfo.pointColInd[ count ] = j;
        // save range info
        cloudInfo.pointRange[ count ] = rangeMat.at<float>( i, j );
        // save extracted cloud
        extractedCloud->push_back( fullCloud->points[ j + i * Horizon_SCAN ] );
        // size of extracted cloud
        ++count;
      }
    }
    cloudInfo.endRingIndex[ i ] = count - 1 - 5;
  }
}

void ImageProjection::publishClouds()
{
  cloudInfo.header         = cloudHeader;
  cloudInfo.cloud_deskewed = publishCloud( pubExtractedCloud, extractedCloud, cloudHeader.stamp, lidarFrame );
  pubLaserCloudInfo.publish( cloudInfo );
}

}  // namespace lio_sam

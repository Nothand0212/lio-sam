#include "imuPreintegration.hpp"


namespace lio_sam
{
IMUPreintegration::IMUPreintegration()
{
  subImu      = nh.subscribe<sensor_msgs::Imu>( imuTopic, 2000, &IMUPreintegration::imuHandler, this, ros::TransportHints().tcpNoDelay() );
  subOdometry = nh.subscribe<nav_msgs::Odometry>( "lio_sam/mapping/odometry_incremental", 5, &IMUPreintegration::odometryHandler, this, ros::TransportHints().tcpNoDelay() );

  pubImuOdometry = nh.advertise<nav_msgs::Odometry>( odomTopic + "_incremental", 2000 );

  boost::shared_ptr<gtsam::PreintegrationParams> p = gtsam::PreintegrationParams::MakeSharedU( imuGravity );
  p->accelerometerCovariance                       = gtsam::Matrix33::Identity( 3, 3 ) * pow( imuAccNoise, 2 );  // acc white noise in continuous
  p->gyroscopeCovariance                           = gtsam::Matrix33::Identity( 3, 3 ) * pow( imuGyrNoise, 2 );  // gyro white noise in continuous
  p->integrationCovariance                         = gtsam::Matrix33::Identity( 3, 3 ) * pow( 1e-4, 2 );         // error committed in integrating position from velocities
  gtsam::imuBias::ConstantBias prior_imu_bias( ( gtsam::Vector( 6 ) << 0, 0, 0, 0, 0, 0 ).finished() );
  ;  // assume zero initial bias

  priorPoseNoise   = gtsam::noiseModel::Diagonal::Sigmas( ( gtsam::Vector( 6 ) << 1e-2, 1e-2, 1e-2, 1e-2, 1e-2, 1e-2 ).finished() );  // rad,rad,rad,m, m, m
  priorVelNoise    = gtsam::noiseModel::Isotropic::Sigma( 3, 1e4 );                                                                   // m/s
  priorBiasNoise   = gtsam::noiseModel::Isotropic::Sigma( 6, 1e-3 );                                                                  // 1e-2 ~ 1e-3 seems to be good
  correctionNoise  = gtsam::noiseModel::Diagonal::Sigmas( ( gtsam::Vector( 6 ) << 0.05, 0.05, 0.05, 0.1, 0.1, 0.1 ).finished() );     // rad,rad,rad,m, m, m
  correctionNoise2 = gtsam::noiseModel::Diagonal::Sigmas( ( gtsam::Vector( 6 ) << 1, 1, 1, 1, 1, 1 ).finished() );                    // rad,rad,rad,m, m, m
  // gravity
  priorGravityNoise = gtsam::noiseModel::Diagonal::Sigmas( ( gtsam::Vector( 2 ) << gravityNoise, gravityNoise ).finished() );

  noiseModelBetweenBias = ( gtsam::Vector( 6 ) << imuAccBiasN, imuAccBiasN, imuAccBiasN, imuGyrBiasN, imuGyrBiasN, imuGyrBiasN ).finished();

  imuIntegratorImu_ = new gtsam::PreintegratedImuMeasurements( p, prior_imu_bias );  // setting up the IMU integration for IMU message thread
  imuIntegratorOpt_ = new gtsam::PreintegratedImuMeasurements( p, prior_imu_bias );  // setting up the IMU integration for optimization
}

void IMUPreintegration::resetOptimization()
{
  gtsam::ISAM2Params optParameters;
  optParameters.relinearizeThreshold = 0.1;
  optParameters.relinearizeSkip      = 1;
  optimizer                          = gtsam::ISAM2( optParameters );

  // gtsam::NonlinearFactorGraph newGraphFactors;
  // graphFactors = newGraphFactors;

  // gtsam::Values NewGraphValues;
  // graphValues = NewGraphValues;
  graphFactors = gtsam::NonlinearFactorGraph();
  graphValues  = gtsam::Values();
}

void IMUPreintegration::resetParams()
{
  lastImuT_imu      = -1;
  doneFirstOpt      = false;
  systemInitialized = false;
}

void IMUPreintegration::trimOldIMUData()
{
  while ( !imuQueOpt.empty() )
  {
    if ( ROS_TIME( &imuQueOpt.front() ) < currentCorrectionTime - delta_t )
    {
      lastImuT_opt = ROS_TIME( &imuQueOpt.front() );
      imuQueOpt.pop_front();
    }
    else
    {
      break;
    }
  }
}

void IMUPreintegration::odometryHandler( const nav_msgs::Odometry::ConstPtr& odomMsg )
{
  std::lock_guard<std::mutex> lock( mtx );

  currentCorrectionTime = ROS_TIME( odomMsg );

  // make sure we have imu data to integrate
  if ( imuQueOpt.empty() )
  {
    return;
  }

  float        p_x        = odomMsg->pose.pose.position.x;
  float        p_y        = odomMsg->pose.pose.position.y;
  float        p_z        = odomMsg->pose.pose.position.z;
  float        r_x        = odomMsg->pose.pose.orientation.x;
  float        r_y        = odomMsg->pose.pose.orientation.y;
  float        r_z        = odomMsg->pose.pose.orientation.z;
  float        r_w        = odomMsg->pose.pose.orientation.w;
  bool         degenerate = (int)odomMsg->pose.covariance[ 0 ] == 1 ? true : false;
  gtsam::Pose3 lidarPose  = gtsam::Pose3( gtsam::Rot3::Quaternion( r_w, r_x, r_y, r_z ), gtsam::Point3( p_x, p_y, p_z ) );


  // 0. initialize system
  if ( systemInitialized == false )
  {
    resetOptimization();

    // pop old IMU message
    trimOldIMUData();
    // initial pose
    prevPose_ = lidarPose.compose( lidar2Imu );
    gtsam::PriorFactor<gtsam::Pose3> priorPose( X( 0 ), prevPose_, priorPoseNoise );
    graphFactors.add( priorPose );

    // initial velocity
    prevVel_ = gtsam::Vector3( 0, 0, 0 );
    gtsam::PriorFactor<gtsam::Vector3> priorVel( V( 0 ), prevVel_, priorVelNoise );
    graphFactors.add( priorVel );

    // initial bias
    prevBias_ = gtsam::imuBias::ConstantBias();
    gtsam::PriorFactor<gtsam::imuBias::ConstantBias> priorBias( B( 0 ), prevBias_, priorBiasNoise );
    graphFactors.add( priorBias );

    // add values
    graphValues.insert( X( 0 ), prevPose_ );
    graphValues.insert( V( 0 ), prevVel_ );
    graphValues.insert( B( 0 ), prevBias_ );

    // optimize once
    optimizer.update( graphFactors, graphValues );
    graphFactors.resize( 0 );
    graphValues.clear();

    imuIntegratorImu_->resetIntegrationAndSetBias( prevBias_ );
    imuIntegratorOpt_->resetIntegrationAndSetBias( prevBias_ );

    key               = 1;
    systemInitialized = true;
    return;
  }

  // reset graph for speed
  if ( key == resetPreintegrationNum )
  {
    // get updated noise before reset
    // the last is the newest
    gtsam::noiseModel::Gaussian::shared_ptr updatedPoseNoise = gtsam::noiseModel::Gaussian::Covariance( optimizer.marginalCovariance( X( key - 1 ) ) );
    gtsam::noiseModel::Gaussian::shared_ptr updatedVelNoise  = gtsam::noiseModel::Gaussian::Covariance( optimizer.marginalCovariance( V( key - 1 ) ) );
    gtsam::noiseModel::Gaussian::shared_ptr updatedBiasNoise = gtsam::noiseModel::Gaussian::Covariance( optimizer.marginalCovariance( B( key - 1 ) ) );

    // reset graph
    resetOptimization();

    // add pose
    gtsam::PriorFactor<gtsam::Pose3> priorPose( X( 0 ), prevPose_, updatedPoseNoise );
    graphFactors.add( priorPose );

    // add velocity
    gtsam::PriorFactor<gtsam::Vector3> priorVel( V( 0 ), prevVel_, updatedVelNoise );
    graphFactors.add( priorVel );

    // add bias
    gtsam::PriorFactor<gtsam::imuBias::ConstantBias> priorBias( B( 0 ), prevBias_, updatedBiasNoise );
    graphFactors.add( priorBias );

    // add gravity
    if ( gravityOptimizationFlag )
    {
      // TODO add gravity factor
      // 1. add a gravity estimate function, it will return a bool to indicate if the gravity is valid
      if ( estimateGravity() )
      {
        Eigen::Vector3d           negativeGravityVec = gravityInGlobalVec.normalized();
        gtsam::Unit3              gravityInGlobal( negativeGravityVec[ 0 ], negativeGravityVec[ 1 ], negativeGravityVec[ 2 ] );
        gtsam::Unit3              gravityReferencBody( 0.0f, 0.0f, -1.0f );
        gtsam::Pose3GravityFactor gravityFactor( X( 0 ), gravityInGlobal, priorGravityNoise, gravityReferencBody );
        graphFactors.add( gravityFactor );
      }
    }

    // add values
    graphValues.insert( X( 0 ), prevPose_ );
    graphValues.insert( V( 0 ), prevVel_ );
    graphValues.insert( B( 0 ), prevBias_ );

    // optimize once
    optimizer.update( graphFactors, graphValues );
    graphFactors.resize( 0 );
    graphValues.clear();

    key = 1;
  }


  // 1. integrate imu data and optimize
  while ( !imuQueOpt.empty() )
  {
    // pop and integrate imu data that is between two optimizations
    sensor_msgs::Imu* thisImu = &imuQueOpt.front();
    double            imuTime = ROS_TIME( thisImu );
    if ( imuTime < currentCorrectionTime - delta_t )
    {
      double dt = ( lastImuT_opt < 0 ) ? ( 1.0 / imuRate ) : ( imuTime - lastImuT_opt );
      imuIntegratorOpt_->integrateMeasurement(
          gtsam::Vector3( thisImu->linear_acceleration.x, thisImu->linear_acceleration.y, thisImu->linear_acceleration.z ),
          gtsam::Vector3( thisImu->angular_velocity.x, thisImu->angular_velocity.y, thisImu->angular_velocity.z ),
          dt );

      lastImuT_opt = imuTime;
      imuQueOpt.pop_front();
    }
    else
    {
      break;
    }
  }

  // add imu factor to graph
  const gtsam::PreintegratedImuMeasurements& preint_imu = dynamic_cast<const gtsam::PreintegratedImuMeasurements&>( *imuIntegratorOpt_ );

  gtsam::ImuFactor imu_factor( X( key - 1 ), V( key - 1 ), X( key ), V( key ), B( key - 1 ), preint_imu );
  graphFactors.add( imu_factor );
  // add imu bias between factor
  graphFactors.add( gtsam::BetweenFactor<gtsam::imuBias::ConstantBias>( B( key - 1 ), B( key ), gtsam::imuBias::ConstantBias(), gtsam::noiseModel::Diagonal::Sigmas( sqrt( imuIntegratorOpt_->deltaTij() ) * noiseModelBetweenBias ) ) );

  // add pose factor
  gtsam::Pose3                     curPose = lidarPose.compose( lidar2Imu );
  gtsam::PriorFactor<gtsam::Pose3> pose_factor( X( key ), curPose, degenerate ? correctionNoise2 : correctionNoise );
  graphFactors.add( pose_factor );

  // add gravity
  if ( gravityOptimizationFlag )
  {
    // TODO add gravity factor
    // 1. add a gravity estimate function, it will return a bool to indicate if the gravity is valid
    if ( estimateGravity() && ( key - gravityEstimateWindowSize ) >= 0 )
    {
      Eigen::Vector3d           negativeGravityVec = gravityInGlobalVec.normalized();
      gtsam::Unit3              gravityInGlobal( negativeGravityVec[ 0 ], negativeGravityVec[ 1 ], negativeGravityVec[ 2 ] );
      gtsam::Unit3              gravityReferencBody( 0.0f, 0.0f, -1.0f );
      gtsam::Pose3GravityFactor gravityFactor( X( key - gravityEstimateWindowSize ), gravityInGlobal, priorGravityNoise, gravityReferencBody );
      graphFactors.add( gravityFactor );
    }
  }

  // ROS_INFO_STREAM( "Velocity Before Optimization: " << prevVel_[ 0 ] << " " << prevVel_[ 1 ] << " " << prevVel_[ 2 ] );

  // insert predicted values
  gtsam::NavState propState_ = imuIntegratorOpt_->predict( prevState_, prevBias_ );
  graphValues.insert( X( key ), propState_.pose() );
  graphValues.insert( V( key ), propState_.v() );
  graphValues.insert( B( key ), prevBias_ );

  // optimize
  optimizer.update( graphFactors, graphValues );
  optimizer.update();
  graphFactors.resize( 0 );
  graphValues.clear();

  // Overwrite the beginning of the preintegration for the next step.
  gtsam::Values result = optimizer.calculateEstimate();
  prevPose_            = result.at<gtsam::Pose3>( X( key ) );
  prevVel_             = result.at<gtsam::Vector3>( V( key ) );
  prevState_           = gtsam::NavState( prevPose_, prevVel_ );
  prevBias_            = result.at<gtsam::imuBias::ConstantBias>( B( key ) );

  // ROS_INFO_STREAM( "Velocity After Optimization: " << prevVel_[ 0 ] << " " << prevVel_[ 1 ] << " " << prevVel_[ 2 ] );

  // Reset the optimization preintegration object.
  imuIntegratorOpt_->resetIntegrationAndSetBias( prevBias_ );
  // check optimization
  if ( failureDetection( prevVel_, prevBias_ ) )
  {
    resetParams();
    return;
  }

  // 2. after optiization, re-propagate imu odometry preintegration
  prevStateOdom = prevState_;
  prevBiasOdom  = prevBias_;
  // first pop imu message older than current correction data
  double lastImuQT = -1;
  while ( !imuQueImu.empty() && ROS_TIME( &imuQueImu.front() ) < currentCorrectionTime - delta_t )
  {
    lastImuQT = ROS_TIME( &imuQueImu.front() );
    imuQueImu.pop_front();
  }
  // repropogate
  if ( !imuQueImu.empty() )
  {
    // reset bias use the newly optimized bias
    imuIntegratorImu_->resetIntegrationAndSetBias( prevBiasOdom );
    // integrate imu message from the beginning of this optimization
    for ( int i = 0; i < (int)imuQueImu.size(); ++i )
    {
      sensor_msgs::Imu* thisImu = &imuQueImu[ i ];
      double            imuTime = ROS_TIME( thisImu );
      double            dt      = ( lastImuQT < 0 ) ? ( 1.0 / imuRate ) : ( imuTime - lastImuQT );

      imuIntegratorImu_->integrateMeasurement( gtsam::Vector3( thisImu->linear_acceleration.x, thisImu->linear_acceleration.y, thisImu->linear_acceleration.z ),
                                               gtsam::Vector3( thisImu->angular_velocity.x, thisImu->angular_velocity.y, thisImu->angular_velocity.z ), dt );
      lastImuQT = imuTime;
    }
  }

  ++key;
  doneFirstOpt = true;
}

bool IMUPreintegration::estimateGravity()
{
  // TODO add gravity estimation
  Eigen::Quaterniond rot = Eigen::Quaterniond( prevPose_.rotation().toQuaternion().w(), prevPose_.rotation().toQuaternion().x(), prevPose_.rotation().toQuaternion().y(), prevPose_.rotation().toQuaternion().z() );
  Eigen::Vector3d    pos = Eigen::Vector3d( prevPose_.x(), prevPose_.y(), prevPose_.z() );

  Eigen::Affine3d transform = Eigen::Affine3d( rot );
  transform.translation()   = pos;

  std::shared_ptr<gtsam::PreintegratedImuMeasurements> currentIntgrator;
  currentIntgrator.reset( new gtsam::PreintegratedImuMeasurements( *imuIntegratorOpt_ ) );
  TransformAndPreintegrator temp( transform, currentIntgrator );
  transformAndPreintegratorQueue.emplace_back( temp );
  imuGravityVec.emplace_back( Eigen::Vector3d( prevVel_.x(), prevVel_.y(), prevVel_.z() ) );

  if ( static_cast<int>( transformAndPreintegratorQueue.size() ) > gravityEstimateWindowSize + 1 )
  {
    transformAndPreintegratorQueue.pop_front();
    imuGravityVec.pop_front();

    transformAndPreintegratorQueueTemp.clear();
    transformAndPreintegratorQueueTemp = transformAndPreintegratorQueue;

    const auto& T_w_inv = transformAndPreintegratorQueue.front().transform.inverse();

    for ( std::size_t i = 0; i < transformAndPreintegratorQueue.size(); ++i )
    {
      auto& transAndInregrator = transformAndPreintegratorQueueTemp[ i ];
      // transform to start frame of window
      transAndInregrator.transform = T_w_inv * transAndInregrator.transform;
      // transform to current imu frame
      const auto& R_w_inv = transformAndPreintegratorQueue[ i ].transform.linear().inverse();
      imuGravityVec[ i ]  = R_w_inv * imuGravityVec[ i ];
    }

    // TODO calculate gravity velocity in IMU frame and return true if it is valid
    if ( gravityEstimator.Estimate( transformAndPreintegratorQueueTemp, transform_l_b, imuGravityVec, static_cast<double>( imuGravity ), gravityInBodyVec ) )
    {
      gravityInGlobalVec = transformAndPreintegratorQueue.front().transform.linear() * ( -gravityInBodyVec );

      if ( gravityInGlobalVec[ 2 ] + imuGravity < 0.5 )
      {
        ROS_INFO_STREAM( BOLDMAGENTA << "Succese\t"
                                     << "gravityInGlobalVec: " << gravityInGlobalVec.transpose() << RESET );
        // LOG(INFO) << g_vec_est_G_[0]<<","<<g_vec_est_G_[1]<<","<<g_vec_est_G_[2];
        return true;
      }
      else
      {
        ROS_INFO_STREAM( BOLDRED << "Fail:\t"
                                 << "gravityInGlobalVec: " << gravityInGlobalVec.transpose() << RESET );
        return false;
      }
    }
  }

  return false;
}

bool IMUPreintegration::failureDetection( const gtsam::Vector3& velCur, const gtsam::imuBias::ConstantBias& biasCur )
{
  Eigen::Vector3f vel( velCur.x(), velCur.y(), velCur.z() );
  if ( vel.norm() > 30 )
  {
    ROS_WARN( "Large velocity, reset IMU-preintegration!" );
    return true;
  }

  Eigen::Vector3f ba( biasCur.accelerometer().x(), biasCur.accelerometer().y(), biasCur.accelerometer().z() );
  Eigen::Vector3f bg( biasCur.gyroscope().x(), biasCur.gyroscope().y(), biasCur.gyroscope().z() );
  if ( ba.norm() > 1.0 || bg.norm() > 1.0 )
  {
    ROS_WARN( "Large bias, reset IMU-preintegration!" );
    return true;
  }

  return false;
}

void IMUPreintegration::imuHandler( const sensor_msgs::Imu::ConstPtr& imu_raw )
{
  std::lock_guard<std::mutex> lock( mtx );

  sensor_msgs::Imu thisImu = imuConverter( *imu_raw );

  imuQueOpt.push_back( thisImu );
  imuQueImu.push_back( thisImu );

  if ( doneFirstOpt == false )
  {
    return;
  }

  double imuTime = ROS_TIME( &thisImu );
  double dt      = ( lastImuT_imu < 0 ) ? ( 1.0 / imuRate ) : ( imuTime - lastImuT_imu );
  lastImuT_imu   = imuTime;

  // integrate this single imu message
  imuIntegratorImu_->integrateMeasurement( gtsam::Vector3( thisImu.linear_acceleration.x, thisImu.linear_acceleration.y, thisImu.linear_acceleration.z ),
                                           gtsam::Vector3( thisImu.angular_velocity.x, thisImu.angular_velocity.y, thisImu.angular_velocity.z ), dt );

  // predict odometry
  gtsam::NavState currentState = imuIntegratorImu_->predict( prevStateOdom, prevBiasOdom );

  // publish odometry
  nav_msgs::Odometry odometry;
  odometry.header.stamp    = thisImu.header.stamp;
  odometry.header.frame_id = odometryFrame;
  odometry.child_frame_id  = "odom_imu";

  // transform imu pose to ldiar
  gtsam::Pose3 imuPose   = gtsam::Pose3( currentState.quaternion(), currentState.position() );
  gtsam::Pose3 lidarPose = imuPose.compose( imu2Lidar );

  odometry.pose.pose.position.x    = lidarPose.translation().x();
  odometry.pose.pose.position.y    = lidarPose.translation().y();
  odometry.pose.pose.position.z    = lidarPose.translation().z();
  odometry.pose.pose.orientation.x = lidarPose.rotation().toQuaternion().x();
  odometry.pose.pose.orientation.y = lidarPose.rotation().toQuaternion().y();
  odometry.pose.pose.orientation.z = lidarPose.rotation().toQuaternion().z();
  odometry.pose.pose.orientation.w = lidarPose.rotation().toQuaternion().w();

  odometry.twist.twist.linear.x  = currentState.velocity().x();
  odometry.twist.twist.linear.y  = currentState.velocity().y();
  odometry.twist.twist.linear.z  = currentState.velocity().z();
  odometry.twist.twist.angular.x = thisImu.angular_velocity.x + prevBiasOdom.gyroscope().x();
  odometry.twist.twist.angular.y = thisImu.angular_velocity.y + prevBiasOdom.gyroscope().y();
  odometry.twist.twist.angular.z = thisImu.angular_velocity.z + prevBiasOdom.gyroscope().z();
  pubImuOdometry.publish( odometry );
}
}  // namespace lio_sam

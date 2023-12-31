/**
 *  @file   Pose3GravityFactor.h
 *  @author Frank Dellaert
 *  @brief  Header file for Attitude factor
 *  @date   January 28, 2014
 **/
#ifndef _CARTOGRAPHER_MAPPING_INTERNAL_3D_GRAVITY_FACTOR_H_
#define _CARTOGRAPHER_MAPPING_INTERNAL_3D_GRAVITY_FACTOR_H_

#include <gtsam/geometry/Pose3.h>
#include <gtsam/geometry/Unit3.h>
#include <gtsam/nonlinear/NonlinearFactor.h>
namespace gtsam
{
/**
 * Base class for prior on gravity
 * Example:
 * - measurement is direction of gravity in navigation frame nG
 * - reference is direction of z axis in body frame bF
 * This factor will give zero error if nG is opposite direction of bF
 * @addtogroup Navigation
 */
class GravityFactor
{
  _CARTOGRAPHER_MAPPING_INTERNAL_3D_GRAVITY_FACTOR_H_

protected:
  const Unit3 nZ_, bRef_;  ///< Position measurement in

public:
  /** default constructor - only use for serialization */
  GravityFactor()
  {
  }

  /**
   * @brief Constructor
   * @param nZ measured direction in navigation frame
   * @param bRef reference direction in body frame (default Z-axis in NED frame, i.e., [0; 0; 1])
   */
  GravityFactor( const Unit3& nZ, const Unit3& bRef = Unit3( 0, 0, 1 ) ) : nZ_( nZ ), bRef_( bRef )
  {
  }

  /** vector of errors */
  Vector attitudeError( const Rot3&            p,
                        OptionalJacobian<2, 3> H = boost::none ) const;

  /** Serialization function */
  friend class boost::serialization::access;
  template <class ARCHIVE>
  void serialize( ARCHIVE& ar, const unsigned int /*version*/ )
  {
    ar& boost::serialization::make_nvp( "nZ_", const_cast<Unit3&>( nZ_ ) );
    ar& boost::serialization::make_nvp( "bRef_", const_cast<Unit3&>( bRef_ ) );
  }
};

/**
 * Version of GravityFactor for Rot3
 * @addtogroup Navigation
 */
class GTSAM_EXPORT Rot3GravityFactor : public NoiseModelFactor1<Rot3>, public GravityFactor
{
  typedef NoiseModelFactor1<Rot3> Base;

public:
  /// shorthand for a smart pointer to a factor
  typedef boost::shared_ptr<Rot3GravityFactor> shared_ptr;

  /// Typedef to this class
  typedef Rot3GravityFactor This;

  /** default constructor - only use for serialization */
  Rot3GravityFactor()
  {
  }

  virtual ~Rot3GravityFactor()
  {
  }

  /**
   * @brief Constructor
   * @param key of the Rot3 variable that will be constrained
   * @param nZ measured direction in navigation frame (remove yaw before rotating the gravity vector)
   * @param model Gaussian noise model
   * @param bRef reference direction in body frame (default Z-axis)
   */
  Rot3GravityFactor( Key key, const Unit3& nZ, const SharedNoiseModel& model,
                     const Unit3& bRef = Unit3( 0, 0, 1 ) ) : Base( model, key ), GravityFactor( nZ, bRef )
  {
  }

  /// @return a deep copy of this factor
  virtual gtsam::NonlinearFactor::shared_ptr clone() const
  {
    return boost::static_pointer_cast<gtsam::NonlinearFactor>(
        gtsam::NonlinearFactor::shared_ptr( new This( *this ) ) );
  }

  /** print */
  virtual void print( const std::string& s, const KeyFormatter& keyFormatter =
                                                DefaultKeyFormatter ) const;

  /** equals */
  virtual bool equals( const NonlinearFactor& expected, double tol = 1e-9 ) const;

  /** vector of errors */
  virtual Vector evaluateError( const Rot3&              nRb,  //
                                boost::optional<Matrix&> H = boost::none ) const
  {
    return attitudeError( nRb, H );
  }
  Unit3 nZ() const
  {
    return nZ_;
  }
  Unit3 bRef() const
  {
    return bRef_;
  }

private:
  /** Serialization function */
  friend class boost::serialization::access;
  template <class ARCHIVE>
  void serialize( ARCHIVE& ar, const unsigned int /*version*/ )
  {
    ar& boost::serialization::make_nvp( "NoiseModelFactor1",
                                        boost::serialization::base_object<Base>( *this ) );
    ar& boost::serialization::make_nvp( "GravityFactor",
                                        boost::serialization::base_object<GravityFactor>( *this ) );
  }

public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
};


/**
 * Version of GravityFactor for Pose3
 * @addtogroup Navigation
 */
class GTSAM_EXPORT Pose3GravityFactor : public NoiseModelFactor1<Pose3>,
                                        public GravityFactor
{
  typedef NoiseModelFactor1<Pose3> Base;

public:
  /// shorthand for a smart pointer to a factor
  typedef boost::shared_ptr<Pose3GravityFactor> shared_ptr;

  /// Typedef to this class
  typedef Pose3GravityFactor This;

  /** default constructor - only use for serialization */
  Pose3GravityFactor()
  {
  }

  virtual ~Pose3GravityFactor()
  {
  }

  /**
   * @brief Constructor
   * @param key of the Pose3 variable that will be constrained
   * @param nZ measured direction in navigation frame (remove yaw before rotating the gravity vector)
   * @param model Gaussian noise model
   * @param bRef reference direction in body frame (default Z-axis)
   */
  Pose3GravityFactor( Key key, const Unit3& nZ, const SharedNoiseModel& model,
                      const Unit3& bRef = Unit3( 0, 0, 1 ) ) : Base( model, key ), GravityFactor( nZ, bRef )
  {
  }

  /// @return a deep copy of this factor
  virtual gtsam::NonlinearFactor::shared_ptr clone() const
  {
    return boost::static_pointer_cast<gtsam::NonlinearFactor>(
        gtsam::NonlinearFactor::shared_ptr( new This( *this ) ) );
  }

  /** print */
  virtual void print( const std::string& s, const KeyFormatter& keyFormatter =
                                                DefaultKeyFormatter ) const;

  /** equals */
  virtual bool equals( const NonlinearFactor& expected, double tol = 1e-9 ) const;

  /** vector of errors */
  virtual Vector evaluateError( const Pose3&             nTb,  //
                                boost::optional<Matrix&> H = boost::none ) const
  {
    Vector e = attitudeError( nTb.rotation(), H );
    if ( H )
    {
      Matrix H23             = *H;
      *H                     = Matrix::Zero( 2, 6 );
      H->block<2, 3>( 0, 0 ) = H23;
    }
    return e;
  }
  Unit3 nZ() const
  {
    return nZ_;
  }
  Unit3 bRef() const
  {
    return bRef_;
  }

private:
  /** Serialization function */
  friend class boost::serialization::access;
  template <class ARCHIVE>
  void serialize( ARCHIVE& ar, const unsigned int /*version*/ )
  {
    ar& boost::serialization::make_nvp( "NoiseModelFactor1",
                                        boost::serialization::base_object<Base>( *this ) );
    ar& boost::serialization::make_nvp( "GravityFactor",
                                        boost::serialization::base_object<GravityFactor>( *this ) );
  }

public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
};

}  // namespace gtsam

#endif  //_CARTOGRAPHER_MAPPING_INTERNAL_3D_GRAVITY_FACTOR_H_
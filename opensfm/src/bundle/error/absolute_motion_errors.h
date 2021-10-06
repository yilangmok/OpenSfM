#pragma once

#include <bundle/bundle_adjuster.h>
#include <bundle/error/error_utils.h>
#include <bundle/error/position_functors.h>

#include <Eigen/Eigen>

namespace bundle {

template <class PosFunc>
struct AbsolutePositionError {
  AbsolutePositionError(const PosFunc& pos_func, const Vec3d& pos_prior,
                        double std_deviation_horizontal,
                        double std_deviation_vertical,
                        bool has_std_deviation_param,
                        const PositionConstraintType& type)
      : pos_func_(pos_func),
        pos_prior_(pos_prior),
        scale_xy_(1.0 / std_deviation_horizontal),
        scale_z_(1.0 / std_deviation_vertical),
        has_std_deviation_param_(has_std_deviation_param),
        type_(type) {}

  template <typename T>
  bool operator()(T const* const* p, T* r) const {
    Eigen::Map<Vec3<T>> residual(r);

    // error is : position_prior - adjusted_position
    residual = pos_prior_.cast<T>() - pos_func_(p);
    if (has_std_deviation_param_) {
      residual /= p[1][0];
    } else {
      residual[0] *= T(scale_xy_);
      residual[1] *= T(scale_xy_);
      residual[2] *= T(scale_z_);
    }

    // filter axises to use
    const std::vector<PositionConstraintType> axises = {
        PositionConstraintType::X, PositionConstraintType::Y,
        PositionConstraintType::Z};
    for (int i = 0; i < axises.size(); ++i) {
      if (!HasFlag(axises[i])) {
        residual(i) *= T(0.);
      }
    }
    return true;
  }

  inline bool HasFlag(const PositionConstraintType& flag) const {
    return (int(type_) & int(flag)) == int(flag);
  }

  PosFunc pos_func_;
  Vec3d pos_prior_;
  double scale_xy_;
  double scale_z_;
  bool has_std_deviation_param_;
  PositionConstraintType type_;
};

template <typename T>
T diff_between_angles(T a, T b) {
  T d = a - b;
  if (d > T(M_PI)) {
    return d - T(2 * M_PI);
  } else if (d < -T(M_PI)) {
    return d + T(2 * M_PI);
  } else {
    return d;
  }
}

struct UpVectorError {
  UpVectorError(const Vec3d& acceleration, double std_deviation,
                bool is_rig_shot)
      : is_rig_shot_(is_rig_shot), scale_(1.0 / std_deviation) {
    acceleration_ = acceleration.normalized();
  }

  template <typename T>
  bool operator()(T const* const* p, T* r) const {
    int instance_index = 0;
    int camera_index = FUNCTOR_NOT_SET;
    if (is_rig_shot_) {
      camera_index = 1;
    }
    Vec3<T> R = ShotRotationFunctor(instance_index, camera_index)(p);
    Eigen::Map<Vec3<T>> residual(r);

    const Vec3<T> acceleration = acceleration_.cast<T>();
    const Vec3<T> z_world = RotatePoint(R, acceleration);
    const Vec3<T> z_axis = Vec3d(0, 0, 1).cast<T>();
    residual = T(scale_) * (z_world - z_axis);
    return true;
  }

  Vec3d acceleration_;
  bool is_rig_shot_;
  double scale_;
};

struct PanAngleError {
  PanAngleError(double angle, double std_deviation)
      : angle_(angle), scale_(1.0 / std_deviation) {}

  template <typename T>
  bool operator()(const T* const shot, T* residuals) const {
    Vec3<T> R = ShotRotationFunctor(0, FUNCTOR_NOT_SET)(&shot);

    const Vec3<T> z_axis = Vec3d(0, 0, 1).cast<T>();
    const auto z_world = RotatePoint(R, z_axis);

    if (ceres::abs(z_world(0)) < T(1e-8) && ceres::abs(z_world(1)) < T(1e-8)) {
      residuals[0] = T(0.0);
    } else {
      const T predicted_angle = atan2(z_world(0), z_world(1));
      residuals[0] =
          T(scale_) * diff_between_angles(predicted_angle, T(angle_));
    }
    return true;
  }

  double angle_;
  double scale_;
};

struct TiltAngleError {
  TiltAngleError(double angle, double std_deviation)
      : angle_(angle), scale_(1.0 / std_deviation) {}

  template <typename T>
  bool operator()(const T* const shot, T* residuals) const {
    Vec3<T> R = ShotRotationFunctor(0, FUNCTOR_NOT_SET)(&shot);
    T ez[3] = {T(0), T(0), T(1)};  // ez: A point in front of the camera (z=1)
    T Rt_ez[3];
    ceres::AngleAxisRotatePoint(R.data(), ez, Rt_ez);

    T l = sqrt(Rt_ez[0] * Rt_ez[0] + Rt_ez[1] * Rt_ez[1]);
    T predicted_angle = -atan2(Rt_ez[2], l);

    residuals[0] = T(scale_) * diff_between_angles(predicted_angle, T(angle_));
    return true;
  }

  double angle_;
  double scale_;
};

struct RollAngleError {
  RollAngleError(double angle, double std_deviation)
      : angle_(angle), scale_(1.0 / std_deviation) {}

  template <typename T>
  bool operator()(const T* const shot, T* residuals) const {
    Vec3<T> R = ShotRotationFunctor(0, FUNCTOR_NOT_SET)(&shot);
    T ex[3] = {T(1), T(0), T(0)};  // A point to the right of the camera (x=1)
    T ez[3] = {T(0), T(0), T(1)};  // A point in front of the camera (z=1)
    T Rt_ex[3], Rt_ez[3];
    T tangle_ = T(angle_);
    ceres::AngleAxisRotatePoint(R.data(), ex, Rt_ex);
    ceres::AngleAxisRotatePoint(R.data(), ez, Rt_ez);

    T a[3] = {Rt_ez[1], -Rt_ez[0], T(0)};
    T la = sqrt(a[0] * a[0] + a[1] * a[1]);

    const double eps = 1e-5;
    if (la < eps) {
      residuals[0] = T(0.0);
      return true;
    }

    a[0] /= la;
    a[1] /= la;
    T b[3];
    ceres::CrossProduct(Rt_ex, a, b);
    T sin_roll = Rt_ez[0] * b[0] + Rt_ez[1] * b[1] + Rt_ez[2] * b[2];
    if (sin_roll <= -(1.0 - eps)) {
      residuals[0] = T(0.0);
      return true;
    }

    T predicted_angle = asin(sin_roll);
    residuals[0] = T(scale_) * diff_between_angles(predicted_angle, T(angle_));

    return true;
  }

  double angle_;
  double scale_;
};

struct PositionPriorError {
  PositionPriorError(double* position_prior, double std_deviation)
      : position_prior_(position_prior), scale_(1.0 / std_deviation) {}

  template <typename T>
  bool operator()(const T* const shot, const T* const bias,
                  T* residuals) const {
    Vec3<T> R = ShotRotationFunctor(0, FUNCTOR_NOT_SET)(&bias);
    Vec3<T> t = ShotPositionFunctor(0, FUNCTOR_NOT_SET)(&bias);
    const T* const scale = bias + Bias::Parameter::SCALE;

    Eigen::Map<Vec3d> prior(position_prior_);
    Eigen::Map<Vec3<T>> res(residuals);
    Vec3<T> optical_center = ShotPositionFunctor(0, FUNCTOR_NOT_SET)(&shot);

    res = T(scale_) *
          (optical_center -
           (scale[0] * RotatePoint(R.eval(), prior.cast<T>().eval()) + t));

    return true;
  }

  double* position_prior_;
  double scale_;
};

struct UnitTranslationPriorError {
  UnitTranslationPriorError() {}

  template <typename T>
  bool operator()(const T* const shot, T* residuals) const {
    const T* const t = shot + 3;
    residuals[0] = log(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    return true;
  }
};

struct PointPositionPriorError {
  PointPositionPriorError(double* position, double std_deviation)
      : position_(position), scale_(1.0 / std_deviation) {}

  template <typename T>
  bool operator()(const T* const p, T* residuals) const {
    residuals[0] = T(scale_) * (p[0] - T(position_[0]));
    residuals[1] = T(scale_) * (p[1] - T(position_[1]));
    residuals[2] = T(scale_) * (p[2] - T(position_[2]));
    return true;
  }

  double* position_;
  double scale_;
};

struct HeatmapdCostFunctor {
  explicit HeatmapdCostFunctor(
      const ceres::BiCubicInterpolator<ceres::Grid2D<double>>& interpolator,
      double x_offset, double y_offset, double height, double width,
      double resolution, double std_deviation)
      : interpolator_(interpolator),
        x_offset_(x_offset),
        y_offset_(y_offset),
        height_(height),
        width_(width),
        resolution_(resolution),
        scale_(1. / std_deviation) {}

  template <typename T>
  bool operator()(T const* p, T* residuals) const {
    Vec3<T> position = ShotPositionFunctor(0, FUNCTOR_NOT_SET)(&p);
    const T x_coor = position[0] - x_offset_;
    const T y_coor = position[1] - y_offset_;
    // const T z_coor = x[2]; - Z goes brrrrr
    const T row = height_ / 2. - (y_coor / resolution_);
    const T col = width_ / 2. + (x_coor / resolution_);
    interpolator_.Evaluate(row, col, residuals);
    residuals[0] *= scale_;
    return true;
  }

  static ceres::CostFunction* Create(
      const ceres::BiCubicInterpolator<ceres::Grid2D<double>>& interpolator,
      double x_offset, double y_offset, double height, double width,
      double heatmap_resolution, double std_deviation) {
    return new ceres::AutoDiffCostFunction<HeatmapdCostFunctor, 1, 6>(
        new HeatmapdCostFunctor(interpolator, x_offset, y_offset, height, width,
                                heatmap_resolution, std_deviation));
  }

 private:
  const ceres::BiCubicInterpolator<ceres::Grid2D<double>>& interpolator_;
  const double x_offset_;
  const double y_offset_;
  const double height_;
  const double width_;
  const double resolution_;
  const double scale_;
};
}  // namespace bundle
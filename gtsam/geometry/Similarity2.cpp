/* ----------------------------------------------------------------------------

 * GTSAM Copyright 2010, Georgia Tech Research Corporation,
 * Atlanta, Georgia 30332-0415
 * All Rights Reserved
 * Authors: Frank Dellaert, et al. (see THANKS for the full author list)

 * See LICENSE for the license information

 * -------------------------------------------------------------------------- */

/**
 * @file   Similarity2.cpp
 * @brief  Implementation of Similarity2 transform
 * @author John Lambert
 */

#include <gtsam/geometry/Similarity2.h>

#include <gtsam/geometry/Pose2.h>
#include <gtsam/base/Manifold.h>
#include <gtsam/slam/KarcherMeanFactor-inl.h>

namespace gtsam {

using std::vector;

namespace {
/// Subtract centroids from point pairs.
static Point2Pairs subtractCentroids(const Point2Pairs &abPointPairs,
                                    const Point2Pair &centroids) {
  Point2Pairs d_abPointPairs;
  for (const Point2Pair& abPair : abPointPairs) {
    Point2 da = abPair.first - centroids.first;
    Point2 db = abPair.second - centroids.second;
    d_abPointPairs.emplace_back(da, db);
  }
  return d_abPointPairs;
}

/// Form inner products x and y and calculate scale.
static const double calculateScale(const Point2Pairs &d_abPointPairs,
                                   const Rot2 &aRb) {
  double x = 0, y = 0;
  Point2 da, db;
  for (const Point2Pair& d_abPair : d_abPointPairs) {
    std::tie(da, db) = d_abPair;
    const Vector2 da_prime = aRb * db;
    y += da.transpose() * da_prime;
    x += da_prime.transpose() * da_prime;
  }
  const double s = y / x;
  return s;
}

/// Form outer product H.
static Matrix2 calculateH(const Point2Pairs &d_abPointPairs) {
  Matrix2 H = Z_2x2;
  for (const Point2Pair& d_abPair : d_abPointPairs) {
    H += d_abPair.first * d_abPair.second.transpose();
  }
  return H;
}

/// This method estimates the similarity transform from differences point pairs,
// given a known or estimated rotation and point centroids.
static Similarity2 align(const Point2Pairs &d_abPointPairs, const Rot2 &aRb,
                         const Point2Pair &centroids) {
  const double s = calculateScale(d_abPointPairs, aRb);
  // dividing aTb by s is required because the registration cost function
  // minimizes ||a - sRb - t||, whereas Sim(2) computes s(Rb + t)
  const Point2 aTb = (centroids.first - s * (aRb * centroids.second)) / s;
  return Similarity2(aRb, aTb, s);
}

/// This method estimates the similarity transform from point pairs, given a known or estimated rotation.
// Refer to: http://www5.informatik.uni-erlangen.de/Forschung/Publikationen/2005/Zinsser05-PSR.pdf Chapter 3
static Similarity2 alignGivenR(const Point2Pairs &abPointPairs,
                               const Rot2 &aRb) {
  auto centroids = means(abPointPairs);
  auto d_abPointPairs = subtractCentroids(abPointPairs, centroids);
  return align(d_abPointPairs, aRb, centroids);
}
}  // namespace

Similarity2::Similarity2() :
    t_(0,0), s_(1) {
}

Similarity2::Similarity2(double s) :
    t_(0,0), s_(s) {
}

Similarity2::Similarity2(const Rot2& R, const Point2& t, double s) :
    R_(R), t_(t), s_(s) {
}

Similarity2::Similarity2(const Matrix2& R, const Vector2& t, double s) :
    R_(R), t_(t), s_(s) {
}

Similarity2::Similarity2(const Matrix3& T) :
    R_(T.topLeftCorner<2, 2>()), t_(T.topRightCorner<2, 1>()), s_(1.0 / T(2, 2)) {
}

bool Similarity2::equals(const Similarity2& other, double tol) const {
  return R_.equals(other.R_, tol) && traits<Point3>::Equals(t_, other.t_, tol)
      && s_ < (other.s_ + tol) && s_ > (other.s_ - tol);
}

bool Similarity2::operator==(const Similarity2& other) const {
  return R_.matrix() == other.R_.matrix() && t_ == other.t_ && s_ == other.s_;
}

void Similarity2::print(const std::string& s) const {
  std::cout << std::endl;
  std::cout << s;
  rotation().print("\nR:\n");
  std::cout << "t: " << translation().transpose() << " s: " << scale() << std::endl;
}

Similarity2 Similarity2::identity() {
  return Similarity2();
}
Similarity2 Similarity2::operator*(const Similarity2& S) const {
  return Similarity2(R_ * S.R_, ((1.0 / S.s_) * t_) + R_ * S.t_, s_ * S.s_);
}

Similarity2 Similarity2::inverse() const {
  const Rot2 Rt = R_.inverse();
  const Point2 sRt = Rt * (-s_ * t_);
  return Similarity2(Rt, sRt, 1.0 / s_);
}

Point3 Similarity2::transformFrom(const Point2& p) const {
  const Point2 q = R_ * p + t_;
  return s_ * q;
}

Pose2 Similarity2::transformFrom(const Pose2& T) const {
  Rot2 R = R_.compose(T.rotation());
  Point2 t = Point2(s_ * (R_ * T.translation() + t_));
  return Pose2(R, t);
}

Point2 Similarity2::operator*(const Point2& p) const {
  return transformFrom(p);
}

Similarity2 Similarity2::Align(const Point2Pairs &abPointPairs) {
  // Refer to Chapter 3 of
  // http://www5.informatik.uni-erlangen.de/Forschung/Publikationen/2005/Zinsser05-PSR.pdf
  if (abPointPairs.size() < 2)
    throw std::runtime_error("input should have at least 2 pairs of points");
  auto centroids = means(abPointPairs);
  auto d_abPointPairs = subtractCentroids(abPointPairs, centroids);
  Matrix3 H = calculateH(d_abPointPairs);
  // ClosestTo finds rotation matrix closest to H in Frobenius sense
  Rot2 aRb = Rot2::ClosestTo(H);
  return align(d_abPointPairs, aRb, centroids);
}

Similarity2 Similarity2::Align(const vector<Pose3Pair> &abPosePairs) {
  const size_t n = abPosePairs.size();
  if (n < 2)
    throw std::runtime_error("input should have at least 2 pairs of poses");

  // calculate rotation
  vector<Rot2> rotations;
  Point2Pairs abPointPairs;
  rotations.reserve(n);
  abPointPairs.reserve(n);
  // Below denotes the pose of the i'th object/camera/etc in frame "a" or frame "b"
  Pose2 aTi, bTi;
  for (const Pose2Pair &abPair : abPosePairs) {
    std::tie(aTi, bTi) = abPair;
    const Rot2 aRb = aTi.rotation().compose(bTi.rotation().inverse());
    rotations.emplace_back(aRb);
    abPointPairs.emplace_back(aTi.translation(), bTi.translation());
  }
  const Rot2 aRb_estimate = FindKarcherMean<Rot2>(rotations);

  return alignGivenR(abPointPairs, aRb_estimate);
}

std::ostream &operator<<(std::ostream &os, const Similarity2& p) {
  os << "[" << p.rotation().theta() << " "
      << p.translation().transpose() << " " << p.scale() << "]\';";
  return os;
}

const Matrix3 Similarity2::matrix() const {
  Matrix3 T;
  T.topRows<2>() << R_.matrix(), t_;
  T.bottomRows<1>() << 0, 0, 1.0 / s_;
  return T;
}

Similarity2::operator Pose2() const {
  return Pose2(R_, s_ * t_);
}

} // namespace gtsam

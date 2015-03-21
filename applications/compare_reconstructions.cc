// Copyright (C) 2015 The Regents of the University of California (Regents).
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//
//     * Neither the name of The Regents or University of California nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Please contact the author of this library if you have any questions.
// Author: Chris Sweeney (cmsweeney@cs.ucsb.edu)

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <glog/logging.h>
#include <gflags/gflags.h>
#include <theia/theia.h>

#include <algorithm>
#include <memory>
#include <string>

DEFINE_string(reconstruction1, "", "Reconstruction file to compare.");
DEFINE_string(reconstruction2, "", "Reconstruction file to compare.");
DEFINE_double(robust_alignment_threshold, 0.0,
              "If greater than 0.0, this threshold sets determines inliers for "
              "RANSAC alignment of reconstructions. The inliers are then used "
              "for a least squares alignment.");

using theia::Reconstruction;
using theia::TrackId;
using theia::ViewId;

std::string PrintMeanMedianHistogram(
    const std::vector<double>& sorted_errors,
    const std::vector<double>& histogram_bins) {
  double mean = 0;
  theia::Histogram<double> histogram(histogram_bins);
  for (const auto& error : sorted_errors) {
    histogram.Add(error);
    mean += error;
  }

  mean /= static_cast<double>(sorted_errors.size());
  const std::string error_msg = theia::StringPrintf(
      "Mean = %lf\nMedian = %lf\nHistogram:\n%s",
      mean,
      sorted_errors[sorted_errors.size() / 2],
      histogram.PrintString().c_str());
  return error_msg;
}

double AngularDifference(const Eigen::Vector3d& rotation1,
                         const Eigen::Vector3d& rotation2) {
  Eigen::Matrix3d rotation1_mat(
      Eigen::AngleAxisd(rotation1.norm(), rotation1.normalized()));
  Eigen::Matrix3d rotation2_mat(
      Eigen::AngleAxisd(rotation2.norm(), rotation2.normalized()));
  Eigen::Matrix3d rotation_loop = rotation1_mat.transpose() * rotation2_mat;
  return Eigen::AngleAxisd(rotation_loop).angle();
}

// Aligns the orientations of the models (ignoring the positions) and reports
// the difference in orientations after alignment.
void EvaluateRotations(const Reconstruction& reconstruction1,
                       const Reconstruction& reconstruction2,
                       const std::vector<std::string>& common_view_names) {
  // Gather all the rotations in common with both views.
  std::vector<Eigen::Vector3d> rotations1, rotations2;
  rotations1.reserve(common_view_names.size());
  rotations2.reserve(common_view_names.size());
  for (const std::string& view_name : common_view_names) {
    const ViewId view_id1 = reconstruction1.ViewIdFromName(view_name);
    const ViewId view_id2 = reconstruction2.ViewIdFromName(view_name);
    rotations1.push_back(
        reconstruction1.View(view_id1)->Camera().GetOrientationAsAngleAxis());
    rotations2.push_back(
        reconstruction2.View(view_id2)->Camera().GetOrientationAsAngleAxis());
  }

  // Align the rotation estimations.
  theia::AlignRotations(rotations1, &rotations2);

  // Measure the difference in rotations.
  std::vector<double> rotation_error_degrees(rotations1.size());
  for (int i = 0; i < rotations1.size(); i++) {
    rotation_error_degrees[i] = AngularDifference(rotations1[i], rotations2[i]);
  }
  std::sort(rotation_error_degrees.begin(), rotation_error_degrees.end());

  std::vector<double> histogram_bins = {1, 2, 5, 10, 15, 20, 45};
  const std::string rotation_error_msg =
      PrintMeanMedianHistogram(rotation_error_degrees, histogram_bins);
  LOG(INFO) << "Rotation difference when aligning orientations:\n"
            << rotation_error_msg;
}

// Align the reconstructions then evaluate the pose errors.
void EvaluateAlignedPoseError(
    const std::vector<std::string>& common_view_names,
    const Reconstruction& reconstruction1,
    Reconstruction* reconstruction2) {
  if (FLAGS_robust_alignment_threshold > 0.0) {
    AlignReconstructionsRobust(FLAGS_robust_alignment_threshold,
                               reconstruction1, reconstruction2);
  } else {
    AlignReconstructions(reconstruction1, reconstruction2);
  }

  std::vector<double> rotation_errors_degrees(common_view_names.size());
  std::vector<double> position_errors(common_view_names.size());
  std::vector<double> focal_length_errors(common_view_names.size());
  for (int i = 0; i < common_view_names.size(); i++) {
    const ViewId view_id1 =
        reconstruction1.ViewIdFromName(common_view_names[i]);
    const ViewId view_id2 =
        reconstruction2->ViewIdFromName(common_view_names[i]);
    const theia::Camera& camera1 = reconstruction1.View(view_id1)->Camera();
    const theia::Camera& camera2 = reconstruction2->View(view_id2)->Camera();

    // Rotation error.
    rotation_errors_degrees[i] =
        AngularDifference(camera1.GetOrientationAsAngleAxis(),
                          camera2.GetOrientationAsAngleAxis());

    // Position error.
    position_errors[i] = (camera1.GetPosition() - camera2.GetPosition()).norm();

    // Focal length error.
    focal_length_errors[i] =
        std::abs(camera1.FocalLength() - camera2.FocalLength()) /
        camera1.FocalLength();
  }

  std::sort(rotation_errors_degrees.begin(), rotation_errors_degrees.end());
  std::sort(position_errors.begin(), position_errors.end());
  std::sort(focal_length_errors.begin(), focal_length_errors.end());

  std::vector<double> histogram_bins = {1, 2, 5, 10, 15, 20, 45};
  const std::string rotation_error_msg =
      PrintMeanMedianHistogram(rotation_errors_degrees, histogram_bins);
  LOG(INFO) << "Rotation difference when aligning positions:\n"
            << rotation_error_msg;

  std::vector<double> histogram_bins2 = {1, 5, 10, 50, 100, 1000 };
  const std::string position_error_msg =
      PrintMeanMedianHistogram(position_errors, histogram_bins2);
  LOG(INFO) << "Position difference:\n" << position_error_msg;

  std::vector<double> histogram_bins3 = {0.01, 0.05, 0.2, 0.5, 1, 10, 100};
  const std::string focal_length_error_msg =
      PrintMeanMedianHistogram(focal_length_errors, histogram_bins3);
  LOG(INFO) << "Focal length errors: \n" << focal_length_error_msg;
}

void ComputeTrackLengthHistogram(const Reconstruction& reconstruction) {
  std::vector<int> histogram_bins = {2, 3,  4,  5,  6,  7, 8,
                                     9, 10, 15, 20, 25, 50};
  theia::Histogram<int> histogram(histogram_bins);
  for (const TrackId track_id : reconstruction.TrackIds()) {
    const theia::Track* track = reconstruction.Track(track_id);
    histogram.Add(track->NumViews());
  }
  const std::string hist_msg = histogram.PrintString();
  LOG(INFO) << "Track lengths = \n" << hist_msg;
}

int main(int argc, char* argv[]) {
  google::InitGoogleLogging(argv[0]);
  THEIA_GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);

  std::unique_ptr<Reconstruction> reconstruction1(new Reconstruction());
  CHECK(theia::ReadReconstruction(FLAGS_reconstruction1,
                                  reconstruction1.get()))
      << "Could not read ground truth reconstruction file:"
      << FLAGS_reconstruction1;

  std::unique_ptr<Reconstruction> reconstruction2(new Reconstruction());
  CHECK(theia::ReadReconstruction(FLAGS_reconstruction2, reconstruction2.get()))
      << "Could not read reconstruction file:" << FLAGS_reconstruction2;

  const std::vector<std::string> common_view_names =
      theia::FindCommonViewsByName(*reconstruction1, *reconstruction2);

  // Compare number of cameras.
  LOG(INFO) << "Number of cameras:\n"
            << "\tReconstruction 1: " << reconstruction1->NumViews()
            << "\n\tReconstruction 2: " << reconstruction2->NumViews()
            << "\n\tNumber of Common cameras: "
            << common_view_names.size();

  // Compare number of 3d points.
  LOG(INFO) << "Number of 3d points:\n"
            << "\tReconstruction 1: " << reconstruction1->NumTracks()
            << "\n\tReconstruction 2: " << reconstruction2->NumTracks();

  // Evaluate rotation independent of positions.
  EvaluateRotations(*reconstruction1, *reconstruction2, common_view_names);

  // Align models and evaluate position and rotation errors.
  EvaluateAlignedPoseError(common_view_names,
                           *reconstruction1,
                           reconstruction2.get());

  return 0;
}
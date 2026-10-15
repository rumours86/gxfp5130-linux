
#pragma once

#include <opencv2/core.hpp>
#include <vector>

struct SigfmImgInfo {
    std::vector<cv::KeyPoint> keypoints;
    cv::Mat descriptors;
    // Raw 8-bit frame for the ridge-overlap matcher; empty in older templates.
    cv::Mat ridge;
};
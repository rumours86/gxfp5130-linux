// SIGFM algorithm for libfprint

// Copyright (C) 2022 Matthieu CHARETTE <matthieu.charette@gmail.com>
// Copyright (c) 2022 Natasha England-Elbro <ashenglandelbro@protonmail.com>
// Copyright (c) 2022 Timur Mangliev <tigrmango@gmail.com>

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
//

#include "sigfm.hpp"
#include "binary.hpp"
#include "img-info.hpp"

#include "opencv2/core/persistence.hpp"
#include "opencv2/core/types.hpp"
#include "opencv2/features2d.hpp"
#include "opencv2/imgcodecs.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include <opencv2/opencv.hpp>
#include <vector>

namespace bin {

template<>
struct serializer<SigfmImgInfo> : public std::true_type {
    static void serialize(const SigfmImgInfo& info, stream& out)
    {
        out << info.keypoints << info.descriptors;
        if (!info.ridge.empty()) {
            out << info.ridge;
        }
    }
};

template<>
struct deserializer<SigfmImgInfo> : public std::true_type {
    static SigfmImgInfo deserialize(stream& in)
    {
        SigfmImgInfo info;
        in >> info.keypoints >> info.descriptors;
        // Templates written before the ridge matcher end here.
        if (in.size() > 0) {
            int type, rows, cols;
            in >> type >> rows >> cols;
            if (type != CV_8UC1 || rows <= 0 || cols <= 0 || rows > 512 ||
                cols > 512 || in.size() != std::size_t(rows) * cols) {
                throw std::runtime_error{"bad ridge image"};
            }
            info.ridge.create(rows, cols, CV_8UC1);
            in.read(info.ridge.data, info.ridge.total());
        }
        return info;
    }
};
} // namespace bin

namespace {
// Looser than Lowe's usual 0.75: on the small GXFP5130 patch the ridges are
// periodic, so a correct match often has a close runner-up. 0.75 lost every
// next-day match; 0.85 kept 0/15 wrong-finger attempts (best score 3 of 40).
constexpr auto distance_match = 0.85;
constexpr auto length_match = 0.05;
// Absolute angular tolerance in radians. Unlike a relative comparison, this
// remains well-defined near zero and has uniform meaning around the circle.
constexpr auto angle_tol_rad = 0.10;
constexpr auto min_match = 5;
struct match {
    cv::Point2i p1;
    cv::Point2i p2;
    match(cv::Point2i ip1, cv::Point2i ip2) : p1{ip1}, p2{ip2} {}
    match() : p1{cv::Point2i(0, 0)}, p2{cv::Point2i(0, 0)} {}
    bool operator==(const match& right) const
    {
        return std::tie(this->p1, this->p2) == std::tie(right.p1, right.p2);
    }
    bool operator<(const match& right) const
    {
        // Order the complete correspondence. Ordering by p1.y alone causes a
        // set to discard distinct matches that happen to share a scanline.
        return std::tie(this->p1.y, this->p1.x, this->p2.y, this->p2.x) <
               std::tie(right.p1.y, right.p1.x, right.p2.y, right.p2.x);
    }
};
struct angle {
    double cos;
    double sin;
    match corr_matches[2];
    angle(double cos_, double sin_, match m1, match m2)
        : cos{cos_}, sin{sin_}, corr_matches{m1, m2}
    {
    }
};
} // namespace

SigfmImgInfo* sigfm_copy_info(SigfmImgInfo* info) { return new SigfmImgInfo{*info}; }

int sigfm_keypoints_count(SigfmImgInfo* info) { return info->keypoints.size(); }
unsigned char* sigfm_serialize_binary(SigfmImgInfo* info, int* outlen)
{
    bin::stream s;
    s << *info;
    *outlen = s.size();
    return s.copy_buffer();
}

SigfmImgInfo* sigfm_deserialize_binary(const unsigned char* bytes, int len)
{
    try {
        bin::stream s{bytes, bytes + len};
        auto info = std::make_unique<SigfmImgInfo>();
        s >> *info;
        return info.release();
    }
    catch (const std::exception&) {
        return nullptr;
    }
}

SigfmImgInfo* sigfm_extract(const SigfmPix* pix, int width, int height)
{
    cv::Mat img;
    img.create(height, width, CV_8UC1);
    std::memcpy(img.data, pix, width * height);
    // Even out local contrast so keypoints follow ridges rather than
    // day-to-day skin moisture.
    cv::createCLAHE(2.0, cv::Size{4, 4})->apply(img, img);
    const auto roi = cv::Mat::ones(cv::Size{img.size[1], img.size[0]}, CV_8UC1);
    std::vector<cv::KeyPoint> pts;

    cv::Mat descs;
    cv::SIFT::create()->detectAndCompute(img, roi, pts, descs);

    auto* info = new SigfmImgInfo{pts, descs};
    return info;
}

namespace {
// Ridge-overlap matcher for small press sensors (GXFP51B7, 80x64). SIGFM's
// sparse keypoints tolerate only 5% length change, so two separate touches of
// the same finger, pressed with different force, often score zero. Here the
// high-passed ridge images are aligned with a rigid ECC instead. An affine
// fit was tried first: it stretched and sheared other fingers' ridges onto
// the enrolled ones (0.79 for a wrong finger against 0.69 for a true touch),
// while true touches needed almost no stretch.
bool ridge_prepare(const cv::Mat& raw, cv::Mat& out)
{
    cv::Mat blur;
    raw.convertTo(out, CV_32F, 1. / 255);
    cv::GaussianBlur(out, blur, cv::Size(), 3);
    out -= blur;
    cv::Scalar mean, sd;
    cv::meanStdDev(out, mean, sd);
    return sd[0] >= 1e-6;
}

bool ridge_plausible(const cv::Mat& w, cv::Size size)
{
    cv::Mat valid(size, CV_8U, cv::Scalar(255)), warped;
    cv::warpAffine(valid, warped, w, size,
                   cv::INTER_NEAREST | cv::WARP_INVERSE_MAP);
    const double coverage =
        double(cv::countNonZero(warped)) / double(size.area());
    return coverage > .45;
}

double ridge_score(const cv::Mat& reference, const cv::Mat& probe)
{
    if (reference.size() != probe.size()) {
        return -1;
    }
    cv::Mat x, y;
    if (!ridge_prepare(reference, x) || !ridge_prepare(probe, y)) {
        return -1;
    }
    // Zero rows/columns at the probe border were never observed.
    std::vector<cv::Point> observed;
    cv::findNonZero(probe, observed);
    if (observed.empty()) {
        return -1;
    }
    cv::Mat mask(probe.size(), CV_8U, cv::Scalar(0));
    mask(cv::boundingRect(observed)).setTo(255);
    cv::erode(mask, mask, cv::Mat::ones(7, 7, CV_8U));

    const cv::Point2f centre(probe.cols / 2.f, probe.rows / 2.f);
    std::vector<cv::Mat> seeds;
    const auto shift = cv::phaseCorrelate(x, y);
    seeds.push_back((cv::Mat_<float>(2, 3) << 1, 0, shift.x, 0, 1, shift.y));
    for (int angle = -30; angle <= 30; angle += 10) {
        for (int dx = -20; dx <= 20; dx += 10) {
            for (int dy = -16; dy <= 16; dy += 8) {
                cv::Mat w;
                cv::getRotationMatrix2D(centre, angle, 1).convertTo(w, CV_32F);
                w.at<float>(0, 2) += dx;
                w.at<float>(1, 2) += dy;
                seeds.push_back(w);
            }
        }
    }

    // A short ECC run ranks the seeds; only the best few are refined. On the
    // labelled corpus this gives the same maxima as refining every seed.
    std::vector<std::pair<double, cv::Mat>> coarse;
    for (const auto& seed : seeds) {
        cv::Mat w = seed.clone();
        try {
            const double s = cv::findTransformECC(
                x, y, w, cv::MOTION_EUCLIDEAN,
                cv::TermCriteria(cv::TermCriteria::COUNT | cv::TermCriteria::EPS,
                                 6, 1e-4),
                mask, 3);
            coarse.emplace_back(s, w);
        }
        catch (const cv::Exception&) {
        }
    }
    std::sort(coarse.begin(), coarse.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    if (coarse.size() > 3) {
        coarse.resize(3);
    }

    double best = -1;
    for (auto& [unused, w] : coarse) {
        try {
            const double s = cv::findTransformECC(
                x, y, w, cv::MOTION_EUCLIDEAN,
                cv::TermCriteria(cv::TermCriteria::COUNT | cv::TermCriteria::EPS,
                                 80, 1e-5),
                mask, 3);
            if (s > best && ridge_plausible(w, probe.size())) {
                best = s;
            }
        }
        catch (const cv::Exception&) {
        }
    }
    return best;
}
} // namespace

int sigfm_has_ridge(SigfmImgInfo* info) { return !info->ridge.empty(); }

void sigfm_attach_frame(SigfmImgInfo* info, const SigfmPix* pix, int width,
                        int height)
{
    info->ridge.create(height, width, CV_8UC1);
    std::memcpy(info->ridge.data, pix, std::size_t(width) * height);
}

int sigfm_ridge_scores(SigfmImgInfo* const* enrolled, int count,
                       SigfmImgInfo* frame, double* scores)
{
    try {
        cv::parallel_for_(cv::Range(0, count), [&](const cv::Range& range) {
            for (int i = range.start; i < range.end; i++) {
                scores[i] = enrolled[i]->ridge.empty() || frame->ridge.empty()
                                ? -1
                                : ridge_score(enrolled[i]->ridge, frame->ridge);
            }
        });
        return 0;
    }
    catch (const std::exception&) {
        return -1;
    }
}

int sigfm_match_score(SigfmImgInfo* frame, SigfmImgInfo* enrolled)
{
    try {
        std::vector<std::vector<cv::DMatch>> points;
        auto bfm = cv::BFMatcher::create();
        bfm->knnMatch(frame->descriptors, enrolled->descriptors, points, 2);
        std::set<match> matches_unique;
        int nb_matched = 0;
        for (const auto& pts : points) {
            if (pts.size() < 2) {
                continue;
            }
            const cv::DMatch& match_1 = pts.at(0);
            if (match_1.distance < distance_match * pts.at(1).distance) {
                matches_unique.emplace(
                    match{frame->keypoints.at(match_1.queryIdx).pt,
                          enrolled->keypoints.at(match_1.trainIdx).pt});
                nb_matched++;
            }
        }
        if (nb_matched < min_match) {
            return 0;
        }
        std::vector<match> matches{matches_unique.begin(),
                                   matches_unique.end()};

        std::vector<angle> angles;
        for (std::size_t j = 0; j < matches.size(); j++) {
            match match_1 = matches[j];
            for (std::size_t k = j + 1; k < matches.size(); k++) {
                match match_2 = matches[k];

                int vec_1[2] = {match_1.p1.x - match_2.p1.x,
                                match_1.p1.y - match_2.p1.y};
                int vec_2[2] = {match_1.p2.x - match_2.p2.x,
                                match_1.p2.y - match_2.p2.y};

                double length_1 = sqrt(pow(vec_1[0], 2) + pow(vec_1[1], 2));
                double length_2 = sqrt(pow(vec_2[0], 2) + pow(vec_2[1], 2));

                if (1 - std::min(length_1, length_2) /
                            std::max(length_1, length_2) <=
                    length_match) {

                    double product = length_1 * length_2;
                    auto clamp_unit = [](double value) {
                        return std::clamp(value, -1.0, 1.0);
                    };
                    angles.emplace_back(angle(
                        M_PI / 2 +
                            asin(clamp_unit((vec_1[0] * vec_2[0] +
                                             vec_1[1] * vec_2[1]) /
                                            product)),
                        acos(clamp_unit((vec_1[0] * vec_2[1] -
                                         vec_1[1] * vec_2[0]) /
                                        product)),
                        match_1, match_2));
                }
            }
        }

        if (angles.size() < min_match) {
            return 0;
        }

        int count = 0;
        for (std::size_t j = 0; j < angles.size(); j++) {
            angle angle_1 = angles[j];
            for (std::size_t k = j + 1; k < angles.size(); k++) {
                angle angle_2 = angles[k];

                if (std::fabs(angle_1.sin - angle_2.sin) <= angle_tol_rad &&
                    std::fabs(angle_1.cos - angle_2.cos) <= angle_tol_rad) {

                    count += 1;
                }
            }
        }
        return count;
    }
    catch (...) {
        return -1;
    }
}

void sigfm_free_info(SigfmImgInfo* info) { delete info; }

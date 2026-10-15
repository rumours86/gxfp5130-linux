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
// Ridge matcher for small press sensors (GXFP51B7, 80x64). SIGFM's sparse
// keypoints tolerate only 5% length change, so two separate touches of the
// same finger, pressed with different force, often score zero.
//
// Plain correlation of aligned ridge images is not enough either: on an
// 80x64 patch most fingers show near-parallel ridges of similar spacing, and
// a rigid fit of another finger reached 0.73..0.78 against 0.81..0.95 for
// true touches. So two scores are used that do not depend on the
// parallel-ridge pattern:
//
// - Phase-only correlation (Ito et al.) of the high-passed frames, band
//   limited to half the Nyquist frequency, the probe rotated in 2 degree
//   steps. Whitening the spectrum makes ridge endings, bifurcations and pores
//   count as much as the dominant ridge frequency. The peak gives the pose
//   too, which a rigid ECC then refines.
// - The residual: each frame minus itself smoothed along the local ridge
//   direction. It keeps exactly the detail that parallel ridges lack, and its
//   correlation at the refined pose is near zero unless the pose is a true
//   correspondence.
//
// Each is turned into a z-score against impostor pairs measured on the
// GXFP51B7 (6240 pairs of the enrolled right index against other fingers of
// the same person). For those pairs the two scores are uncorrelated
// (r = 0.04), also in the tail, so requiring both multiplies their chance
// rates. An affine fit was tried for the pose and dropped: it stretched other
// fingers' ridges onto the enrolled ones.
constexpr int poc_width = 160; // zero padding: no wrap-around for 80x64
constexpr int poc_height = 128;
constexpr double poc_band = 0.5;
constexpr int poc_max_angle = 30;
constexpr int poc_angle_step = 2;
constexpr int poc_max_dx = 40;
constexpr int poc_max_dy = 32;
constexpr double residual_mean = 0.0625;
constexpr double residual_sd = 0.0455;
constexpr double poc_mean = 0.1335;
constexpr double poc_sd = 0.0113;
// Both scores need at least this much evidence on their own.
constexpr double ridge_min_z = 2;
// The sensor has a fixed pattern of vertical stripes. A frame of a barely
// touching finger is mostly that pattern once stretched, and two such frames
// correlate at zero shift: a family member's blank touch scored 16 against a
// weak enrolled frame. Frames whose stripes (the fy = 0 row of the spectrum)
// carry more than this share of the energy are blank: real touches had at
// most 0.075, blank ones 0.24..0.41.
constexpr double ridge_max_stripes = 0.15;
// An enrolled frame must show ridges (structure-tensor coherence above 0.5)
// over at least this share of its area; a partial touch leaves the rest to
// the stripes. Enrolled frames had 0.76..1 before a 24-stage enrollment let
// two partial ones in (0.34, 0.38), one of which a left index touch matched.
constexpr double ridge_min_coherent = 0.5;
constexpr double ridge_none = -100;

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

// Local direction along the ridges, from the smoothed structure tensor.
cv::Mat ridge_orientation(const cv::Mat& hp)
{
    cv::Mat gx, gy;
    cv::Sobel(hp, gx, CV_32F, 1, 0, 3);
    cv::Sobel(hp, gy, CV_32F, 0, 1, 3);
    cv::Mat jxx = gx.mul(gx), jyy = gy.mul(gy), jxy = gx.mul(gy);
    for (auto* j : {&jxx, &jyy, &jxy}) {
        cv::GaussianBlur(*j, *j, cv::Size(), 4);
    }
    cv::Mat ori(hp.size(), CV_32F);
    for (int y = 0; y < hp.rows; y++) {
        for (int x = 0; x < hp.cols; x++) {
            const float a = jxx.at<float>(y, x), b = jyy.at<float>(y, x),
                        c = jxy.at<float>(y, x);
            ori.at<float>(y, x) = 0.5f * std::atan2(2 * c, a - b) + CV_PI / 2;
        }
    }
    return ori;
}

// The frame minus itself smoothed along the ridges (Gaussian, sigma 3 px).
cv::Mat ridge_residual(const cv::Mat& hp)
{
    constexpr double sigma = 3;
    constexpr int reach = 8;
    float weights[2 * reach + 1];
    for (int t = -reach; t <= reach; t++) {
        weights[t + reach] = std::exp(-0.5 * t * t / (sigma * sigma));
    }
    const cv::Mat ori = ridge_orientation(hp);
    cv::Mat out(hp.size(), CV_32F);
    for (int y = 0; y < hp.rows; y++) {
        for (int x = 0; x < hp.cols; x++) {
            const float th = ori.at<float>(y, x);
            const float cx = std::cos(th), cy = std::sin(th);
            double sum = 0, wsum = 0;
            for (int t = -reach; t <= reach; t++) {
                const float px = x + t * cx, py = y + t * cy;
                if (px < 0 || py < 0 || px > hp.cols - 1 || py > hp.rows - 1) {
                    continue;
                }
                const int x0 = int(px), y0 = int(py);
                const int x1 = std::min(x0 + 1, hp.cols - 1);
                const int y1 = std::min(y0 + 1, hp.rows - 1);
                const float fx = px - x0, fy = py - y0;
                const float v = (1 - fx) * (1 - fy) * hp.at<float>(y0, x0) +
                                fx * (1 - fy) * hp.at<float>(y0, x1) +
                                (1 - fx) * fy * hp.at<float>(y1, x0) +
                                fx * fy * hp.at<float>(y1, x1);
                sum += weights[t + reach] * v;
                wsum += weights[t + reach];
            }
            out.at<float>(y, x) = hp.at<float>(y, x) - float(sum / wsum);
        }
    }
    return out;
}

// Unit-magnitude spectrum inside the band, zero elsewhere.
cv::Mat poc_spectrum(const cv::Mat& hp, const cv::Mat& mask)
{
    cv::Mat win, soft;
    cv::createHanningWindow(win, hp.size(), CV_32F);
    mask.convertTo(soft, CV_32F, 1. / 255);
    cv::GaussianBlur(soft, soft, cv::Size(), 2);
    cv::Mat pad(poc_height, poc_width, CV_32F, cv::Scalar(0));
    cv::Mat(hp.mul(win).mul(soft))
        .copyTo(pad(cv::Rect((poc_width - hp.cols) / 2,
                             (poc_height - hp.rows) / 2, hp.cols, hp.rows)));
    cv::Mat spectrum;
    cv::dft(pad, spectrum, cv::DFT_COMPLEX_OUTPUT);
    for (int y = 0; y < poc_height; y++) {
        const int fy = y <= poc_height / 2 ? y : y - poc_height;
        for (int x = 0; x < poc_width; x++) {
            const int fx = x <= poc_width / 2 ? x : x - poc_width;
            auto& v = spectrum.at<cv::Vec2f>(y, x);
            const float mag = std::hypot(v[0], v[1]);
            const bool kept =
                std::abs(fx) <= poc_band * poc_width / 2 &&
                std::abs(fy) <= poc_band * poc_height / 2 && (fx || fy);
            v = kept && mag > 1e-12f ? v / mag : cv::Vec2f(0, 0);
        }
    }
    return spectrum;
}

int poc_kept()
{
    int kept = 0;
    for (int fy = -poc_height / 2 + 1; fy <= poc_height / 2; fy++) {
        for (int fx = -poc_width / 2 + 1; fx <= poc_width / 2; fx++) {
            kept += std::abs(fx) <= poc_band * poc_width / 2 &&
                    std::abs(fy) <= poc_band * poc_height / 2 && (fx || fy);
        }
    }
    return kept;
}

// Only shifts that leave the frames overlapping.
cv::Mat poc_shift_mask()
{
    cv::Mat m(poc_height, poc_width, CV_8U, cv::Scalar(0));
    for (int y = 0; y < poc_height; y++) {
        const int sy = y <= poc_height / 2 ? y : y - poc_height;
        for (int x = 0; x < poc_width; x++) {
            const int sx = x <= poc_width / 2 ? x : x - poc_width;
            if (std::abs(sx) <= poc_max_dx && std::abs(sy) <= poc_max_dy) {
                m.at<uchar>(y, x) = 255;
            }
        }
    }
    return m;
}

// Share of the (windowed, DC-free) spectral energy in vertical stripes.
double ridge_stripes(const cv::Mat& hp)
{
    cv::Mat win, spectrum;
    cv::createHanningWindow(win, hp.size(), CV_32F);
    cv::dft(hp.mul(win), spectrum, cv::DFT_COMPLEX_OUTPUT);
    double stripes = 0, all = 0;
    for (int y = 0; y < spectrum.rows; y++) {
        for (int x = 0; x < spectrum.cols; x++) {
            const auto v = spectrum.at<cv::Vec2f>(y, x);
            const double e = double(v[0]) * v[0] + double(v[1]) * v[1];
            if (x || y) {
                all += e;
                stripes += y ? 0 : e;
            }
        }
    }
    return all > 0 ? stripes / all : 1;
}

// Share of the observed area with oriented ridges.
double ridge_coherent(const cv::Mat& hp, const cv::Mat& mask)
{
    cv::Mat gx, gy;
    cv::Sobel(hp, gx, CV_32F, 1, 0, 3);
    cv::Sobel(hp, gy, CV_32F, 0, 1, 3);
    cv::Mat jxx = gx.mul(gx), jyy = gy.mul(gy), jxy = gx.mul(gy);
    for (auto* j : {&jxx, &jyy, &jxy}) {
        cv::GaussianBlur(*j, *j, cv::Size(), 4);
    }
    int coherent = 0, observed = 0;
    for (int y = 0; y < hp.rows; y++) {
        for (int x = 0; x < hp.cols; x++) {
            if (!mask.at<uchar>(y, x)) {
                continue;
            }
            const float a = jxx.at<float>(y, x), b = jyy.at<float>(y, x),
                        c = jxy.at<float>(y, x);
            observed++;
            coherent += a + b > 1e-12f &&
                        std::sqrt((a - b) * (a - b) + 4 * c * c) / (a + b) > .5f;
        }
    }
    return observed ? double(coherent) / observed : 0;
}

struct RidgeFrame {
    cv::Mat hp, mask, residual;
    // The reference keeps one spectrum, the probe one per rotation.
    std::vector<cv::Mat> spectra;
    std::vector<int> angles;
};

bool ridge_frame(const cv::Mat& raw, bool rotations, RidgeFrame& f)
{
    if (!ridge_prepare(raw, f.hp)) {
        return false;
    }
    // Zero rows/columns at the border were never observed.
    std::vector<cv::Point> observed;
    cv::findNonZero(raw, observed);
    if (observed.empty()) {
        return false;
    }
    f.mask = cv::Mat(raw.size(), CV_8U, cv::Scalar(0));
    f.mask(cv::boundingRect(observed)).setTo(255);
    cv::erode(f.mask, f.mask, cv::Mat::ones(7, 7, CV_8U));
    if (ridge_stripes(f.hp) > ridge_max_stripes) {
        return false;
    }
    // Enrolled frames (no rotations) must also be mostly ridges.
    if (!rotations && ridge_coherent(f.hp, f.mask) < ridge_min_coherent) {
        return false;
    }
    f.residual = ridge_residual(f.hp);
    if (!rotations) {
        f.spectra.push_back(poc_spectrum(f.hp, f.mask));
        f.angles.push_back(0);
        return true;
    }
    const cv::Point2f centre(raw.cols / 2.f, raw.rows / 2.f);
    for (int a = -poc_max_angle; a <= poc_max_angle; a += poc_angle_step) {
        const cv::Mat r = cv::getRotationMatrix2D(centre, a, 1);
        cv::Mat hp, mask;
        cv::warpAffine(f.hp, hp, r, raw.size(), cv::INTER_LINEAR);
        cv::warpAffine(f.mask, mask, r, raw.size(), cv::INTER_NEAREST);
        f.spectra.push_back(poc_spectrum(hp, mask));
        f.angles.push_back(a);
    }
    return true;
}

double masked_ncc(const cv::Mat& a, const cv::Mat& b, const cv::Mat& mask)
{
    if (cv::countNonZero(mask) < 50) {
        return -1;
    }
    cv::Scalar ma, sa, mb, sb;
    cv::meanStdDev(a, ma, sa, mask);
    cv::meanStdDev(b, mb, sb, mask);
    if (sa[0] < 1e-9 || sb[0] < 1e-9) {
        return -1;
    }
    cv::Mat pa = a - ma[0], pb = b - mb[0];
    return cv::mean(pa.mul(pb), mask)[0] / (sa[0] * sb[0]);
}

// Raw scores: POC peak (about 1 for identical frames) and the residual
// correlation at the refined pose (-1 when there is no plausible pose).
void ridge_compare(const RidgeFrame& ref, const RidgeFrame& probe,
                   double& poc, double& residual)
{
    static const double scale = double(poc_width * poc_height) / poc_kept();
    static const cv::Mat shifts = poc_shift_mask();
    poc = 0;
    residual = -1;
    cv::Point peak;
    int angle = 0;
    cv::Mat cross, surface;
    for (std::size_t k = 0; k < probe.spectra.size(); k++) {
        cv::mulSpectrums(ref.spectra[0], probe.spectra[k], cross, 0, true);
        cv::idft(cross, surface, cv::DFT_REAL_OUTPUT | cv::DFT_SCALE);
        double top;
        cv::Point at;
        cv::minMaxLoc(surface, nullptr, &top, nullptr, &at, shifts);
        if (top * scale > poc) {
            poc = top * scale;
            peak = at;
            angle = probe.angles[k];
        }
    }
    if (poc <= 0) {
        return;
    }
    // The probe rotated by `angle` and shifted by the peak matches the
    // reference: reference(x) ~ rotated(x - p) = probe(R^-1 (x - p)).
    const float px = peak.x <= poc_width / 2 ? peak.x : peak.x - poc_width;
    const float py = peak.y <= poc_height / 2 ? peak.y : peak.y - poc_height;
    const cv::Size size = probe.hp.size();
    cv::Mat inv, w;
    cv::invertAffineTransform(
        cv::getRotationMatrix2D(cv::Point2f(size.width / 2.f, size.height / 2.f),
                                angle, 1),
        inv);
    inv.convertTo(w, CV_32F);
    w.at<float>(0, 2) -= w.at<float>(0, 0) * px + w.at<float>(0, 1) * py;
    w.at<float>(1, 2) -= w.at<float>(1, 0) * px + w.at<float>(1, 1) * py;
    try {
        cv::findTransformECC(
            ref.hp, probe.hp, w, cv::MOTION_EUCLIDEAN,
            cv::TermCriteria(cv::TermCriteria::COUNT | cv::TermCriteria::EPS,
                             80, 1e-5),
            probe.mask, 3);
    }
    catch (const cv::Exception&) {
        return;
    }
    if (!ridge_plausible(w, size)) {
        return;
    }
    cv::Mat mask, res;
    cv::warpAffine(probe.mask, mask, w, size,
                   cv::INTER_NEAREST | cv::WARP_INVERSE_MAP);
    cv::warpAffine(probe.residual, res, w, size,
                   cv::INTER_LINEAR | cv::WARP_INVERSE_MAP);
    residual = masked_ncc(ref.residual, res, ref.mask & mask);
}

double ridge_score(const RidgeFrame& ref, const RidgeFrame& probe)
{
    double poc, residual;
    ridge_compare(ref, probe, poc, residual);
    if (residual <= -1) {
        return ridge_none;
    }
    const double zp = (poc - poc_mean) / poc_sd;
    const double zr = (residual - residual_mean) / residual_sd;
    return std::min(zp, zr) < ridge_min_z ? std::min(zp, zr) : zp + zr;
}
} // namespace

int sigfm_has_ridge(SigfmImgInfo* info) { return !info->ridge.empty(); }

void sigfm_attach_frame(SigfmImgInfo* info, const SigfmPix* pix, int width,
                        int height)
{
    info->ridge.create(height, width, CV_8UC1);
    std::memcpy(info->ridge.data, pix, std::size_t(width) * height);
}

int sigfm_ridge_usable(SigfmImgInfo* info)
{
    try {
        RidgeFrame f;
        return !info->ridge.empty() && ridge_frame(info->ridge, false, f);
    }
    catch (const std::exception&) {
        return 0;
    }
}

int sigfm_ridge_scores(SigfmImgInfo* const* enrolled, int count,
                       SigfmImgInfo* frame, double* scores)
{
    try {
        RidgeFrame probe;
        const bool probe_ok =
            !frame->ridge.empty() && ridge_frame(frame->ridge, true, probe);
        cv::parallel_for_(cv::Range(0, count), [&](const cv::Range& range) {
            for (int i = range.start; i < range.end; i++) {
                RidgeFrame ref;
                scores[i] = probe_ok && !enrolled[i]->ridge.empty() &&
                                    enrolled[i]->ridge.size() ==
                                        frame->ridge.size() &&
                                    ridge_frame(enrolled[i]->ridge, false, ref)
                                ? ridge_score(ref, probe)
                                : ridge_none;
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

#include "opencv2/core.hpp"
#include "opencv2/core/types.hpp"
#include "sigfm.hpp"
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "binary.hpp"
#include "tests-embedded.hpp"

#include "img-info.hpp"
#include <opencv2/opencv.hpp>

namespace cv {
bool operator==(const cv::KeyPoint& lhs, const cv::KeyPoint& rhs)
{
    return lhs.angle == rhs.angle && lhs.class_id == rhs.class_id &&
           lhs.octave == rhs.octave && lhs.size == rhs.size &&
           lhs.response == rhs.response && lhs.pt == rhs.pt;
}

} // namespace cv

namespace {
bool comp_mats(const cv::Mat& lhs, const cv::Mat& rhs)
{
    return std::equal(lhs.datastart, lhs.dataend, rhs.datastart, rhs.dataend);
}

std::string to_str(const cv::KeyPoint& k)
{
    std::stringstream s;
    s << "angle: " << k.angle << ", class_id: " << k.class_id
      << ", octave: " << k.octave << ", size: " << k.size
      << ", reponse: " << k.response << ", ptx: " << k.pt.x
      << ", pty: " << k.pt.y;
    return s.str();
}

// One-hot descriptors. A nonzero @ratio adds an orthogonal offset c to each,
// so against plain one-hot descriptors every point's nearest/second-nearest
// distance ratio is c / sqrt(2 + c^2) = @ratio.
SigfmImgInfo synthetic_info(const std::vector<cv::Point2f>& points,
                            double ratio = 0)
{
    constexpr auto descriptor_width = 128;
    const auto n = static_cast<int>(points.size());
    const auto offset =
        static_cast<float>(std::sqrt(2 * ratio * ratio / (1 - ratio * ratio)));
    std::vector<cv::KeyPoint> keypoints;
    cv::Mat descriptors = cv::Mat::zeros(n, descriptor_width, CV_32FC1);

    keypoints.reserve(points.size());
    for (int i = 0; i < n; i++) {
        keypoints.emplace_back(points[i], 1.0f);
        descriptors.at<float>(i, i) = 1.0f;
        descriptors.at<float>(i, n + i) = offset;
    }

    return SigfmImgInfo{keypoints, descriptors};
}

const std::vector<cv::Point2f> scanline = {
    {0, 10}, {10, 10}, {20, 10}, {30, 10}, {40, 10}, {50, 10}};
const std::vector<cv::Point2f> scanline_shifted = {
    {3, 17}, {13, 17}, {23, 17}, {33, 17}, {43, 17}, {53, 17}};

} // namespace

template<typename T>
void check_vec(const std::vector<T>& vs)
{
    for (auto i : vs) {
        bin::stream s;
        s << i;
        T iv;
        s >> iv;
        CHECK(i == iv);
    }
}

TEST_SUITE("matching")
{
    TEST_CASE("distinct correspondences on one scanline are retained")
    {
        auto frame = synthetic_info({{0, 10},
                                     {10, 10},
                                     {20, 10},
                                     {30, 10},
                                     {40, 10},
                                     {50, 10}});
        auto enrolled = synthetic_info({{3, 17},
                                        {13, 17},
                                        {23, 17},
                                        {33, 17},
                                        {43, 17},
                                        {53, 17}});

        CHECK(sigfm_match_score(&frame, &enrolled) >= 40);
    }

    TEST_CASE("quarter-turn rotations have a finite consistent angle")
    {
        auto frame = synthetic_info({{0, 0},
                                     {10, 3},
                                     {22, 8},
                                     {35, 15},
                                     {49, 24},
                                     {64, 35}});
        auto enrolled = synthetic_info({{100, 20},
                                        {97, 30},
                                        {92, 42},
                                        {85, 55},
                                        {76, 69},
                                        {65, 84}});

        CHECK(sigfm_match_score(&frame, &enrolled) >= 40);
    }

    TEST_CASE("matches with a close runner-up pass the ratio test")
    {
        // 0.8 is kept by the 0.85 ratio test; Lowe's usual 0.75 dropped it.
        auto frame = synthetic_info(scanline, 0.8);
        auto enrolled = synthetic_info(scanline_shifted);

        CHECK(sigfm_match_score(&frame, &enrolled) >= 40);
    }

    TEST_CASE("matches with a closer runner-up are rejected")
    {
        auto frame = synthetic_info(scanline, 0.9);
        auto enrolled = synthetic_info(scanline_shifted);

        CHECK(sigfm_match_score(&frame, &enrolled) == 0);
    }

    TEST_CASE("a low-contrast copy of a frame still matches")
    {
        // At 1/8 contrast SIFT alone finds no keypoints; the CLAHE step in
        // sigfm_extract restores them.
        constexpr auto w = 80;
        constexpr auto h = 64;
        std::vector<SigfmPix> crop(w * h);
        std::vector<SigfmPix> faded(w * h);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                crop[y * w + x] = embedded::capture_aes3500[(96 + y) * 256 + 88 + x];
                faded[y * w + x] = 128 + (crop[y * w + x] - 128) / 8;
            }
        }
        SigfmImgInfo* enrolled = sigfm_extract(crop.data(), w, h);
        SigfmImgInfo* frame = sigfm_extract(faded.data(), w, h);
        REQUIRE(enrolled != nullptr);
        REQUIRE(frame != nullptr);

        CHECK(sigfm_match_score(frame, enrolled) >= 40);
        sigfm_free_info(enrolled);
        sigfm_free_info(frame);
    }
}

TEST_SUITE("ridge")
{
    // 80x64 crops of the embedded AES3500 capture stand in for the GXFP51B7
    // press sensor, which has the same frame size.
    static std::vector<SigfmPix> crop(int x0, int y0, int dx = 0, int dy = 0)
    {
        constexpr auto w = 80;
        constexpr auto h = 64;
        std::vector<SigfmPix> out(w * h, 0);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                // Pixels shifted in from outside the crop stay 0, like
                // unobserved border columns.
                const int sx = x - dx;
                const int sy = y - dy;
                if (sx < 0 || sy < 0 || sx >= w || sy >= h) {
                    continue;
                }
                out[y * w + x] =
                    embedded::capture_aes3500[(y0 + sy) * 256 + x0 + sx];
            }
        }
        return out;
    }

    static double score(const std::vector<SigfmPix>& enrolled,
                 const std::vector<SigfmPix>& probe)
    {
        SigfmImgInfo a{};
        SigfmImgInfo b{};
        sigfm_attach_frame(&a, enrolled.data(), 80, 64);
        sigfm_attach_frame(&b, probe.data(), 80, 64);
        SigfmImgInfo* gallery[] = {&a};
        double result = -2;
        REQUIRE(sigfm_ridge_scores(gallery, 1, &b, &result) == 0);
        return result;
    }

    // The GXFP51B7 threshold, see drivers/gxfp51b7.c.
    constexpr double threshold = 10;

    TEST_CASE("an identical frame matches")
    {
        const auto f = crop(88, 96);
        CHECK(score(f, f) > 50);
    }

    TEST_CASE("a shifted touch matches")
    {
        CHECK(score(crop(88, 96), crop(88, 96, 9, -6)) > threshold);
    }

    TEST_CASE("a rotated touch matches")
    {
        const cv::Mat print(256, 256, CV_8UC1,
                            const_cast<unsigned char*>(embedded::capture_aes3500));
        cv::Mat turned;
        cv::warpAffine(print, turned,
                       cv::getRotationMatrix2D(cv::Point2f(128, 128), 12, 1),
                       print.size(), cv::INTER_LINEAR);
        const cv::Mat probe = turned(cv::Rect(88, 96, 80, 64)).clone();
        CHECK(score(crop(88, 96),
                    std::vector<SigfmPix>(probe.datastart, probe.dataend)) >
              threshold);
    }

    TEST_CASE("other parts of the print stay below the threshold")
    {
        // Periodic ridges correlate, but neither score holds up without a
        // true correspondence: these 132 pairs score at most 1.2.
        double worst = -100;
        for (int a = 0; a < 12; a++) {
            for (int b = 0; b < 12; b++) {
                if (a != b) {
                    worst = std::max(worst, score(crop(a % 3 * 80, a / 3 * 64),
                                                  crop(b % 3 * 80, b / 3 * 64)));
                }
            }
        }
        CHECK(worst < 2);
        // The highest of 17384 non-overlapping pairs on a 16 px grid.
        CHECK(score(crop(96, 96), crop(64, 0)) < threshold);
    }

    TEST_CASE("a flat frame is rejected")
    {
        const std::vector<SigfmPix> flat(80 * 64, 128);
        CHECK(score(crop(88, 96), flat) == -100);
    }

    TEST_CASE("frames without a stored image score -100")
    {
        SigfmImgInfo a{};
        SigfmImgInfo b{};
        const auto f = crop(88, 96);
        sigfm_attach_frame(&b, f.data(), 80, 64);
        SigfmImgInfo* gallery[] = {&a};
        double result = 0;
        REQUIRE(sigfm_ridge_scores(gallery, 1, &b, &result) == 0);
        CHECK(result == -100);
    }

    TEST_CASE("the frame survives serialization")
    {
        const auto f = crop(88, 96);
        SigfmImgInfo* info = sigfm_extract(f.data(), 80, 64);
        REQUIRE(info != nullptr);
        CHECK(!sigfm_has_ridge(info));
        int plain_len;
        unsigned char* plain = sigfm_serialize_binary(info, &plain_len);

        sigfm_attach_frame(info, f.data(), 80, 64);
        int len;
        unsigned char* bytes = sigfm_serialize_binary(info, &len);
        CHECK(len == plain_len + 3 * int(sizeof(int)) + 80 * 64);

        SigfmImgInfo* back = sigfm_deserialize_binary(bytes, len);
        REQUIRE(back != nullptr);
        CHECK(sigfm_has_ridge(back));
        CHECK(comp_mats(info->ridge, back->ridge));

        // Templates from before the ridge matcher still load.
        SigfmImgInfo* old = sigfm_deserialize_binary(plain, plain_len);
        REQUIRE(old != nullptr);
        CHECK(!sigfm_has_ridge(old));

        // A truncated frame is refused rather than read past the end.
        CHECK(sigfm_deserialize_binary(bytes, len - 1) == nullptr);

        sigfm_free_info(back);
        sigfm_free_info(old);
        sigfm_free_info(info);
        free(bytes);
        free(plain);
    }
}

TEST_SUITE("binary")
{

    TEST_CASE("float can be stored and restored")
    {
        check_vec<float>({3, 2.4, 6.7});
    }

    TEST_CASE("size_t can be stored and restored")
    {
        check_vec<std::size_t>({2, 5, 803, 900});
    }
    TEST_CASE("number can be stored and restored")
    {
        check_vec<int>({5, 3, 10, 16, 24, 900});
    }
    TEST_CASE("image can be stored and restored")
    {
        cv::Mat input;
        input.create(256, 256, CV_8UC1);
        std::memcpy(input.data, embedded::capture_aes3500, 256 * 256);
        bin::stream s;
        s << input;

        cv::Mat output;
        s >> output;
        CHECK(std::equal(input.datastart, input.dataend, output.datastart,
                         output.dataend));
    }

    TEST_CASE("vector of values can be stored and restored")
    {
        std::vector inputs = {3, 5, 1, 7};
        bin::stream s;
        s << inputs;

        std::vector<int> outputs;
        s >> outputs;
        CHECK(outputs == inputs);
    }

    TEST_CASE("keypoints can be stored and restored")
    {
        cv::KeyPoint pt;
        pt.angle = 20;
        pt.octave = 3;
        pt.response = 3;
        pt.size = 40;
        pt.pt = cv::Point2f{3, 1};

        bin::stream s;
        s << pt;

        cv::KeyPoint ptout;
        s >> ptout;
        CHECK(to_str(pt) == to_str(ptout));
    }
    TEST_CASE("sigfm img info can be stored and restored")
    {
        constexpr auto img_w = 256;
        constexpr auto img_h = 256;
        constexpr auto img = embedded::capture_aes3500;
        SigfmImgInfo* info = sigfm_extract(img, img_w, img_h);
        REQUIRE(info != nullptr);
        const auto inf1desc = info->descriptors;
        cv::Mat descout;
        bin::stream s;
        s << inf1desc;
        s >> descout;
        CHECK(comp_mats(inf1desc, descout));

        int slen;
        const auto bin_data = sigfm_serialize_binary(info, &slen);
        int slen2;
        SigfmImgInfo* info2 = sigfm_deserialize_binary(bin_data, slen);
        REQUIRE(info2);
        const auto bin_data2 = sigfm_serialize_binary(info2, &slen2);
        CHECK(slen == slen2);
        CHECK(std::equal(bin_data, bin_data + slen, bin_data2,
                         bin_data2 + slen2));

        REQUIRE(info->keypoints == info2->keypoints);
        REQUIRE(std::equal(
            info->descriptors.datastart, info->descriptors.dataend,
            info2->descriptors.datastart, info2->descriptors.dataend));
        sigfm_free_info(info);
        sigfm_free_info(info2);
    }
}

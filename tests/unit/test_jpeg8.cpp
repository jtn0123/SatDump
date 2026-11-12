// 8-bit JPEG (image/io/jpegio.cpp and image/jpeg_utils.cpp), which the copy of IJG libjpeg 6b
// in libs/jpeg (from DCMTK) decodes and encodes on every system. These tests pin its results:
// the JPEG pictures in resources/ (the map pictures composites and map overlays use), small
// streams made with encoder settings SatDump itself never uses, cut-off and damaged streams,
// and pictures SatDump saves (pixels and bytes). Another libjpeg (such as Debian's
// libjpeg-turbo) must give the same results here before it replaces the bundled copy.
// In the sanitizer build they also check that libjpeg's memory is released when it refuses a
// picture, and that libjpeg itself has no undefined behaviour on these pictures.
#include "doctest.h"
#include "image/image.h"
#include "image/io.h"
#include "image/jpeg_utils.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using satdump::image::Image;

    // FNV-1a, 64 bit
    struct Hash
    {
        uint64_t h = 1469598103934665603ULL;

        void bytes(const void *p, size_t n)
        {
            const uint8_t *b = (const uint8_t *)p;
            for (size_t i = 0; i < n; i++)
            {
                h ^= b[i];
                h *= 1099511628211ULL;
            }
        }

        void u64(uint64_t v) { bytes(&v, sizeof(v)); }
    };

    uint64_t image_hash(Image &img)
    {
        Hash hash;
        hash.u64(img.depth());
        hash.u64(img.width());
        hash.u64(img.height());
        hash.u64(img.channels());
        if (img.size() > 0)
            hash.bytes(img.raw_data(), img.size() * img.typesize());
        return hash.h;
    }

    uint64_t bytes_hash(const std::vector<uint8_t> &v)
    {
        Hash hash;
        hash.u64(v.size());
        hash.bytes(v.data(), v.size());
        return hash.h;
    }

    std::vector<uint8_t> from_base64(const std::string &s)
    {
        std::vector<uint8_t> out;
        uint32_t acc = 0;
        int bits = 0;
        for (char c : s)
        {
            int v;
            if (c >= 'A' && c <= 'Z')
                v = c - 'A';
            else if (c >= 'a' && c <= 'z')
                v = c - 'a' + 26;
            else if (c >= '0' && c <= '9')
                v = c - '0' + 52;
            else if (c == '+')
                v = 62;
            else if (c == '/')
                v = 63;
            else
                continue; // '=' padding
            acc = (acc << 6) | v;
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out.push_back(uint8_t(acc >> bits));
            }
        }
        return out;
    }

    std::vector<uint8_t> read_file(const fs::path &p)
    {
        std::ifstream f(p, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }

    void write_file(const fs::path &p, const std::vector<uint8_t> &v)
    {
        std::ofstream f(p, std::ios::binary);
        f.write((const char *)v.data(), v.size());
    }

    // A folder of the test's own in the temporary folder, removed afterwards
    struct TempDir
    {
        fs::path path;

        explicit TempDir(const std::string &name)
        {
            path = fs::temp_directory_path() / (name + "-" + std::to_string(::getpid()));
            fs::remove_all(path);
            fs::create_directories(path);
        }

        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    struct Stream
    {
        const char *name;
        size_t size;
        const char *base64;
    };

    // 33x19 pictures with sharp edges and noise, made with libjpeg's own encoder: grayscale,
    // the common chroma samplings (4:4:4, 4:2:2, 4:4:0, 4:2:0, 4:1:1), progressive, restart
    // markers after every MCU, CMYK, and a lossless (SOF3) stream from the DCMTK copy
    const Stream STREAMS[] = {
    {"gray", 692,
        "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcp"
        "LDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAATACEBAREA/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgED"
        "AwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RF"
        "RkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJ"
        "ytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/9oACAEBAAA/AMC91vxT8VJIJpdNsz/ZpYf6L+7GXxgHzHOeE7f/AFq1"
        "7T+ztNIi0SX7RHKNkjSqSRtPGOF65Y5x2q3DJdxFYdLj+0TqQziTGAT+I9D+RrdspPDnw9nNxf6hJB9uJIeVGkB2dcbE4+93"
        "9RVf+1vjF/0Kuj/99xf/ACRXHNrj6yYf7A0KTSY4iRcDTydsmcbd3lqBkAHGc9TWrbaXBEDFFOsmc/MOox2HP19O1aSaj9ga"
        "GfTNLOqSuv75LY5KHtnapxnLdf1rVs9A0vw9b3D+LdSj155lLWNlqEatM7KCWWBJHO53LINq4ydvqK3f+Etl/wCigeDf/AM/"
        "/JlcD4gtIPCn2eHRYxaxzM28H95nB4+/n1NZT3k9rpsMkLhWZjk7Qf516T4a0yz0+/kNrD5fnY3/ADE5wox1PHU143P438RX"
        "Vk2vzaiW1PSdThgspvJjHlJPDOJRtC7Wz5adQcY4xk1yHlL/ALX/AH0a/9k="},
    {"gray_progressive", 657,
        "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcp"
        "LDAxNDQ0Hyc5PTgyPC4zNDL/wgALCAATACEBAREA/8QAFwABAQEBAAAAAAAAAAAAAAAABAUDBv/aAAgBAQAAAAGezV+cZaVP"
        "gE6XjY//xAAdEAABBAMBAQAAAAAAAAAAAAACAAEDBAUSExEU/9oACAEBAAEFAjmtZVx5xpnJkD18e/XMLv2QxMy6aIIIq4/W"
        "6sC1VbuMdaMIze7YIPF//8QALhAAAQMCAgUNAQAAAAAAAAAAAQADEQISEyEjMkGBsQQQFDE0NVFSYZGSodLi/9oACAEBAAY/"
        "AgS3Ro/LlxUMm4HrlQ0Lj6q6twi/xzXZWfr9IYDBaA1sPb7KAZQLbWKdtqq6W4H51KHBnuld4cj+H9qkMi0HegQjaIlY5c0j"
        "TgFBgZSDPDm//8QAIBAAAQQCAgMBAAAAAAAAAAAAAQARIWExkVFxEEHxgf/aAAgBAQABPyHKNf27UqqBBxq01NGSlnTA4dC1"
        "95BLTIcHYoI2QdfgR9NDtFAqLiCZGQJSS4gV4El2pf6RL1iTwrS2aR8+x9mOZjgmXtf/2gAIAQEAAAAQpJP/xAAkEAEBAAIC"
        "AQEJAAAAAAAAAAABEQAhMUFRcRBhgZGhseHw8f/aAAgBAQABPxB5Sh0WYNbrr+MuJzZKVqa5VWdYnQQMQX4nh+TjWpKHHvOP"
        "1eT2Bk6xQJ2TgCgMt5cLby6ck6N+vjrLzbZ1fVpLeX1xUxAgiptY6K+8Z/cYjhAXlHXfy4UFCsP3xy4DmbBOXXLjlYrE6Y2O"
        "0ZNSufsWf//Z"},
    {"ycc_444", 1772,
        "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAUDBAQEAwUEBAQFBQUGBwwIBwcHBw8LCwkMEQ8SEhEPERETFhwXExQaFRERGCEY"
        "Gh0dHx8fExciJCIeJBweHx7/2wBDAQUFBQcGBw4ICA4eFBEUHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4e"
        "Hh4eHh4eHh4eHh4eHh7/wAARCAATACEDAREAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAA"
        "AgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6"
        "Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXG"
        "x8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREA"
        "AgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5"
        "OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPE"
        "xcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDk/D3hrVNWDJe2SRy2yhVMLquNwyQSzHP3"
        "R096+goYbAZLRnUdX2VKVtZWvK17Wst7X0Su+lzlznizNeMZxqqnFqne9k18Vv5peWttvmep6WNJ0TVrTT5b66jl1ZtlqFiZ"
        "2dk5Y4QYVQHBJbGByTjNeJjcXisbh6lFU5UY0+V3jKKm97ptOXLdpKKi7tp668p4lSrFUJyyy04R1m27WXTflvfVWV9dNWYm"
        "o+MLmW40PUNEbSdZ1a3t5orgLZzRwWzuRnyt5UgsMIWYnIQYCbmB9uGQTrup/akZxi9Y2s3LlV7SdnCPeKbim3a7smvocDlN"
        "SFOth4wlGDcXZyjdpae9Z2snqklo27uVotU/D2lWWhRpLAsd5dgAMr5O0jt0H59fpmuDHZvUzL6u6MISlFSvLkkkm3F+4pPS"
        "1lq1fso3s/pv7Qyvh6Tnja8qcau19b8ujtyxdviWj8t9TqP+EV8Uf88of++Y/wD4qvzC/D//AD8X/k/+R4P+uPGP/QDT/wDA"
        "l/8ALDnm8VafDf2tv4es9HuDcsxupE1eKCOBQyYdjg/3iQACxwxAODX6TSyDF5hWrVMfUlU2cOak2o6Suqacna9kpO+rUed9"
        "T5mTxmaUnVw9CeGjDpThJ87abV1FQ25Xq7Jc1m431wtJ1bV7rTNOsrO/1SJ7ZWlub2TUZH+0PIBjjICgbTgAkAE9SST9JUw+"
        "XYOvV1hdxi4uULx72jqvaaO8pRkrNwWuqj9d/YmBwuIqNqE1PRJQiuVK+2sm27+893btZLp/D+lM4EGnW0kTbFUyCMliB93k"
        "Z9+5618vmWbLEypVsXUVBRVtaluZLu5NOTjrq77t9bvnxufRwrVWhS9vJ35lBrR+dk99bXS2e/Tq4IfDOi6UJ213RQ19avLZ"
        "XEjJKzurBSIkJCTMcOCoYbW2ZUhhn4WeZ43HVqDp4acYJ+9GnWs2moSXtHCLlTjZ2Wik1zWcHHXCjk8sHOcs8csVZrlpTj72"
        "zbUVKUpJzvBJJLn2TutOO/4WBD/z2/8ALmu//j9c3+oL/nf/AIKh/wDKz7z2VX/oZ4P/AMJqf/y45PX7S207V7nw9YwRwabZ"
        "6pexwwqOQBJjlvvMcIgyxJwoHTiv6Vw+Fp43Jp1K926lOHM7tX020asnrdKyd3fdni1aUcuo0sThtJ1Yx5nve0Vayd0vik/d"
        "S1dzp9Ps7dDNbLHiIZ4DH09etflmCzDEZjWp1MTLmfoutr7HxM8fiIUo1Yy1s+i8/Kx3EEsth4y8LaXZSNb2upjUY7sRnDMI"
        "7dSm1/vIQScFSDX5zSzzG4jLnj6sk6tKbjF8sdE3Tvpa2t2m2m7Nq9mz6jJcowcMDjMUoe/FUmm22ruck7ptpq3RpryPAD45"
        "8U+JbCDV9b1T7XfafIFtZTbxKYxIpV+FUA5CKOQcY471+mf2XhMjw8YYCHIqnxbu9rNfFfa72PKnjK2FpSxdJ2qU5xjF2Wil"
        "GrzK2zvyrdO1tLFz+yrH/ni3/f1v8a/S/wDVjK/+fX/k0v8AM/LfbT7n/9k="},
    {"ycc_422", 1463,
        "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAUDBAQEAwUEBAQFBQUGBwwIBwcHBw8LCwkMEQ8SEhEPERETFhwXExQaFRERGCEY"
        "Gh0dHx8fExciJCIeJBweHx7/2wBDAQUFBQcGBw4ICA4eFBEUHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4e"
        "Hh4eHh4eHh4eHh4eHh7/wAARCAATACEDASEAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAA"
        "AgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6"
        "Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXG"
        "x8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREA"
        "AgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5"
        "OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPE"
        "xcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDk/D3hrVNWDJe2SRy2yhVMLquNwyQSzHP3"
        "R0969T0saTomrWmny311HLqzbLULEzs7JyxwgwqgOCS2MDknGa9LGuGFoLAYW70lfWKlTi7yTvp70mm00nZJv3WlfyMyzjE5"
        "ziKmZ1Yr2lXWCWiairTerdrJdWvK5iaj4wuZbjQ9Q0RtJ1nVre3miuAtnNHBbO5GfK3lSCwwhZichBgJuYGn4e0qy0KNJYFj"
        "vLsABlfJ2kdug/Pr9M1tiac8BQlSnG9arupq9uWalFpqy2bb1a5mkvhlFfTZXDD5VSWMxkpU6dPrdS5U5SSvypveS26tu1tu"
        "o/4RXxR/zyh/75j/APiqK/Kb8P8A/Pxf+T/5HJ/rjxj/ANANP/wJf/LDnm8VafDf2tv4es9HuDcsxupE1eKCOBQyYdjg/wB4"
        "kAAscMQDg1haTq2r3WmadZWd/qkT2ytLc3smoyP9oeQDHGQFA2nABIAJ6kkn9by/LIYem8XjpKVSTfvTp2u/f5W7u7ivdSV9"
        "uSCa5k1wZXgljG8ZiafJHRxpyh7qVpRag3Zau03yx35bnT+H9KZwINOtpIm2KpkEZLED7vIz79z1rq4IfDOi6UJ213RQ19av"
        "LZXEjJKzurBSIkJCTMcOCoYbW2ZUhhn4ziDNa1WpanBzqTabjCracFeOi0ck5q7bSvbmnrZnZVn/AGhXvOLlhqTvNJJwrcyu"
        "o9I3jKOnxO92krM47/hYEP8Az2/8ua7/APj9FfM/6gv+d/8AgqH/AMrPuPZVf+hng/8Awmp//Ljk9ftLbTtXufD1jBHBptnq"
        "l7HDCo5AEmOW+8xwiDLEnCgdOK6fT7O3QzWyx4iGeAx9PXrX7ZxHOVOWEjF6VI3l5tSuvTWT2t+Ct8VnVWeDao0XaMU5Jb6y"
        "tJ73e7btstlZWO4gllsPGXhbS7KRre11MajHdiM4ZhHbqU2v95CCTgqQa8APjnxT4lsINX1vVPtd9p8gW1lNvEpjEilX4VQD"
        "kIo5BxjjvX5rw7UeNVTG10nVhCDTslrO8JaJJO8YRWq0tpZtlVKMMvw2Glh1yvllPv70Z1GnrfZxWm3lq73P7Ksf+eLf9/W/"
        "xor9x/1Yyv8A59f+TS/zPyP20+5//9k="},
    {"ycc_440", 1469,
        "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAUDBAQEAwUEBAQFBQUGBwwIBwcHBw8LCwkMEQ8SEhEPERETFhwXExQaFRERGCEY"
        "Gh0dHx8fExciJCIeJBweHx7/2wBDAQUFBQcGBw4ICA4eFBEUHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4e"
        "Hh4eHh4eHh4eHh4eHh7/wAARCAATACEDARIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAA"
        "AgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6"
        "Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXG"
        "x8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREA"
        "AgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5"
        "OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPE"
        "xcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDk/D3hrVNWDJe2SRy2yhVMLquNwyQSzHP3"
        "R0967NvFWnw39rb+HrPR7g3LMbqRNXigjgUMmHY4P94kAAscMQDg161GeJh7WGAwqja1+ZpKW9rKMteu8r28728rG5njs3Xt"
        "cVWniOXZr3FG/wDjV5NpaW7O/Q6DSxpOiataafLfXUcurNstQsTOzsnLHCDCqA4JLYwOScZrgtJ1bV7rTNOsrO/1SJ7ZWlub"
        "2TUZH+0PIBjjICgbTgAkAE9SSTpicqrT9pQxFbRKLirxX3WST3TtJpvSyskdMskxbdSlVap/yuLVpPrde+2lpu47s6LUfGFz"
        "LcaHqGiNpOs6tb280VwFs5o4LZ3Iz5W8qQWGELMTkIMBNzAy+H9KZwINOtpIm2KpkEZLED7vIz79z1rHEYnLaL92PtFPd1Ha"
        "zW3LdR280/Rat9ijlmBUozm+SbT0V9u6hF7efm77md4e0qy0KNJYFjvLsABlfJ2kdug/Pr9M13kEPhnRdKE7a7ooa+tXlsri"
        "RklZ3VgpESEhJmOHBUMNrbMqQwz81iOKPrc8PCTcrXV1Cokm+V3bcZSs9HeKdreh6EMxzChKToUY0NfedW8k9NLKnLm0W+jv"
        "eJl/8Ir4o/55Q/8AfMf/AMVWF/wsCH/nt/5c13/8fr5P6tmv/QFD7l/8md/9l5n/ANBdf/wPDf8AyZyev2ltp2r3Ph6xgjg0"
        "2z1S9jhhUcgCTHLfeY4RBliThQOnFFf01gaVOvknNVipOVOF7q99Ot9/mVjYRw2Fw9WguWU4x5mtHK0Y2u1q7Xe/d9zp9Ps7"
        "dDNbLHiIZ4DH09etFfkWVYmtialOVabk9N2327nwNXEVo0IyU3ez1u/M7iCWWw8ZeFtLspGt7XUxqMd2IzhmEdupTa/3kIJO"
        "CpBor8zw+YYqpk88ROrJ1IVLRk2+ZJundJ3uk+qR9vk+Dw8ctx1ZU0ppUmnZXT55bPdHgB8c+KfEthBq+t6p9rvtPkC2spt4"
        "lMYkUq/CqAchFHIOMcd6K/X8bhKGAoU44SCpqV78qUb25bXta9rvfufOV69SlhalanJqcakEmnZpONW6T3SdldLey7Fz+yrH"
        "/ni3/f1v8aK/U/7KwP8Az5h/4Cv8j8v9pLuf/9k="},
    {"ycc_420", 1315,
        "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAUDBAQEAwUEBAQFBQUGBwwIBwcHBw8LCwkMEQ8SEhEPERETFhwXExQaFRERGCEY"
        "Gh0dHx8fExciJCIeJBweHx7/2wBDAQUFBQcGBw4ICA4eFBEUHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4e"
        "Hh4eHh4eHh4eHh4eHh7/wAARCAATACEDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAA"
        "AgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6"
        "Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXG"
        "x8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREA"
        "AgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5"
        "OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPE"
        "xcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDk/D3hrVNWDJe2SRy2yhVMLquNwyQSzHP3"
        "R0969T0saTomrWmny311HLqzbLULEzs7JyxwgwqgOCS2MDknGa59vFWnw39rb+HrPR7g3LMbqRNXigjgUMmHY4P94kAAscMQ"
        "Dg1haTq2r3WmadZWd/qkT2ytLc3smoyP9oeQDHGQFA2nABIAJ6kkn2I4bEYmlbl9jS1SV43d+bfVpK6TSi1v7zetvOVPG5tU"
        "liak3Ny+Ko9G9Gk4p+8ldKMrpt9LLU6LUfGFzLcaHqGiNpOs6tb280VwFs5o4LZ3Iz5W8qQWGELMTkIMBNzA0/D2lWWhRpLA"
        "sd5dgAMr5O0jt0H59fpmtHw/pTOBBp1tJE2xVMgjJYgfd5GffuetdXBD4Z0XShO2u6KGvrV5bK4kZJWd1YKREhISZjhwVDDa"
        "2zKkMM+RmmcYbCJ0aF7Sau7Scpe9GSbtF8vK22rxTa+FN3v7NCvHCNQwMVKUftS+CCd2ubl5W005K0LtPddHl/8ACK+KP+eU"
        "P/fMf/xVFYX/AAsCH/nt/wCXNd//AB+ivgfq2a/9AUPuX/yZ6/8AZmZ/9Bdf/wADw3/yZyev2ltp2r3Ph6xgjg02z1S9jhhU"
        "cgCTHLfeY4RBliThQOnFdPp9nboZrZY8RDPAY+nr1oor9r4lk4ywSTtzR187STV++rb17vueFxBOVCpyUnypRuktEm1Ft6dW"
        "22+71O4gllsPGXhbS7KRre11MajHdiM4ZhHbqU2v95CCTgqQa8APjnxT4lsINX1vVPtd9p8gW1lNvEpjEilX4VQDkIo5Bxjj"
        "vRRX5rwvOVejVr1XzTUKbUnq05SlGTTeqbikn3SSeiRtjIRw2Fwzorl9yctNNVOrZ6dVZWe+i7Fz+yrH/ni3/f1v8aKKK/oL"
        "+ysD/wA+Yf8AgK/yPx7nl3P/2Q=="},
    {"ycc_411", 1293,
        "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAUDBAQEAwUEBAQFBQUGBwwIBwcHBw8LCwkMEQ8SEhEPERETFhwXExQaFRERGCEY"
        "Gh0dHx8fExciJCIeJBweHx7/2wBDAQUFBQcGBw4ICA4eFBEUHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4e"
        "Hh4eHh4eHh4eHh4eHh7/wAARCAATACEDAUEAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAA"
        "AgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6"
        "Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXG"
        "x8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREA"
        "AgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5"
        "OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPE"
        "xcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDk/D3hrVNWDJe2SRy2yhVMLquNwyQSzHP3"
        "R0969T0saTomrWmny311HLqzbLULEzs7JyxwgwqgOCS2MDknGaxNR8YXMtxoeoaI2k6zq1vbzRXAWzmjgtncjPlbypBYYQsx"
        "OQgwE3MDT8PaVZaFGksCx3l2AAyvk7SO3Qfn1+ma9WUeT2k4K8KUeSN7OLk1ypSS1TV5Rb0V4rpvnlmLeLx1TMsT7sdak3Hp"
        "d8701duXllpd7q99F1H/AAivij/nlD/3zH/8VRRRX5Zfh/8A5+L/AMn/AMjo/wBceMf+gGn/AOBL/wCWHPN4q0+G/tbfw9Z6"
        "PcG5ZjdSJq8UEcChkw7HB/vEgAFjhiAcGsLSdW1e60zTrKzv9Uie2Vpbm9k1GR/tDyAY4yAoG04AJABPUkk9P4f0pnAg062k"
        "ibYqmQRksQPu8jPv3PWurgh8M6LpQnbXdFDX1q8tlcSMkrO6sFIiQkJMxw4KhhtbZlSGGf0LOnLL8sjSXvVZauz9nOUnp5ty"
        "1lNbvmi+upzYaklhoRlC0qz5pLltJQV5ODT1cOVcivZcs1payfHf8LAh/wCe3/lzXf8A8fooor8//wBQX/O//BUP/lZ997Kr"
        "/wBDPB/+E1P/AOXHJ6/aW2navc+HrGCODTbPVL2OGFRyAJMct95jhEGWJOFA6cV0+n2duhmtljxEM8Bj6evWu4gllsPGXhbS"
        "7KRre11MajHdiM4ZhHbqU2v95CCTgqQa8APjnxT4lsINX1vVPtd9p8gW1lNvEpjEilX4VQDkIo5BxjjvX6PxDUlPOKeHlrFX"
        "kk+8acZxd99JNve3R6aHxdTTFVv+naSj5Lmivnokru7/ABLn9lWP/PFv+/rf40UUV+mf6sZX/wA+v/Jpf5n5J7afc//Z"},
    {"ycc_420_progressive", 1216,
        "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAUDBAQEAwUEBAQFBQUGBwwIBwcHBw8LCwkMEQ8SEhEPERETFhwXExQaFRERGCEY"
        "Gh0dHx8fExciJCIeJBweHx7/2wBDAQUFBQcGBw4ICA4eFBEUHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4e"
        "Hh4eHh4eHh4eHh4eHh7/wgARCAATACEDASIAAhEBAxEB/8QAGQABAQEBAQEAAAAAAAAAAAAAAAUEAwYH/8QAGAEBAQADAAAA"
        "AAAAAAAAAAAABAYCBQf/2gAMAwEAAhADEAAAAZPqZ+FhqPHRWG3KwNAuTULYNz5+TWfYdBjv/8QAGxAAAgMBAQEAAAAAAAAA"
        "AAAAAgMAAQQUExX/2gAIAQEAAQUCz5mtivJLWbCss6gRXLqnUujU1pLzqlVmSr6FR4itqwGVdhs7tWkPIJ//xAAlEQABAgMH"
        "BQAAAAAAAAAAAAABAgMAESEEBRIxQWHRFFNxkaH/2gAIAQMBAT8BwvWtRcUZzzVxrsYQsNUYEyNTkPMpb5R01p7q/bfMXgSh"
        "Uk0pxDwDbTeClCfqomY//8QAKxEAAQEFBAoDAAAAAAAAAAAAAQIAAxEhcQQFYYESExQiMUFRUlPwkaHB/9oACAECAQE/AQ7e"
        "PE8NBOWPss2tVsdutxHOsTMHpKFKNq7V4R7m15GBc4j9a6yVoUtUzBM6kg/TbK47B8N//8QAJxAAAgECBAYCAwAAAAAAAAAA"
        "AQIDABESEyEiBDEyUWGhcYGRpNH/2gAIAQEABj8C3pYr2pYy7Ay9OlQSQ5U0qqQ2wgL8VcWdq5D1Srw6Qti6jmgAVGiPKMOr"
        "OZDuvVo1I82q+fDvW6Mdfx3rn+y39puHQARpK4A+6K20rhYkOFZMwNbwtCWaXE8Z2nCNL1y91//EACMQAQACAgIBAwUAAAAA"
        "AAAAAAERMQAhQWFREHGRgbHB0fH/2gAIAQEAAT8hkQAQTC/d6yXmYilUuqN84gMUip7RdS+OJcADyR4z+T+2M3AYism3597x"
        "+IFAl9lYjuEBydZPsroCUY0p3qdMa36PmTYf5W0X4wEXyYzDyyYIhs+mUPZEaIaOjO38s//aAAwDAQACAAMAAAAQZKPgc9//"
        "xAAcEQEAAgMBAQEAAAAAAAAAAAABESEAMUFRYXH/2gAIAQMBAT8QFFexS0ghoSEgryC8dCfgNlNhERE0dnHP8q4EgKBRLXVV"
        "fW8VR41WRtdIId0eZ9s//8QAHREBAQACAgMBAAAAAAAAAAAAAREAITFRQWHx0f/aAAgBAgEBPxCL8kC03y2gUEEc7O4G1iKx"
        "aAsWxUoU4Lb88ywRnJ7gS97V3294q8WFtFALsUA9gDoM+S/M/8QAIBABAQADAAIBBQAAAAAAAAAAAREAITFBUWEQcZHB8P/a"
        "AAgBAQABPxAweyRIqKl4584IqJbVVDACrENrLhYkJDy70IjRoYSJ7iBS0eOH579r9CCEhgeotH2QBUQMchdc6omqAKgKAvVV"
        "FL1AlB7F+fL3CJQONiHRkKB0oFwM/qIbAjfRDqWA5rEYhuhevfcPUJVAR5qKxQ4z+hKgmkBoWxk15z+i/ef/2Q=="},
    {"ycc_420_restart", 1331,
        "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAUDBAQEAwUEBAQFBQUGBwwIBwcHBw8LCwkMEQ8SEhEPERETFhwXExQaFRERGCEY"
        "Gh0dHx8fExciJCIeJBweHx7/2wBDAQUFBQcGBw4ICA4eFBEUHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4e"
        "Hh4eHh4eHh4eHh4eHh7/wAARCAATACEDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAA"
        "AgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6"
        "Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXG"
        "x8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREA"
        "AgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5"
        "OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPE"
        "xcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/90ABAAB/9oADAMBAAIRAxEAPwDk/D3hrVNWDJe2SRy2yhVMLquN"
        "wyQSzHP3R0969T0saTomrWmny311HLqzbLULEzs7JyxwgwqgOCS2MDknGa59vFWnw39rb+HrPR7g3LMbqRNXigjgUMmHY4P9"
        "4kAAscMQDg1haTq2r3WmadZWd/qkT2ytLc3smoyP9oeQDHGQFA2nABIAJ6kkn2I4bEYmlbl9jS1SV43d+bfVpK6TSi1v7zet"
        "vOVPG5tUliak3Ny+Ko9G9Gk4p+8ldKMrpt9LLU//0Lmo+MLmW40PUNEbSdZ1a3t5orgLZzRwWzuRnyt5UgsMIWYnIQYCbmBp"
        "+HtKstCjSWBY7y7AAZXydpHboPz6/TNaPh/SmcCDTraSJtiqZBGSxA+7yM+/c9a6uCHwzoulCdtd0UNfWry2VxIySs7qwUiJ"
        "CQkzHDgqGG1tmVIYZeaZxhsInRoXtJq7tJyl70ZJu0Xy8rbavFNr4U3e/RQrxwjUMDFSlH7Uvggndrm5eVtNOStC7T3XR//R"
        "6j/hFfFH/PKH/vmP/wCKorC/4WBD/wA9v/Lmu/8A4/RXwX1bNf8AoCh9y/8Akz3/AOzMz/6C6/8A4Hhv/kz/0ue1+0ttO1e5"
        "8PWMEcGm2eqXscMKjkASY5b7zHCIMsScKB04rp9Ps7dDNbLHiIZ4DH09etFFfXcSycZYJJ25o6+dpJq/fVt6933K4gnKhU5K"
        "T5Uo3SWiTai29Orbbfd6n//T76CWWw8ZeFtLspGt7XUxqMd2IzhmEdupTa/3kIJOCpBrwA+OfFPiWwg1fW9U+132nyBbWU28"
        "SmMSKVfhVAOQijkHGOO9FFeHwvOVejVr1XzTUKbUnq05SlGTTeqbikn3SSeiR0YyEcNhcM6K5fcnLTTVTq2enVWVnvoux//U"
        "4r+yrH/ni3/f1v8AGiiiv2b+ysD/AM+Yf+Ar/I/JueXc/9k="},
    {"cmyk", 2697,
        "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"},
    {"gray_lossless", 472,
        "/9j/4AAQSkZJRgABAQAAAQABAAD/wwALCAATACEBAREA/8QAGAABAQEBAQAAAAAAAAAAAAAAAwcIAgD/2gAIAQEAAQAA87vB"
        "nd3d30O7vBnd3d3gzu+h3d37d3gzu7u76Hd3gzu7u7wZ3fQ7u79u7wZ3d3d9Du7wZ3d3d4M7vod3d4E7u7u+h3d4M7u7u8Gd"
        "30O7u7u8G7d3d3fQ7u8Gd3d3eDO76Hd3d3eDdu7u7vod3eDO7u7vBnd9Du7u7vBu3d9Du7wZ3d3d4M7vod3d3d4M7voft3fQ"
        "7u8Gd3d3eDO76Hd3d3eDO76H7d30O7vBnd3d3gzu+h3d3d3gzu+h9Bu7wZ3d3d9Du+BtDu7u7vBnd9Du7v27vBnd3d30O+Bn"
        "0O7u7u8Gd30O7u/bu8Gd3d3fQ74GfQ7u7u7wZ3fQ7u+BoE7u7u+h3d4M7u7u8Gd30O7u7u8G7d3d3fQ7u8Gd3d3eDO76Hd3d"
        "3wNoft3d3d9Du7wZ3d3d4M7vod3d3fA2h+3d9Du7wZ3d3d4M7vod3d3d4M7vBu3d9Du7wZ3d3d4M7vod3d3d4M7vBu3d9Du7"
        "wZ3d3d4M7vod3d3d4M7vBtBu7wZ3d3d4M7vod3d3d4M7vofAzu//2Q=="},
    };

    struct Expected
    {
        const char *name;
        uint64_t hash;
    };

    // Recorded with the bundled IJG 6b (DCMTK) before the switch
    const Expected EXPECTED[] = {
        {"resources/maps/map_aweeri.jpg", 0x245ff852fca1cc31ULL},
        {"resources/maps/map_ne2.jpg", 0xc903d90c95947039ULL},
        {"resources/maps/map_ne2_div2.jpg", 0xd3bf33e2b7d3e11eULL},
        {"resources/maps/nasa.jpg", 0xb62b01c1ef5620a3ULL},
        {"resources/maps/nasa_hd.jpg", 0x9100f98ee0797c55ULL},
        {"resources/maps/nasa_night.jpg", 0x90502f8633ef93b3ULL},
        {"resources/descriptions/img/cloudphase.jpg", 0xb06edea511510c45ULL},
        {"resources/descriptions/img/firetemp.jpg", 0xe7b0e8ac87be4ab3ULL},
        {"gray load_jpeg(buffer)", 0x350a2ffbaae57a7dULL},
        {"gray decompress_jpeg", 0x350a2ffbaae57a7dULL},
        {"gray decompress_jpeg(ignore_errors)", 0x350a2ffbaae57a7dULL},
        {"gray_progressive load_jpeg(buffer)", 0x350a2ffbaae57a7dULL},
        {"gray_progressive decompress_jpeg", 0x350a2ffbaae57a7dULL},
        {"gray_progressive decompress_jpeg(ignore_errors)", 0x350a2ffbaae57a7dULL},
        {"ycc_444 load_jpeg(buffer)", 0xe92e027ee8938698ULL},
        {"ycc_422 load_jpeg(buffer)", 0x915b155a2e953428ULL},
        {"ycc_440 load_jpeg(buffer)", 0xe6ec07d832eefb93ULL},
        {"ycc_420 load_jpeg(buffer)", 0xac3130e6a9cfb717ULL},
        {"ycc_411 load_jpeg(buffer)", 0x26127b148d0b9a9fULL},
        {"ycc_420_progressive load_jpeg(buffer)", 0xac3130e6a9cfb717ULL},
        {"cmyk load_jpeg(buffer)", 0xb57233584874aebeULL},
        {"gray_lossless load_jpeg(buffer)", 0x766bf42e2af70a35ULL},
        {"gray_lossless decompress_jpeg", 0x766bf42e2af70a35ULL},
        {"gray_lossless decompress_jpeg(ignore_errors)", 0x766bf42e2af70a35ULL},
        {"gray cut at 60% load_jpeg(buffer)", 0x3603c6ec3ffa8415ULL},
        {"gray cut at 60% decompress_jpeg", 0x3603c6ec3ffa8415ULL},
        {"gray cut at 60% decompress_jpeg(ignore_errors)", 0x3603c6ec3ffa8415ULL},
        {"gray damaged load_jpeg(buffer)", 0x80b07c5d82e70f0fULL},
        {"gray damaged decompress_jpeg", 0x80b07c5d82e70f0fULL},
        {"gray_progressive damaged load_jpeg(buffer)", 0x171bb79dc8e18503ULL}, // refused: no picture
        {"gray_progressive damaged decompress_jpeg", 0x171bb79dc8e18503ULL},
        {"gray_progressive cut at 60% load_jpeg(buffer)", 0x31549233b8316248ULL},
        {"gray_progressive cut at 60% decompress_jpeg", 0x31549233b8316248ULL},
        {"gray_progressive cut at 60% decompress_jpeg(ignore_errors)", 0x31549233b8316248ULL},
        {"ycc_444 cut at 60% load_jpeg(buffer)", 0x293d99f9b9f11d1eULL},
        {"ycc_444 damaged load_jpeg(buffer)", 0x982125ac8c42e1aaULL},
        {"ycc_422 cut at 60% load_jpeg(buffer)", 0x1c27a09e9b6f275eULL},
        {"ycc_422 damaged load_jpeg(buffer)", 0x9b1225d2e33d1fc4ULL},
        {"ycc_440 cut at 60% load_jpeg(buffer)", 0x27ec1a4cb41ba774ULL},
        {"ycc_440 damaged load_jpeg(buffer)", 0x1453a179835d53adULL},
        {"ycc_420 cut at 60% load_jpeg(buffer)", 0x6c6dab19c38357a9ULL},
        {"ycc_420 damaged load_jpeg(buffer)", 0xeb543a03b09b53f8ULL},
        {"ycc_411 cut at 60% load_jpeg(buffer)", 0xd51a5cbffa3745a0ULL},
        {"ycc_411 damaged load_jpeg(buffer)", 0x125df8a599279676ULL},
        {"ycc_420_progressive cut at 60% load_jpeg(buffer)", 0x95a69ef809f290abULL},
        {"ycc_420_progressive damaged load_jpeg(buffer)", 0x6829eb58d97ed139ULL},
        {"ycc_420_restart cut at 60% load_jpeg(buffer)", 0x392e8b10b471d027ULL},
        {"ycc_420_restart damaged load_jpeg(buffer)", 0x769cddd9646e3428ULL},
        {"cmyk cut at 60% load_jpeg(buffer)", 0x8735d7eb5ad754d9ULL},
        {"cmyk damaged load_jpeg(buffer)", 0x1287aaa3054ca53eULL},
        {"gray_lossless cut at 60% load_jpeg(buffer)", 0x23dc83aa67ca24cdULL},
        {"gray_lossless cut at 60% decompress_jpeg", 0x23dc83aa67ca24cdULL},
        {"gray_lossless cut at 60% decompress_jpeg(ignore_errors)", 0x23dc83aa67ca24cdULL},
        {"gray_lossless damaged load_jpeg(buffer)", 0x0c6e1c27f4b30929ULL},
        {"gray_lossless damaged decompress_jpeg", 0x0c6e1c27f4b30929ULL},
        {"8-bit 1x1x1 save_jpeg bytes", 0xcee7f81805e5693fULL},
        {"8-bit 1x1x1 save_jpeg pixels", 0x27ff636b97f4de0fULL},
        {"8-bit 1x1x1 save_jpeg_mem bytes", 0xcee7f81805e5693fULL},
        {"8-bit 1x1x1 save_jpeg_mem pixels", 0x27ff636b97f4de0fULL},
        {"8-bit 7x5x1 save_jpeg bytes", 0xe4c3a7e18a3fea15ULL},
        {"8-bit 7x5x1 save_jpeg pixels", 0xb3e1a257290535f1ULL},
        {"8-bit 7x5x1 save_jpeg_mem bytes", 0xe4c3a7e18a3fea15ULL},
        {"8-bit 7x5x1 save_jpeg_mem pixels", 0xb3e1a257290535f1ULL},
        {"8-bit 37x23x1 save_jpeg bytes", 0x422304889ba5389eULL},
        {"8-bit 37x23x1 save_jpeg pixels", 0x700bc88adf3606c1ULL},
        {"8-bit 37x23x1 save_jpeg_mem bytes", 0x422304889ba5389eULL},
        {"8-bit 37x23x1 save_jpeg_mem pixels", 0x700bc88adf3606c1ULL},
        {"16-bit 320x240x1 save_jpeg bytes", 0x8c9b47a478ba6986ULL},
        {"16-bit 320x240x1 save_jpeg pixels", 0xdbf9e9c40ad275ccULL},
        {"16-bit 320x240x1 save_jpeg_mem bytes", 0x8c9b47a478ba6986ULL},
        {"16-bit 320x240x1 save_jpeg_mem pixels", 0xdbf9e9c40ad275ccULL},
        {"8-bit 37x23x3 save_jpeg bytes", 0xdecb11dee41dfddfULL},
        {"8-bit 37x23x3 save_jpeg pixels", 0x2da075fac24ae413ULL},
        {"8-bit 37x23x3 save_jpeg_mem bytes", 0xdecb11dee41dfddfULL},
        {"8-bit 37x23x3 save_jpeg_mem pixels", 0x2da075fac24ae413ULL},
        {"16-bit 37x23x3 save_jpeg bytes", 0xdecb11dee41dfddfULL},
        {"16-bit 37x23x3 save_jpeg pixels", 0x2da075fac24ae413ULL},
        {"16-bit 37x23x3 save_jpeg_mem bytes", 0xdecb11dee41dfddfULL},
        {"16-bit 37x23x3 save_jpeg_mem pixels", 0x2da075fac24ae413ULL},
        {"8-bit 64x48x3 save_jpeg bytes", 0x075c9e5a1c49eb2aULL},
        {"8-bit 64x48x3 save_jpeg pixels", 0xa6216d4391a4d5fcULL},
        {"8-bit 64x48x3 save_jpeg_mem bytes", 0x075c9e5a1c49eb2aULL},
        {"8-bit 64x48x3 save_jpeg_mem pixels", 0xa6216d4391a4d5fcULL},
        {"8-bit 641x479x3 save_jpeg bytes", 0x77cb6f7d180ae7ebULL},
        {"8-bit 641x479x3 save_jpeg pixels", 0xd11dc3989eb7bf43ULL},
        {"8-bit 641x479x3 save_jpeg_mem bytes", 0x77cb6f7d180ae7ebULL},
        {"8-bit 641x479x3 save_jpeg_mem pixels", 0xd11dc3989eb7bf43ULL},
        {"8-bit 37x23x4 save_jpeg bytes", 0x60089889e0cf7196ULL},
        {"8-bit 37x23x4 save_jpeg pixels", 0x977a70ea4288f96dULL},
        {"8-bit 37x23x4 save_jpeg_mem bytes", 0xdecb11dee41dfddfULL},
        {"8-bit 37x23x4 save_jpeg_mem pixels", 0x2da075fac24ae413ULL},
        {"16-bit 37x23x4 save_jpeg bytes", 0x9f191c3a23ddf167ULL},
        {"16-bit 37x23x4 save_jpeg pixels", 0xcb396cc2bbb1d703ULL},
        {"16-bit 37x23x4 save_jpeg_mem bytes", 0xdecb11dee41dfddfULL},
        {"16-bit 37x23x4 save_jpeg_mem pixels", 0x2da075fac24ae413ULL},
        {"ycc_420_restart load_jpeg(buffer)", 0xac3130e6a9cfb717ULL},
    };

    void check(const std::string &name, uint64_t h)
    {
        uint64_t want = 0;
        bool found = false;
        for (auto &e : EXPECTED)
            if (name == e.name)
            {
                want = e.hash;
                found = true;
            }
        INFO(name);
        CHECK(found);
        CHECK(h == want);
    }

    // A test picture: gradients, sharp edges and noise, different in each channel
    Image test_picture(int depth, size_t w, size_t h, int channels)
    {
        Image img(depth, w, h, channels);
        uint32_t lcg = 12345u;
        for (int c = 0; c < channels; c++)
            for (size_t y = 0; y < h; y++)
                for (size_t x = 0; x < w; x++)
                {
                    lcg = lcg * 1103515245u + 12345u;
                    int v = int((x * (5 + 3 * c) + y * (2 + 7 * c)) * 255 / (w + h + 1));
                    if (((x / 6) + (y / 5) + c) % 4 == 0)
                        v = 255 - v;
                    v += int((lcg >> 16) % 31) - 15;
                    v = v < 0 ? 0 : (v > 255 ? 255 : v);
                    img.set(c, y * w + x, depth == 16 ? (v << 8) | (v ^ 0x5A) : v);
                }
        return img;
    }
} // namespace

TEST_CASE("JPEG: the pictures in resources/ decode to the same pixels as before")
{
    const char *files[] = {
        "maps/map_aweeri.jpg",
        "maps/map_ne2.jpg",
        "maps/map_ne2_div2.jpg",
        "maps/nasa.jpg",
        "maps/nasa_hd.jpg",
        "maps/nasa_night.jpg",
        "descriptions/img/cloudphase.jpg",
        "descriptions/img/firetemp.jpg",
    };
    for (auto f : files)
    {
        Image img;
        satdump::image::load_jpeg(img, std::string(SATDUMP_TEST_SOURCE_DIR) + "/resources/" + f);
        REQUIRE(img.size() > 0);
        check(std::string("resources/") + f, image_hash(img));
    }
}

TEST_CASE("JPEG: streams with other encoder settings decode to the same pixels as before")
{
    TempDir dir("satdump-jpeg8-test");
    for (auto &s : STREAMS)
    {
        std::vector<uint8_t> data = from_base64(s.base64);
        REQUIRE(data.size() == s.size);
        std::string name = s.name;
        bool gray = name.rfind("gray", 0) == 0;

        Image img;
        satdump::image::load_jpeg(img, data.data(), data.size());
        uint64_t from_buffer = image_hash(img);
        check(name + " load_jpeg(buffer)", from_buffer);

        // The file reader gives the same pixels as the memory one
        fs::path p = dir.path / (name + ".jpg");
        write_file(p, data);
        Image img2;
        satdump::image::load_jpeg(img2, p.string());
        CHECK(image_hash(img2) == from_buffer);

        if (gray) // decompress_jpeg is for grayscale streams only
        {
            Image a = satdump::image::decompress_jpeg(data.data(), data.size());
            check(name + " decompress_jpeg", image_hash(a));
            Image b = satdump::image::decompress_jpeg(data.data(), data.size(), true);
            check(name + " decompress_jpeg(ignore_errors)", image_hash(b));
        }
    }
}

TEST_CASE("JPEG: cut-off and damaged streams decode to the same pixels as before")
{
    TempDir dir("satdump-jpeg8-damaged-test");
    for (auto &s : STREAMS)
    {
        std::string name = s.name;
        std::vector<uint8_t> data = from_base64(s.base64);
        bool gray = name.rfind("gray", 0) == 0;

        // Cut off: the decoder warns and fills the rest
        std::vector<uint8_t> cut(data.begin(), data.begin() + data.size() * 6 / 10);
        Image img;
        satdump::image::load_jpeg(img, cut.data(), cut.size());
        check(name + " cut at 60% load_jpeg(buffer)", image_hash(img));
        if (gray)
        {
            Image a = satdump::image::decompress_jpeg(cut.data(), cut.size());
            check(name + " cut at 60% decompress_jpeg", image_hash(a));
            Image b = satdump::image::decompress_jpeg(cut.data(), cut.size(), true);
            check(name + " cut at 60% decompress_jpeg(ignore_errors)", image_hash(b));
        }

        // Damaged picture data (past the headers). libjpeg refuses the progressive grayscale
        // one outright, which is the error path of load_jpeg and decompress_jpeg
        std::vector<uint8_t> bad = data;
        for (size_t f : {75, 76, 90})
            bad[bad.size() * f / 100] ^= 0x5A;
        Image img3;
        satdump::image::load_jpeg(img3, bad.data(), bad.size());
        check(name + " damaged load_jpeg(buffer)", image_hash(img3));
        fs::path p = dir.path / (name + "-damaged.jpg");
        write_file(p, bad);
        Image img4;
        satdump::image::load_jpeg(img4, p.string());
        CHECK(image_hash(img4) == image_hash(img3));
        if (gray)
        {
            Image a = satdump::image::decompress_jpeg(bad.data(), bad.size());
            check(name + " damaged decompress_jpeg", image_hash(a));
        }
    }
}

TEST_CASE("JPEG: pictures SatDump saves are the same bytes and pixels as before")
{
    TempDir dir("satdump-jpeg8-save-test");
    struct Size
    {
        int depth;
        size_t w, h;
        int channels;
    };
    const Size sizes[] = {{8, 1, 1, 1}, {8, 7, 5, 1}, {8, 37, 23, 1}, {16, 320, 240, 1}, {8, 37, 23, 3},
                          {16, 37, 23, 3}, {8, 64, 48, 3}, {8, 641, 479, 3}, {8, 37, 23, 4}, {16, 37, 23, 4}};
    for (auto &sz : sizes)
    {
        std::string name = std::to_string(sz.depth) + "-bit " + std::to_string(sz.w) + "x" + std::to_string(sz.h) + "x" + std::to_string(sz.channels);
        Image img = test_picture(sz.depth, sz.w, sz.h, sz.channels);

        fs::path p = dir.path / "saved.jpg";
        satdump::image::save_jpeg(img, p.string());
        std::vector<uint8_t> file = read_file(p);
        check(name + " save_jpeg bytes", bytes_hash(file));
        Image back;
        satdump::image::load_jpeg(back, p.string());
        check(name + " save_jpeg pixels", image_hash(back));

        std::vector<uint8_t> mem = satdump::image::save_jpeg_mem(img);
        check(name + " save_jpeg_mem bytes", bytes_hash(mem));
        Image back2;
        satdump::image::load_jpeg(back2, mem.data(), mem.size());
        check(name + " save_jpeg_mem pixels", image_hash(back2));
        if (sz.channels == 1)
        {
            Image back3 = satdump::image::decompress_jpeg(mem.data(), mem.size());
            CHECK(image_hash(back3) == image_hash(back2));
        }
    }
}

TEST_CASE("JPEG: a picture too wide for JPEG is refused as before")
{
    // libjpeg refuses pictures over 65500 pixels wide; SatDump then saves nothing (the file it
    // opened stays empty). In the sanitizer build this checks that the refusal releases
    // libjpeg's memory and the output manager of save_jpeg_mem.
    TempDir dir("satdump-jpeg8-wide-test");
    for (int channels : {1, 3})
    {
        INFO(channels, " channels");
        Image img = test_picture(8, 65501, 1, channels);
        std::vector<uint8_t> mem = satdump::image::save_jpeg_mem(img);
        CHECK(mem.empty());

        fs::path p = dir.path / "wide.jpg";
        satdump::image::save_jpeg(img, p.string());
        REQUIRE(fs::exists(p));
        CHECK(fs::file_size(p) == 0);
    }
}

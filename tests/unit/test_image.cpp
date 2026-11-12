#include "doctest.h"
#include "image/image.h"
#include "image/meta.h"
#include "image/processing.h"

using satdump::image::Image;

TEST_CASE("Image: an image of bit depth 0 has maxval 0 (no negative shift)")
{
    Image img(0, 0, 0, 1);
    CHECK(img.maxval() == 0);
    CHECK(Image(8, 1, 1, 1).maxval() == 255);
    CHECK(Image(16, 1, 1, 1).maxval() == 65535);
}

TEST_CASE("Image processing: empty images are left alone (no reads past the end)")
{
    // A pass with no data produces 0x0 images; the sanitizer build catches any stray read
    Image empty8(8, 0, 0, 3);
    satdump::image::white_balance(empty8);
    satdump::image::equalize(empty8, false);
    satdump::image::equalize(empty8, true);
    CHECK(empty8.width() == 0);

    Image depth0(0, 0, 0, 1);
    satdump::image::equalize(depth0, false);
    CHECK(depth0.maxval() == 0);
}

TEST_CASE("Image processing: white balance leaves a flat channel unchanged (was a division by 0)")
{
    Image img(8, 16, 16, 2);
    for (size_t i = 0; i < 16 * 16; i++)
    {
        img.set(0, i, 77);         // flat
        img.set(1, i, i % 2 ? 200 : 40); // two levels: stretched to the full range
    }
    satdump::image::white_balance(img);
    for (size_t i = 0; i < 16 * 16; i++)
    {
        CHECK(img.get(0, i) == 77);
        CHECK(img.get(1, i) == (i % 2 ? 255 : 0));
    }
}

TEST_CASE("Image: copying an image that has no buffer gives an empty image")
{
    // A product with a missing image holds a default Image; copying it read from a null pointer
    Image none;
    Image copy(none);
    CHECK(copy.width() == 0);
    CHECK(copy.raw_data() == nullptr);

    Image some(8, 2, 2, 1);
    some.set(0, 3, 9);
    Image copy2(some);
    CHECK(copy2.get(0, 3) == 9);
}

TEST_CASE("Image: new metadata replaces the old in place (it used to leak the old metadata)")
{
    Image img(8, 4, 4, 1);
    satdump::image::set_metadata(img, {{"a", 1}});
    const void *first = img.metadata_obj;
    satdump::image::set_metadata(img, {{"b", 2}});
    CHECK(img.metadata_obj == first);
    CHECK(satdump::image::get_metadata(img) == nlohmann::json{{"b", 2}});

    Image other(8, 4, 4, 1);
    satdump::image::set_metadata(other, {{"c", 3}});
    const void *other_first = other.metadata_obj;
    other = img;
    CHECK(other.metadata_obj == other_first);
    CHECK(satdump::image::get_metadata(other) == nlohmann::json{{"b", 2}});
}

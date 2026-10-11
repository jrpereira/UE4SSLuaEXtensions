// Tail line splitting, held partial lines, long-line pieces and resets.

#include <TestSupport.hpp>

#include <FileBridgeTail.hpp>

#include <string>
#include <vector>

using namespace UE4SSLuaFileBridge::Core;
namespace Flag = UE4SSLuaFileBridge::NativeContract::DeliveryFlag;

namespace
{
void lines()
{
    TailState tail(false);
    std::vector<TailDelivery> out;
    tail.feed("one\ntw", out);
    CHECK(out.size() == 1 && out[0].text == "one" && out[0].offset == 0 && out[0].flags == 0);
    CHECK(tail.held_bytes() == 2 && tail.read_offset() == 6);
    tail.feed("o\r\nthree\n\n", out);
    CHECK(out.size() == 4);
    CHECK(out[1].text == "two" && out[1].offset == 4);
    CHECK(out[2].text == "three" && out[2].offset == 9);
    CHECK(out[3].text.empty() && out[3].offset == 15);
    CHECK(tail.read_offset() == 16 && tail.held_bytes() == 0);
}

void start_at_end()
{
    TailState tail(false);
    tail.start_at(100);
    std::vector<TailDelivery> out;
    tail.feed("x\n", out);
    CHECK(out.size() == 1 && out[0].offset == 100 && out[0].flags == 0);
}

void chunks()
{
    TailState tail(true);
    std::vector<TailDelivery> out;
    tail.feed("abc\ndef", out);
    tail.feed(std::string("\0g", 2), out);
    CHECK(out.size() == 2);
    CHECK(out[0].text == "abc\ndef" && out[0].offset == 0);
    CHECK(out[1].text == std::string("\0g", 2) && out[1].offset == 7);
}

void long_lines()
{
    TailState tail(false, 4);
    std::vector<TailDelivery> out;
    tail.feed("abcdefghij\nxy", out);
    CHECK(out.size() == 3);
    CHECK(out[0].text == "abcd" && out[0].offset == 0 && out[0].flags == Flag::partial);
    CHECK(out[1].text == "efgh" && out[1].offset == 4 && out[1].flags == Flag::partial);
    CHECK(out[2].text == "ij" && out[2].offset == 8 && out[2].flags == 0);
    out.clear();
    // Exactly the limit, then the newline in a later read: last piece isn't partial.
    TailState exact(false, 4);
    exact.feed("abcd", out);
    CHECK(out.empty() && exact.held_bytes() == 4);
    exact.feed("\n", out);
    CHECK(out.size() == 1 && out[0].text == "abcd" && out[0].flags == 0);
}

void truncation_and_rotation()
{
    TailState tail(false);
    std::vector<TailDelivery> out;
    tail.feed("old\npart", out);
    CHECK(!tail.observe_size(8));
    CHECK(tail.observe_size(3)); // truncated
    CHECK(tail.read_offset() == 0 && tail.held_bytes() == 0);
    tail.feed("new\nnext\n", out);
    CHECK(out.size() == 3);
    CHECK(out[1].text == "new" && out[1].offset == 0 && (out[1].flags & Flag::reset) != 0);
    CHECK(out[2].text == "next" && out[2].flags == 0);

    tail.reset(); // rotation
    out.clear();
    tail.feed("r", out);
    CHECK(out.empty());
    tail.feed("\n", out);
    CHECK(out.size() == 1 && out[0].flags == Flag::reset);

    TailState chunked(true);
    chunked.reset();
    out.clear();
    chunked.feed("z", out);
    CHECK(out.size() == 1 && out[0].flags == Flag::reset);
}
}

int main()
{
    lines();
    start_at_end();
    chunks();
    long_lines();
    truncation_and_rotation();
    return LexTests::finish("TailTests");
}

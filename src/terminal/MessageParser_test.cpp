/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <terminal/MessageParser.h>
#include <terminal/Parser.h>

#include <crispy/base64.h>

#include <catch2/catch.hpp>

#include <iostream>

using terminal::MessageParser;
using namespace std::string_view_literals;

TEST_CASE("MessageParser.empty", "[MessageParser]")
{
    auto const m = MessageParser::parse("");
    CHECK(m.body().size() == 0);
    CHECK(m.headers().size() == 0);
}

TEST_CASE("MessageParser.headers.one", "[MessageParser]")
{
    SECTION("without value") {
        auto const m = MessageParser::parse("name=");
        REQUIRE(!!m.header("name"));
        CHECK(*m.header("name") == "");
    }
    SECTION("with value") {
        auto const m = MessageParser::parse("name=value");
        CHECK(m.header("name"));
        CHECK(*m.header("name") == "value");
    }
}

TEST_CASE("MessageParser.header.base64")
{
    auto const m = MessageParser::parse(fmt::format("name=!{}", crispy::base64::encode("\033\0\x07"sv)));
    CHECK(m.header("name"));
    CHECK(*m.header("name") == "\033\0\x07"sv);
}

TEST_CASE("MessageParser.headers.many", "[MessageParser]")
{
    SECTION("without value") {
        auto const m = MessageParser::parse("name=,name2=");
        CHECK(m.body().size() == 0);
        REQUIRE(!!m.header("name"));
        REQUIRE(!!m.header("name2"));
        CHECK(m.header("name")->empty());
        CHECK(m.header("name2")->empty());
    }
    SECTION("with value") {
        auto const m = MessageParser::parse("name=value,name2=other");
        CHECK(m.body().size() == 0);
        REQUIRE(!!m.header("name"));
        REQUIRE(!!m.header("name2"));
        CHECK(*m.header("name") == "value");
        CHECK(*m.header("name2") == "other");
    }
    SECTION("mixed value 1") {
        auto const m = MessageParser::parse("name=,name2=other");
        CHECK(m.body().size() == 0);
        REQUIRE(!!m.header("name"));
        REQUIRE(!!m.header("name2"));
        CHECK(*m.header("name") == "");
        CHECK(*m.header("name2") == "other");
    }
    SECTION("mixed value 2") {
        auto const m = MessageParser::parse("name=some,name2=");
        CHECK(m.body().size() == 0);
        REQUIRE(!!m.header("name"));
        REQUIRE(!!m.header("name2"));
        CHECK(*m.header("name") == "some");
        CHECK(*m.header("name2") == "");
    }

    SECTION("superfluous comma 1") {
        auto const m = MessageParser::parse(",foo=text,,,bar=other,");
        CHECK(m.headers().size() == 2);
        REQUIRE(!!m.header("foo"));
        REQUIRE(!!m.header("bar"));
        CHECK(*m.header("foo") == "text");
        CHECK(*m.header("bar") == "other");
    }

    SECTION("superfluous comma many") {
        auto const m = MessageParser::parse(",,,foo=text,,,bar=other,,,");
        CHECK(m.headers().size() == 2);
        REQUIRE(m.header("foo"));
        REQUIRE(m.header("bar"));
        CHECK(*m.header("foo") == "text");
        CHECK(*m.header("bar") == "other");
    }
}

TEST_CASE("MessageParser.body", "[MessageParser]")
{
    SECTION("empty body") {
        auto const m = MessageParser::parse(";");
        CHECK(m.headers().size() == 0);
        CHECK(m.body().size() == 0);
    }

    SECTION("simple body") {
        auto const m = MessageParser::parse(";foo");
        CHECK(m.headers().size() == 0);
        CHECK(m.body() == std::vector<uint8_t>{'f', 'o', 'o'});
    }

    SECTION("headers and body") {
        auto const m = MessageParser::parse("a=A,bee=eeeh;foo");
        CHECK(m.body() == std::vector<uint8_t>{'f', 'o', 'o'});
        REQUIRE(m.header("a"));
        REQUIRE(m.header("bee"));
        CHECK(*m.header("a") == "A");
        CHECK(*m.header("bee") == "eeeh");
    }

    SECTION("binary body") { // ESC \x1b \033
        auto const m = MessageParser::parse("a=A,bee=eeeh;\0\x1b\xff"sv);
        CHECK(m.body() == std::vector<uint8_t>{0x00, 0x1b, 0xff});
        REQUIRE(!!m.header("a"));
        REQUIRE(m.header("bee"));
        CHECK(*m.header("a") == "A");
        CHECK(*m.header("bee") == "eeeh");
    }
}

class MessageParserTest : public terminal::BasicParserEvents
{
  private:
    std::unique_ptr<terminal::ParserExtension> parserExtension_;

  public:
    terminal::Message message;

    void hook(char) override
    {
        parserExtension_ = std::make_unique<MessageParser>([&](terminal::Message&& _message) {
            message = std::move(_message);
        });
    }

    void put(char32_t _char) override
    {
        if (parserExtension_)
            parserExtension_->pass(_char);
    }

    void unhook() override
    {
        if (parserExtension_)
        {
            parserExtension_->finalize();
            parserExtension_.reset();
        }
    }
};

TEST_CASE("MessageParser.VT_embedded")
{
    auto vtEvents = MessageParserTest{};
    auto vtParser = terminal::parser::Parser{vtEvents};

    vtParser.parseFragment(fmt::format("\033Pxa=foo,b=bar;!{}\033\\", crispy::base64::encode("abc")));

    REQUIRE(!!vtEvents.message.header("a"));
    REQUIRE(!!vtEvents.message.header("b"));
    CHECK(*vtEvents.message.header("a") == "foo");
    CHECK(*vtEvents.message.header("b") == "bar");
    CHECK(vtEvents.message.body() == std::vector<uint8_t>{'a', 'b', 'c'});
}

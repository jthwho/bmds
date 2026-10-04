/**
 * @file      error.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <cerrno>
#include <cstring>
#include <bmds/error.h>
#include <bmds/detail/esperr.h>

using bmds::Error;

TEST_CASE("Error") {
        SUBCASE("default is Ok") {
                Error err;
                CHECK(err.isOk());
                CHECK_FALSE(err.isError());
                CHECK(err == Error::Ok);
        }

        SUBCASE("comparison and code") {
                Error err = Error::NoMem;
                CHECK(err.isError());
                CHECK(err.code() == Error::NoMem);
                CHECK(err == Error::NoMem);
                CHECK(err != Error::Timeout);
        }

        SUBCASE("every code has a distinct name and a description") {
                for (int i = 0; i < Error::CodeCount; ++i) {
                        Error a(static_cast<Error::Code>(i));
                        INFO("code ", i);
                        REQUIRE(a.name() != nullptr);
                        REQUIRE(a.desc() != nullptr);
                        CHECK(std::strlen(a.name()) > 0);
                        CHECK(std::strlen(a.desc()) > 0);
                        for (int j = 0; j < i; ++j) {
                                CHECK(std::strcmp(a.name(), Error(static_cast<Error::Code>(j)).name()) != 0);
                        }
                }
                CHECK(std::strcmp(Error(Error::NoMem).name(), "NoMem") == 0);
        }

        SUBCASE("out-of-range code is reported as Unknown") {
                Error err(static_cast<Error::Code>(250));
                CHECK(std::strcmp(err.name(), "Unknown") == 0);
        }

        SUBCASE("fromEspErr") {
                namespace E = bmds::Detail::EspErr;
                CHECK(Error::fromEspErr(E::Ok) == Error::Ok);
                CHECK(Error::fromEspErr(E::NoMem) == Error::NoMem);
                CHECK(Error::fromEspErr(E::InvalidArg) == Error::Invalid);
                CHECK(Error::fromEspErr(E::Timeout) == Error::Timeout);
                CHECK(Error::fromEspErr(E::NotFound) == Error::NotExist);
                CHECK(Error::fromEspErr(E::Fail) == Error::LibraryFailure);
                CHECK(Error::fromEspErr(0x7fff) == Error::LibraryFailure);
        }

        SUBCASE("syserr") {
                CHECK(Error::syserr(0) == Error::Ok);
                CHECK(Error::syserr(ENOENT) == Error::NotExist);
                CHECK(Error::syserr(ENOMEM) == Error::NoMem);
                CHECK(Error::syserr(EAGAIN) == Error::WouldBlock);
                CHECK(Error::syserr(ENOTSUP) == Error::NotSupported);
                CHECK(Error::syserr(EOPNOTSUPP) == Error::NotSupported);
                CHECK(Error::syserr(EPIPE) == Error::BrokenPipe);
                CHECK(Error::syserr(-12345) == Error::UnsupportedSystemError);
                errno = EBUSY;
                CHECK(Error::syserr() == Error::Busy);
        }

        SUBCASE("is constexpr and one byte") {
                constexpr Error err = Error::Full;
                static_assert(err.isError());
                static_assert(sizeof(Error) == 1);
        }
}

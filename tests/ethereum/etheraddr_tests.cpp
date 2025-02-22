/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

 #include <boost/test/unit_test.hpp>
 
 #include <graphene/utilities/CommonData.hpp>
 
 using namespace graphene::utilities;
 
 BOOST_AUTO_TEST_SUITE( ether_address_tests )
 
 BOOST_AUTO_TEST_CASE( non_checksum_test )
 {
    BOOST_CHECK(passesAddressChecksum("0x416f9f8fa0f2e133ad7e1ff52a85e4fbc2b00253", false));

    BOOST_CHECK(!passesAddressChecksum("0x416f9f8fa0f2e133ad7e1ff52a85e4fbc2b0025x", false));

    BOOST_CHECK(passesAddressChecksum("0x416f9f8fa0f2E133ad7e1FF52a85e4fBC2B00253", false));
 }

 BOOST_AUTO_TEST_CASE( serialization_json_test )
 {
    BOOST_CHECK(passesAddressChecksum("0x416f9f8fa0f2E133ad7e1FF52a85e4fBC2B00253", true));

    BOOST_CHECK(!passesAddressChecksum("0x416F9f8fa0f2E133ad7e1FF52a85e4fBC2B00253", true));
 }
 
 BOOST_AUTO_TEST_SUITE_END()
 
 //#define BOOST_TEST_MODULE "C++ Unit Tests for Graphene Blockchain Database"
#include <cstdlib>
#include <iostream>
#include <boost/test/included/unit_test.hpp>

boost::unit_test::test_suite* init_unit_test_suite(int argc, char* argv[]) {
   std::srand(time(NULL));
   std::cout << "Random number generator seeded to " << time(NULL) << std::endl;
   return nullptr;
}
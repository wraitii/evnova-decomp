// Precompiled header for evnova_tests (EVNOVA_ENABLE_PCH).
//
// The test target pays the same libc++ prefix as the runtime plus Catch2:
// catch_tostring.hpp and catch_test_macros.hpp together accounted for ~29 s of
// inclusive header time across the 45 test TUs.
#pragma once

#include "nova_pch.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

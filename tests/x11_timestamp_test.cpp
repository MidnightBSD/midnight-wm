// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "x11_timestamp.hpp"

using midnight::timestamp_before;
static_assert(!timestamp_before(10, 10));
static_assert(timestamp_before(10, 11));
static_assert(!timestamp_before(11, 10));
static_assert(timestamp_before(0xfffffff0U, 0x00000010U));
static_assert(!timestamp_before(0x00000010U, 0xfffffff0U));
static_assert(timestamp_before(0xfffffffeU, 1));
int main() {}

#pragma once
// Shim: the few assertions the vendored Xenia headers use, as no-ops.
#include <cassert>
#define assert_true(expr) ((void)0)
#define assert_false(expr) ((void)0)
#define assert_zero(expr) ((void)0)
#define assert_not_zero(expr) ((void)0)
#define assert_unhandled_case(v) ((void)0)
#define assert_always(...) ((void)0)
#define assert_size(type, size) static_assert(sizeof(type) == (size), #type " size")
#define assert_not_null(expr) ((void)0)
#define static_assert_size(type, size) static_assert(sizeof(type) == (size), #type " size")

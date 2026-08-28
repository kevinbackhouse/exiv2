// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef EXIV2_RECURSION_GUARD_HPP
#define EXIV2_RECURSION_GUARD_HPP

// *****************************************************************************
#include <cstddef>
#include "error.hpp"

// *****************************************************************************
// namespace extensions
namespace Exiv2 {

class EXIV2API RecursionLimit final {
  friend class RecursionGuard;

 public:
  explicit RecursionLimit(size_t limit) : remaining_(limit) {
  }

  size_t remaining() const {
    return remaining_;
  }

 private:
  //! Remaining quota of recursive calls. Gets decremented at the
  //! beginning of a recursive function and incremented at the end. An
  //! exception will be thrown if this number hits zero.
  size_t remaining_;
};  // class RecursionLimit

/*!
  @brief Used to prevent excessively deep recursion (which could
  cause stack exhaustion if Exiv2 is run on a very deeply nested file.
  It's a friend class of Image so that it
  can modify Image::max_recursion_depth_.

  Usage: use Image::getRecursionGuard() to create a local variable of
  type Image::RecursionGuard at the beginning of a recursive
  method. Image::getRecursionGuard() decrements max_recursion_depth_
  and RecursionGuard increments it back to its original value in its
  desctructor (which will be called when the function exits).

  Example usage:

  void MyImage::recursiveParse(...) {
    const auto recGuard = this->getRecursionGuard();

    ...
  }
*/
class EXIV2API RecursionGuard final {
 public:
  explicit RecursionGuard(RecursionLimit& limit) : limit_(limit) {
    if (limit_.remaining_ == 0) {
      throw Error(ErrorCode::kerMaxRecursionDepth);
    }
    limit_.remaining_--;
  }

  ~RecursionGuard() {
    limit_.remaining_++;
  }

  // Prevent copying
  RecursionGuard() = default;
  RecursionGuard(const RecursionGuard&) = delete;
  RecursionGuard& operator=(const RecursionGuard&) = delete;
  RecursionGuard(RecursionGuard&&) = delete;
  RecursionGuard& operator=(RecursionGuard&&) = delete;

 private:
  RecursionLimit& limit_;
};  // class RecursionGuard

}  // namespace Exiv2

#define RECURSION_GUARD(limit) Exiv2::RecursionGuard _recursion_guard(limit)

#endif  // EXIV2_RECURSION_GUARD_HPP

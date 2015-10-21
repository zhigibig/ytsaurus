#pragma once

#include "common.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

//! A simple guard that executes a given function at the end of scope.
class TFinallyGuard
    : public TNonCopyable
{
public:
    explicit TFinallyGuard(std::function<void()> finally)
        : Finally_(std::move(finally))
    { }

    void Release()
    {
        Released_ = true;
    }

    ~TFinallyGuard()
    {
        if (!Released_) {
            Finally_();
        }
    }

private:
    bool Released_ = false;
    const std::function<void()> Finally_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

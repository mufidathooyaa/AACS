#pragma once
#include <stdint.h>

// Input true means the active-LOW GPIO is pressed. Sample every ~10 ms.
// Any observed release cancels the hold; button bounce can only delay a reset.
class ButtonHold {
 public:
  bool update(bool pressed, uint32_t now) {
    if (!pressed) {
      holding_ = false;
      fired_ = false;
      return false;
    }
    if (!holding_) {
      holding_ = true;
      since_ = now;
    }
    if (!fired_ && uint32_t(now - since_) >= 3000U) {
      fired_ = true;
      return true;
    }
    return false;
  }
 private:
  bool holding_ = false;
  bool fired_ = false;
  uint32_t since_ = 0;
};

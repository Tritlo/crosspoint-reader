#pragma once
#include "activities/Activity.h"

class BootActivity final : public Activity {
 public:
  explicit BootActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity(ActivityId::Boot, "Boot", renderer, mappedInput) {}
  void onEnter() override;
};

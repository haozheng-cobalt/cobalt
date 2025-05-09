// Copyright 2016 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef MEDIA_STARBOARD_SBPLAYER_SET_BOUNDS_HELPER_H_
#define MEDIA_STARBOARD_SBPLAYER_SET_BOUNDS_HELPER_H_

#include <utility>

#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "ui/gfx/geometry/rect.h"

namespace media {

class SbPlayerBridge;

class SbPlayerSetBoundsHelper
    : public base::RefCountedThreadSafe<SbPlayerSetBoundsHelper> {
 public:
  SbPlayerSetBoundsHelper() {}

  void SetPlayerBridge(SbPlayerBridge* player_bridge);
  bool SetBounds(int x, int y, int width, int height);

 private:
  base::Lock lock_;
  SbPlayerBridge* player_bridge_ GUARDED_BY(lock_) = nullptr;
  std::optional<gfx::Rect> rect_ GUARDED_BY(lock_);

  SbPlayerSetBoundsHelper(const SbPlayerSetBoundsHelper&) = delete;
  void operator=(const SbPlayerSetBoundsHelper&) = delete;
};

}  // namespace media

#endif  // MEDIA_STARBOARD_SBPLAYER_SET_BOUNDS_HELPER_H_

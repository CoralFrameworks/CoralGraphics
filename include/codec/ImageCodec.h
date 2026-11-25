/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Transitional forwarding header. The image codec belongs to Graphics, not to the media `codec`
// module; the real header is <graphics/ImageCodec.h>. Keep this for one release so existing
// consumers still compile, then delete it (see docs/frameworks/02-迁移实施方案.md B-4).
#ifndef CORAL_CODEC_IMAGE_CODEC_FORWARD_H
#define CORAL_CODEC_IMAGE_CODEC_FORWARD_H

#include <graphics/ImageCodec.h>

#endif // CORAL_CODEC_IMAGE_CODEC_FORWARD_H

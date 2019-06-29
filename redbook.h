/*
Copyright 2019 Brad Parker

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#ifndef REDBOOK_H__
#define REDBOOK_H__

retro_audio_sample_batch_t audio_batch_cb;
retro_audio_sample_t audio_cb;
retro_video_refresh_t video_cb;

void redbook_init(int width, int height, uint32_t *buf);

void redbook_free(void);

void redbook_run_frame(unsigned input_state);

#endif /* REDBOOK_H__ */

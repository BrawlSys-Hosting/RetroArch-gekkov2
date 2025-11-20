#include <stddef.h>
#include <stdint.h>

#include "netplay_private.h"

#include "../../audio/audio_driver.h"
#include "../../gfx/video_driver.h"
#include "../../input/input_driver.h"

void video_frame_net(const void *data,
   unsigned width, unsigned height, size_t pitch)
{
   video_driver_frame(data, width, height, pitch);
}

void audio_sample_net(int16_t left, int16_t right)
{
   audio_driver_sample(left, right);
}

size_t audio_sample_batch_net(const int16_t *data, size_t frames)
{
   return audio_driver_sample_batch(data, frames);
}

int16_t input_state_net(unsigned port, unsigned device,
   unsigned idx, unsigned id)
{
   return input_driver_state_wrapper(port, device, idx, id);
}

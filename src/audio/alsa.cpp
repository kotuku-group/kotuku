#ifdef ALSA_ENABLED

#include "device_enum.h"

//********************************************************************************************************************
// NB: Can be called on destruction or deactivation.

static void free_alsa(extAudio *Self)
{
   stop_audio_worker(Self);
   Self->BufferFrames = Self->PeriodFrames = 0;
   Self->AudioBuffer.clear();
   if (Self->sndlog) { snd_output_close(Self->sndlog); Self->sndlog = nullptr; }
   if (Self->Handle) { snd_pcm_close(Self->Handle); Self->Handle = nullptr; }
   if (Self->MixHandle) { snd_mixer_close(Self->MixHandle); Self->MixHandle = nullptr; }
}

//********************************************************************************************************************

static ERR init_audio(extAudio *Self)
{
   kt::Log log(__FUNCTION__);
   struct snd::SetVolume setvol;
   snd_pcm_hw_params_t *hwparams;
   snd_pcm_stream_t stream;
   snd_pcm_t *pcmhandle;
   snd_mixer_elem_t *elem;
   snd_mixer_selem_id_t *sid;
   int err, index;
   int16_t channel;
   long pmin, pmax;
   int dir;
   std::string pcm_name;

   if (Self->Handle) {
      log.msg("Audio system is already active.");
      return ERR::Okay;
   }

   log.msg("Initialising sound card device.");

   // If 'plughw:0,0' is used, we get ALSA's software mixer, which allows us to set any kind of output options.
   // If 'hw:0,0' is used, we get precise hardware information.  Otherwise stick to 'default'.

   if (!Self->Device.empty()) pcm_name = Self->Device;
   else pcm_name = "default";

   if (iequals("default", pcm_name)) {
      log.msg("Using the default audio output device.");
   }
   else if (pcm_name.find(':') IS std::string::npos and pcm_name != "null") {
      // Find specific device by ID
      auto selected_device = ALSADeviceEnumerator::find_device_by_id(pcm_name);
      if (selected_device.card_number IS -1) {
         log.warning("Requested device '%s' not found.", pcm_name.c_str());
         return ERR::NoSupport;
      }

      pcm_name = selected_device.device_name;
      log.msg("Using specified device: %s (%s)",
              selected_device.card_id.c_str(), selected_device.card_name.c_str());
   }

   snd_output_stdio_attach(&Self->sndlog, stderr, 0);

   // If a mix handle is open from a previous Activate() attempt, close it

   if (Self->MixHandle) {
      snd_mixer_close(Self->MixHandle);
      Self->MixHandle = nullptr;
   }

   std::vector<VolumeCtl> volctl;
   auto configure_mixer = [&]() -> ERR {
      // Mixer initialisation, for controlling volume

      if ((err = snd_mixer_open(&Self->MixHandle, 0)) < 0) {
         log.warning("snd_mixer_open() %s", snd_strerror(err));
         return ERR::SystemCall;
      }

      if ((err = snd_mixer_attach(Self->MixHandle, pcm_name.c_str())) < 0) {
         log.warning("snd_mixer_attach() %s", snd_strerror(err));
         return ERR::SystemCall;
      }

      if ((err = snd_mixer_selem_register(Self->MixHandle, nullptr, nullptr)) < 0) {
         log.warning("snd_mixer_selem_register() %s", snd_strerror(err));
         return ERR::SystemCall;
      }

      if ((err = snd_mixer_load(Self->MixHandle)) < 0) {
         log.warning("snd_mixer_load() %s", snd_strerror(err));
         return ERR::SystemCall;
      }

      // Build a list of all available volume controls

      snd_mixer_selem_id_alloca(&sid);
      int voltotal = 0;
      for (elem=snd_mixer_first_elem(Self->MixHandle); elem; elem=snd_mixer_elem_next(elem)) voltotal++;

      log.msg("%d mixer controls have been reported by alsa.", voltotal);

      if (voltotal < 1) {
         log.warning("Aborting due to lack of mixers for the sound device.");
         return ERR::NoSupport;
      }

      volctl.reserve(32);

      index = 0;
      for (elem=snd_mixer_first_elem(Self->MixHandle); elem; elem=snd_mixer_elem_next(elem)) {
         volctl.resize(volctl.size() + 1);

         snd_mixer_selem_get_id(elem, sid);
         if (!snd_mixer_selem_is_active(elem)) continue;

         if ((volctl[index].Flags & VCF::CAPTURE) != VCF::NIL) {
            snd_mixer_selem_get_capture_volume_range(elem, &pmin, &pmax);
         }
         else snd_mixer_selem_get_playback_volume_range(elem, &pmin, &pmax);

         if (pmin >= pmax) continue; // Ignore mixers with no range

         log.trace("Mixer Control '%s',%i", snd_mixer_selem_id_get_name(sid), snd_mixer_selem_id_get_index(sid));

         volctl[index].Name = snd_mixer_selem_id_get_name(sid);

         for (channel=0; channel < (int)volctl[index].Channels.size(); channel++) volctl[index].Channels[channel] = -1;

         VCF flags = VCF::NIL;
         if (snd_mixer_selem_has_playback_volume(elem))        flags |= VCF::PLAYBACK;
         if (snd_mixer_selem_has_capture_volume(elem))         flags |= VCF::CAPTURE;
         if (snd_mixer_selem_has_capture_volume_joined(elem))  flags |= VCF::JOINED;
         if (snd_mixer_selem_has_playback_volume_joined(elem)) flags |= VCF::JOINED;
         if (snd_mixer_selem_is_capture_mono(elem))            flags |= VCF::MONO;
         if (snd_mixer_selem_is_playback_mono(elem))           flags |= VCF::MONO;

         // Get the current channel volumes

         volctl[index].Channels.resize(std::ssize(glAlsaConvert));
         if ((flags & VCF::MONO) IS VCF::NIL) {
            for (channel=0; channel < std::ssize(glAlsaConvert); channel++) {
               if (snd_mixer_selem_has_playback_channel(elem, (snd_mixer_selem_channel_id_t)glAlsaConvert[channel]))   {
                  long vol;
                  snd_mixer_selem_get_playback_volume(elem, (snd_mixer_selem_channel_id_t)glAlsaConvert[channel], &vol);
                  volctl[index].Channels[channel] = vol;
               }
            }
         }
         else volctl[index].Channels[0] = 0;

         // By default, input channels need to be muted.  This is because some rare PC's have been noted to cause high
         // pitched feedback, e.g. when the microphone channel is on.  All playback channels are enabled by default.

         if ((snd_mixer_selem_has_capture_switch(elem)) and (!snd_mixer_selem_has_playback_switch(elem))) {
            for (channel=0; channel < std::ssize(glAlsaConvert); channel++) {
               flags |= VCF::MUTE;
               snd_mixer_selem_set_capture_switch(elem, (snd_mixer_selem_channel_id_t)channel, 0);
            }
         }
         else if (snd_mixer_selem_has_playback_switch(elem)) {
            for (channel=0; channel < std::ssize(glAlsaConvert); channel++) {
               snd_mixer_selem_set_capture_switch(elem, (snd_mixer_selem_channel_id_t)channel, 1);
            }
         }

         volctl[index].Flags = flags;

         index++;
      }

      log.msg("Configured %d mixer controls.", index);

      return ERR::Okay;
   };
   if (pcm_name IS "null" or configure_mixer() != ERR::Okay) {
      if (Self->MixHandle) { snd_mixer_close(Self->MixHandle); Self->MixHandle = nullptr; }
      volctl = { VolumeCtl("Master", VCF::PLAYBACK, Self->MasterVolume) };
      log.msg("PCM has no hardware mixer; using software volume.");
   }

   snd_pcm_hw_params_alloca(&hwparams); // Stack allocation, no need to free it

   stream = SND_PCM_STREAM_PLAYBACK;
   if ((err = snd_pcm_open(&pcmhandle, pcm_name.c_str(), stream, SND_PCM_NONBLOCK)) < 0) {
      log.warning("snd_pcm_open(%s) %s", pcm_name.c_str(), snd_strerror(err));
      return ERR::SystemCall;
   }

   std::unique_ptr<snd_pcm_t, decltype(&snd_pcm_close)> pcm_owner(pcmhandle, snd_pcm_close);

   // Set access type, either SND_PCM_ACCESS_RW_INTERLEAVED or SND_PCM_ACCESS_RW_NONINTERLEAVED.

   if ((err = snd_pcm_hw_params_any(pcmhandle, hwparams)) < 0) {
      log.warning("Broken configuration for this PCM: no configurations available");
      return ERR::SystemCall;
   }

   if ((err = snd_pcm_hw_params_set_access(pcmhandle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
      log.warning("set_access() %d %s", err, snd_strerror(err));
      return ERR::SystemCall;
   }

   // Set the preferred audio bit format

   if (Self->BitDepth != 8 and Self->BitDepth != 16 and Self->BitDepth != 32) return ERR::NoSupport;

   if (Self->BitDepth IS 32) {
      if ((err = snd_pcm_hw_params_set_format(pcmhandle, hwparams, SND_PCM_FORMAT_FLOAT_LE)) < 0) {
         log.warning("set_format(32) %s", snd_strerror(err));
         return ERR::SystemCall;
      }
   }
   else if (Self->BitDepth IS 16) {
      if ((err = snd_pcm_hw_params_set_format(pcmhandle, hwparams, SND_PCM_FORMAT_S16_LE)) < 0) {
         log.warning("set_format(16) %s", snd_strerror(err));
         return ERR::SystemCall;
      }
   }
   else if ((err = snd_pcm_hw_params_set_format(pcmhandle, hwparams, SND_PCM_FORMAT_U8)) < 0) {
      log.warning("set_format(8) %s", snd_strerror(err));
      return ERR::SystemCall;
   }

   // Retrieve the bit rate from alsa

   snd_pcm_format_t bitformat;
   snd_pcm_hw_params_get_format(hwparams, &bitformat);

   switch (bitformat) {
      case SND_PCM_FORMAT_S16_LE:
      case SND_PCM_FORMAT_S16_BE:
      case SND_PCM_FORMAT_U16_LE:
      case SND_PCM_FORMAT_U16_BE:
         Self->BitDepth = 16;
         break;

      case SND_PCM_FORMAT_S8:
      case SND_PCM_FORMAT_U8:
         Self->BitDepth = 8;
         break;

      case SND_PCM_FORMAT_FLOAT_LE:
         Self->BitDepth = 32;
         break;

      default:
         log.warning("Hardware uses an unsupported audio format.");
         return ERR::NoSupport;
   }

   log.msg("ALSA bit rate: %d", Self->BitDepth);

   // Set the output rate to the rate that we are using internally.  ALSA will use the nearest possible rate allowed
   // by the hardware.

   dir = 0;
   if ((err = snd_pcm_hw_params_set_rate_near(pcmhandle, hwparams, (uint32_t *)&Self->OutputRate, &dir)) < 0) {
      log.warning("set_rate_near() %s", snd_strerror(err));
      return ERR::SystemCall;
   }

   // Set number of channels

   uint32_t channels = ((Self->Flags & ADF::STEREO) != ADF::NIL) ? 2 : 1;
   if ((err = snd_pcm_hw_params_set_channels_near(pcmhandle, hwparams, &channels)) < 0) {
      log.warning("set_channels_near(%d) %s", channels, snd_strerror(err));
      return ERR::SystemCall;
   }

   if (channels IS 2) Self->Stereo = true;
   else Self->Stereo = false;

   if ((channels < 1) or (channels > 2)) return ERR::NoSupport;

   // Keep the format/rate/channel constraints for the single conservative retry.
   snd_pcm_hw_params_t *base;
   snd_pcm_hw_params_alloca(&base);
   snd_pcm_hw_params_copy(base, hwparams);
   const int requested_periods = Self->Periods;
   const int requested_frames = Self->PeriodSize;
   bool configured = false;
   for (int attempt = 0; attempt < 2; ++attempt) {
      snd_pcm_hw_params_copy(hwparams, base);
      unsigned periods = attempt ? 4 : requested_periods;
      snd_pcm_uframes_t frames = attempt ? 1024 : requested_frames;
      unsigned count_min, count_max;
      snd_pcm_uframes_t frames_min, frames_max;
      dir = 0;
      if (snd_pcm_hw_params_get_periods_min(hwparams, &count_min, &dir) < 0 or
          snd_pcm_hw_params_get_periods_max(hwparams, &count_max, &dir) < 0 or
          snd_pcm_hw_params_get_period_size_min(hwparams, &frames_min, &dir) < 0 or
          snd_pcm_hw_params_get_period_size_max(hwparams, &frames_max, &dir) < 0) break;
      periods = std::clamp(periods, count_min, count_max);
      frames = std::clamp(frames, frames_min, frames_max);
      snd_pcm_uframes_t buffer = 0;
      dir = 0;
      err = snd_pcm_hw_params_set_periods_near(pcmhandle, hwparams, &periods, &dir);
      if (err >= 0) err = snd_pcm_hw_params_set_period_size_near(pcmhandle, hwparams, &frames, &dir);
      if (err >= 0) {
         buffer = frames * periods;
         err = snd_pcm_hw_params_set_buffer_size_near(pcmhandle, hwparams, &buffer);
      }
      if (err >= 0) err = snd_pcm_hw_params(pcmhandle, hwparams);
      if (err >= 0) err = snd_pcm_hw_params_get_period_size(hwparams, &frames, &dir);
      if (err >= 0) err = snd_pcm_hw_params_get_buffer_size(hwparams, &buffer);
      if (err >= 0) err = snd_pcm_hw_params_get_periods(hwparams, &periods, &dir);
      const auto frame_bytes = audio_frame_bytes(Self->BitDepth, channels IS 2);
      if (err >= 0 and periods >= 2 and audio_buffer_valid(frames, buffer, frame_bytes)) {
         Self->PeriodSize = frames;
         Self->Periods = periods;
         Self->PeriodFrames = frames;
         Self->BufferFrames = buffer;
         Self->FrameBytes = frame_bytes;
         configured = true;
         break;
      }
      log.warning("ALSA request %d x %d frames failed (%s); %s.", requested_periods, requested_frames,
         snd_strerror(err), attempt ? "fallback failed" : "retrying 4 x 1024 frames");
      snd_pcm_hw_free(pcmhandle);
   }
   if (!configured) return ERR::NoSupport;

   snd_pcm_sw_params_t *swparams;
   snd_pcm_sw_params_alloca(&swparams);
   if (snd_pcm_sw_params_current(pcmhandle, swparams) < 0 or
       snd_pcm_sw_params_set_avail_min(pcmhandle, swparams, Self->PeriodFrames) < 0 or
       snd_pcm_sw_params_set_start_threshold(pcmhandle, swparams,
          Self->BufferFrames - Self->PeriodFrames) < 0 or
       snd_pcm_sw_params_set_stop_threshold(pcmhandle, swparams, Self->BufferFrames) < 0 or
       snd_pcm_sw_params(pcmhandle, swparams) < 0 or snd_pcm_prepare(pcmhandle) < 0) return ERR::SystemCall;

   Self->AudioBuffer.resize(Self->PeriodFrames * Self->FrameBytes);
   Self->reset_lag();
   log.msg(VLF::INFO, "ALSA requested %d x %d frames; negotiated %d x %d, buffer %lu frames (%.2f ms).",
      requested_periods, requested_frames, Self->Periods, Self->PeriodSize,
      (unsigned long)Self->BufferFrames, 1000.0 * audio_latency(Self->BufferFrames, Self->OutputRate));
   if ((Self->Flags & ADF::SYSTEM_WIDE) != ADF::NIL) {
      log.msg("Applying user configured volumes.");

      auto oldctl = Self->Volumes;
      Self->Volumes = volctl;

      for (int i=0; i < std::ssize(volctl); i++) {
         int j;
         for (j=0; j < std::ssize(oldctl); j++) {
            if (volctl[i].Name == oldctl[j].Name) {
               setvol.Index   = i;
               setvol.Name    = std::string_view{};
               setvol.Flags   = SVF::NIL;
               setvol.Channel = -1;
               setvol.Volume  = oldctl[j].Channels[0];
               if (setvol.Volume >= 0) {
                  if ((oldctl[j].Flags & VCF::MUTE) != VCF::NIL) setvol.Flags |= SVF::MUTE;
                  else setvol.Flags |= SVF::UNMUTE;
                  Action(snd::SetVolume::id, Self, &setvol);
               }
               break;
            }
         }

         // Mixers without user configuration retain their current system volume and mute state.
      }
   }
   else {
      log.msg("Skipping preset volumes.");
      Self->Volumes = volctl;
   }

   // Free existing volume measurements and apply the information that we read from alsa.

   Self->Handle = pcm_owner.release();

   return ERR::Okay;
}

#endif
